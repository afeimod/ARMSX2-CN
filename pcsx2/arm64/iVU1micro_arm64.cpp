// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0
//
// ARM64 VU1 Recompiler — Main driver.
// Phase 2: CompileBlock is a proper code emitter. For each pair it emits:
//   - cycle++ and TPC advance inline
//   - ARM64 BL calls to stall helpers with compile-time-resolved uregs/lregs ptrs
//   - Upper instruction via recVU1_UpperTable (direct BL, no runtime table lookup)
//   - Lower instruction via recVU1_LowerTable (same)
//   - Inline branch/ebit countdown
// VF/VI hazard pairs fall back to vu1Exec for correctness.

#include "Common.h"
#include "DebugTools/Debug.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "Memory.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "VUops.h"
#include "arm64/arm64Emitter.h"
#include "arm64/AsmHelpers.h"
#include "arm64/iVU1micro_arm64.h"
#include "arm64/iVU1IR_arm64.h"
#include "common/Perf.h"

#include <algorithm>
#include <cfenv>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace vixl::aarch64;

// Global instance
recArmVU1 CpuArmVU1;

// VU1 per-cycle interpreter entry point (defined in VU1microInterp.cpp)
extern void vu1Exec(VURegs* VU);

// Flush helpers declared in VU1microInterp.cpp
extern void _vuFlushAll(VURegs* VU);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);

// Deferred XGKICK fire helper — defined in iVU1Lower_arm64.cpp.
// Called one pair after an XGKICK to match microVU's 1-pair delay semantics.
extern void vu1_XGKICK_fire_deferred(VURegs* VU);

// Hazard-fallback XGKICK bridge — defined in iVU1Lower_arm64.cpp.
// Emitted after vu1Exec for an XGKICK pair that took the vi_hazard
// fallback path below. Translates the interp's VU1.xgkick* state into the
// JIT's s_vu1_pending_xgkick_addr scratch and clears VU1.xgkickenable so
// a later hazard fallback's _vuTestPipes doesn't trip the broken
// _vuXGKICKTransfer loop.
extern void vu1_XGKICK_capture_from_interp(VURegs* VU);

// CHECK_XGKICKHACK sync tick — defined in iVU1Lower_arm64.cpp. Advances
// the paced XGKICK transfer by `cycles` cycles without flushing. Emitted
// at memwrite pairs under hack mode to mirror microVU's mVU_XGKICK_SYNC
// (microVU_Compile.inl:895).
extern void vu1_XGKICK_hack_sync(VURegs* VU, u32 cycles);

// Recognize XGKICK by raw lower opcode word. Dispatch path is
//   recVU1_LowerTable[0x40] -> recVU1_LowerOP_Table[0x3C] (T3_00)
//     -> recVU1_LowerOP_T3_00_Table[0x1B] = recVU1_XGKICK
// so the unique bit pattern is:
//   (lower >> 25) == 0x40, (lower & 0x3f) == 0x3C, ((lower >> 6) & 0x1f) == 0x1B
static inline bool isXgkickOp(u32 lower)
{
	return ((lower >> 25) == 0x40u) &&
	       ((lower & 0x3fu) == 0x3Cu) &&
	       (((lower >> 6) & 0x1fu) == 0x1Bu);
}

// Recognize unconditional PC-relative branches (B / BAL) for Phase 2 block
// linking. These are the only branch ops with a single compile-time-known
// target and no runtime condition. Opcode encodings cross-referenced
// against recVU1_LowerTable in iVU1Lower_arm64.cpp:
//   B   : top7 = 0x20
//   BAL : top7 = 0x21
static inline bool isUnconditionalBranchOp(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x20u || top7 == 0x21u;
}

// Recognize conditional PC-relative branches (IBEQ / IBNE / IBLTZ / IBGTZ
// / IBLEZ / IBGEZ) for Phase 3 block linking. Both target PCs (taken and
// not-taken) are compile-time known; only the runtime condition chooses
// between them. Opcode encodings:
//   IBEQ  : top7 = 0x28
//   IBNE  : top7 = 0x29
//   IBLTZ : top7 = 0x2C
//   IBGTZ : top7 = 0x2D
//   IBLEZ : top7 = 0x2E
//   IBGEZ : top7 = 0x2F
static inline bool isConditionalBranchOp(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x28u || top7 == 0x29u
	    || (top7 >= 0x2Cu && top7 <= 0x2Fu);
}

// Recognize indirect branches (JR / JALR) for Phase 4 runtime dispatch.
// Target is `(VU->VI[_Is_].US[0] & 0xFFFF) * 8` — unknown at compile time.
// Opcode encodings:
//   JR   : top7 = 0x24
//   JALR : top7 = 0x25
static inline bool isIndirectBranchOp(u32 lower)
{
	const u32 top7 = lower >> 25;
	return top7 == 0x24u || top7 == 0x25u;
}

// Recognize lower opcodes that write to VU1.Mem. Under CHECK_XGKICKHACK,
// CompileBlock's pre-walk accumulates xgkick cycles between memwrite
// boundaries and emits a vu1_XGKICK_hack_sync tick at each memwrite pair
// — mirrors mVUlow.isMemWrite set by upstream microVU's analyze pass
// (microVU_Analyze.inl:332 for SQ family, microVU_Lower.inl:1230/1273
// for ISW/ISWR). Opcode encodings cross-referenced against
// recVU1_LowerTable (iVU1Lower_arm64.cpp:2515) and the T3 sub-tables.
//
//   SQ   : top7 = 0x01
//   ISW  : top7 = 0x05
//   SQI  : top7 = 0x40, sub = 0x3D (T3_01), idx = 0x0D
//   SQD  : top7 = 0x40, sub = 0x3F (T3_11), idx = 0x0D
//   ISWR : top7 = 0x40, sub = 0x3F (T3_11), idx = 0x0F
static inline bool isMemWriteOp(u32 lower)
{
	const u32 top7 = lower >> 25;
	if (top7 == 0x01u) return true; // SQ
	if (top7 == 0x05u) return true; // ISW
	if (top7 != 0x40u) return false;
	const u32 sub = lower & 0x3fu;
	const u32 idx = (lower >> 6) & 0x1fu;
	if (sub == 0x3Du && idx == 0x0Du) return true; // SQI
	if (sub == 0x3Fu && idx == 0x0Du) return true; // SQD
	if (sub == 0x3Fu && idx == 0x0Fu) return true; // ISWR
	return false;
}

// Predicate: does this lower instruction word decode to a NOP on VU1?
// Only WAITQ and WAITP — the EFU ops that are NOP on VU0 are real on VU1.
// Used by step 8 to elide the Mov+Str+emitVU1Lower scaffold for these ops
// since recVU1_WAITQ / recVU1_WAITP emit zero instructions.
//
// Opcode encodings:
//   WAITQ : top7 = 0x40, sub = 0x3F (T3_11), idx = 0x0E
//   WAITP : top7 = 0x40, sub = 0x3F (T3_11), idx = 0x1E
static inline bool isVU1LowerNOP(u32 lower)
{
	if ((lower >> 25) != 0x40u) return false;
	if ((lower & 0x3fu) != 0x3Fu) return false;
	const u32 idx = (lower >> 6) & 0x1fu;
	return idx == 0x0Eu || idx == 0x1Eu;
}

// ============================================================================
//  Runtime probe for the SH2 / VI[16] divergence at pc=0x0648 (2026-05-06).
//  Shadow-verify caught: at this pair, INTERP drains the FMAC pipe slot
//  (writing VI[REG_STATUS_FLAG] = 0xc0) but JIT does not (leaves it at 0x40).
//  Hypothesis: skip_info[i].skipTestPipes is true here when it shouldn't be.
//  This helper logs the runtime FMAC pipe state right before the JIT's
//  step-6 TestPipes gate, so we can compare against the compile-time
//  skipTestPipes decision (logged at compile time, separately).
// ============================================================================
[[maybe_unused]] static void vu1_probe_pc0x648()
{
	// Fire only on entries where the pipe has work AND we've never seen
	// THIS slot configuration before (keyed on fmaccount + fmacreadpos +
	// the slot's sCycle to dedupe). The shadow-verify halt at this PC
	// happens with fmaccount=1, fmacreadpos=1, so we want that case logged.
	// Skip the fmaccount==0 first-entry case (already logged once and not
	// the divergent state).
	if (VU1.fmaccount == 0)
		return;
	const fmacPipe& s = VU1.fmac[VU1.fmacreadpos & 3];
	// Dedupe by VI[16] PRE-drain — that's the field the harness flags as
	// divergent. We want to see entries with VI[16]=0x40 specifically (the
	// divergent PRE state). Earlier dedupes by sCycle missed this because
	// the same slot recurs across many block invocations with VI[16]
	// already drained to 0xc0. Bumping the dedupe to also include VI[16]
	// + slot.statusflag + slot.flagreg surfaces the divergent shape.
	// Cap the log to first 32 unique states so we don't drown the tombstone.
	const u32 vi16    = VU1.VI[REG_STATUS_FLAG].UL;
	const u32 macflag = VU1.macflag;
	struct ProbeKey { u32 vi16; u32 statusflag; u32 flagreg; u32 macflag; };
	static ProbeKey s_seen[32] = {};
	static int      s_seen_count = 0;
	const ProbeKey k{vi16, s.statusflag, (u32)s.flagreg, macflag};
	for (int i = 0; i < s_seen_count; i++)
	{
		if (s_seen[i].vi16 == k.vi16 && s_seen[i].statusflag == k.statusflag
		 && s_seen[i].flagreg == k.flagreg && s_seen[i].macflag == k.macflag)
			return;
	}
	if (s_seen_count < 32)
		s_seen[s_seen_count++] = k;
	// Use Error (not WriteLn) so the line survives the std::abort in
	// vu1_shadow_halt — WriteLn output goes through a buffered stdio path
	// that may not flush on abort.
	Console.Error("VU1 PROBE pc=0x0648 RUNTIME fmaccount=%u fmacreadpos=%u "
	                "fmacwritepos=%u cycle=%llu  slot[%u]: sCycle=%llu Cycle=%u "
	                "regupper=%u reglower=%u flagreg=0x%x xyzwupper=0x%x xyzwlower=0x%x "
	                "macflag=0x%08x statusflag=0x%08x clipflag=0x%08x  "
	                "VI[16]=0x%08x  matures? (cycle - sCycle = %lld) >= Cycle (%u) -> %d",
		VU1.fmaccount, VU1.fmacreadpos, VU1.fmacwritepos,
		(unsigned long long)VU1.cycle, VU1.fmacreadpos & 3,
		(unsigned long long)s.sCycle, s.Cycle,
		s.regupper, s.reglower, (u32)s.flagreg, s.xyzwupper, s.xyzwlower,
		s.macflag, s.statusflag, s.clipflag,
		VU1.VI[REG_STATUS_FLAG].UL,
		(long long)((s64)VU1.cycle - (s64)s.sCycle), s.Cycle,
		(int)((s64)(VU1.cycle - s.sCycle) >= (s64)s.Cycle));
}

// ============================================================================
//  Shadow-compare debug harness (VU1_SHADOW_VERIFY).
//
//  Mirrors the VU0 harness in iVU0micro_arm64.cpp. Hooks into the native
//  (non-fallback) per-pair path. Pre-pair: snapshots VU1+Mem to s_v1_shadow_pre.
//  Post-pair: saves VU1 (the JIT result) to s_v1_shadow_post, restores VU1
//  from s_v1_shadow_pre, runs vu1Exec(&VU1) for the SAME pair via interp,
//  then compares interp result (VU1) against JIT result (s_v1_shadow_post)
//  field-by-field. Logs the first divergent field per pair, dumps state, and
//  aborts via std::abort (debuggerd → tombstone with native backtrace).
//
//  Constraints:
//    - MUST be used with THREAD_VU1 = false (MTVU off). Under MTVU, VU1 runs
//      on a separate thread and the interp re-run would race with cross-
//      thread state mutations. The verify function runtime-checks THREAD_VU1
//      and bails when it's true.
//    - XGKICK pairs are skipped — re-running interp would emit duplicate GIF
//      packet data. The hook gates on isXgkickOp(lower) at compile time.
//    - Hazard-fallback pairs (vu1Exec) are not verified — interp result
//      trivially equals JIT.
//
//  Limitations:
//    - VU1.Mem is 16KB (vs VU0's 4KB); snapshot cost is 4x higher per pair.
//      Use the cycle window in InterpFlags.h to scope to the area of interest.
//    - GIF / vif1Regs / cpuRegs.cycle are NOT snapshotted in v1. Pairs that
//      mutate those (XGKICK, certain D/T-bit firings) need extension if
//      they're suspected of hosting the divergence.
// ============================================================================
#ifdef VU1_SHADOW_VERIFY
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unwind.h>
#include <dlfcn.h>

alignas(16) static u8 s_v1_shadow_pre [sizeof(VURegs)];
alignas(16) static u8 s_v1_shadow_post[sizeof(VURegs)];
// VU1.Mem (16KB data memory) is allocated separately from VURegs and only
// referenced by pointer inside the struct. SQ/SQI/SQD/ISWR ops write here;
// a buggy JIT codegen for any of those would silently corrupt VU1.Mem
// without tripping any VURegs comparison. Mirror snapshot/restore/compare
// for the Mem buffer so the harness covers stores too.
alignas(16) static u8 s_v1_shadow_pre_mem [VU1_MEMSIZE];
alignas(16) static u8 s_v1_shadow_post_mem[VU1_MEMSIZE];

// Latched on the first detected divergence. Once set, vu1_shadow_verify
// becomes a no-op so a bad scene doesn't flood logcat with thousands of
// follow-on errors before the user can see the original failure. Halt path
// also sets it before std::abort.
static std::atomic<bool> s_v1_shadow_diverged{false};

static void vu1_shadow_snapshot()
{
	if (s_v1_shadow_diverged.load(std::memory_order_relaxed))
		return;
	// MTVU bail: under THREAD_VU1, the interp re-run would race with the
	// MTVU thread's other VU1 work. Skip the harness entirely. The verify
	// hook also bails for symmetry.
	if (THREAD_VU1)
		return;
	std::memcpy(s_v1_shadow_pre, &VU1, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(s_v1_shadow_pre_mem, VU1.Mem, VU1_MEMSIZE);
}

static void vu1_shadow_dump_state(const char* label, const VURegs* state)
{
	Console.Error("--- %s ---", label);
	for (u32 v = 0; v < 32; v++)
	{
		Console.Error("  VF[%2u] = {%08x %08x %08x %08x}", v,
			state->VF[v].UL[0], state->VF[v].UL[1], state->VF[v].UL[2], state->VF[v].UL[3]);
	}
	Console.Error("  ACC    = {%08x %08x %08x %08x}",
		state->ACC.UL[0], state->ACC.UL[1], state->ACC.UL[2], state->ACC.UL[3]);
	for (u32 v = 0; v < 32; v++)
		Console.Error("  VI[%2u] = %08x", v, state->VI[v].UL);
	Console.Error("  Q=%08x  P=%08x  I=%08x  R=%08x",
		state->q.UL, state->p.UL, state->VI[REG_I].UL, state->VI[REG_R].UL);
	Console.Error("  cycle=%llu  macflag=%08x  statusflag=%08x  clipflag=%08x",
		(unsigned long long)state->cycle, state->macflag, state->statusflag, state->clipflag);
	Console.Error("  ebit=%u  branch=%u  branchpc=0x%04x  flags=%08x",
		state->ebit, state->branch, state->branchpc, state->flags);
	Console.Error("  fmacwritepos=%u  fmacreadpos=%u  fmaccount=%u",
		state->fmacwritepos, state->fmacreadpos, state->fmaccount);
	Console.Error("  ialuwritepos=%u  ialureadpos=%u  ialucount=%u",
		state->ialuwritepos, state->ialureadpos, state->ialucount);
	Console.Error("  xgkickenable=%u  xgkickaddr=%08x  xgkickdiff=%u  xgkicksizeremaining=%u",
		state->xgkickenable, state->xgkickaddr, state->xgkickdiff, state->xgkicksizeremaining);
}

struct ShadowUnwindCtx
{
	u32 frames_seen = 0;
	static constexpr u32 kMaxFrames = 32;
};

static _Unwind_Reason_Code vu1_shadow_unwind_cb(struct _Unwind_Context* ctx, void* arg)
{
	auto* state = static_cast<ShadowUnwindCtx*>(arg);
	if (state->frames_seen >= ShadowUnwindCtx::kMaxFrames)
		return _URC_END_OF_STACK;

	const uintptr_t pc = _Unwind_GetIP(ctx);
	if (!pc)
		return _URC_END_OF_STACK;

	Dl_info info{};
	const char* sym = "?";
	uintptr_t off = 0;
	if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname)
	{
		sym = info.dli_sname;
		off = pc - reinterpret_cast<uintptr_t>(info.dli_saddr);
	}
	else if (info.dli_fname)
	{
		sym = info.dli_fname;
		off = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
	}
	Console.Error("  #%2u  pc=0x%016lx  %s+0x%lx",
		state->frames_seen, (unsigned long)pc, sym, (unsigned long)off);
	state->frames_seen++;
	return _URC_NO_REASON;
}

static void vu1_shadow_halt(u32 pc, const char* first_field, const char* first_detail)
{
	if (s_v1_shadow_diverged.exchange(true, std::memory_order_acq_rel))
		return;

	const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + (pc & VU1_PROGMASK));
	const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + ((pc + 4) & VU1_PROGMASK));

	Console.Error("============================================================");
	Console.Error(" VU1 SHADOW DIVERGENCE  pc=0x%04x", pc);
	Console.Error("   first divergent field: %s  %s", first_field, first_detail);
	Console.Error("   pair opcodes: lower=0x%08x  upper=0x%08x", lower, upper);
	Console.Error("============================================================");

	const VURegs* pre   = reinterpret_cast<const VURegs*>(s_v1_shadow_pre);
	const VURegs* jit   = reinterpret_cast<const VURegs*>(s_v1_shadow_post);
	vu1_shadow_dump_state("PRE-PAIR (input state)", pre);
	vu1_shadow_dump_state("JIT RESULT", jit);
	vu1_shadow_dump_state("INTERP RESULT (truth)", &VU1);

	Console.Error("--- Native C++ backtrace ---");
	ShadowUnwindCtx ctx;
	_Unwind_Backtrace(&vu1_shadow_unwind_cb, &ctx);

	Console.Error("============================================================");
	Console.Error(" Aborting — tombstone will follow");
	Console.Error("============================================================");
	std::abort();
}

static void vu1_shadow_log(u32 pc, const char* field, const char* fmt, ...)
{
	char detail[224];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	vu1_shadow_halt(pc, field, detail);
}

static void vu1_shadow_verify(u32 pc)
{
	if (s_v1_shadow_diverged.load(std::memory_order_relaxed))
	{
		// Already halted — but the abort hasn't fired on this thread yet.
		// Restore JIT result so the VM doesn't drift further while we wait.
		std::memcpy(&VU1, s_v1_shadow_post, sizeof(VURegs));
		if (VU1.Mem)
			std::memcpy(VU1.Mem, s_v1_shadow_post_mem, VU1_MEMSIZE);
		return;
	}
	if (THREAD_VU1)
		return;

#if defined(VU1_SHADOW_VERIFY_FROM_CYCLE) || defined(VU1_SHADOW_VERIFY_TO_CYCLE)
	{
		const u64 cur = VU1.cycle;
#ifdef VU1_SHADOW_VERIFY_FROM_CYCLE
		if (cur < (VU1_SHADOW_VERIFY_FROM_CYCLE)) return;
#endif
#ifdef VU1_SHADOW_VERIFY_TO_CYCLE
		if (cur > (VU1_SHADOW_VERIFY_TO_CYCLE))   return;
#endif
	}
#endif

	// Snapshot post-JIT state.
	std::memcpy(s_v1_shadow_post, &VU1, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(s_v1_shadow_post_mem, VU1.Mem, VU1_MEMSIZE);

	// Patch s_v1_shadow_pre's TPC to match the JIT's compile-time-known pc —
	// vu1Exec reads its pair from Micro[VI[REG_TPC]], and a stale TPC from
	// a prior block's tail would point at the wrong pair. Force TPC = pc so
	// interp runs the SAME pair the JIT just executed. Same fix as VU0.
	auto* pre_regs = reinterpret_cast<VURegs*>(s_v1_shadow_pre);
	pre_regs->VI[REG_TPC].UL = pc;

	// Restore pre-pair state and run interp on the same pair. Mem rolled
	// back so interp's stores land on the same starting buffer.
	std::memcpy(&VU1, s_v1_shadow_pre, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(VU1.Mem, s_v1_shadow_pre_mem, VU1_MEMSIZE);
	vu1Exec(&VU1);

	// Compare. Same field list as VU0 (excluding VPU_STAT — see VU0 comment
	// for why VI[REG_VPU_STAT] is excluded from per-pair compare).
	const VURegs* jit  = reinterpret_cast<const VURegs*>(s_v1_shadow_post);
	const VURegs* iref = &VU1;
	bool diverged = false;

	auto check_u32 = [&](const char* name, u32 j, u32 i) -> bool {
		if (j != i) {
			vu1_shadow_log(pc, name, "jit=0x%08x interp=0x%08x", j, i);
			return true;
		}
		return false;
	};

	// Float-bit ULP-tolerant compare for FP lanes. PS2 VU lacks fused FMA, so
	// the JIT emits separate Fmul+Fadd to match interp's two-rounding behavior
	// (see emitTernaryFmac and emitOpFmac comments). However, NEON intermediate
	// rounding can still diverge from x86-host interp by a few low-mantissa
	// bits when VU operands fall in specific ranges (denormal-near, signed
	// zero, half-ulp midpoints). The visible game output is sub-pixel; the
	// harness's role is to catch correctness bugs, not micro-rounding.
	//
	// Threshold: kVfUlpTolerance = 16. Larger differences continue to fire
	// the harness — those are real bugs (not FP precision).
	auto ulp_close = [](u32 a, u32 b, int max_ulps) -> bool {
		if (a == b) return true;
		// Different signs (one +0, one -0 too) → not "close" via ULP.
		if ((a >> 31) != (b >> 31)) return false;
		// As same-sign biased ints, |a - b| is the ULP distance.
		const u32 diff = (a > b) ? (a - b) : (b - a);
		return diff <= static_cast<u32>(max_ulps);
	};
	constexpr int kVfUlpTolerance = 16;

	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (std::memcmp(&jit->VF[v], &iref->VF[v], sizeof(VECTOR)) == 0)
			continue;
		bool lane_close = true;
		for (int l = 0; l < 4; l++)
		{
			if (!ulp_close(jit->VF[v].UL[l], iref->VF[v].UL[l], kVfUlpTolerance))
			{
				lane_close = false;
				break;
			}
		}
		if (lane_close) continue;
		char fname[16];
		std::snprintf(fname, sizeof(fname), "VF[%u]", v);
		vu1_shadow_log(pc, fname,
			"jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
			jit->VF[v].UL[0], jit->VF[v].UL[1], jit->VF[v].UL[2], jit->VF[v].UL[3],
			iref->VF[v].UL[0], iref->VF[v].UL[1], iref->VF[v].UL[2], iref->VF[v].UL[3]);
		diverged = true;
	}

	if (!diverged && std::memcmp(&jit->ACC, &iref->ACC, sizeof(VECTOR)) != 0)
	{
		// Same ULP-tolerant compare for ACC.
		bool acc_close = true;
		for (int l = 0; l < 4; l++)
		{
			if (!ulp_close(jit->ACC.UL[l], iref->ACC.UL[l], kVfUlpTolerance))
			{
				acc_close = false;
				break;
			}
		}
		if (!acc_close)
		{
			vu1_shadow_log(pc, "ACC",
				"jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
				jit->ACC.UL[0], jit->ACC.UL[1], jit->ACC.UL[2], jit->ACC.UL[3],
				iref->ACC.UL[0], iref->ACC.UL[1], iref->ACC.UL[2], iref->ACC.UL[3]);
			diverged = true;
		}
	}

	// macflag / statusflag / clipflag — by-design divergence under vuFlagHack.
	// JIT's Pass 1 (`pair_needs_flags`) elides flag writebacks for FMAC pairs
	// with no in-block flag reader, gated on `EmuConfig.Speedhacks.vuFlagHack`
	// (memo: `armsx2_vuflaghack_honor.md`). Interp always writes the flags.
	// When the speedhack is ON (default), these will diverge whenever the
	// elision fires — same correctness story as VIBackupCycles.
	//
	// Real flag-corruption bugs (where the JIT *should* have written flags
	// but wrote the wrong value) still surface downstream via FCAND/FCOR/IBxx
	// readers diverging on the VI[] array compare AND via the fmac[] slot
	// comparison further down (when reg != 0 and a real reader retires).
	const bool flagHackOn = EmuConfig.Speedhacks.vuFlagHack;
	if (!flagHackOn)
	{
		if (!diverged) diverged = check_u32("macflag",    jit->macflag,    iref->macflag);
		if (!diverged) diverged = check_u32("statusflag", jit->statusflag, iref->statusflag);
		if (!diverged) diverged = check_u32("clipflag",   jit->clipflag,   iref->clipflag);
	}

	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (v == REG_VPU_STAT) continue;
		// VI[REG_Q] and VI[REG_P] are pipeline-retire COPIES of VU->q / VU->p
		// (the authoritative user-visible pipeline outputs). Retirement happens
		// inside _vuTestPipes / _vuFlushAll when the pipe slot matures
		// (`cycle - sCycle >= Cycle`). JIT and interp can disagree on EXACTLY
		// which pair the mature slot retires at — both end up with the same
		// VU->q / VU->p, but the VI[] copy snapshot lands on a different pair.
		// We DO check VU->q and VU->p directly above; that's the user-visible
		// state. The VI[] copies are by-design timing-skewed under the
		// per-pair shadow scope. A real bug in retirement (wrong VALUE, not
		// wrong timing) shows up downstream — Q/P readers see stale values
		// and diverge on VU->q / VU->p in the next few pairs.
		if (v == REG_Q || v == REG_P) continue;
		if (jit->VI[v].UL != iref->VI[v].UL)
		{
			char fname[16];
			std::snprintf(fname, sizeof(fname), "VI[%u]", v);
			diverged = check_u32(fname, jit->VI[v].UL, iref->VI[v].UL);
		}
	}

	if (!diverged && jit->cycle != iref->cycle)
	{
		vu1_shadow_log(pc, "cycle", "jit=%llu interp=%llu",
			(unsigned long long)jit->cycle, (unsigned long long)iref->cycle);
		diverged = true;
	}
	if (!diverged) diverged = check_u32("ebit",         jit->ebit,         iref->ebit);
	if (!diverged) diverged = check_u32("branch",       jit->branch,       iref->branch);
	if (!diverged) diverged = check_u32("branchpc",     jit->branchpc,     iref->branchpc);
	if (!diverged) diverged = check_u32("flags",        jit->flags,        iref->flags);

	// Delay-branch state (matters for branch-in-branch-delay-slot edge cases).
	if (!diverged) diverged = check_u32("delaybranchpc",   jit->delaybranchpc, iref->delaybranchpc);
	if (!diverged) diverged = check_u32("takedelaybranch",
		(u32)jit->takedelaybranch, (u32)iref->takedelaybranch);

	// VIBackup state (rolls back VI on branch backup-cycles unwind).
	//
	// Skipped intentionally — the JIT's analyzeBranchVI pass (Pass 1) elides
	// the entire VI-backup machinery when no IBxx-reader within the relevant
	// look-ahead window needs the rollback. The interp unconditionally backs
	// up every VI write (VIBackupCycles=2 + VIOldValue + VIRegNumber). When
	// the JIT correctly elides, divergence on these three fields is by-
	// design — see `armsx2_analyze_branch_vi.md` ("Runtime VIBackupCycles=2
	// gap unchanged"). Comparing them spams the log with intended diffs and
	// hides actual JIT bugs further down the field list.
	//
	// If a JIT bug DOES corrupt the rollback path, it manifests downstream:
	// the next IBxx pair reads the wrong VI value and diverges on the VI
	// array compare (which we DO check) and on `branch`/`branchpc`. So
	// masking these three is correctness-preserving.

	// Pending Q/P (set by start-of-FDIV/EFU; consumed when result retires).
	if (!diverged) diverged = check_u32("pending_q", jit->pending_q, iref->pending_q);
	if (!diverged) diverged = check_u32("pending_p", jit->pending_p, iref->pending_p);

	if (!diverged && jit->q.UL != iref->q.UL)
		diverged = check_u32("q", jit->q.UL, iref->q.UL);
	if (!diverged && jit->p.UL != iref->p.UL)
		diverged = check_u32("p", jit->p.UL, iref->p.UL);

	if (!diverged) diverged = check_u32("fmacwritepos", jit->fmacwritepos, iref->fmacwritepos);
	if (!diverged) diverged = check_u32("fmacreadpos",  jit->fmacreadpos,  iref->fmacreadpos);
	if (!diverged) diverged = check_u32("fmaccount",    jit->fmaccount,    iref->fmaccount);

	for (u32 i = 0; i < 4 && !diverged; i++)
	{
		fmacPipe jp = jit->fmac[i];
		fmacPipe ip = iref->fmac[i];
		// Normalize: when reg=0 (no real VF writeback), xyzw mask is don't-care.
		if (jp.regupper == 0) jp.xyzwupper = 0;
		if (jp.reglower == 0) jp.xyzwlower = 0;
		if (ip.regupper == 0) ip.xyzwupper = 0;
		if (ip.reglower == 0) ip.xyzwlower = 0;
		// Under vuFlagHack, JIT elides flag computation for FMAC slots whose
		// pair_needs_flags is false. The slot's macflag/statusflag/clipflag
		// fields stay zero in JIT, computed in interp. Zero them on both
		// sides so the slot-content compare ignores flag fields when the
		// speedhack is on. flagreg/regupper/reglower/xyzw* still compared.
		if (flagHackOn)
		{
			jp.macflag = ip.macflag = 0;
			jp.statusflag = ip.statusflag = 0;
			jp.clipflag = ip.clipflag = 0;
		}
		// JIT optimization (iVU1micro_arm64.cpp:3466 emitFMACAddPair):
		// the JIT only stores fmac[i].clipflag when flagreg & (1<<REG_CLIP_FLAG)
		// is set, matching _vuFMACflush's read gate (VUops.cpp:54-55 only reads
		// the slot's clipflag under that same gate). Interp ALWAYS stores
		// clipflag (VUops.cpp:367). Both produce the same observable result —
		// VI[REG_CLIP_FLAG] downstream — but the slot field diverges. Mask it
		// on both sides when the JIT's gate condition is false.
		if ((jp.flagreg & (1u << REG_CLIP_FLAG)) == 0u
			&& (ip.flagreg & (1u << REG_CLIP_FLAG)) == 0u)
		{
			jp.clipflag = ip.clipflag = 0;
		}
		if (std::memcmp(&jp, &ip, sizeof(fmacPipe)) != 0)
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "fmac[%u]", i);
			vu1_shadow_log(pc, fname,
				"jit{ru=%u rl=%u fr=%d xu=%u xl=%u sC=%llu C=%u m=%08x s=%08x c=%08x} "
				"interp{ru=%u rl=%u fr=%d xu=%u xl=%u sC=%llu C=%u m=%08x s=%08x c=%08x}",
				jp.regupper, jp.reglower, jp.flagreg, jp.xyzwupper, jp.xyzwlower,
				(unsigned long long)jp.sCycle, jp.Cycle, jp.macflag, jp.statusflag, jp.clipflag,
				ip.regupper, ip.reglower, ip.flagreg, ip.xyzwupper, ip.xyzwlower,
				(unsigned long long)ip.sCycle, ip.Cycle, ip.macflag, ip.statusflag, ip.clipflag);
			diverged = true;
		}
	}

	if (!diverged) diverged = check_u32("ialuwritepos", jit->ialuwritepos, iref->ialuwritepos);
	if (!diverged) diverged = check_u32("ialureadpos",  jit->ialureadpos,  iref->ialureadpos);
	if (!diverged) diverged = check_u32("ialucount",    jit->ialucount,    iref->ialucount);
	for (u32 i = 0; i < 4 && !diverged; i++)
	{
		const ialuPipe& jp = jit->ialu[i];
		const ialuPipe& ip = iref->ialu[i];
		if (std::memcmp(&jp, &ip, sizeof(ialuPipe)) != 0)
		{
			char fname[24];
			std::snprintf(fname, sizeof(fname), "ialu[%u]", i);
			vu1_shadow_log(pc, fname, "ialu slot mismatch (compare ialuPipe struct)");
			diverged = true;
		}
	}

	if (!diverged && std::memcmp(VU1.Mem, s_v1_shadow_post_mem, VU1_MEMSIZE) != 0)
	{
		// Mem mismatch: SQ/SQI/SQD/ISWR JIT emit produced a different store
		// than interp. Find the first differing byte for the log.
		u32 first_diff = 0;
		for (u32 b = 0; b < VU1_MEMSIZE; b++)
		{
			if (VU1.Mem[b] != s_v1_shadow_post_mem[b])
			{
				first_diff = b;
				break;
			}
		}
		vu1_shadow_log(pc, "Mem",
			"first byte mismatch at offset 0x%04x  jit=0x%02x interp=0x%02x",
			first_diff, s_v1_shadow_post_mem[first_diff], VU1.Mem[first_diff]);
		diverged = true;
	}

	// Restore JIT result so the VM continues with whatever JIT produced —
	// we want to find the bug, not silently fix it. Only matters if we
	// haven't aborted (no divergence detected).
	if (!diverged)
	{
		std::memcpy(&VU1, s_v1_shadow_post, sizeof(VURegs));
		if (VU1.Mem)
			std::memcpy(VU1.Mem, s_v1_shadow_post_mem, VU1_MEMSIZE);
	}
}

// ============================================================================
//  Block-level shadow harness
//
//  Per-pair shadow flushes the VF (NEON) cache before every snapshot/verify
//  BL — that's correct for finding per-pair semantic bugs, but it MASKS
//  cross-pair coherence bugs. Specifically: under VU1_DEFER_VF_WRITES, pair
//  K's FMAC writeback stays in a NEON cache slot until a flush site fires.
//  If pair K+M reads VF[X] memory directly (e.g. via inline Ldr in MTIR /
//  MFP / MFIR / LQ family) without a paired vfCacheFlushOne(X), it sees
//  stale memory. Per-pair shadow's pre-snapshot flush forces memory coherent
//  at every pair boundary, so K+M's emitted code reads correct memory under
//  shadow but stale memory in real execution. Bug invisible to per-pair.
//
//  Block-level shadow runs the WHOLE block end-to-end with the cache living
//  across pairs (matching real execution), then compares end-of-block state.
//  Block epilogue's vfCacheFlushAndInvalidate commits all deferred writes
//  before snapshot-post. A coherence bug shows up as a divergent VF/VI/Mem
//  in the end state.
//
//  Gated separately on VU1_BLOCK_SHADOW_VERIFY so per-pair (`VU1_SHADOW_VERIFY`)
//  can run independently for finding per-pair bugs. Enable both for full
//  coverage; expect per-pair to fire first if the bug surfaces per-pair.
// ============================================================================

#ifdef VU1_BLOCK_SHADOW_VERIFY
alignas(16) static u8 s_v1_block_shadow_pre [sizeof(VURegs)];
alignas(16) static u8 s_v1_block_shadow_post[sizeof(VURegs)];
alignas(16) static u8 s_v1_block_shadow_pre_mem [VU1_MEMSIZE];
alignas(16) static u8 s_v1_block_shadow_post_mem[VU1_MEMSIZE];
static bool s_v1_block_shadow_skip = false;

static void vu1_block_shadow_snapshot()
{
	if (s_v1_shadow_diverged.load(std::memory_order_relaxed)) { s_v1_block_shadow_skip = true; return; }
	if (THREAD_VU1) { s_v1_block_shadow_skip = true; return; }
	s_v1_block_shadow_skip = false;
	std::memcpy(s_v1_block_shadow_pre, &VU1, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(s_v1_block_shadow_pre_mem, VU1.Mem, VU1_MEMSIZE);
}

// startPC: JIT's compile-time block start (byte units within the program).
// numPairs: max number of pairs the JIT compiled. Interp may stop sooner if
// VPU_STAT clears (ebit) — the loop checks the same condition each iter.
static void vu1_block_shadow_verify(u32 startPC, u32 numPairs)
{
	if (s_v1_block_shadow_skip) return;
	if (s_v1_shadow_diverged.load(std::memory_order_relaxed)) return;

	// Capture post-JIT block state. By this point the JIT's epilogue has
	// run vfCacheFlushAndInvalidate, so all deferred writes are in memory.
	std::memcpy(s_v1_block_shadow_post, &VU1, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(s_v1_block_shadow_post_mem, VU1.Mem, VU1_MEMSIZE);

	// Restore pre-block state for interp re-run.
	std::memcpy(&VU1, s_v1_block_shadow_pre, sizeof(VURegs));
	if (VU1.Mem)
		std::memcpy(VU1.Mem, s_v1_block_shadow_pre_mem, VU1_MEMSIZE);
	VU1.VI[REG_TPC].UL = startPC;

	// Replay interp pair-by-pair. VU1's program-end signal is VPU_STAT
	// bit 0x100 cleared by vu1EbitDone. The numPairs cap prevents runaway
	// if a JIT bug produces a post-state where VPU_STAT stays set.
	for (u32 i = 0; i < numPairs; i++)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
		{
			if (VU1.branch)
			{
				VU1.VI[REG_TPC].UL = VU1.branchpc;
				VU1.branch = 0;
			}
			break;
		}
		vu1Exec(&VU1);
	}

	// Compare end-of-block JIT state (in s_v1_block_shadow_post*) vs interp
	// end-state (now in live VU1 / VU1.Mem after the replay).
	const VURegs* jit  = reinterpret_cast<const VURegs*>(s_v1_block_shadow_post);
	const VURegs* iref = &VU1;
	bool diverged = false;

	auto blk_check_u32 = [&](const char* name, u32 j, u32 i) -> bool {
		if (j != i) {
			vu1_shadow_log(startPC, name, "BLOCK jit=0x%08x interp=0x%08x", j, i);
			return true;
		}
		return false;
	};

	// VFs — strict bit-exact compare at block scope (no ULP tolerance:
	// block-end state is settled, intermediate rounding has converged).
	// Real coherence bugs produce stale-read garbage, not 1-ULP drift.
	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (std::memcmp(&jit->VF[v], &iref->VF[v], sizeof(VECTOR)) != 0)
		{
			char fname[16];
			std::snprintf(fname, sizeof(fname), "VF[%u]", v);
			vu1_shadow_log(startPC, fname,
				"BLOCK jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
				jit->VF[v].UL[0], jit->VF[v].UL[1], jit->VF[v].UL[2], jit->VF[v].UL[3],
				iref->VF[v].UL[0], iref->VF[v].UL[1], iref->VF[v].UL[2], iref->VF[v].UL[3]);
			diverged = true;
		}
	}

	if (!diverged && std::memcmp(&jit->ACC, &iref->ACC, sizeof(VECTOR)) != 0)
	{
		vu1_shadow_log(startPC, "ACC",
			"BLOCK jit={%08x,%08x,%08x,%08x} interp={%08x,%08x,%08x,%08x}",
			jit->ACC.UL[0], jit->ACC.UL[1], jit->ACC.UL[2], jit->ACC.UL[3],
			iref->ACC.UL[0], iref->ACC.UL[1], iref->ACC.UL[2], iref->ACC.UL[3]);
		diverged = true;
	}

	for (u32 v = 0; v < 32 && !diverged; v++)
	{
		if (v == REG_VPU_STAT) continue;
		if (v == REG_Q || v == REG_P) continue;
		if (jit->VI[v].UL != iref->VI[v].UL)
		{
			char fname[16];
			std::snprintf(fname, sizeof(fname), "VI[%u]", v);
			diverged = blk_check_u32(fname, jit->VI[v].UL, iref->VI[v].UL);
		}
	}

	if (!diverged && std::memcmp(VU1.Mem, s_v1_block_shadow_post_mem, VU1_MEMSIZE) != 0)
	{
		u32 first_diff = 0;
		for (u32 b = 0; b < VU1_MEMSIZE; b++)
		{
			if (VU1.Mem[b] != s_v1_block_shadow_post_mem[b])
			{
				first_diff = b;
				break;
			}
		}
		vu1_shadow_log(startPC, "Mem",
			"BLOCK first byte mismatch at offset 0x%04x  jit=0x%02x interp=0x%02x",
			first_diff, s_v1_block_shadow_post_mem[first_diff], VU1.Mem[first_diff]);
		diverged = true;
	}

	// Restore JIT post-state (we want the VM to keep running with whatever
	// the JIT produced — finding the bug, not fixing it).
	if (!diverged)
	{
		std::memcpy(&VU1, s_v1_block_shadow_post, sizeof(VURegs));
		if (VU1.Mem)
			std::memcpy(VU1.Mem, s_v1_block_shadow_post_mem, VU1_MEMSIZE);
	}
}
#endif // VU1_BLOCK_SHADOW_VERIFY
#endif // VU1_SHADOW_VERIFY

// ============================================================================
//  microIR Pass 1 — analyze
// ============================================================================
//
// See iVU1IR_arm64.h for the design rationale. This populates the per-pair
// `microOp` overlay from the already-filled `_VURegsNum` arrays + raw
// instruction words, plus the block-level summary flags. Cheap walk: O(N)
// over numPairs (max 256), no BL emission, no allocation.
//
// Branch classification mirrors the LOWER_OPCODE[128] table in VUops.cpp:
//   B    = 0x20, BAL  = 0x21
//   JR   = 0x24, JALR = 0x25
//   IBEQ = 0x28, IBNE = 0x29
//   IBLTZ= 0x2C, IBGTZ= 0x2D, IBLEZ= 0x2E, IBGEZ= 0x2F
//
// Flag-reader ops (drive swapOps): all in the 0x10..0x1F range:
//   FCEQ=0x10, FCAND=0x12, FCOR=0x13
//   FSEQ=0x14, FSAND=0x16, FSOR=0x17
//   FMEQ=0x18, FMAND=0x1A, FMOR=0x1B
//   FCGET=0x1C
// FCSET (0x11) and FSSET (0x15) are flag WRITERS, not readers — excluded
// from the swapOps gate (the old port's mVUanalyzeSflag didn't set swapOps
// for FSSET; it has its own isFSSET path).
namespace armvu1ir
{

static inline BranchKind classifyBranch(u32 lower)
{
	const u32 top7 = lower >> 25;
	switch (top7)
	{
		case 0x20u: return BR_B;
		case 0x21u: return BR_BAL;
		case 0x24u: return BR_JR;
		case 0x25u: return BR_JALR;
		case 0x28u: return BR_IBEQ;
		case 0x29u: return BR_IBNE;
		case 0x2Cu: return BR_IBLTZ;
		case 0x2Du: return BR_IBGTZ;
		case 0x2Eu: return BR_IBLEZ;
		case 0x2Fu: return BR_IBGEZ;
		default:    return BR_NONE;
	}
}

// FSAND/FSEQ/FSOR/FMAND/FMEQ/FMOR/FCAND/FCEQ/FCOR/FCGET — the lower-pipe
// flag-reader ops the old port marks with swapOps. FCSET / FSSET are flag
// WRITERS and intentionally excluded; they don't have the same flag-instance
// read hazard against an upper-side flag write.
static inline bool isFlagReaderOp(u32 lower)
{
	const u32 top7 = lower >> 25;
	switch (top7)
	{
		case 0x10u: // FCEQ
		case 0x12u: // FCAND
		case 0x13u: // FCOR
		case 0x14u: // FSEQ
		case 0x16u: // FSAND
		case 0x17u: // FSOR
		case 0x18u: // FMEQ
		case 0x1Au: // FMAND
		case 0x1Bu: // FMOR
		case 0x1Cu: // FCGET
			return true;
		default:
			return false;
	}
}

static inline bool isFSSETOp(u32 lower)
{
	return (lower >> 25) == 0x15u;
}

void mvu1AnalyzeBlock(
	u32 startPC,
	u32 numPairs,
	const _VURegsNum* uregs_data,
	const _VURegsNum* lregs_data,
	microIR& ir)
{
	ir.count   = numPairs;
	ir.startPC = startPC;
	ir.has_ebit          = false;
	ir.has_branch        = false;
	ir.has_dbit_or_tbit  = false;
	ir.has_ibxx          = false;
	ir.has_vi_backup_set = false;
	ir.has_xgkick        = false;

	// First sweep: per-pair classification + branch/EOB detection.
	u32 pc = startPC;
	for (u32 i = 0; i < numPairs; i++)
	{
		const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
		const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + pc);
		const _VURegsNum& uregs = uregs_data[i];
		const _VURegsNum& lregs = lregs_data[i];

		microOp& mo = ir.info[i];
		mo.upper   = upper;
		mo.lower   = lower;
		mo.pc      = pc;

		mo.iBit = ((upper >> 31) & 1) != 0;
		mo.eBit = ((upper >> 30) & 1) != 0;
		mo.dBit = ((upper >> 28) & 1) != 0;
		mo.tBit = ((upper >> 27) & 1) != 0;
		mo.mBit = ((upper >> 29) & 1) != 0;

		mo.isEOB    = false;  // patched in below
		mo.isBdelay = false;  // patched in below

		// I-bit pairs have no lower instruction — keep all lower-derived
		// flags zero. The CompileBlock pre-walk already left lregs_data[i]
		// zeroed for these.
		if (mo.iBit)
		{
			mo.branch              = BR_NONE;
			mo.isNOP               = true;
			mo.isFSSET             = false;
			mo.isFlagRead          = false;
			mo.isMemWrite          = false;
			mo.isKick              = false;
			mo.vf_write_collision  = false;
			mo.vf_read_after_write = false;
			mo.clip_write_collision  = false;
			mo.clip_read_after_write = false;
			mo.swapOps             = false;
			mo.noWriteVF           = false;
			mo.backupVF            = false;
		}
		else
		{
			mo.branch     = classifyBranch(lower);
			mo.isFlagRead = isFlagReaderOp(lower);
			mo.isFSSET    = isFSSETOp(lower);
			mo.isMemWrite = isMemWriteOp(lower);
			mo.isKick     = isXgkickOp(lower);
			mo.isNOP      = isVU1LowerNOP(lower);

			// VF hazard summary — refined to consider XYZW lane overlap, not
			// just VF reg index. The original (REG-only) detection treated
			// every "same VF reg" pair as a conflict, but partial-lane writes
			// (e.g., MULx writing only X) don't actually conflict with reads
			// of disjoint lanes (e.g., DIV reading W). Lane-overlap matches
			// what aliasFmac() does in the Stage A+B pre-walk.
			//
			// uregs.VFwrite==0 means the upper isn't writing a VF; without
			// that, no read-after-write or write-write conflict is possible.
			const u8 uW_reg  = uregs.VFwrite;
			const u8 uW_xyzw = uregs.VFwxyzw;
			mo.vf_write_collision = (uW_reg != 0) &&
				(lregs.VFwrite == uW_reg) &&
				((uW_xyzw & lregs.VFwxyzw) != 0);
			mo.vf_read_after_write = (uW_reg != 0) && (
				(lregs.VFread0 == uW_reg && (uW_xyzw & lregs.VFr0xyzw) != 0) ||
				(lregs.VFread1 == uW_reg && (uW_xyzw & lregs.VFr1xyzw) != 0));

			// CLIP hazard summary — matches the vi_hazard expression.
			const bool uWritesClip = (uregs.VIwrite & (1u << REG_CLIP_FLAG)) != 0;
			mo.clip_write_collision  = uWritesClip && (lregs.VIwrite & (1u << REG_CLIP_FLAG));
			mo.clip_read_after_write = uWritesClip && (lregs.VIread  & (1u << REG_CLIP_FLAG));

			// swapOps mirrors mVUanalyzeS/M/Cflag: set when the lower is a
			// flag-reader writing a non-zero VI target. The old port also
			// gates this on `It != 0` (otherwise the op is folded to NOP);
			// detect "non-zero VI target" via VIwrite bits, since for
			// FCAND/FCOR/FCEQ the target is fixed to VI[1] (always non-zero
			// in the VI-bitmask sense) and for FSAND/FMAND/FCGET the target
			// is encoded in `_It_` and zero would zero out the VIwrite mask.
			mo.swapOps = mo.isFlagRead && (lregs.VIwrite != 0);

			// Reserved — not yet computed natively. Will be filled in when
			// the doSwapOp / VF backup native fast-path lands.
			mo.noWriteVF = false;
			mo.backupVF  = false;

			// Block-level summary updates.
			if (mo.branch != BR_NONE)
				ir.has_branch = true;
			const u32 lopc = lower >> 25;
			const bool is_IBxx =
				lopc == 0x28u || lopc == 0x29u ||           // IBEQ, IBNE
				(lopc >= 0x2Cu && lopc <= 0x2Fu);            // IBLTZ/GTZ/LEZ/GEZ
			if (is_IBxx)
				ir.has_ibxx = true;
			if (mo.isKick)
				ir.has_xgkick = true;
			// Mirrors the block_has_vi_backup_set heuristic in CompileBlock:
			// any lower writing VI[0..15] with a non-BRANCH pipe could call
			// emitBackupVI. Overapproximated, soundness-safe.
			if ((lregs.VIwrite & 0xFFFFu) != 0u && lregs.pipe != VUPIPE_BRANCH)
				ir.has_vi_backup_set = true;
		}

		if (mo.eBit) ir.has_ebit         = true;
		if (mo.dBit) ir.has_dbit_or_tbit = true;
		if (mo.tBit) ir.has_dbit_or_tbit = true;

		pc = (pc + 8) & (VU1_PROGSIZE - 1);
	}

	// Second sweep: mark isBdelay (the pair following any branch) and
	// isEOB (the last pair in the block, plus the pair after a branch's
	// delay slot if the delay also has an E-bit). The arm64 compiler
	// terminates the block at the delay slot of a branch or at an E-bit
	// pair, so isEOB is just `i == numPairs - 1`.
	if (numPairs > 0)
		ir.info[numPairs - 1].isEOB = true;

	for (u32 i = 0; i + 1 < numPairs; i++)
	{
		if (ir.info[i].branch != BR_NONE)
			ir.info[i + 1].isBdelay = true;
	}

	// analyzeBranchVI (audit item #12): per-pair gate for VI backup BLs.
	// Default false; flipped true when either:
	//   (a) Some downstream branch within 4 pairs reads a VI this pair
	//       writes — the branch will need the OLD value, so we must
	//       backup before overwriting. Mirrors x86 microVU_Analyze.inl
	//       analyzeBranchVI's backward walk.
	//   (b) This pair is in the last 4 pairs of the block AND writes a VI
	//       in [1..15]. Conservative: a successor block's branch in its
	//       first 4 pairs may read this VI; without cross-block analysis
	//       we can't prove it doesn't.
	for (u32 i = 0; i < numPairs; i++)
		ir.info[i].needs_vi_backup = false;

	// Pass (a): forward branch scan + backward writer walk. The 4-pair
	// window matches the IALU pipe maturity. JR/JALR also read a VI for
	// their target → included via lregs.VIread bits regardless of branch
	// kind. Mask off VI[0] (hardwired zero) and VI[16..31] (flag/special).
	const u32 cacheable_vi_mask = 0xFFFEu; // VI[1..15] only
	for (u32 i = 0; i < numPairs; i++)
	{
		if (ir.info[i].branch == BR_NONE)
			continue;
		const u32 branch_reads = lregs_data[i].VIread & cacheable_vi_mask;
		if (branch_reads == 0)
			continue;
		// Walk backward up to 4 pairs (or to block start). Find the
		// LATEST writer of each VI bit in branch_reads.
		const u32 lookback_start = (i >= 4) ? (i - 4) : 0;
		for (u32 j = i; j > lookback_start; j--)
		{
			const u32 prev = j - 1;
			const u32 wrote = lregs_data[prev].VIwrite & branch_reads;
			if (wrote != 0)
			{
				ir.info[prev].needs_vi_backup = true;
				// Mask off the bits we just resolved — earlier writers
				// of those bits are overwritten by this later writer
				// before reaching the branch.
				const u32 remaining = branch_reads & ~wrote;
				if (remaining == 0)
					break;
			}
		}
	}

	// Pass (b): cross-block conservative — last 4 pairs that write VI
	// [1..15] need backup since the successor may branch on them.
	const u32 tail_start = (numPairs >= 4) ? (numPairs - 4) : 0;
	for (u32 i = tail_start; i < numPairs; i++)
	{
		if ((lregs_data[i].VIwrite & cacheable_vi_mask) != 0
		    && lregs_data[i].pipe != VUPIPE_BRANCH)
			ir.info[i].needs_vi_backup = true;
	}

	// Update has_vi_backup_set to reflect the precise post-analyzeBranchVI
	// truth, not the pre-walk overapproximation. The earlier sweep already
	// initialized it from the wide heuristic; tighten here so step 6b's
	// decrement-elision (skip_vibackup_decrement in CompileBlock) is exact.
	ir.has_vi_backup_set = false;
	for (u32 i = 0; i < numPairs; i++)
	{
		if (ir.info[i].needs_vi_backup)
		{
			ir.has_vi_backup_set = true;
			break;
		}
	}

	// Dead VF write elision (FMAC opt #14): forward-walk up to 4 pairs ahead
	// to find a covering overwrite of this pair's VF write with no
	// intervening read of any live lane. Memory consistency is preserved by
	// the overwrite's own write-through Str — the downstream block sees the
	// up-to-date value at flush. Pairs in the last 4 are still eligible:
	// the overwrite (if found within block) provides the persisted value;
	// if no in-block overwrite is found, dead==false and the write fires
	// normally. Hazard pairs (vf_read_after_write / vf_write_collision)
	// fall through interp anyway, so the flag is moot for them — but
	// computing it uniformly is cheap and harmless.
	for (u32 i = 0; i < numPairs; i++)
	{
		ir.info[i].dead_vf_write_upper = false;
		ir.info[i].dead_vf_write_lower = false;
	}

	auto isDeadWrite = [&](u32 K, u32 dst_vf, u32 dst_mask) -> bool
	{
		if (dst_vf == 0 || dst_mask == 0)
			return false;
		u32 covered = 0;
		const u32 limit = std::min<u32>(K + 4, numPairs - 1);
		for (u32 j = K + 1; j <= limit; j++)
		{
			const _VURegsNum& uj = uregs_data[j];
			const _VURegsNum& lj = lregs_data[j];

			// Reads of dst_vf from this pair (both upper and lower).
			u32 reads = 0;
			if (uj.VFread0 == dst_vf) reads |= uj.VFr0xyzw;
			if (uj.VFread1 == dst_vf) reads |= uj.VFr1xyzw;
			if (lj.VFread0 == dst_vf) reads |= lj.VFr0xyzw;
			if (lj.VFread1 == dst_vf) reads |= lj.VFr1xyzw;

			// If any still-live lane of K's write is read, K is alive.
			const u32 live = dst_mask & ~covered;
			if (reads & live)
				return false;

			// Add this pair's writes (upper + lower) to the covered mask.
			if (uj.VFwrite == dst_vf) covered |= uj.VFwxyzw;
			if (lj.VFwrite == dst_vf) covered |= lj.VFwxyzw;

			if ((covered & dst_mask) == dst_mask)
				return true;
		}
		return false;
	};

	for (u32 i = 0; i < numPairs; i++)
	{
		const _VURegsNum& ui = uregs_data[i];
		const _VURegsNum& li = lregs_data[i];
		ir.info[i].dead_vf_write_upper = isDeadWrite(i, ui.VFwrite, ui.VFwxyzw);
		ir.info[i].dead_vf_write_lower = isDeadWrite(i, li.VFwrite, li.VFwxyzw);
	}

	// Same-VF different-lane batching (FMAC opt #17): adjacent FMAC upper
	// writers targeting the same VF, K writing X-only and K+1 writing
	// Y-only (or vice versa) batch into one Str d covering the XY half.
	// Saves 1 store per matched pair vs the partial-lane peephole's two
	// Str s.
	//
	// Stash uses d10 (lower 64 of v10) — AAPCS64 callee-saved, so BLs
	// in K's tail (branch/ebit/XGKICK countdowns) or K+1's prologue
	// (stall checks, TestPipes) can't clobber it. The upper 64 of v10
	// is caller-saved, so we deliberately restrict to the XY pair only.
	// ZW-style batching would need a different stash.
	for (u32 i = 0; i < numPairs; i++)
	{
		ir.info[i].batch_with_next = false;
		ir.info[i].batch_from_prev = false;
	}

	for (u32 i = 0; i + 1 < numPairs; i++)
	{
		const _VURegsNum& ui = uregs_data[i];
		const _VURegsNum& un = uregs_data[i + 1];
		const _VURegsNum& li = lregs_data[i];
		const _VURegsNum& ln = lregs_data[i + 1];

		// Both pairs must have FMAC upper writers targeting the same VF.
		if (ui.pipe != VUPIPE_FMAC || un.pipe != VUPIPE_FMAC)
			continue;
		const u32 vfX = ui.VFwrite;
		if (vfX == 0 || vfX != un.VFwrite)
			continue;

		// Strict XY pattern: K writes exactly X (mask 0x8), K+1 writes
		// exactly Y (mask 0x4), or the symmetric flip. Other lane pairs
		// would need lanes 2/3 of the stash which sit in the caller-
		// saved upper 64 of v10.
		const bool xy = (ui.VFwxyzw == 0x8u && un.VFwxyzw == 0x4u);
		const bool yx = (ui.VFwxyzw == 0x4u && un.VFwxyzw == 0x8u);
		if (!xy && !yx)
			continue;

		// K+1 must not read VF[X] in any lane. Reading K's deferred lane
		// would see stale memory; reading any other lane is technically
		// safe but we forbid all reads to keep the cache contract simple.
		if (un.VFread0 == vfX || un.VFread1 == vfX)
			continue;
		if (ln.VFread0 == vfX || ln.VFread1 == vfX)
			continue;

		// K's lower must not read OR write VF[X]. A lower read sees the
		// pre-K cache slot (we deferred K's upper); a lower write would
		// add a second deferred-mask layer we don't track.
		if (li.VFread0 == vfX || li.VFread1 == vfX)
			continue;
		if (li.VFwrite == vfX)
			continue;

		// Hazard pairs run through vu1Exec interp, never reach the JIT
		// writeback paths. Skip both endpoints.
		if (ir.info[i].vf_read_after_write || ir.info[i].vf_write_collision)
			continue;
		if (ir.info[i].clip_read_after_write || ir.info[i].clip_write_collision)
			continue;
		if (ir.info[i + 1].vf_read_after_write || ir.info[i + 1].vf_write_collision)
			continue;
		if (ir.info[i + 1].clip_read_after_write || ir.info[i + 1].clip_write_collision)
			continue;

		ir.info[i].batch_with_next     = true;
		ir.info[i + 1].batch_from_prev = true;
	}

	// ABS-of-known-positive elimination (FMAC opt #4). Forward-walk the
	// block tracking per-VF per-lane "known non-negative" sign state. The
	// canonical pattern is `MAX vfD, vfA, vf0` — vf0's bit pattern is
	// (0,0,0,1) so each lane of the result is max(vfA.lane, 0_or_1) which
	// is always sign-bit-zero. A subsequent `ABS vfE, vfD` with the
	// matching xyzw mask is then a no-op: we replace Fabs with a direct
	// vfCacheLoadInto(fs, v5), saving one Fabs insn.
	//
	// Lane state is u8 per VF; bit 3 = X, 2 = Y, 1 = Z, 0 = W (FMAC
	// xyzw convention). Set means "this lane's sign bit is known 0."
	// VF[0] is the hardwired (0,0,0,1) constant — always non-neg in
	// every lane.
	for (u32 i = 0; i < numPairs; i++)
		ir.info[i].abs_src_known_non_neg = false;

	{
		u8 lane_state[32] = {0};
		lane_state[0] = 0xFu;

		for (u32 i = 0; i < numPairs; i++)
		{
			const _VURegsNum& ui = uregs_data[i];
			const _VURegsNum& li = lregs_data[i];
			const u32 upper = ir.info[i].upper;
			const u32 op6   = upper & 0x3Fu;
			const u32 ft    = (upper >> 16) & 0x1Fu;
			const u32 fdsub = (upper >> 6)  & 0x1Fu;

			// ABS detection: upper opcode 0x3D, FD-sub-table 0x01,
			// idx 7. Match by full upper bit pattern.
			const bool is_abs = (op6 == 0x3Du) && (fdsub == 7u);
			if (is_abs && ui.VFwxyzw != 0)
			{
				const u32 fs   = (upper >> 11) & 0x1Fu;
				const u32 mask = ui.VFwxyzw;
				if ((lane_state[fs] & mask) == mask)
					ir.info[i].abs_src_known_non_neg = true;
			}

			// Hazard pair runs interp; conservative — drop everything.
			// The pair's writes will hit memory through interp, and we
			// can't statically prove sign of those writes.
			if (ir.info[i].vf_read_after_write || ir.info[i].vf_write_collision
			    || ir.info[i].clip_read_after_write || ir.info[i].clip_write_collision)
			{
				for (int v = 1; v < 32; v++)
					lane_state[v] = 0;
				continue;
			}

			// MAX-with-vf0 detection. Broadcast forms (MAXx/y/z/w =
			// 0x10..0x13) and the full-vector form (MAX = 0x2B). MAXi
			// (0x1D) broadcasts the I scalar — sign unknown — skip.
			//
			// MAXw with ft==0 broadcasts vf0.w = 1.0, so result is
			// max(?, 1) ≥ 1 ≥ 0; still non-negative. Full MAX with
			// ft==0 picks vf0 = (0,0,0,1) as the second operand, so
			// per-lane result is ≥ 0 (XYZ) or ≥ 1 (W).
			bool produces_non_neg = false;
			if (ft == 0 && ((op6 >= 0x10u && op6 <= 0x13u) || op6 == 0x2Bu))
				produces_non_neg = true;
			if (is_abs)
				produces_non_neg = true;

			if (ui.VFwrite != 0)
			{
				const u32 v    = ui.VFwrite;
				const u32 mask = ui.VFwxyzw;
				if (produces_non_neg)
					lane_state[v] |= static_cast<u8>(mask);
				else
					lane_state[v] &= static_cast<u8>(~mask);
			}

			// Lower writes are conservatively unknown (LQ/MOVE/MR32/
			// MFIR/MFP can carry any sign). Clear the written lanes.
			if (li.VFwrite != 0)
			{
				const u32 v    = li.VFwrite;
				const u32 mask = li.VFwxyzw;
				lane_state[v] &= static_cast<u8>(~mask);
			}
		}
	}
}

} // namespace armvu1ir

// ============================================================================
//  Rec emitter dispatch entry points (defined in iVU1Upper/Lower_arm64.cpp).
//  These replaced recVU1_UpperTable[64] / recVU1_LowerTable[128] — direct
//  switch dispatchers in the same TU as the per-op emitters, so the
//  compiler can inline small emitters and drop the indirect-call overhead.
// ============================================================================

void emitVU1Upper(u32 upper);
void emitVU1Lower(u32 lower);

// Flag-deferral state owned by iVU1Upper_arm64.cpp. Set per-pair before
// dispatching the upper emitter — when false, FMAC arithmetic emitters
// skip the BL vu1_fmac_writeback and inline a NEON clamp + store instead.
extern bool g_vu1NeedsFlags;
extern u32 g_vu1CurrentPC;
// analyzeBranchVI gate (audit item #12) — set per-pair from
// ir.info[i].needs_vi_backup before the lower emit. When false, the
// emitBackupVI BL is elided entirely.
extern bool g_vu1NeedsVIBackup;
// U/O flag-bits gate. Set once per block from CompileBlock pre-walk —
// true iff any in-block op reads MAC/STATUS_FLAG, the overflow gamefix
// is enabled, or vuFlagHack is off (exact mode). When false,
// emitFmacInlineWriteback skips ~12 NEON + ~6 GPR insn per writeback.
extern bool g_vu1NeedsUOFlags;
// Dead VF write elision (FMAC opt #14). Set per-pair-per-emit before
// dispatching emitVU1Upper (using upper analysis) and emitVU1Lower
// (using lower analysis). When true, the FMAC writeback's VF cache store
// is elided — flags still update, ACC writes never elided.
extern bool g_vu1DeadVFWrite;
// Same-VF different-lane batching (FMAC opt #17). Set per-pair-per-emit
// before dispatching emitVU1Upper. batch_with_next defers the K writeback
// to a callee-saved stash; batch_from_prev merges and emits one combined
// store. Only set on the first/second halves of a Pass-1-detected XY
// pair; ACC writes ignore the gates.
extern bool g_vu1BatchWithNext;
extern bool g_vu1BatchFromPrev;
// ABS-of-known-positive elimination (FMAC opt #4). Set per-pair-per-emit
// before dispatching emitVU1Upper from Pass 1's `abs_src_known_non_neg`.
// Consumed by emitAbsFmac to swap Fabs for a direct vfCacheLoadInto into v5.
extern bool g_vu1AbsSrcKnownNonNeg;
// vf_read_after_write deferred writeback. CompileBlock sets the flag +
// resets idx to -1 before the upper emit; the FMAC writeback emit spills
// v5 to the stash and records idx/xyzw. CompileBlock clears the flag
// before lower emits, then after lower commits via vfCacheStore.
extern bool g_vu1DeferVfWriteback;
extern int  g_vu1DeferredVfIdx;
extern u8   g_vu1DeferredXyzw;
extern u8   g_vu1DeferredVfStash[16];

// ============================================================================
//  Block cache
// ============================================================================

// VU1_PROGSIZE / VU1_PROGMASK come from VUmicro.h
static constexpr u32 VU1_NUM_SLOTS       = VU1_PROGSIZE / 8; // one slot per instruction-pair
static constexpr u32 VU1_MAX_BLOCK_PAIRS = 256;

// Reserve a slice at the start of the JIT region for the constant pool.
static constexpr u32 POOL_SIZE = 64 * 1024;

// Block linking scaffolding (Phase 1).
//
// Every compiled block has three labeled positions:
//
//   codeEntry:    the prologue. Enter here on first dispatch from
//                 recArmVU1::Execute — loads x21/x24/x25 from memory into
//                 the cached regs, then falls through to linkEntry.
//
//   linkEntry:    right after the prologue. Future phases (direct link
//                 from a predecessor block) will B to this label, skipping
//                 the prologue — the caller's x21/x23/x24/x25 are live
//                 and trusted. Phase 1 doesn't emit any B here yet; it
//                 just records the address.
//
//   returnExit:   right after the block-end XGKICK drain and before the
//                 epilogue flushes. Future phases (linked exit) will place
//                 a patch slot here that either B's to the next block's
//                 linkEntry or falls through to the flush+Ret path. Phase
//                 1 still falls through unconditionally.
//
// Keeping the labels in the block metadata from day 1 lets later phases
// patch without re-walking code. Recording addresses is free; no code is
// emitted for the labels themselves.
// A single direct-link exit slot: one branch instruction (B or B.eq) in
// compiled code that jumps to either (a) the common flush+epilogue path
// at some fixed address after the selector, or (b) a successor block's
// linkEntry. Phase 3 introduces up to TWO exits per block to cover
// conditional branches — exits[0] = not-taken (fall-through PC after
// delay slot), exits[1] = taken (imm-relative target). For Phase 2's
// unconditional case exits[0] is the only slot and exits[1] is unused.
//
//   target_pc       : PROGMASK-wrapped PC this exit steers to, or
//                     LINK_TARGET_NONE if this slot is unused.
//   patch_site      : address of the branch instruction, or nullptr
//                     if unused. This is the mutable 4 bytes rewritten
//                     by patchLinkSite / unpatchLinkSite.
//   fallthrough     : address the patch site should point to in its
//                     UNLINKED state. For exits[0] this is always
//                     patch_site + 4. For exits[1] (the B.eq in the
//                     conditional layout) this is patch_site + 8, which
//                     skips past exits[0]'s B instruction to the flush
//                     path. Storing per-slot simplifies unlink.
//   current_target  : the patch site's live target — either fallthrough
//                     (unlinked) or a successor's linkEntry (linked).
struct LinkExit
{
	u32 target_pc;
	u8* patch_site;
	u8* fallthrough;
	u8* current_target;
};
static constexpr u32 LINK_TARGET_NONE = ~0u;

struct VU1BlockEntry
{
	u8*  codeEntry;  // prologue entry (dispatcher path) — nullptr = not yet compiled
	u8*  linkEntry;  // post-prologue entry (direct-link target for predecessors)
	u8*  returnExit; // post-drain label where the exit-selector sequence lives

	// Direct-link state. num_exits is the number of ACTIVE LinkExit slots
	// in exits[]:
	//   0 : not linkable (ebit end, indirect JR/JALR, or block exits before
	//       the returnExit selector could discriminate a valid target).
	//   1 : single exit (unconditional B/BAL, or max-size fall-through).
	//       exits[0] is the only slot; exits[1] is zeroed and unused.
	//       No runtime discrimination — the slot is unconditionally taken.
	//   2 : two exits (conditional IBxx branch). exits[0] = NOT-TAKEN
	//       (static fallthrough after delay slot), exits[1] = TAKEN
	//       (branch's imm-relative target). A runtime compare of VU1.VI
	//       [REG_TPC] against exits[1].target_pc selects which slot fires.
	u32      num_exits;
	LinkExit exits[2];

	u32  numPairs;

	// Content-keyed program cache. `snapshot` holds a private copy of the
	// VU1.Micro bytes this variant was compiled against (numPairs*8 bytes).
	// `findVariant` picks the variant whose snapshot matches the live
	// micro at dispatch time; a snapshot miss triggers a fresh compile and
	// pushes a new variant onto the slot's deque. The x86 microVU uses the
	// same design (see x86/microVU.cpp mVUsearchProg) and it's what keeps
	// GOW2 from thrashing the 64 MB bump allocator every 1.5 s.
	//
	// Owned by the variant — freed in deleteAllVariants.
	u8*  snapshot;

	// Set by Clear() when this variant's slot is in a cleared range. The
	// next dispatch that picks this variant re-runs tryForwardLink +
	// patchWaitingPredecessors to re-wire block-link edges (Clear already
	// unpatched incoming edges for correctness). The flag then clears.
	bool needsRelink;

#ifdef VU1_PROFILE_BLOCKS
	// Per-block execution counter, bumped at linkEntry by the JIT-emitted
	// increment. Dumped to logcat on shutdown via DumpTopBlocks(). Guarded
	// by VU1_PROFILE_BLOCKS (defined in arm64/InterpFlags.h) so the field
	// and counter-bump disappear entirely in shipping builds.
	u64  execCount;
#endif
};

// Content-keyed program cache — one deque of compiled variants per slot.
// Most slots carry 0 or 1 variant in steady state; a slot grows a deque
// only when the EE uploads different bytecode programs to the same PC.
// Front of the deque is the MRU variant — findVariant bubbles hits forward.
static std::deque<VU1BlockEntry*> s_variants[VU1_NUM_SLOTS];

// Reverse index: for each VU1 slot S, the list of variants that have at
// least one exit targeting S. Lets patchWaitingPredecessors and Clear()
// skip the 2048-slot full walk and look at only the relevant predecessors.
//
// A variant is added once per UNIQUE target_pc among its exits[] (so a
// self-loop or cond-branch where both exits hit the same slot is recorded
// once, not twice). target_pc is immutable for the variant's lifetime, so
// entries never need updating until deleteAllVariants() — the only path
// that frees variants — wipes both s_variants and s_waitingForSlot.
static std::vector<VU1BlockEntry*> s_waitingForSlot[VU1_NUM_SLOTS];

static void indexVariantExits(VU1BlockEntry* blk)
{
	for (u32 e = 0; e < blk->num_exits; e++)
	{
		const u32 target_pc = blk->exits[e].target_pc;
		if (target_pc == LINK_TARGET_NONE)
			continue;
		bool dup = false;
		for (u32 j = 0; j < e; j++)
		{
			if (blk->exits[j].target_pc == target_pc)
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		const u32 target_slot = target_pc / 8;
		if (target_slot < VU1_NUM_SLOTS)
			s_waitingForSlot[target_slot].push_back(blk);
	}
}

// Per-slot cap on the variant deque. Without this, a slot grows unboundedly
// as the EE uploads slightly-different VU bytecode (animated UI, particles,
// per-frame matrix updates) — each unique snapshot becomes a new permanent
// variant. findVariant's linear scan + memcmps then dominate dispatch over
// time, producing the "main menu sits at 70% then climbs to 100% VU usage"
// drift. 8 covers any sane number of distinct programs at one PC; the LRU
// is evicted via destroyVariant when this cap is hit.
static constexpr u32 kVariantCapPerSlot = 8;
static u8* s_code_base  = nullptr;
static u8* s_code_write = nullptr;
static u8* s_code_end   = nullptr;
static ArmConstantPool s_pool;

// Cycle limit for the current recArmVU1::Execute call. Set to `startcycles
// + cycles` at the top of Execute; read at every linkEntry by the inlined
// cycle-budget check so linked blocks yield back to the outer dispatch
// loop when the budget is exhausted. Without this, tight VU1 loops that
// conditionally-link back to themselves (IBxx loop:) never return to
// Execute and hang the emulator. Single-reader / single-writer on the
// executing thread (EE under non-MTVU, MTVU thread under MTVU).
static u64 s_vu1_cycle_limit = 0;

// Set by vu1EbitDone when the ebit countdown reaches 0 (microprogram finished).
// Under non-MTVU this is redundant with the VPU_STAT 0x100 clear, but under
// THREAD_VU1 we can't touch VPU_STAT from the VU thread (cross-thread race
// on EE state), so the dispatch loop uses this flag to break instead.
// Reset at the top of recArmVU1::Execute. Single-writer, single-reader on
// the same thread (VU thread under MTVU, EE thread otherwise).
static bool s_vu1_program_ended = false;

// ============================================================================
//  Runtime helper functions called from compiled blocks
// ============================================================================

// Check D/T bits at runtime (depends on FBRST which is a runtime value).
//
// Under THREAD_VU1 this runs on the MTVU thread, so:
//   1. Read vu1Thread.vuFBRST (EE-thread snapshot sent via ExecuteVU) rather
//      than VU0.VI[REG_FBRST].UL (live EE-thread state — cross-thread race).
//   2. Do NOT write VU0.VI[REG_VPU_STAT] or call hwIntcIrq() from the VU
//      thread. Instead, atomically OR a flag into vu1Thread.mtvuInterrupts;
//      Get_MTVUChanges() (MTVU.cpp:351) processes it on the EE thread after
//      the MTVU execute completes, doing the VPU_STAT update and the IRQ
//      raise there.
// Mirrors x86 microVU's mVUTBit / mVUEBit + mVUDTendProgram path
// (microVU_Misc.inl:272-282, microVU_Branch.inl:335-375).
// D-bit under MTVU ends the program via InterruptFlagVUEBit (no IRQ) — same
// as x86 microVU's D-bit path, which calls mVUDTendProgram → mVUEBit.
static void vu1CheckDTBits(u32 upper)
{
	const u32 fbrst = THREAD_VU1 ? vu1Thread.vuFBRST : VU0.VI[REG_FBRST].UL;

	if (upper & 0x10000000) // D flag
	{
		if (fbrst & 0x400)
		{
			if (THREAD_VU1)
			{
				vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
			}
			else
			{
				VU0.VI[REG_VPU_STAT].UL |= 0x200;
				hwIntcIrq(INTC_VU1);
			}
			VU1.ebit = 1;
		}
	}
	if (upper & 0x08000000) // T flag
	{
		if (fbrst & 0x800)
		{
			if (THREAD_VU1)
			{
				vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUTBit, std::memory_order_release);
			}
			else
			{
				VU0.VI[REG_VPU_STAT].UL |= 0x400;
				hwIntcIrq(INTC_VU1);
			}
			VU1.ebit = 1;
		}
	}
}

// End-of-microprogram cleanup (called when ebit countdown hits 0).
//
// XGKICK drain:
//   Non-hack path: step 15 or the block-end drain in CompileBlock handles
//   pending_xgkick_fire, and vu1_XGKICK_fire_deferred never sets
//   VU1.xgkickenable, so there's nothing to clean up here.
//   Hack path: VU1.xgkickenable is set by vu1_XGKICK_hack_capture and
//   deliberately persists across block boundaries (block-end drain gates
//   on !xgkickhack). At ebit the microprogram is done, so any remaining
//   paced bytes must be flushed here. Mirrors x86 mVUendProgram
//   (microVU_Branch.inl:174-178, `if (CHECK_XGKICKHACK) mVU_XGKICK_SYNC(true)`)
//   and VU1 interp VU1microInterp.cpp:197-198.
static void vu1EbitDone(VURegs* VU)
{
	VU->VIBackupCycles = 0;
	_vuFlushAll(VU);
	// VPU_STAT running bit + VEW: under THREAD_VU1, vu1ExecMicro on the EE
	// thread already cleared VPU_STAT (VU1micro.cpp:52) before queuing the
	// MTVU execute, and VEW is owned by the VIF DMA path (Vif1_Dma.cpp:258).
	// Writing them from here under MTVU is a cross-thread race on EE state.
	// x86 microVU doesn't touch either at end-of-microprogram; we match that.
	if (!THREAD_VU1)
	{
		VU0.VI[REG_VPU_STAT].UL &= ~0x100;
		vif1Regs.stat.VEW = false;
	}
	// Signal the dispatch loop that the microprogram is finished. Under
	// non-MTVU the VPU_STAT clear above also breaks the loop, but that
	// gate is unreliable under THREAD_VU1 (VPU_STAT 0x100 is cleared on
	// the EE side before queueing and, with INSTANT_VU1, is never re-set),
	// so the loop uses this flag as its termination signal.
	s_vu1_program_ended = true;
	// Flush any in-flight hack-mode paced transfer. No-op when xgkickenable
	// is false (the non-hack JIT path never sets it — H-1 keeps it false
	// even after hazard-fallback interp XGKICKs).
	if (VU1.xgkickenable)
		_vuXGKICKTransfer(0, true);
	// Under INSTANT_VU1 the cycle reference switches from VU1.cycle to
	// cpuRegs.cycle at ebit because VU1 runs synchronously with the EE —
	// matches VU1microInterp.cpp:202-203.
	if (INSTANT_VU1)
		VU1.xgkicklastcycle = cpuRegs.cycle;
}

// Handle takedelaybranch state when branch countdown fires.
static void vu1HandleDelayBranch(VURegs* VU)
{
	if (VU->takedelaybranch)
	{
		VU->branch          = 1;
		VU->branchpc        = VU->delaybranchpc;
		VU->takedelaybranch = false;
	}
}

// (vu1DecrementVIBackup removed — now inlined directly into the per-pair
//  loop via emitDecrementVIBackup, see below.)

// ============================================================================
//  Specialized stall helpers — invoked from JIT with compile-time-constant
//  args. These exist so the per-pair codegen does NOT have to dereference
//  a runtime _VURegsNum* and re-do the pipe switch every pair (the way the
//  generic _vuTest*Stalls / _vuAdd*Stalls helpers do).
//
//  Each helper is called from the JIT only when the corresponding compile-
//  time precondition is true (e.g. vu1_TestFMACStallReg is only emitted
//  when uregs.pipe == VUPIPE_FMAC AND uregs.VFread{0,1} != 0).
// ============================================================================

// Mirrors _vuFMACTestStall in VUops.cpp:210, but takes (reg,xyzw) directly
// in argument registers instead of via _VURegsNum*.
// Calling convention: takes `cycle` and `fmaccount` as args (matches the
// JIT-pinned VU1_CYCLE_REG / VU1_FMACCOUNT_REG), returns the post-check
// cycle (may be bumped if a pending FMAC slot stalls this read). JIT site
// passes x21 / w26 → x2 / w1, captures x0 → x21 — eliminates the per-BL
// Str+Ldr round-trip through VU->cycle / VU->fmaccount memory.
static u64 vu1_TestFMACStallReg(VURegs* VU, u32 fmaccount, u64 cycle, u32 reg, u32 xyzw)
{
	u32 i = 0;
	for (int currentpipe = VU->fmacreadpos; i < fmaccount;
	     currentpipe = (currentpipe + 1) & 3, i++)
	{
		const fmacPipe& slot = VU->fmac[currentpipe];
		if ((cycle - slot.sCycle) >= slot.Cycle)
			continue;

		if ((slot.regupper == reg && (slot.xyzwupper & xyzw))
			|| (slot.reglower == reg && (slot.xyzwlower & xyzw)))
		{
			const u64 newCycle = (u64)slot.Cycle + slot.sCycle;
			if (newCycle > cycle)
				cycle = newCycle;
		}
	}
	return cycle;
}

// Combined two-read FMAC stall check. Most FMAC ops have two VF reads
// (ADD/SUB/MUL/MADD/MSUB all read two operands), so the JIT used to
// emit two separate BL vu1_TestFMACStallReg calls per pair — paying
// the full emitVu1Call overhead twice (vfCacheFlushAndInvalidate emits
// Strs for every dirty VF cache slot, ~3-6 per pair in FMAC-heavy
// blocks like FFXII vertex T&L). One BL with both (reg, xyzw) pairs
// halves that overhead. reg0/reg1 == 0 means "skip this check"
// (mirrors emitFMACStallChecks's pre-BL guard).
//
// Walks the FMAC ring once, checks both pairs per slot. Same semantics
// as two back-to-back vu1_TestFMACStallReg calls — VU->cycle update
// is monotonic so order of checks doesn't matter for the final cycle.
//
// Same arg/return convention as vu1_TestFMACStallReg.
static u64 vu1_TestFMACStallReg2(VURegs* VU, u32 fmaccount, u64 cycle, u32 reg0, u32 xyzw0, u32 reg1, u32 xyzw1)
{
	u32 i = 0;
	for (int currentpipe = VU->fmacreadpos; i < fmaccount;
	     currentpipe = (currentpipe + 1) & 3, i++)
	{
		const fmacPipe& slot = VU->fmac[currentpipe];
		if ((cycle - slot.sCycle) >= slot.Cycle)
			continue;

		const bool hit0 = reg0 != 0 && (
			(slot.regupper == reg0 && (slot.xyzwupper & xyzw0)) ||
			(slot.reglower == reg0 && (slot.xyzwlower & xyzw0)));
		const bool hit1 = reg1 != 0 && (
			(slot.regupper == reg1 && (slot.xyzwupper & xyzw1)) ||
			(slot.reglower == reg1 && (slot.xyzwlower & xyzw1)));

		if (hit0 || hit1)
		{
			const u64 newCycle = (u64)slot.Cycle + slot.sCycle;
			if (newCycle > cycle)
				cycle = newCycle;
		}
	}
	return cycle;
}

// FDIV pipe wait portion of _vuTestFDIVStalls (the FMAC test is called
// separately by the JIT when needed).
static void vu1_TestFDIVPipeWait(VURegs* VU)
{
	if (VU->fdiv.enable != 0)
	{
		u64 newCycle = VU->fdiv.Cycle + VU->fdiv.sCycle;
		if (newCycle > VU->cycle)
			VU->cycle = newCycle;
	}
}

// EFU pipe wait portion of _vuTestEFUStalls. NOTE: this mutates
// efu.Cycle (decrements by 1) — see the comment in VUops.cpp:269 for why.
static void vu1_TestEFUPipeWait(VURegs* VU)
{
	if (VU->efu.enable == 0)
		return;
	VU->efu.Cycle -= 1;
	u64 newCycle = VU->efu.sCycle + VU->efu.Cycle;
	if (newCycle > VU->cycle)
		VU->cycle = newCycle;
}

// Mirrors _vuTestALUStalls (VUops.cpp:278) — takes the constant VIread mask
// directly. Used for branch instructions (VUPIPE_BRANCH lower).
static void vu1_TestALUStallReg(VURegs* VU, u32 VIread)
{
	u32 i = 0;
	for (int currentpipe = VU->ialureadpos; i < VU->ialucount;
	     currentpipe = (currentpipe + 1) & 3, i++)
	{
		if ((VU->cycle - VU->ialu[currentpipe].sCycle) >= VU->ialu[currentpipe].Cycle)
			continue;

		if (VU->ialu[currentpipe].reg & VIread)
		{
			u64 newCycle = VU->ialu[currentpipe].Cycle + VU->ialu[currentpipe].sCycle;
			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
}

// Stage C1 (2026-04-11): vu1_FMACAddPair / vu1_FDIVAdd / vu1_EFUAdd /
// vu1_IALUAdd were removed. The corresponding per-pair pipeline adds are
// now emitted as inline store sequences by emitFMACAddPair and
// emitLowerNonFMACAdd below, eliminating four BL round-trips per pair.

// VU1-specialized _vuTestPipes. Mirrors the body of _vuTestPipes / _vuFMACflush /
// _vuFDIVflush / _vuEFUflush / _vuIALUflush from VUops.cpp, with two deletions:
//
//  1. No XGKICK transfer block. The arm64 rec bypasses VU1.xgkickenable via
//     the vu1_XGKICK capture hack (see project_rec_vu1_xgkick_hack) — kicks
//     are fired one pair later by vu1_XGKICK_fire_deferred. `_vuTestPipes`'s
//     `if (VU1.xgkickenable) _vuXGKICKTransfer(...)` would never trigger for
//     us anyway, so eliding it avoids a dead load+branch every pair.
//
//  2. No `do { } while (flushed)` retry loop. None of the four flush functions
//     enqueue anything, so a single pass is always equivalent to the fixpoint.
//
// Called once per pair (step 6 of CompileBlock), so keeping it tight matters.
// Kept in sync with the originals — when any of the VUops.cpp flush bodies
// change, this must be updated too.
//
// Calling convention: takes `fmaccount_in` and `cycle` as args (matches the
// JIT-pinned VU1_FMACCOUNT_REG / VU1_CYCLE_REG = w26 / x21). Returns the
// post-flush fmaccount in x0. The JIT site passes w26 → w1 and x21 → x2,
// captures w0 → w26 — eliminating the per-BL Str+Ldr round-trip through
// VU->fmaccount AND VU->cycle memory. Helper does NOT write VU->cycle, so
// no cycle reload needed JIT-side. VU->fmaccount memory stays stale during
// a block (prologue/epilogue still flush for cross-block coherence with
// cold paths like _vuFlushAll/vu1Exec).
static u32 vu1_TestPipes_VU1(VURegs* VU, u32 fmaccount_in, u64 cycle)
{
	// Fast-path: all rings empty / no FDIV/EFU enabled. Common case under
	// MTVU on simple geometry blocks. Pre-perfape (Ape Escape 3 main menu)
	// this function was ~15% of MTVU thread; the bulk of that was 3 dead
	// Strs on the all-empty exit path (fmacreadpos/ialureadpos/ialucount
	// each rewritten with their unchanged loaded value). Early-return
	// avoids the Strs and the IALU-loop setup entirely.
	//
	// The fdiv/efu/ialucount loads here CSE with the loads in the slow
	// path below (same VURegs* base), so the fast-path adds 3 cmp+branch
	// insns to the slow path — cheap.
	const u32 fdiv_enable_in = VU->fdiv.enable;
	const u32 efu_enable_in  = VU->efu.enable;
	const u32 ialucount_in   = VU->ialucount;
	if (fmaccount_in == 0 && fdiv_enable_in == 0 && efu_enable_in == 0 && ialucount_in == 0)
		return 0;

	// Second fast-path (S26 simpleperf 2026-05-06: vu1_TestPipes_VU1 was
	// 17.1% of total CPU). Most BLs on the slow path have fmaccount > 0
	// while the other three rings are empty — i.e., back-to-back FMAC pairs
	// where the front slot hasn't matured yet (Cycle=4, only 1-2 cycles
	// have elapsed since enqueue). For that case the helper does no work:
	// FMAC drain breaks on the first iteration, FDIV/EFU/IALU all skip via
	// their zero gates. Returning fmaccount_in unchanged matches the slow
	// path exactly. Costs 1 load (fmacreadpos) + 2 loads (sCycle, Cycle) +
	// 1 sub + 1 cmp + 1 branch — ~6 cycles vs ~20+ for entering the slow
	// path's drain loops.
	if (fdiv_enable_in == 0 && efu_enable_in == 0 && ialucount_in == 0)
	{
		const fmacPipe& slot = VU->fmac[VU->fmacreadpos];
		if ((cycle - slot.sCycle) < slot.Cycle)
			return fmaccount_in;
	}

	u32 fmaccount = fmaccount_in;

	// --- FMAC flush ---
	int fmacreadpos = VU->fmacreadpos;
	const u32 fmacreadpos_in = static_cast<u32>(fmacreadpos);
	while (fmaccount > 0)
	{
		const fmacPipe& slot = VU->fmac[fmacreadpos];
		if ((cycle - slot.sCycle) < slot.Cycle)
			break;

		if (slot.flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = slot.clipflag;

		if (slot.flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30)
				| (slot.statusflag & 0xFC0)
				| (slot.statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0)
				| (slot.statusflag & 0xF)
				| ((slot.statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = slot.macflag;

		fmacreadpos = (fmacreadpos + 1) & 3;
		fmaccount--;
	}
	// Only Str when actually advanced — the unchanged-write was hot.
	if (static_cast<u32>(fmacreadpos) != fmacreadpos_in)
		VU->fmacreadpos = fmacreadpos;

	// --- FDIV flush ---
	if (VU->fdiv.enable != 0
		&& (cycle - VU->fdiv.sCycle) >= VU->fdiv.Cycle)
	{
		VU->fdiv.enable = 0;
		VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
		VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF)
			| (VU->fdiv.statusflag & 0xC30);
	}

	// --- EFU flush ---
	if (VU->efu.enable != 0
		&& (cycle - VU->efu.sCycle) >= VU->efu.Cycle)
	{
		VU->efu.enable = 0;
		VU->VI[REG_P].UL = VU->efu.reg.UL;
	}

	// --- IALU flush (pop only, no flag writes) ---
	int ialureadpos = VU->ialureadpos;
	u32 ialucount = ialucount_in; // initial value already loaded above for the gate
	while (ialucount > 0)
	{
		const ialuPipe& slot = VU->ialu[ialureadpos];
		if ((cycle - slot.sCycle) < slot.Cycle)
			break;
		ialureadpos = (ialureadpos + 1) & 3;
		ialucount--;
	}
	if (ialucount != ialucount_in)
	{
		VU->ialureadpos = ialureadpos;
		VU->ialucount = ialucount;
	}

	return fmaccount;
}

// ============================================================================
//  Block analysis helpers
// ============================================================================

static bool PairHasEbit(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
	return (upper >> 30) & 1;
}

static bool PairHasBranch(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
	if ((upper >> 31) & 1)
		return false; // I-bit: lower field is immediate, not an opcode
	const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + pc);
	_VURegsNum lregs{};
	// Sub-table dispatchers (e.g. LowerOP) read VU1.code internally, so the
	// global must be primed — otherwise the LowerOP index selects from stale
	// state and can hit the Unknown slot (which pxFail-aborts in debug).
	VU1.code = lower;
	VU1regs_LOWER_OPCODE[lower >> 25](&lregs);
	return lregs.pipe == VUPIPE_BRANCH;
}

// Detect pairs containing an illegal/reserved lower opcode so we can truncate
// the block at them. Mirrors x86 microVU's mVUcheckBadOp (microVU_Compile.inl)
// which sets mVUinfo.isEOB when dispatch routes to Unknown. VU1 uses a switch
// dispatcher (emitVU1Lower) so we enumerate valid top-level encodings here
// rather than doing pointer comparison like VU0. Sub-table Unknown ops still
// fall through to interp at runtime and are harmless — they just don't get
// the tighter block bound.
//
// I-bit pairs are exempted: their lower word is the I-register literal, not
// an opcode. BIOS writes a reversed-NOP pair (0x8000033c for the upper half)
// that's excluded to avoid a spurious truncation on the BIOS boot path.
static bool PairHasBadOp(u32 pc)
{
	const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
	if ((upper >> 31) & 1)
		return false;
	if (upper == 0x8000033c)
		return false;
	const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + pc);
	const u32 top7  = lower >> 25;
	// Valid top-level encodings per emitVU1Lower switch dispatcher.
	switch (top7)
	{
		case 0x00: // LQ
		case 0x01: // SQ
		case 0x04: // ILW
		case 0x05: // ISW
		case 0x08: // IADDIU
		case 0x09: // ISUBIU
		case 0x10: // FCEQ
		case 0x11: // FCSET
		case 0x12: // FCAND
		case 0x13: // FCOR
		case 0x14: // FSEQ
		case 0x15: // FSSET
		case 0x16: // FSAND
		case 0x17: // FSOR
		case 0x18: // FMEQ
		case 0x1A: // FMAND
		case 0x1B: // FMOR
		case 0x1C: // FCGET
		case 0x20: // B
		case 0x21: // BAL
		case 0x24: // JR
		case 0x25: // JALR
		case 0x28: // IBEQ
		case 0x29: // IBNE
		case 0x2C: // IBLTZ
		case 0x2D: // IBGTZ
		case 0x2E: // IBLEZ
		case 0x2F: // IBGEZ
		case 0x40: // LowerOP sub-dispatch
			return false;
		default:
			return true;
	}
}

static u32 AnalyzeBlock(u32 startPC)
{
	u32 pairs = 0;
	u32 pc    = startPC;

	while (pairs < VU1_MAX_BLOCK_PAIRS)
	{
		const bool ebit   = PairHasEbit(pc);
		const bool branch = PairHasBranch(pc);
		const bool bad_op = PairHasBadOp(pc);

		pairs++;
		pc = (pc + 8) & (VU1_PROGSIZE - 1);

		if (ebit || branch)
		{
			// Include the one delay-slot pair then stop.
			pairs++;
			break;
		}
		// Bad op: include the current pair (still dispatches to interp) but
		// no delay slot, and truncate the block. Matches x86 microVU's
		// mVUinfo.isEOB on bad op. Benefit is earlier block boundary so we
		// re-enter dispatch and don't keep compiling past a definitely-bad
		// opcode. Same pattern as VU0 C-7 fix.
		if (bad_op)
			break;
	}

	return pairs;
}

// Phase 2+3+4 block linking: determine this block's static exit set.
//
// Returns:
//   - num_exits = 0, indirect = false  → not linkable (ebit)
//   - num_exits = 1, indirect = false  → unconditional direct link or max-size fall-through
//   - num_exits = 2, indirect = false  → conditional branch (both targets known)
//   - num_exits = 0, indirect = true   → JR / JALR (Phase 4 runtime dispatch)
//
//   ebit (program end)                                → kind=0, indirect=0
//   JR / JALR                                         → kind=0, indirect=1
//   Unconditional B / BAL                             → kind=1
//       target_pcs[0] = PC-relative target
//   Max-size block (no branch/ebit in 256 pairs)      → kind=1
//       target_pcs[0] = startPC + 256*8 (fall-through)
//   Conditional IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ     → kind=2
//       target_pcs[0] = NOT-TAKEN fall-through (after delay slot)
//       target_pcs[1] = TAKEN PC-relative target
//
// For branch-terminated blocks, AnalyzeBlock puts the branch at pair
// (numPairs-2) and the delay slot at (numPairs-1). Ebit is bit 30 of upper.
// imm11 decoding matches W_Imm11 in iVU1Lower_arm64.cpp.
struct BlockLinkExits
{
	u32  num_exits;
	u32  target_pcs[2];
	bool indirect;
};

static BlockLinkExits computeBlockLinkExits(u32 startPC, u32 numPairs)
{
	BlockLinkExits out = {};
	out.target_pcs[0] = LINK_TARGET_NONE;
	out.target_pcs[1] = LINK_TARGET_NONE;

	if (numPairs == 0)
		return out;

	// Max-size block: single fall-through.
	if (numPairs >= VU1_MAX_BLOCK_PAIRS)
	{
		out.num_exits     = 1;
		out.target_pcs[0] = (startPC + numPairs * 8u) & VU1_PROGMASK;
		return out;
	}

	// Branch- or ebit-terminated. Terminator at pair numPairs-2.
	const u32 term_pc = (startPC + (numPairs - 2u) * 8u) & VU1_PROGMASK;
	const u32 upper   = *reinterpret_cast<const u32*>(VU1.Micro + term_pc + 4);

	if ((upper >> 30) & 1)
		return out; // ebit — no successor

	// I-bit upper: AnalyzeBlock wouldn't have treated this pair as a branch.
	if ((upper >> 31) & 1)
		return out;

	const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + term_pc);

	// Compute the PC-relative target (same for B/BAL and conditional IBxx).
	const s32 imm11 = (lower & 0x400u)
		? static_cast<s32>(0xFFFFFC00u | (lower & 0x3FFu))
		: static_cast<s32>(lower & 0x3FFu);
	const u32 tpc_val    = (term_pc + 8u) & VU1_PROGMASK;
	const u32 imm_target = (tpc_val + static_cast<u32>(imm11 * 8)) & VU1_PROGMASK;

	// Fall-through PC = startPC + numPairs * 8 (right after the delay slot).
	const u32 fallthrough_pc = (startPC + numPairs * 8u) & VU1_PROGMASK;

	if (isUnconditionalBranchOp(lower))
	{
		out.num_exits     = 1;
		out.target_pcs[0] = imm_target;
		return out;
	}

	if (isConditionalBranchOp(lower))
	{
		// Exit 0 is NOT-TAKEN (runtime TPC will equal fallthrough_pc when
		// the condition was false). Exit 1 is TAKEN (runtime TPC equals
		// imm_target when the branch fired through countdown). The
		// discriminating compare at returnExit reads VU1.VI[REG_TPC] and
		// does `B.eq exits[1].patch_site` if TPC == imm_target, else falls
		// through to exits[0].
		out.num_exits     = 2;
		out.target_pcs[0] = fallthrough_pc;
		out.target_pcs[1] = imm_target;
		return out;
	}

	if (isIndirectBranchOp(lower))
	{
		// JR / JALR: target computed at runtime from VI[_Is_]. No static
		// patch slot; a runtime BL dispatcher at returnExit looks up the
		// target's linkEntry and tail-Brs to it (falling through to
		// flushes+Ret if not compiled yet). See Phase 4 emit below.
		out.indirect = true;
		return out;
	}

	// Anything else with VUPIPE_BRANCH we don't know statically.
	return out;
}

// ============================================================================
//  Block linking — patch helpers (Phase 2)
// ============================================================================

// Batched icache flush. Per-patch FlushInstructionCache costs ~3 barriers
// (dsb ish, dsb ish, isb) — for wide Clear()s that unpatch hundreds of exit
// sites, that's ms-scale lag (visible in FFXII, GTASA, Darkwatch when the
// EE re-uploads VU1 micro programs). Batching dedupes cache lines and pays
// the barrier overhead exactly once per call site.
//
// Each LinkExit patch is 4 bytes (single B-imm26). A single 64-byte cache
// line covers 16 patch sites, so dedup wins materially when patches cluster
// (each block's exits[0]/exits[1] are adjacent in JIT memory).
//
// Cache line size hard-coded to 64 — true for every Snapdragon Oryon /
// Cortex-A7x / Cortex-X core. Querying CTR_EL0 once at init would be
// trivial future work if a target ships with 32 or 128 byte lines.
struct VU1IcacheBatch
{
	std::vector<uintptr_t> lines;
	static constexpr uintptr_t LINE_MASK = ~uintptr_t(63);

	void note(void* addr)
	{
		lines.push_back(reinterpret_cast<uintptr_t>(addr) & LINE_MASK);
	}

	void flush()
	{
		if (lines.empty())
			return;
		std::sort(lines.begin(), lines.end());
		lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
#ifdef ARCH_ARM64
		for (uintptr_t line : lines)
			asm volatile("dc cvau, %0" ::"r"(line) : "memory");
		asm volatile("dsb ish" ::: "memory");
		for (uintptr_t line : lines)
			asm volatile("ic ivau, %0" ::"r"(line) : "memory");
		asm volatile("dsb ish" ::: "memory");
		asm volatile("isb" ::: "memory");
#else
		// Fallback: single range flush. Only the unit-test build path
		// reaches here; the JIT itself only runs on ARM64.
		uintptr_t lo = lines.front();
		uintptr_t hi = lines.back() + 64;
		HostSys::FlushInstructionCache(reinterpret_cast<void*>(lo),
			static_cast<u32>(hi - lo));
#endif
		lines.clear();
	}
};

// Rewrite a single LinkExit's patch site to jump to `target` (typically
// another block's linkEntry, or the exit's own fallthrough address for
// unpatching). No-op if the slot already points at `target`. Handles
// I-cache coherency via armEmitJmpPtr's internal FlushInstructionCache,
// unless the caller passes a `batch` to defer the flush.
//
// The single-thread assumption: this is called from recArmVU1::Execute
// (block compile path) or recArmVU1::Clear (invalidation). Both run on the
// same thread that eventually executes compiled blocks — either the EE
// thread (non-MTVU) or the MTVU thread (MTVU via Clear from EE needs
// external serialization, same as the pre-existing codeEntry=nullptr
// invalidation). Intra-thread patching is safe with armEmitJmpPtr's
// FlushInstructionCache call; we don't add extra barriers here.
//
// When `batch` is non-null the icache flush is deferred — the caller must
// invoke batch->flush() before any code that may execute through the
// patched site runs. Used by Clear() and the post-Clear relink path in
// Execute() to amortize per-patch barrier overhead across many sites.
static void patchLinkSite(LinkExit& exit, u8* target, VU1IcacheBatch* batch = nullptr)
{
	if (!exit.patch_site)
		return;
	if (exit.current_target == target)
		return;
	armEmitJmpPtr(exit.patch_site, target, /* flush_icache */ batch == nullptr);
	if (batch)
		batch->note(exit.patch_site);
	exit.current_target = target;
}

// Restore an exit's patch site to its unlinked fallthrough target.
// For num_exits==1 (Phase 2 unconditional), fallthrough is the flush
// path immediately after the B. For num_exits==2 (Phase 3 conditional)
// both exits' fallthrough is ALSO the flush path — exits[1]'s B.eq
// fallthrough skips past exits[0]'s B to reach it.
static void unpatchLinkSite(LinkExit& exit, VU1IcacheBatch* batch = nullptr)
{
	if (!exit.patch_site)
		return;
	patchLinkSite(exit, exit.fallthrough, batch);
}

// Detach and free a single variant. Caller has already removed it from
// s_variants[my_slot]; this routine drops it from the reverse index and
// releases heap allocations. Compiled code in the JIT buffer is left in
// place — it'll get reclaimed at the next deleteAllVariants (code-buffer
// full reset / Reset / Shutdown).
//
// Predecessors that had patches pointing at this variant's linkEntry get
// reverted to fall-through so they don't jump into code that may be
// overwritten by a future compile occupying the same buffer space. After
// this routine returns the slot's `patchWaitingPredecessors` will re-link
// any matching exits to the new MRU variant at my_slot.
static void destroyVariant(VU1BlockEntry* blk, u32 my_slot)
{
	if (blk->linkEntry && my_slot < VU1_NUM_SLOTS)
	{
		// Batch the unpatch icache flushes — eviction at a hot slot can
		// touch every predecessor with a live patch, paying ~3 barriers
		// each per call without batching.
		VU1IcacheBatch flush_batch;
		for (VU1BlockEntry* pred : s_waitingForSlot[my_slot])
		{
			if (pred == blk)
				continue;
			for (u32 e = 0; e < pred->num_exits; e++)
			{
				LinkExit& exit = pred->exits[e];
				if (exit.current_target == blk->linkEntry)
					unpatchLinkSite(exit, &flush_batch);
			}
		}
		flush_batch.flush();
	}

	for (u32 e = 0; e < blk->num_exits; e++)
	{
		const u32 target_pc = blk->exits[e].target_pc;
		if (target_pc == LINK_TARGET_NONE)
			continue;
		bool dup = false;
		for (u32 j = 0; j < e; j++)
		{
			if (blk->exits[j].target_pc == target_pc)
			{
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		const u32 target_slot = target_pc / 8;
		if (target_slot >= VU1_NUM_SLOTS)
			continue;
		auto& vec = s_waitingForSlot[target_slot];
		vec.erase(std::remove(vec.begin(), vec.end(), blk), vec.end());
	}

	delete[] blk->snapshot;
	delete blk;
}

// Content-keyed variant lookup. Scans the slot's deque for a variant whose
// snapshot matches the live VU1.Micro bytes at `pc`; MRU-bubbles a hit to
// the front so repeated dispatches find it in one compare. Miss → nullptr.
//
// Fast-reject on first-8-byte compare before the full memcmp — most
// mismatches fail here. VU1.Micro is 16-byte aligned and all slot PCs
// are 8-byte aligned, so the u64 load is safe.
static VU1BlockEntry* findVariant(u32 pc)
{
	const u32 slot = pc / 8;
	auto& deque = s_variants[slot];
	if (deque.empty())
		return nullptr;

	const u8* live = VU1.Micro + pc;
	const u64 live_head = *reinterpret_cast<const u64*>(live);

	for (auto it = deque.begin(); it != deque.end(); ++it)
	{
		VU1BlockEntry* blk = *it;
		const u64 snap_head = *reinterpret_cast<const u64*>(blk->snapshot);
		if (snap_head != live_head)
			continue;
		const u32 snap_bytes = blk->numPairs * 8;
		if (snap_bytes > 8
			&& std::memcmp(blk->snapshot + 8, live + 8, snap_bytes - 8) != 0)
			continue;

		if (it != deque.begin())
		{
			deque.erase(it);
			deque.push_front(blk);
		}
		return blk;
	}
	return nullptr;
}

// Called right after a block compiles: for each active exit, if its static
// target has a currently-live compiled variant (one whose snapshot matches
// live VU1.Micro at target_pc), patch that exit's slot to jump directly.
//
// `batch` allows the caller to coalesce icache flushes across this call
// and a subsequent patchWaitingPredecessors. Caller MUST flush before any
// patched code is executed.
static void tryForwardLink(VU1BlockEntry& block, VU1IcacheBatch* batch = nullptr)
{
	for (u32 e = 0; e < block.num_exits; e++)
	{
		LinkExit& exit = block.exits[e];
		if (exit.target_pc == LINK_TARGET_NONE)
			continue;
		VU1BlockEntry* target = findVariant(exit.target_pc);
		if (target && target->linkEntry)
			patchLinkSite(exit, target->linkEntry, batch);
	}
}

// Phase 4 runtime dispatcher for JR/JALR block exits. Given a runtime TPC,
// return the target block's linkEntry (which trusts our cached regs and
// skips its own prologue), or nullptr if the target isn't compiled yet.
//
// On nullptr, the JIT-emitted caller falls through to its own flush+Ret
// path; Execute's outer loop then dispatches normally, compiling the
// target. The NEXT JR/JALR to the same TPC hits this helper again and
// returns the now-compiled linkEntry, enabling direct tail-B.
//
// findVariant content-matches against live micro: if the matched variant's
// snapshot equals VU1.Micro[target_pc..+N*8], its linkEntry is safe to
// jump into. Otherwise return nullptr and let the outer Execute loop
// re-dispatch against live bytes.
static u8* vu1_indirect_dispatch(u32 tpc)
{
	const u32 pc = tpc & VU1_PROGMASK;
	VU1BlockEntry* blk = findVariant(pc);
	return blk ? blk->linkEntry : nullptr;
}

// Called right after a block compiles at `my_pc` with `my_linkEntry`: walk
// the reverse index of variants whose exits target this slot and patch
// them forward. Each variant's exits[] are independent — a pred variant
// compiled against one bytecode may link to a completely different target
// variant than another pred compiled against a different bytecode at the
// same PC, hence the per-exit target_pc check inside the inner loop.
//
// Cost: O(predecessors_of_my_slot * max_exits). For typical menu/UI VU
// programs with 1-3 exits per block, this is dozens to hundreds of pairs,
// not the millions the pre-index version touched as variants accumulated.
static void patchWaitingPredecessors(u32 my_pc, u8* my_linkEntry, VU1IcacheBatch* batch = nullptr)
{
	if (!my_linkEntry)
		return;
	const u32 my_slot = my_pc / 8;
	if (my_slot >= VU1_NUM_SLOTS)
		return;
	for (VU1BlockEntry* pred : s_waitingForSlot[my_slot])
	{
		for (u32 e = 0; e < pred->num_exits; e++)
		{
			LinkExit& exit = pred->exits[e];
			if (exit.patch_site != nullptr
			    && exit.target_pc == my_pc
			    && exit.current_target != my_linkEntry)
			{
				patchLinkSite(exit, my_linkEntry, batch);
			}
		}
	}
}

// ============================================================================
//  Block compilation
// ============================================================================

// Pinned VU1 base register used throughout compiled blocks.
// x23 is callee-saved (AAPCS64) and not clobbered by C function calls.
static const auto VU1_BASE_REG = x23;

// Stage C2 (2026-04-11): pinned VU->cycle register for the duration of a
// compiled block. Loaded once at block entry, used directly by step 1
// (cycle++), step 6b (VIBackup decrement), and every inline pipeline add
// (FMAC/FDIV/EFU/IALU sCycle store). Flushed back to memory before any BL
// that reads or writes `VU->cycle`, and reloaded afterwards when the BL
// may have mutated it. Block-end flushes back to memory before restoring
// the caller's x21. Callee-saved — BLs won't clobber it.
static const auto VU1_CYCLE_REG = x21;

// Emit `Str x21, [VU1_BASE, cycle_off]`. Call immediately before a BL that
// reads `VU->cycle`.
static void emitFlushCycleReg(int64_t cycle_off)
{
	armAsm->Str(VU1_CYCLE_REG, MemOperand(VU1_BASE_REG, cycle_off));
}

// Emit `Ldr x21, [VU1_BASE, cycle_off]`. Call immediately after a BL that
// may have mutated `VU->cycle`.
static void emitReloadCycleReg(int64_t cycle_off)
{
	armAsm->Ldr(VU1_CYCLE_REG, MemOperand(VU1_BASE_REG, cycle_off));
}

// Stage C3 (2026-04-11): pinned VU->fmacwritepos / VU->ialuwritepos registers
// for the duration of a compiled block. Loaded once at block entry, used
// directly by emitFMACAddPair (slot address math) / emitLowerNonFMACAdd (IALU
// slot address + wpos advance) / step 14 (FMAC wpos advance). Flushed before
// and reloaded after vu1Exec (the only BL we emit that reads/writes wpos —
// every other BL touches only fmacreadpos / fmaccount / ialureadpos /
// ialucount, which stay memory-resident). Block-end flushes back before the
// epilogue restores the caller's x24/x25.
//
// These are 32-bit-wide (u32) fields; we use w24/w25 for all arithmetic and
// let the implicit zero-extend-on-32-bit-write rule keep x24/x25 valid as
// the 64-bit form for the slot-address math in emitFMACAddPair /
// emitLowerNonFMACAdd.
static const auto VU1_FMAC_WPOS_REG = w24;
static const auto VU1_IALU_WPOS_REG = w25;

// Phase-6 opt #1: pinned pointer to the linkEntry gate global. Callee-
// saved (x19-x28 per AAPCS64). Loaded once at block prologue, restored
// at epilogue. Collapses linkEntry's termination check from ~5
// instructions (adrp+add+ldr) down to ~3 (direct Ldr via the pinned base).
// Since every linked-chain block entry runs this gate, the compounded
// savings are significant.
//
// The cycle-limit half of opt #1 (formerly x26 → &s_vu1_cycle_limit) was
// dropped to free x26 for VU1_FMACCOUNT_REG below — the cycle-limit
// address now materializes fresh in scratch x5 inside the linkEntry gate
// (+2 insn per gate execution). The fmaccount pin saves more in the per-
// pair body than the gate loses, for any block with FMAC pairs.
//
//   x27 → &s_vu1_program_ended (THREAD_VU1) OR &VU0 (!THREAD_VU1,
//         used with vpu_stat_off for the VPU_STAT 0x100 test)
static const auto VU1_TERM_ADDR_REG        = x27;

// Phase-9b (2026-04-25): pinned VU->fmaccount register. u32 ring counter
// incremented by emitFMACAddPair (every FMAC pair) and decremented by
// helpers that drain the FMAC pipe — _vuFMACflush (via _vuTestPipes,
// _vuFlushAll, vu1Exec, hack_xgkick capture path, emitVU1InterpBL). Read
// (not written) by vu1_TestFMACStallReg per stall check.
//
// Pinning saves the per-FMAC-pair Ldr+Add+Str (3 insn) → Add (1 insn) at
// the cost of flushing/reloading around the BLs above. Net win is modest
// (~0.5 insn/FMAC pair after offset costs) but the infrastructure mirrors
// the cycle/wpos pins.
//
//   w26 ↔ VU->fmaccount  (32-bit u32; x26 zero-extended for 64-bit form)
//
// Reuses the x26 slot freed by dropping the cycle-limit pin above. The
// prologue Stp/Ldp save/restore at [sp+56..63] is unchanged — same
// physical reg, just with fmaccount semantics now.
static const auto VU1_FMACCOUNT_REG = w26;

// Phase-7 (2026-04-20): pinned VU->macflag / VU->statusflag / VU->clipflag
// registers for the duration of a compiled block. These three u32 fields are
// read by emitFMACAddPair (captures into fmac pipe slot — every FMAC pair)
// and read/written by the FMAC arith writeback (emitFmacInlineWriteback),
// the FDIV stall-add (statusflag snapshot), FSSET/FCSET/CLIP, and FDIV/
// SQRT/RSQRT's statusflag update paths. Pinning them eliminates a memory
// round-trip per FMAC pair (3 Ldrs → 0) plus saves the scattered Ldr/Str
// pairs in the flag-writing ops.
//
// Loaded once at block prologue, flushed at block epilogue (and around the
// vu1Exec hazard fallback — the only default-build BL that can mutate these
// fields. Other BLs write VI[REG_*_FLAG] instead, which is a separate
// "committed" slot not cached here).
//
//   w19 ↔ VU->macflag
//   w20 ↔ VU->statusflag
//   w28 ↔ VU->clipflag
// All three are 32-bit u32 fields — w-reg writes zero-extend into x-reg,
// so the x19/x20/x28 64-bit forms stay canonical.
static const auto VU1_MACFLAG_REG    = w19;
static const auto VU1_STATUSFLAG_REG = w20;
static const auto VU1_CLIPFLAG_REG   = w28;

// Phase-8 (2026-04-22): pinned VU->ACC register. Must match the alias in
// iVU1Upper_arm64.cpp. Loaded at block prologue, flushed at epilogue, and
// flushed/reloaded around the vu1Exec hazard fallback (the only default-
// build BL that can mutate ACC). Other BLs (TestPipes, XGKICK helpers,
// CheckDTBits, EbitDone, HandleDelayBranch, stall probes) all leave ACC
// untouched.
//
// Reg choice: q16. Caller-saved on AAPCS64 (upper half of d8-d15 is caller-
// saved; q-form gives no callee-save either), so we MUST flush+reload
// around any BL that could clobber/mutate ACC. Audit above confirms only
// vu1Exec qualifies on the default build.
static const auto VU1_ACC_REG = v16;

static void emitFlushWposRegs(int64_t fmacwpos_off, int64_t ialuwpos_off)
{
	armAsm->Str(VU1_FMAC_WPOS_REG, MemOperand(VU1_BASE_REG, fmacwpos_off));
	armAsm->Str(VU1_IALU_WPOS_REG, MemOperand(VU1_BASE_REG, ialuwpos_off));
}

static void emitReloadWposRegs(int64_t fmacwpos_off, int64_t ialuwpos_off)
{
	armAsm->Ldr(VU1_FMAC_WPOS_REG, MemOperand(VU1_BASE_REG, fmacwpos_off));
	armAsm->Ldr(VU1_IALU_WPOS_REG, MemOperand(VU1_BASE_REG, ialuwpos_off));
}

// Phase-9b: VU->fmaccount pin flush/reload. Flush before any BL that reads
// VU->fmaccount (vu1_TestFMACStallReg's loop bound, _vuTestPipes, _vuFMACflush
// via _vuFlushAll/vu1EbitDone/vu1Exec/emitVU1InterpBL/hack_xgkick capture).
// Reload after BLs that decrement VU->fmaccount (everything in that list
// except vu1_TestFMACStallReg, which is read-only — flush only there).
//
// #18 from FMAC optimization deep-dive — fmaccount batching:
// emitFMACAddPair would normally bump w26 by 1 every FMAC pair. For runs of
// consecutive FMAC pairs with no intervening BL that needs the value,
// collapse those Adds into a single Add at the next flush site (or block
// end). s_vu1_deferred_fmaccount tracks the pending count; Flush drains it
// into w26 before the Str; Reload zeroes it (the post-BL memory value is
// authoritative; any pending count is stale).
static int s_vu1_deferred_fmaccount = 0;

static void emitFlushFmaccountReg(int64_t fmaccount_off)
{
	if (s_vu1_deferred_fmaccount > 0)
	{
		armAsm->Add(VU1_FMACCOUNT_REG, VU1_FMACCOUNT_REG, s_vu1_deferred_fmaccount);
		s_vu1_deferred_fmaccount = 0;
	}
	armAsm->Str(VU1_FMACCOUNT_REG, MemOperand(VU1_BASE_REG, fmaccount_off));
}

// Drain any pending deferred fmaccount increment into the pinned reg, but
// DO NOT emit the Str to memory. Use when the next BL receives fmaccount
// as an argument (in w1) instead of via memory — saves the per-BL Str.
// Helpers using this convention: vu1_TestPipes_VU1, vu1_TestFMACStallReg,
// vu1_TestFMACStallReg2.
static void emitDrainFmaccountReg()
{
	if (s_vu1_deferred_fmaccount > 0)
	{
		armAsm->Add(VU1_FMACCOUNT_REG, VU1_FMACCOUNT_REG, s_vu1_deferred_fmaccount);
		s_vu1_deferred_fmaccount = 0;
	}
}

static void emitReloadFmaccountReg(int64_t fmaccount_off)
{
	// Memory holds the authoritative value post-BL; any pending defer is stale.
	s_vu1_deferred_fmaccount = 0;
	armAsm->Ldr(VU1_FMACCOUNT_REG, MemOperand(VU1_BASE_REG, fmaccount_off));
}

// Flag register flush/reload. Called at block epilogue and around the
// vu1Exec hazard-fallback BL.
static void emitFlushFlagRegs(int64_t macflag_off, int64_t statusflag_off, int64_t clipflag_off)
{
	armAsm->Str(VU1_MACFLAG_REG,    MemOperand(VU1_BASE_REG, macflag_off));
	armAsm->Str(VU1_STATUSFLAG_REG, MemOperand(VU1_BASE_REG, statusflag_off));
	armAsm->Str(VU1_CLIPFLAG_REG,   MemOperand(VU1_BASE_REG, clipflag_off));
}

static void emitReloadFlagRegs(int64_t macflag_off, int64_t statusflag_off, int64_t clipflag_off)
{
	armAsm->Ldr(VU1_MACFLAG_REG,    MemOperand(VU1_BASE_REG, macflag_off));
	armAsm->Ldr(VU1_STATUSFLAG_REG, MemOperand(VU1_BASE_REG, statusflag_off));
	armAsm->Ldr(VU1_CLIPFLAG_REG,   MemOperand(VU1_BASE_REG, clipflag_off));
}

// Phase-8: pinned ACC register flush/reload. Called at block prologue/
// epilogue and around vu1Exec (hazard fallback).
static void emitFlushAccReg(int64_t acc_off)
{
	armAsm->Str(VU1_ACC_REG.Q(), MemOperand(VU1_BASE_REG, acc_off));
}

static void emitReloadAccReg(int64_t acc_off)
{
	armAsm->Ldr(VU1_ACC_REG.Q(), MemOperand(VU1_BASE_REG, acc_off));
}

// ============================================================================
//  VF register cache (Phase 2: deferred writes + per-lane dirty tracking)
// ============================================================================
//
// Mirrors the cross-pair VF residency design from the old port-in-place at
// ARMSX2-master/x86/microVU_IR.h's microRegAlloc. The current rewrite re-Ldr/
// Str's VF[fs]/VF[ft]/VF[fd] for every FMAC pair (~3 memory ops/op); this
// cache keeps recently-read VFs in NEON regs across pairs, so a matrix-
// transform chain reading the same vertex VF four times pays one Ldr instead
// of four. Phase 2 extends with deferred writes — store results into the
// cache slot, defer the actual memory Str to block-end / BL / hazard.
//
// Slot fields:
//   vfreg       : -1 = empty, else 0..31 (the VF index resident in this slot)
//   last_use    : monotonic counter for LRU eviction
//   valid_lanes : bitmask of lanes (bit 3=X, 2=Y, 1=Z, 0=W) holding the
//                 authoritative value. Always 0 (empty) or 0xF (full) — we
//                 force-load the full VF before any partial write so reads
//                 always see a complete value. Simplifies read paths.
//   dirty_lanes : subset of valid_lanes that are unflushed. Flush emits a
//                 partial-lane Str matching this mask, then clears it.
//
// Slot pool: v17..v24 (8 slots). v16 is ACC (pinned). v0..v15 are FMAC scratch.
// All NEON regs are caller-saved across BL on AAPCS64, so every BL site MUST
// flush dirty + invalidate — see emitVu1Call / emitVU1InterpBL wiring.
//
// Compile-time only — these helpers track state during emit, not at runtime.
// vfCacheReset() emits no code; it just zeroes the tracker. The emitted code
// path naturally arrives at the same NEON state because emit-side Ldr/Mov/
// Str decisions are deterministic given the tracker.

static constexpr int kVfCacheSize = 8;
static constexpr int kVfCacheBaseReg = 17; // v17..v24

struct VfCacheSlot
{
	int  vfreg;        // -1 = empty
	u32  last_use;
	u8   valid_lanes;  // 0 or 0xF (we force full-load on miss before partial writes)
	u8   dirty_lanes;  // subset of valid_lanes
};

static VfCacheSlot s_vfCache[kVfCacheSize];
static u32 s_vfCacheClock;

// Byte offset of VF[reg] from VU1_BASE_REG (= &VU1). Same formula as the
// per-file vfOff() in iVU1Upper_arm64.cpp / iVU1Lower_arm64.cpp; duplicated
// here because those are file-static.
static constexpr int64_t vfCacheOffsetOf(int vfreg)
{
	return static_cast<int64_t>(offsetof(VURegs, VF))
		+ static_cast<int64_t>(vfreg) * static_cast<int64_t>(sizeof(VECTOR));
}

// Reset the compile-time tracker. Emits no code. Call at block prologue
// (cache cold). NOT safe to call mid-block if any slot has dirty_lanes
// without flushing first — that would silently drop deferred writes. Use
// vfCacheFlushAndInvalidate() in that case.
void vfCacheReset()
{
	for (int i = 0; i < kVfCacheSize; i++)
	{
		s_vfCache[i].vfreg = -1;
		s_vfCache[i].last_use = 0;
		s_vfCache[i].valid_lanes = 0;
		s_vfCache[i].dirty_lanes = 0;
	}
	s_vfCacheClock = 0;
}

// Find vfreg in cache. Returns slot index 0..kVfCacheSize-1 on hit, -1 on miss.
static int vfCacheFind(int vfreg)
{
	for (int i = 0; i < kVfCacheSize; i++)
	{
		if (s_vfCache[i].vfreg == vfreg)
			return i;
	}
	return -1;
}

// Forward-declare the partial-lane store emitter — used by the eviction
// path to flush dirty lanes of an LRU victim before reusing its slot.
static void vfCacheEmitPartialLaneStore(int slot, int vfreg, u8 lanes);

// Pick a slot to allocate for vfreg. Returns slot index. Prefers empty
// slots; falls back to LRU eviction. If the LRU victim has dirty lanes,
// emit a partial-lane Str to flush them before reusing the slot.
static int vfCacheAllocSlot(int vfreg)
{
	int empty = -1;
	int lru = 0;
	u32 lru_stamp = ~0u;
	for (int i = 0; i < kVfCacheSize; i++)
	{
		if (s_vfCache[i].vfreg < 0 && empty < 0)
			empty = i;
		if (s_vfCache[i].last_use < lru_stamp)
		{
			lru_stamp = s_vfCache[i].last_use;
			lru = i;
		}
	}
	const int slot = (empty >= 0) ? empty : lru;
	if (s_vfCache[slot].dirty_lanes != 0)
	{
		vfCacheEmitPartialLaneStore(slot, s_vfCache[slot].vfreg,
			s_vfCache[slot].dirty_lanes);
	}
	s_vfCache[slot].vfreg = vfreg;
	s_vfCache[slot].valid_lanes = 0;
	s_vfCache[slot].dirty_lanes = 0;
	s_vfCache[slot].last_use = ++s_vfCacheClock;
	return slot;
}

// NEON register code for cache slot `i`. v17..v24.
static int vfCacheSlotReg(int slot)
{
	return kVfCacheBaseReg + slot;
}

// Emit partial-lane Str for the slot's dirty lanes back to VU1.VF[vfreg].
// Mirrors emitPartialLaneStore in iVU1Upper_arm64.cpp but operates on the
// cache slot reg directly instead of v5. Used by allocSlot's LRU eviction
// and the bulk vfCacheFlushDirty path.
//
// Special-cases the all-lanes (0xF) full Str since it's a single insn.
// Other masks fall back to lane-by-lane stores via Mov-to-scratch + Str s.
// This is correct but not optimal — the FMAC path's emitPartialLaneStore
// has more peephole patterns (Str d for adjacent dual lanes, etc.); we
// could mirror those if profiling shows it matters.
static void vfCacheEmitPartialLaneStore(int slot, int vfreg, u8 lanes)
{
	if (vfreg <= 0 || lanes == 0)
		return;
	const int64_t base = vfCacheOffsetOf(vfreg);
	const a64::VRegister slotReg(vfCacheSlotReg(slot), 128);

	if (lanes == 0xF)
	{
		armAsm->Str(slotReg.Q(), a64::MemOperand(VU1_BASE_REG, base));
		return;
	}

	// Adjacent dual-lane fast paths.
	if (lanes == 0xC)
	{
		armAsm->Str(slotReg.D(), a64::MemOperand(VU1_BASE_REG, base + 0));
		return;
	}
	if (lanes == 0x3)
	{
		// Rotate high-64 of slot into v2 low-64 then Str d2 at +8.
		armAsm->Ext(a64::v2.V16B(), slotReg.V16B(), slotReg.V16B(), 8);
		armAsm->Str(a64::d2, a64::MemOperand(VU1_BASE_REG, base + 8));
		return;
	}

	// Single-lane and triple-lane fall to per-lane stores via v2 scratch.
	auto emitLaneS = [&](int lane) {
		if (lane == 0)
		{
			armAsm->Str(slotReg.S(), a64::MemOperand(VU1_BASE_REG, base + 0));
		}
		else
		{
			armAsm->Mov(a64::v2.V4S(), 0, slotReg.V4S(), lane);
			armAsm->Str(a64::s2, a64::MemOperand(VU1_BASE_REG, base + lane * 4));
		}
	};

	if (lanes & 0x8) emitLaneS(0); // X
	if (lanes & 0x4) emitLaneS(1); // Y
	if (lanes & 0x2) emitLaneS(2); // Z
	if (lanes & 0x1) emitLaneS(3); // W
}

// Internal: ensure a slot for vfreg is loaded with all 4 lanes valid.
// Returns the slot index. Allocates and Ldrs from memory on miss; on hit,
// just bumps the LRU clock. The returned slot has valid_lanes == 0xF;
// dirty_lanes are unchanged (a hit on a partially-dirty slot keeps that).
static int vfCacheEnsureLoaded(int vfreg)
{
	int slot = vfCacheFind(vfreg);
	if (slot < 0)
	{
		slot = vfCacheAllocSlot(vfreg);
		const a64::VRegister slotReg(vfCacheSlotReg(slot), 128);
		armAsm->Ldr(slotReg.Q(), a64::MemOperand(VU1_BASE_REG, vfCacheOffsetOf(vfreg)));
		s_vfCache[slot].valid_lanes = 0xF;
	}
	else
	{
		s_vfCache[slot].last_use = ++s_vfCacheClock;
	}
	return slot;
}

// Emit code to materialize VF[vfreg] into `scratch`. Cache hit → Mov from the
// resident slot. Cache miss → Ldr into a slot, then Mov to scratch.
//
// `vfreg` of 0 short-circuits to a plain Ldr (VF0 holds the constant
// {0,0,0,1}; not worth reserving a slot to cache it).
void vfCacheLoadInto(int vfreg, const a64::VRegister& scratch)
{
	// FMAC opt #16: loading into v1 destroys any cached broadcast there.
	// vixl's v1 has GetCode() == 1; comparing by code avoids dragging the
	// vixl global v1 into this header-light TU's cache reset hook.
	if (scratch.GetCode() == 1)
		vu1BroadcastCacheReset();

	if (vfreg == 0)
	{
		armAsm->Ldr(scratch.Q(), a64::MemOperand(VU1_BASE_REG, vfCacheOffsetOf(0)));
		return;
	}
	const int slot = vfCacheEnsureLoaded(vfreg);
	const a64::VRegister slotReg(vfCacheSlotReg(slot), 128);
	if (slotReg.GetCode() != scratch.GetCode())
		armAsm->Mov(scratch.Q(), slotReg.Q());
}

// Like vfCacheLoadInto but returns the resident NEON reg directly so the
// caller can use it in-place without an extra Mov. Caller must NOT modify
// the returned register — it's the cache's authoritative copy.
a64::VRegister vfCacheLoadResident(int vfreg)
{
	const int slot = vfCacheEnsureLoaded(vfreg);
	return a64::VRegister(vfCacheSlotReg(slot), 128);
}

// FMAC opt #6 (cache-read variant): cheap residency probe. Lets a caller
// ask "is vfreg in a slot already?" without triggering vfCacheLoadResident's
// allocate-and-Ldr miss path. CLIP uses this to skip 4 memory Ldrs when
// fs/ft happen to be resident from a recent FMAC pair.
bool vfCacheIsResident(int vfreg)
{
	return vfCacheFind(vfreg) >= 0;
}

// Phase 2.5 (write-through): merge `src_reg`'s `xyzw_mask` lanes into
// VF[vfreg], updating the cache slot AND immediately storing to memory.
// dirty_lanes stays at 0 — slot is always clean, just holds the up-to-date
// value so subsequent reads of vfreg in this block hit cache instead of
// reloading from memory.
//
// Why write-through instead of deferred writes:
//   The deferred-write Phase 2 design dropped graphics in GoW2 — the
//   suspected cause is a coherence path (cross-block, BL fallback, or
//   pipeline ring slot) that reads VF memory directly without going
//   through the cache flush machinery. Rather than chase every such path,
//   write-through guarantees memory is always coherent at the cost of
//   one extra Str per FMAC writeback. Read-side cache wins (the dominant
//   perf benefit per the old port comparison) are preserved.
//
// xyzw_mask uses the FMAC convention: bit 3 = X, bit 2 = Y, bit 1 = Z,
// bit 0 = W. Lane indices in NEON `Mov v_dst.s[lane], v_src.s[lane]` are
// 0..3 (X..W), so lane = 3 - bit_position when iterating high to low.
//
// vfreg == 0 is a no-op: VF0 is hardwired to {0,0,0,1} and the interpreter
// silently drops writes to it.
void vfCacheStore(int vfreg, const a64::VRegister& src_reg, u8 xyzw_mask)
{
	if (vfreg <= 0 || xyzw_mask == 0)
		return;

	// FMAC opt #16: writing VF[vfreg] makes any cached broadcast of the
	// same VF stale. Notified before the slot/Mov/Str dance so subsequent
	// emitLoadBroadcast(vfreg, _) re-emits the load.
	vu1BroadcastCacheNoteVfWritten(vfreg);

	int slot = vfCacheFind(vfreg);
	if (slot < 0)
	{
		slot = vfCacheAllocSlot(vfreg);
	}

	const a64::VRegister slotReg(vfCacheSlotReg(slot), 128);

	// Force-load if partial write and slot doesn't have all lanes valid —
	// preserves unmodified lanes when subsequent reads come from cache.
	if (xyzw_mask != 0xF && s_vfCache[slot].valid_lanes != 0xF)
	{
		armAsm->Ldr(slotReg.Q(), a64::MemOperand(VU1_BASE_REG, vfCacheOffsetOf(vfreg)));
	}
	s_vfCache[slot].valid_lanes = 0xF;

	// Merge result lanes into slotReg.
	if (xyzw_mask == 0xF)
	{
		if (slotReg.GetCode() != src_reg.GetCode())
			armAsm->Mov(slotReg.V16B(), src_reg.V16B());
	}
	else
	{
		if (xyzw_mask & 0x8) armAsm->Mov(slotReg.V4S(), 0, src_reg.V4S(), 0);
		if (xyzw_mask & 0x4) armAsm->Mov(slotReg.V4S(), 1, src_reg.V4S(), 1);
		if (xyzw_mask & 0x2) armAsm->Mov(slotReg.V4S(), 2, src_reg.V4S(), 2);
		if (xyzw_mask & 0x1) armAsm->Mov(slotReg.V4S(), 3, src_reg.V4S(), 3);
	}

#ifdef VU1_DEFER_VF_WRITES
	// Phase 2: deferred writes. Mark the just-written lanes as dirty;
	// memory Str is emitted at the next flush site (LRU eviction in
	// vfCacheAllocSlot, BL via vfCacheFlushAndInvalidate, inline-bypass
	// via vfCacheFlushOne, block end). Saves 1 Str per FMAC pair when a
	// subsequent op in the same block re-reads or re-writes the same VF.
	//
	// Run with VU1_SHADOW_VERIFY enabled to catch missed flush sites —
	// any path that reads VF memory directly without going through the
	// cache will surface as a per-pair divergence (likely VF[N] mismatch).
	s_vfCache[slot].dirty_lanes |= xyzw_mask;
#else
	// Phase 2.5: write through to memory immediately. Pass xyzw_mask, not
	// any accumulated dirty_lanes — only the just-modified lanes need to
	// hit memory; force-load already wrote unmodified lanes back to
	// matching memory values. dirty_lanes stays 0.
	vfCacheEmitPartialLaneStore(slot, vfreg, xyzw_mask);
#endif
	s_vfCache[slot].last_use = ++s_vfCacheClock;
}

// Flush every dirty slot to memory. Slots stay loaded (valid_lanes intact)
// so subsequent reads still hit the cache. Used at sites where memory must
// be coherent but NEON state is preserved (e.g., before a BL that READS but
// doesn't clobber NEON — rare; most use vfCacheFlushAndInvalidate instead).
void vfCacheFlushDirty()
{
	for (int i = 0; i < kVfCacheSize; i++)
	{
		if (s_vfCache[i].dirty_lanes == 0)
			continue;
		vfCacheEmitPartialLaneStore(i, s_vfCache[i].vfreg, s_vfCache[i].dirty_lanes);
		s_vfCache[i].dirty_lanes = 0;
	}
}

// Phase 2: flush dirty lanes of a single VF, then drop its slot. Used by
// op emitters that bypass the cache (inline Ldr/Str on VF memory) to keep
// memory coherent with deferred writes for that one VF, without losing
// the cache state of every other VF.
void vfCacheFlushOne(int vfreg)
{
	if (vfreg <= 0)
		return;
	const int slot = vfCacheFind(vfreg);
	if (slot < 0)
		return;
	if (s_vfCache[slot].dirty_lanes != 0)
	{
		vfCacheEmitPartialLaneStore(slot, s_vfCache[slot].vfreg,
			s_vfCache[slot].dirty_lanes);
	}
	s_vfCache[slot].vfreg = -1;
	s_vfCache[slot].valid_lanes = 0;
	s_vfCache[slot].dirty_lanes = 0;
	s_vfCache[slot].last_use = 0;
}

// Flush dirty slots, then drop all tracker state. Used at every BL (NEON
// is caller-saved) and at block epilogue (linked successors don't share
// our compile-time slot map).
void vfCacheFlushAndInvalidate()
{
	for (int i = 0; i < kVfCacheSize; i++)
	{
		if (s_vfCache[i].dirty_lanes != 0)
		{
			vfCacheEmitPartialLaneStore(i, s_vfCache[i].vfreg,
				s_vfCache[i].dirty_lanes);
		}
		s_vfCache[i].vfreg = -1;
		s_vfCache[i].valid_lanes = 0;
		s_vfCache[i].dirty_lanes = 0;
		s_vfCache[i].last_use = 0;
	}
	s_vfCacheClock = 0;
	// FMAC opt #16: BL clobbers caller-saved v1, so the broadcast cache
	// (which lives in v1) is invalidated alongside the per-VF cache.
	vu1BroadcastCacheReset();
}

// Drop the cached copy of `vfreg`, if any. Call when external code (e.g., a
// BL helper that doesn't go through emitVu1Call) has just modified VU1.VF[
// vfreg] memory — the cache must drop the now-stale slot. If the slot is
// dirty when this is called, the deferred writes are silently dropped:
// callers must flush first if they care about losing the deferred values.
// In practice this should only be called when the external code FULLY
// overwrites VF[vfreg] (so dropping deferred writes is moot — they'd have
// been overwritten anyway).
void vfCacheInvalidate(int vfreg)
{
	if (vfreg <= 0)
		return;
	const int slot = vfCacheFind(vfreg);
	if (slot >= 0)
	{
		s_vfCache[slot].vfreg = -1;
		s_vfCache[slot].valid_lanes = 0;
		s_vfCache[slot].dirty_lanes = 0;
		s_vfCache[slot].last_use = 0;
	}
}

// Drop every cached entry without flushing. Phase 1 used this around BLs
// (write-through meant nothing was dirty); Phase 2 should NOT call this on
// a hot block path because deferred writes would silently disappear. Use
// vfCacheFlushAndInvalidate instead. This entry point is preserved for
// emergency-reset cases (e.g., compile-time errors, code-buffer reset).
void vfCacheInvalidateAll()
{
	for (int i = 0; i < kVfCacheSize; i++)
	{
		s_vfCache[i].vfreg = -1;
		s_vfCache[i].valid_lanes = 0;
		s_vfCache[i].dirty_lanes = 0;
		s_vfCache[i].last_use = 0;
	}
	s_vfCacheClock = 0;
}

// ============================================================================
//  VI register cache (write-through, mirrors VF cache architecture)
// ============================================================================
//
// The current arm64 VU JIT re-loads/stores every VI to memory per op (Ldrh +
// arith + Strh = 3 memory ops per IALU op). The old port-in-place of x86
// microVU at ARMSX2-master kept VIs resident in callee-saved x86 GPRs across
// pairs via microRegAlloc::allocGPR. This cache mirrors the design but uses
// caller-saved arm64 GPRs (w9..w15, 7 slots) — same as the VF cache, with
// flush+invalidate around every BL.
//
// VI registers are 16-bit on PS2 (low halfword of REG_VI). The cache stores
// them zero-extended in 32-bit w-regs. Sign extension on reads is the
// consumer's job (Sxth from cached w-reg). Writes Strh the low 16 bits.
//
// Write-through (no deferred writes):
//   - vfCache had a deferred-write attempt (Phase 2) that broke graphics in
//     GoW2; we shipped Phase 2.5 write-through instead. Apply the same lesson
//     here — VIbackup and the interpreter-helper paths read VI memory
//     directly, and any deferred-write coherence gap would silently corrupt
//     branch targets / memory addresses.
//
// VI[0] is hardwired to 0: reads short-circuit to wzr, writes are dropped.
//
// Pool: w9..w15. These are caller-saved on AAPCS64 and unused by the VU1
// emit's per-op scratch (which uses w4..w6 / x5-x7) and by VU1's pinned
// state regs (w19, w20, x21-x28). vixl's macro-assembler reserves IP0/IP1
// (x16/x17), so this pool is also safe from internal vixl spills.
//
// Compile-time only — these helpers track state during emit, not at runtime.

static constexpr int kViCacheSize = 7;
static constexpr int kViCacheBaseReg = 9; // w9..w15

struct ViCacheSlot
{
	int  vireg;     // -1 = empty, else 1..15 (vireg 0 is hardwired-zero, never cached)
	u32  last_use;  // monotonic counter for LRU eviction
};

static ViCacheSlot s_viCache[kViCacheSize];
static u32 s_viCacheClock;

// Byte offset of VI[reg] from VU1_BASE_REG. REG_VI is 16 bytes; the 16-bit
// VI value lives in the low halfword (matches Ldrh/Strh access).
static constexpr int64_t viCacheOffsetOf(int vireg)
{
	return static_cast<int64_t>(offsetof(VURegs, VI))
		+ static_cast<int64_t>(vireg) * static_cast<int64_t>(sizeof(REG_VI));
}

void viCacheReset()
{
	for (int i = 0; i < kViCacheSize; i++)
	{
		s_viCache[i].vireg = -1;
		s_viCache[i].last_use = 0;
	}
	s_viCacheClock = 0;
}

static int viCacheFind(int vireg)
{
	for (int i = 0; i < kViCacheSize; i++)
	{
		if (s_viCache[i].vireg == vireg)
			return i;
	}
	return -1;
}

static int viCacheSlotReg(int slot)
{
	return kViCacheBaseReg + slot;
}

// Pick a slot to allocate for vireg. Prefer empty, fall back to LRU. Write-
// through means slots are never dirty; eviction is a pure tracker reset.
static int viCacheAllocSlot(int vireg)
{
	int empty = -1;
	int lru = 0;
	u32 lru_stamp = ~0u;
	for (int i = 0; i < kViCacheSize; i++)
	{
		if (s_viCache[i].vireg < 0 && empty < 0)
			empty = i;
		if (s_viCache[i].last_use < lru_stamp)
		{
			lru_stamp = s_viCache[i].last_use;
			lru = i;
		}
	}
	const int slot = (empty >= 0) ? empty : lru;
	s_viCache[slot].vireg = vireg;
	s_viCache[slot].last_use = ++s_viCacheClock;
	return slot;
}

// Helper: build a 32-bit (w-form) Register for slot N. vixl globals like w9
// are of type `Register`, not `WRegister`; the latter is just a constructor
// alias that returns a Register. Use Register(code, 32) to mirror that.
static a64::Register viCacheSlotWReg(int slot)
{
	return a64::Register(viCacheSlotReg(slot), 32);
}

// Internal: ensure a slot is loaded with VI[vireg] zero-extended in its
// w-reg. Returns the slot index.
static int viCacheEnsureLoaded(int vireg)
{
	int slot = viCacheFind(vireg);
	if (slot < 0)
	{
		slot = viCacheAllocSlot(vireg);
		const a64::Register slotReg = viCacheSlotWReg(slot);
		armAsm->Ldrh(slotReg, a64::MemOperand(VU1_BASE_REG, viCacheOffsetOf(vireg)));
	}
	else
	{
		s_viCache[slot].last_use = ++s_viCacheClock;
	}
	return slot;
}

// Materialize VI[vireg] into `scratch`. Cache hit → Mov from cached w-reg;
// miss → Ldrh into a slot, then Mov to scratch. vireg == 0 → Mov scratch, wzr.
void viCacheLoadInto(int vireg, const a64::Register& scratch)
{
	if (vireg == 0)
	{
		armAsm->Mov(scratch, a64::wzr);
		return;
	}
	const int slot = viCacheEnsureLoaded(vireg);
	const a64::Register slotReg = viCacheSlotWReg(slot);
	if (slotReg.GetCode() != scratch.GetCode())
		armAsm->Mov(scratch, slotReg);
}

// Like viCacheLoadInto but returns the resident w-reg directly. Caller must
// NOT modify it — it's the cache's authoritative copy. Returns wzr for VI[0].
//
// NOTE: vixl rejects wzr as a source for many macros — Sxth/Sxtb/Sxtw/Uxth/
// Uxtb (extend) AND Lsl/Lsr/Asr (shift macros all assert !rn.IsZero()).
// Callers that need a sign-extended read MUST go through viCacheLoadSignedInto.
// Callers that need to shift the result MUST handle the vireg==0 case with
// a direct `Mov dest, wzr` (target value is 0 anyway). Simple Mov / Add /
// And / Orr / Sub / Cmp on the result are fine — those accept wzr.
a64::Register viCacheLoadResident(int vireg)
{
	if (vireg == 0)
		return a64::wzr;
	const int slot = viCacheEnsureLoaded(vireg);
	return viCacheSlotWReg(slot);
}

// Materialize VI[vireg] sign-extended into `dest`. Used by consumers that
// need signed semantics (IBxx hazard reads, MFIR broadcast, LQ/SQ index
// computation). Special-cases vireg==0 to `Mov dest, wzr` because vixl's
// Sxth asserts on wzr source.
void viCacheLoadSignedInto(int vireg, const a64::Register& dest)
{
	if (vireg == 0)
	{
		armAsm->Mov(dest, a64::wzr);
		return;
	}
	const int slot = viCacheEnsureLoaded(vireg);
	armAsm->Sxth(dest, viCacheSlotWReg(slot));
}

// Write-through: copy `src_reg` (low 16 bits) into the cache slot AND Strh
// to VI[vireg] memory. dirty tracking unused (always clean). vireg == 0 is
// silently dropped (VI[0] hardwired).
void viCacheStore(int vireg, const a64::Register& src_reg)
{
	if (vireg <= 0)
		return;

	int slot = viCacheFind(vireg);
	if (slot < 0)
		slot = viCacheAllocSlot(vireg);

	const a64::Register slotReg = viCacheSlotWReg(slot);

	// Cache slot: copy src into the slot reg (zero-extend the low 16 bits).
	// vixl's Uxth asserts !rn.IsZero(), so route wzr-source through Mov which
	// already produces a zero-extended zero in the slot.
	if (src_reg.IsZero())
		armAsm->Mov(slotReg, a64::wzr);
	else if (slotReg.GetCode() != src_reg.GetCode())
		armAsm->Uxth(slotReg, src_reg);
	else
		armAsm->Uxth(slotReg, slotReg); // self-truncate: in-place 16-bit clamp

	// Memory: Strh the low 16 bits.
	armAsm->Strh(slotReg, a64::MemOperand(VU1_BASE_REG, viCacheOffsetOf(vireg)));

	s_viCache[slot].last_use = ++s_viCacheClock;
}

// Drop any cached copy of `vireg`. Call after external code (e.g., a BL) has
// modified VI memory and the slot mapping is stale. Write-through means there
// are no dirty bits to flush — this is purely a tracker reset.
void viCacheInvalidate(int vireg)
{
	if (vireg <= 0)
		return;
	const int slot = viCacheFind(vireg);
	if (slot >= 0)
	{
		s_viCache[slot].vireg = -1;
		s_viCache[slot].last_use = 0;
	}
}

void viCacheInvalidateAll()
{
	for (int i = 0; i < kViCacheSize; i++)
	{
		s_viCache[i].vireg = -1;
		s_viCache[i].last_use = 0;
	}
	s_viCacheClock = 0;
}

// Convenience alias matching the VF cache naming. Write-through means no
// flush emit is ever needed — this is functionally identical to invalidate.
void viCacheFlushOne(int vireg)
{
	viCacheInvalidate(vireg);
}

void viCacheFlushAndInvalidate()
{
	viCacheInvalidateAll();
}

// Wrapper for armEmitCall that flushes deferred VF writes and drops the
// cache tracker. Use for every BL in the VU1 emit path — AAPCS64 caller-
// saves all NEON regs, AND the helper may read VF memory and would see
// stale values without the flush. Phase 2 emits Strs for any dirty lanes
// here, then resets the tracker.
//
// Also drops the VI cache tracker — the cache's GPR pool (w9..w15) is
// caller-saved and clobbered by any BL.
void emitVu1Call(const void* fn)
{
	vfCacheFlushAndInvalidate();
	viCacheInvalidateAll();
	armEmitCall(fn);
}

// Specialized BL for helpers that are provably NEON-free leaf functions —
// i.e., the compiler emitted only x/w GPR instructions, no v/q/d/s ops,
// no nested BL, no callee-saved spills. For these the BL preserves:
//   - v17..v24 (VF cache slots) — vfCacheFlushAndInvalidate skipped.
//   - v1 (broadcast cache reg)  — vu1BroadcastCacheReset skipped.
//
// The VI cache tracker IS still invalidated: AAPCS64 caller-saves w9..w15
// (the VI cache GPR pool), and the helper still clobbers them as scratch
// registers regardless of NEON usage.
//
// Verified-NEON-free callees (objdump 2026-05-16, Debug build, clang 18):
//   - vu1_TestPipes_VU1     (19.49% of total CPU on GoW2 in-game)
//   - vu1_TestFMACStallReg  (4.26%)
//   - vu1_TestFMACStallReg2 (2.50%)
// All three are leaf, scalar-only, with no FP/NEON instructions.
//
// If a future change to any of these helpers introduces NEON usage (e.g.,
// memcpy of a 16-byte struct, vectorized ring scan, FP arithmetic), THIS
// PATH MUST BE REVERTED for that callee or VF cache slot values will be
// silently corrupted. Re-run the objdump filter to verify after any edit:
//   llvm-objdump -d --disassemble-symbols=<mangled> iVU1micro_arm64.cpp.o
// and grep for v/q/d/s register operands.
void emitVu1CallNeonFree(const void* fn)
{
	viCacheInvalidateAll();
	armEmitCall(fn);
}

// ISTUB helper — emits the full pinned-register flush / interpreter BL /
// reload dance for ops that routed to the C interpreter via
// REC_VU1_UPPER_INTERP / REC_VU1_LOWER_INTERP. Keeps the hybrid harness
// (INTERP_VU_UPPER / FDIV / IALU / LOADSTORE / BRANCH / MISC) correct
// against Phases 7/8 — without this, the interpreter reads stale memory
// (our pins hold the authoritative values) and any writes it makes don't
// survive the return into JIT code.
//
// Mirrors the vu1Exec hazard-fallback pattern in CompileBlock: cycle
// (x21), fmac/ialu wpos (x24/x25), flag regs (w19/w20/w28), ACC (q16)
// all flush+reload.
//
// Exposed (non-static) so iVU1Upper_arm64.cpp and iVU1Lower_arm64.cpp
// can call it from the REC_VU1_*_INTERP macros.
void emitVU1InterpBL(const void* interp_fn)
{
	const int64_t cycle_off      = (int64_t)offsetof(VURegs, cycle);
	const int64_t fmacwpos_off   = (int64_t)offsetof(VURegs, fmacwritepos);
	const int64_t ialuwpos_off   = (int64_t)offsetof(VURegs, ialuwritepos);
	const int64_t fmaccount_off  = (int64_t)offsetof(VURegs, fmaccount);
	const int64_t macflag_off    = (int64_t)offsetof(VURegs, macflag);
	const int64_t statusflag_off = (int64_t)offsetof(VURegs, statusflag);
	const int64_t clipflag_off   = (int64_t)offsetof(VURegs, clipflag);
	const int64_t acc_off        = (int64_t)offsetof(VURegs, ACC);

	emitFlushCycleReg(cycle_off);
	emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
	emitFlushFmaccountReg(fmaccount_off);
	emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
	emitFlushAccReg(acc_off);
	// Phase 2: emit Strs for any deferred VF writes before the BL — the
	// interpreter reads VF memory and would see stale values otherwise.
	// Then drop the tracker since BL clobbers caller-saved NEON.
	vfCacheFlushAndInvalidate();
	// VI cache tracker drop: w9..w15 are caller-saved, clobbered by BL.
	// Write-through means memory is already coherent; just reset the
	// tracker so post-BL emits don't reference stale slot mappings.
	viCacheInvalidateAll();
	armEmitCall(interp_fn);
	emitReloadCycleReg(cycle_off);
	emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
	emitReloadFmaccountReg(fmaccount_off);
	emitReloadFlagRegs(macflag_off, statusflag_off, clipflag_off);
	emitReloadAccReg(acc_off);
}

// ============================================================================
//  Inline emit helpers for per-pair housekeeping
//
//  These replace the BL _vuTest*Stalls / BL _vuClearFMAC / BL _vuAdd*Stalls /
//  BL vu1DecrementVIBackup calls with compile-time-specialized inline code.
//  Most pipes (NOP, MOVE, LQ, IADD, FCAND, ...) end up emitting *zero*
//  instructions for stall housekeeping; only FMAC/FDIV/EFU/IALU/BRANCH
//  pipes emit real work.
//
//  All helpers assume:
//    x23 = &VU1       (VU1_BASE_REG, pinned for the entire block)
//    x22 = cyclesBefore (set by step 1 of every pair; Mov'd from x21)
//    x21 = VU->cycle   (VU1_CYCLE_REG, Stage C2 hoisted cycle counter)
//    x4-x7, x0-x3 are scratch (clobbered freely)
//
//  Any helper that emits a BL to a function which reads or writes
//  `VU->cycle` must flush x21 to memory first (emitFlushCycleReg) and, if
//  the BL may have mutated cycle, reload afterwards (emitReloadCycleReg).
// ============================================================================

// Emit BL vu1_TestFMACStallReg(VU, reg, xyzw) only when reg != 0 AND the
// compile-time pipeline tracker has not already proven no FMAC slot aliases
// (skip0/skip1 flags come from Stage A of the mVUregs port — see the
// "Compile-time pipeline state tracking" pre-walk in CompileBlock).
//
// vu1_TestFMACStallReg reads `VU->cycle` and conditionally writes it when
// a stall adjustment is needed, so the Stage C2 cached cycle register
// (x21) must be flushed/reloaded around each BL.
//
// Phase-9b: vu1_TestFMACStallReg also reads (but never writes) VU->fmaccount
// as the loop bound, so the pinned VU1_FMACCOUNT_REG (w26) must be flushed
// before each BL. No reload — the BL doesn't mutate fmaccount.
static void emitFMACStallChecks(const _VURegsNum& regs, bool skip0, bool skip1)
{
	const bool active0 = !skip0 && regs.VFread0 != 0;
	const bool active1 = !skip1 && regs.VFread1 != 0;

	if (!active0 && !active1)
		return;

	// Refactored arg/return convention (cycle in/out via x0, fmaccount in via
	// w1) eliminates the per-BL emitFlush/ReloadCycleReg + emitFlush
	// FmaccountReg memory round-trip. Drain any deferred fmaccount Add
	// into w26 first so the arg is correct.
	emitDrainFmaccountReg();

	// Round-2 fast-path: fmaccount==0 means the helper's ring-walk loop has
	// no iterations and returns cycle unchanged. Skip the entire BL+cache-
	// flush+arg-setup machinery in that case. Hits the start of every FMAC
	// sequence (before the pipe has filled) and any time the pipe drains
	// fully between sequences — common on bursty FMAC code. Same shape as
	// the case-VUPIPE_BRANCH ialucount==0 gate.
	//
	// Cache state: vfCacheFlushAndInvalidate (called inside emitVu1Call) is
	// a runtime no-op under Phase 2.5 write-through (dirty_lanes always 0);
	// the compile-time tracker reset still applies to downstream emit, which
	// the post-bind code handles regardless of which path runs at runtime.
	Label fmac_stall_skip;
	armAsm->Cbz(VU1_FMACCOUNT_REG, &fmac_stall_skip);

	if (active0 && active1)
	{
		// Combined: one BL handles both reads. Args: VU=x0, fmaccount=w1,
		// cycle=x2, reg0=w3, xyzw0=w4, reg1=w5, xyzw1=w6. Returns new cycle.
		armAsm->Mov(w1, VU1_FMACCOUNT_REG);
		armAsm->Mov(x2, VU1_CYCLE_REG);
		armAsm->Mov(w3, regs.VFread0);
		armAsm->Mov(w4, regs.VFr0xyzw);
		armAsm->Mov(w5, regs.VFread1);
		armAsm->Mov(w6, regs.VFr1xyzw);
		armAsm->Mov(x0, VU1_BASE_REG);
		// vu1_TestFMACStallReg2 is NEON-free + writes no memory we cache.
		emitVu1CallNeonFree(reinterpret_cast<const void*>(vu1_TestFMACStallReg2));
		armAsm->Mov(VU1_CYCLE_REG, x0);
	}
	else
	{
		if (active0)
		{
			armAsm->Mov(w1, VU1_FMACCOUNT_REG);
			armAsm->Mov(x2, VU1_CYCLE_REG);
			armAsm->Mov(w3, regs.VFread0);
			armAsm->Mov(w4, regs.VFr0xyzw);
			armAsm->Mov(x0, VU1_BASE_REG);
			// vu1_TestFMACStallReg is NEON-free + read-only.
			emitVu1CallNeonFree(reinterpret_cast<const void*>(vu1_TestFMACStallReg));
			armAsm->Mov(VU1_CYCLE_REG, x0);
		}
		if (active1)
		{
			emitDrainFmaccountReg(); // active0 path may have deferred again — defensive
			armAsm->Mov(w1, VU1_FMACCOUNT_REG);
			armAsm->Mov(x2, VU1_CYCLE_REG);
			armAsm->Mov(w3, regs.VFread1);
			armAsm->Mov(w4, regs.VFr1xyzw);
			armAsm->Mov(x0, VU1_BASE_REG);
			emitVu1CallNeonFree(reinterpret_cast<const void*>(vu1_TestFMACStallReg));
			armAsm->Mov(VU1_CYCLE_REG, x0);
		}
	}
	armAsm->Bind(&fmac_stall_skip);
}

// Inline replacement for BL _vuTestUpperStalls.
// Upper instructions only have an FMAC pipe; everything else is a no-op.
static void emitTestUpperStalls(const _VURegsNum& uregs, bool skipFMACStall0, bool skipFMACStall1)
{
	if (uregs.pipe == VUPIPE_FMAC)
		emitFMACStallChecks(uregs, skipFMACStall0, skipFMACStall1);
}

// Inline replacement for BL _vuTestLowerStalls.
// Lower instructions can be FMAC, FDIV, EFU, or BRANCH (ALU). Other pipes
// (IALU, NONE) are no-ops. Stage B threads through FDIV/EFU/ALU wait skip
// flags in addition to Stage A's FMAC stall skips.
//
// EFU wait note: vu1_TestEFUPipeWait has a mandatory `efu.Cycle -= 1` side
// effect when enable!=0, so skipping is ONLY sound when the pre-walk proved
// the EFU pipe is entirely empty at this pair (no in-block add AND carry-in
// worst-case retired, gate = 54 cycles). Same reasoning applies to FDIV wait
// (gate 12) and ALU stall check (gate 3).
static void emitTestLowerStalls(const _VURegsNum& lregs,
	bool skipFMACStall0, bool skipFMACStall1,
	bool skipFDIVWait, bool skipEFUWait, bool skipALUStall)
{
	const int64_t cycle_off = (int64_t)offsetof(VURegs, cycle);

	switch (lregs.pipe)
	{
		case VUPIPE_FMAC:
			emitFMACStallChecks(lregs, skipFMACStall0, skipFMACStall1);
			break;
		case VUPIPE_FDIV:
			emitFMACStallChecks(lregs, skipFMACStall0, skipFMACStall1);
			if (!skipFDIVWait)
			{
				// Inline of vu1_TestFDIVPipeWait. Helper body is 5 lines of
				// straight-line math, but the BL machinery (cycle flush +
				// vfCacheFlushAndInvalidate + viCacheInvalidateAll + BL +
				// cycle reload) dominated the actual work. Inlining keeps
				// VU1_CYCLE_REG (x21) live and skips both cache flushes.
				//
				// C reference (iVU1micro_arm64.cpp:1082):
				//   if (VU->fdiv.enable != 0) {
				//       u64 newCycle = VU->fdiv.Cycle + VU->fdiv.sCycle;
				//       if (newCycle > VU->cycle) VU->cycle = newCycle;
				//   }
				//
				// Worst-case taken: 7 insns. fdiv-idle fast-path: 2 insns.
				// Scratch x4/x5 are caller-saved and unused at this point
				// in the per-pair emit (only fmacstall checks ran above and
				// they don't preserve x4/x5 across the next pair).
				const int64_t fdiv_off = (int64_t)offsetof(VURegs, fdiv);
				const int64_t d_enable = (int64_t)offsetof(fdivPipe, enable);
				const int64_t d_sCycle = (int64_t)offsetof(fdivPipe, sCycle);
				const int64_t d_Cycle  = (int64_t)offsetof(fdivPipe, Cycle);

				Label fdiv_skip;
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_enable));
				armAsm->Cbz(w4, &fdiv_skip);
				// newCycle (x4) = sCycle (u64) + Cycle (u32, zero-extended via Wreg load)
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_Cycle));
				armAsm->Ldr(x5, MemOperand(VU1_BASE_REG, fdiv_off + d_sCycle));
				armAsm->Add(x4, x5, x4);
				// VU->cycle (VU1_CYCLE_REG = x21) = max(cycle, newCycle)
				armAsm->Cmp(x4, VU1_CYCLE_REG);
				armAsm->Csel(VU1_CYCLE_REG, x4, VU1_CYCLE_REG, hi);
				armAsm->Bind(&fdiv_skip);
			}
			break;
		case VUPIPE_EFU:
			emitFMACStallChecks(lregs, skipFMACStall0, skipFMACStall1);
			if (!skipEFUWait)
			{
				// Inline of vu1_TestEFUPipeWait. Same shape as the FDIV
				// inline above with one twist: when enable!=0 the helper
				// MUST decrement efu.Cycle by 1 (see VUops.cpp:269 — it's
				// part of the pipeline-drain semantics, not a no-op skip).
				// The decrement is preserved by the Sub+Str pair before
				// the cycle-max selection.
				//
				// C reference (iVU1micro_arm64.cpp:1094):
				//   if (VU->efu.enable == 0) return;
				//   VU->efu.Cycle -= 1;
				//   u64 newCycle = VU->efu.sCycle + VU->efu.Cycle;
				//   if (newCycle > VU->cycle) VU->cycle = newCycle;
				//
				// Worst-case taken: 9 insns. efu-idle fast-path: 2 insns.
				const int64_t efu_off = (int64_t)offsetof(VURegs, efu);
				const int64_t e_enable = (int64_t)offsetof(efuPipe, enable);
				const int64_t e_sCycle = (int64_t)offsetof(efuPipe, sCycle);
				const int64_t e_Cycle  = (int64_t)offsetof(efuPipe, Cycle);

				Label efu_skip;
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, efu_off + e_enable));
				armAsm->Cbz(w4, &efu_skip);
				// efu.Cycle -= 1 (mandatory side effect)
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, efu_off + e_Cycle));
				armAsm->Sub(w4, w4, 1);
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, efu_off + e_Cycle));
				// newCycle (x4) = sCycle (u64) + Cycle (post-decrement, zero-ext)
				armAsm->Ldr(x5, MemOperand(VU1_BASE_REG, efu_off + e_sCycle));
				armAsm->Add(x4, x5, x4);
				armAsm->Cmp(x4, VU1_CYCLE_REG);
				armAsm->Csel(VU1_CYCLE_REG, x4, VU1_CYCLE_REG, hi);
				armAsm->Bind(&efu_skip);
			}
			break;
		case VUPIPE_BRANCH:
			// Unconditional B/BAL have VIread == 0; the ALU stall test
			// would be a no-op, so skip the BL entirely.
			if (!skipALUStall && lregs.VIread != 0)
			{
				// Runtime fast-path: ialucount==0 (IALU pipe empty) → the
				// helper's ring loop is a no-op. Skip the BL machinery
				// entirely (cycle Str/Ldr, Mov x0/w1, BL, return). Common
				// after back-to-back branches without intervening IALU
				// writes. Cost on the skip path: 2 insns (Ldr+Cbz). Cost
				// on the taken path: same as before plus the Ldr+Cbz
				// (~2 extra). Worth it because the fast-path is the
				// dominant case on branch-heavy code.
				//
				// Cache state: vfCacheFlushAndInvalidate (called inside
				// emitVu1Call) is a runtime no-op under Phase 2.5
				// write-through (dirty_lanes always 0); only its compile-
				// time tracker reset matters, which still applies to
				// downstream emit regardless of which runtime path runs.
				const int64_t ialucount_off = (int64_t)offsetof(VURegs, ialucount);
				Label alu_skip;
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, ialucount_off));
				armAsm->Cbz(w4, &alu_skip);
				emitFlushCycleReg(cycle_off);
				armAsm->Mov(x0, VU1_BASE_REG);
				armAsm->Mov(w1, lregs.VIread);
				emitVu1Call(reinterpret_cast<const void*>(vu1_TestALUStallReg));
				emitReloadCycleReg(cycle_off);
				armAsm->Bind(&alu_skip);
			}
			break;
		default:
			break;
	}
}

// Inline replacement for BL vu1DecrementVIBackup.
// VIBackupCycles is a u8 field; in the common case it's 0 and we skip the
// whole block via CBZ. Otherwise we compute (VU->cycle - x22) and saturate.
// Stage C2: the VU->cycle load is skipped — we use the cached VU1_CYCLE_REG
// (x21) directly, which is always up-to-date at this point in the pair
// (step 1 bumped it, the lower-stall BLs have been flushed/reloaded, and
// step 6's TestPipes BL does not write cycle).
//
// Uses w4, w5, x6 as scratch (all caller-saved).
static void emitDecrementVIBackup(int64_t /*cycle_off*/, int64_t vibackup_off)
{
	Label skip;

	// w4 = VIBackupCycles (zero-extended from u8)
	armAsm->Ldrb(w4, MemOperand(VU1_BASE_REG, vibackup_off));
	armAsm->Cbz(w4, &skip);

	// x6 = elapsed = VU->cycle - cyclesBefore
	//       cycle is in VU1_CYCLE_REG (x21), cyclesBefore is in x22.
	armAsm->Sub(x6, VU1_CYCLE_REG, x22);

	// elapsed is at most ~few; w6 is fine. Compare against u8 VIBackupCycles in w4.
	armAsm->Cmp(w6, w4);
	Label do_subtract;
	armAsm->B(&do_subtract, lo); // elapsed < VIBackupCycles
	// elapsed >= VIBackupCycles → store 0
	armAsm->Strb(wzr, MemOperand(VU1_BASE_REG, vibackup_off));
	armAsm->B(&skip);

	armAsm->Bind(&do_subtract);
	armAsm->Sub(w4, w4, w6);
	armAsm->Strb(w4, MemOperand(VU1_BASE_REG, vibackup_off));

	armAsm->Bind(&skip);
}

// Stage C1 inline FMAC pipeline add. Writes directly into
// &VU->fmac[fmacwritepos] and bumps fmaccount, matching the body that used
// to live in vu1_FMACAddPair. fmacwritepos itself is advanced in step 14
// (unchanged) — this helper uses the pre-advance value, same as before.
// Stage C2: sCycle is stored directly from the pinned VU1_CYCLE_REG (x21),
// eliminating the `Ldr x6, [VU1_BASE, cycle_off]` that C1 emitted.
// Stage C3: fmacwritepos is read directly from the pinned VU1_FMAC_WPOS_REG
// (x24/w24), eliminating the `Ldr w4, [VU1_BASE, fmacwpos_off]` that C1/C2
// emitted. x24's upper 32 bits are guaranteed zero by the zero-extend-on-
// 32-bit-write rule — every write to w24 in this file is a 32-bit op.
//
// Scratch: w4/x4, w5/x5, w6/x6, x7, x8. x7 holds &VU->fmac[wpos] (fmac_off
// already folded in, so fmacPipe field offsets fit in Stp/Ldp's imm7 range).
// x8 is a transient base for the Ldp of VU->statusflag+clipflag whose offset
// from VU1_BASE exceeds Ldp's imm7 range. All caller-saved per AAPCS64; no
// BL between here and the last use, so they don't need preservation.
static void emitFMACAddPair(const _VURegsNum& uregs, const _VURegsNum& lregs)
{
	const bool upperFMAC = (uregs.pipe == VUPIPE_FMAC);
	const bool lowerFMAC = (lregs.pipe == VUPIPE_FMAC);
	if (!upperFMAC && !lowerFMAC)
		return;

	const u32 regUpper    = upperFMAC ? uregs.VFwrite  : 0u;
	const u32 xyzwUpper   = upperFMAC ? uregs.VFwxyzw  : 0u;
	const u32 regLower    = lowerFMAC ? lregs.VFwrite  : 0u;
	const u32 xyzwLower   = lowerFMAC ? lregs.VFwxyzw  : 0u;
	const u32 flagregBoth = (upperFMAC ? uregs.VIwrite : 0u) |
	                        (lowerFMAC ? lregs.VIwrite : 0u);

	const int64_t fmac_off       = (int64_t)offsetof(VURegs, fmac);
	// Phase-7: macflag/statusflag/clipflag live in pinned regs w19/w20/w28
	// for the whole block, so no offsetof lookups are needed here.
	// Phase-9b: fmaccount lives in the pinned VU1_FMACCOUNT_REG (w26); no
	// memory load/store here either — the per-pair `Add` bumps the pin.
	static_assert(offsetof(fmacPipe, clipflag) == offsetof(fmacPipe, statusflag) + 4,
		"Stp(statusflag, clipflag) requires adjacent u32 layout in fmacPipe");
	static_assert(offsetof(fmacPipe, macflag) == offsetof(fmacPipe, Cycle) + 4,
		"Stp(Cycle, macflag) requires adjacent u32 layout in fmacPipe");
	static_assert(offsetof(fmacPipe, reglower) == offsetof(fmacPipe, regupper) + 4,
		"Stp(regupper, reglower) requires adjacent u32 layout in fmacPipe");
	static_assert(offsetof(fmacPipe, xyzwupper) == offsetof(fmacPipe, flagreg) + 4,
		"Stp(flagreg, xyzwupper) requires adjacent u32 layout in fmacPipe");

	const int64_t f_regupper   = (int64_t)offsetof(fmacPipe, regupper);
	const int64_t f_flagreg    = (int64_t)offsetof(fmacPipe, flagreg);
	const int64_t f_xyzwlower  = (int64_t)offsetof(fmacPipe, xyzwlower);
	const int64_t f_sCycle     = (int64_t)offsetof(fmacPipe, sCycle);
	const int64_t f_Cycle      = (int64_t)offsetof(fmacPipe, Cycle);
	const int64_t f_statusflag = (int64_t)offsetof(fmacPipe, statusflag);
	const int64_t f_clipflag   = (int64_t)offsetof(fmacPipe, clipflag);

	// x7 = &VU->fmac[wpos]. We bake fmac_off into x7 so all fmacPipe field
	// offsets (0..47) land inside Stp/Ldp's signed-7-bit×4 imm range, which
	// lets us pair adjacent u32 fields into one store each. Shift-folded
	// setup keeps this at 3 insns, same as the pre-Stp version:
	//   (wpos*3) << 4  ==  wpos*48.
	armAsm->Add(x5, x24, Operand(x24, LSL, 1));          // x5 = wpos*3
	armAsm->Add(x7, VU1_BASE_REG, Operand(x5, LSL, 4));  // x7 = VU1_BASE + wpos*48
	armAsm->Add(x7, x7, fmac_off);                        // x7 = &VU->fmac[wpos]

	// Pick source register for a compile-time u32 field — wzr when the
	// value is 0 (skips the Mov). Stp/Str accept wzr as a source.
	auto regFor = [&](u32 value, const Register& scratch) -> Register {
		if (value == 0)
			return wzr;
		armAsm->Mov(scratch, value);
		return scratch;
	};

	// fmacPipe layout: regupper(0) reglower(4) flagreg(8) xyzwupper(12)
	//                  xyzwlower(16) _pad(20) sCycle(24) Cycle(32)
	//                  macflag(36) statusflag(40) clipflag(44).
	// All-u32 adjacent pairs get Stp'd.

	// regupper + reglower → one Stp
	{
		Register r1 = regFor(regUpper, w5);
		Register r2 = regFor(regLower, w6);
		armAsm->Stp(r1, r2, MemOperand(x7, f_regupper));
	}

	// flagreg + xyzwupper → one Stp
	{
		Register r1 = regFor(flagregBoth, w5);
		Register r2 = regFor(xyzwUpper,   w6);
		armAsm->Stp(r1, r2, MemOperand(x7, f_flagreg));
	}

	// xyzwlower sits alone (next 4 bytes are padding before sCycle).
	{
		Register r = regFor(xyzwLower, w5);
		armAsm->Str(r, MemOperand(x7, f_xyzwlower));
	}

	// sCycle (u64) = VU->cycle — from the pinned VU1_CYCLE_REG (x21).
	armAsm->Str(VU1_CYCLE_REG, MemOperand(x7, f_sCycle));

	// Cycle(const 4) + macflag(from pinned w19) → one Stp. macflag is
	// always committed unconditionally by _vuFMACflush (VUops.cpp) so the
	// slot must carry a valid macflag regardless of VIwrite bits.
	armAsm->Mov(w5, 4);
	armAsm->Stp(w5, VU1_MACFLAG_REG, MemOperand(x7, f_Cycle));

	// statusflag: ALWAYS stored. _vuFMACflush (VUops.cpp:59-62) reads
	// fmac[i].statusflag in BOTH branches of its flagreg-gated if/else —
	// the else branch ORs fmac[i].statusflag's Z/S bits into
	// VI[REG_STATUS_FLAG] regardless of whether this op wrote the flag.
	// Skipping the store leaves stale bits from whatever op previously
	// occupied this ring slot, which leaks into VI[REG_STATUS_FLAG] on
	// the next flush → corrupted FSAND/FSEQ/FSOR reads → missing geometry
	// in Shadow of the Colossus (this was the bug in commit c591194b1).
	//
	// clipflag: conditionally stored. _vuFMACflush (VUops.cpp:54-55) reads
	// fmac[i].clipflag ONLY when flagreg & REG_CLIP_FLAG is set, so
	// skipping the store for ops whose flagreg doesn't have that bit (the
	// common case — only the CLIP op sets it) is safe. This was the
	// correct half of commit c591194b1; keeping that saving.
	const bool need_clip = (flagregBoth & (1u << REG_CLIP_FLAG)) != 0u;
	if (need_clip)
		armAsm->Stp(VU1_STATUSFLAG_REG, VU1_CLIPFLAG_REG, MemOperand(x7, f_statusflag));
	else
		armAsm->Str(VU1_STATUSFLAG_REG, MemOperand(x7, f_statusflag));

	// fmaccount++ — Phase-9b: bumped in the pinned VU1_FMACCOUNT_REG (w26).
	// Block-end flush + per-BL flush (around vu1_TestFMACStallReg /
	// vu1_TestPipes_VU1 / vu1Exec / vu1EbitDone / hack_xgkick / interp BLs)
	// keeps memory in sync wherever a downstream reader expects it.
	//
	// #18 deep-dive: defer the Add. emitFlushFmaccountReg drains the
	// accumulated count into w26 before the Str. Saves N-1 Adds across N
	// consecutive FMAC pairs with no flush between them.
	s_vu1_deferred_fmaccount++;
}

// Stage C1 inline pipeline add for non-FMAC lower pipes (FDIV/EFU/IALU).
// FMAC lowers are handled by emitFMACAddPair above. All field stores are
// emitted directly into VU->fdiv / VU->efu / VU->ialu[ialuwritepos], so
// there is no BL into a C helper.
//
// Stage C2: all sCycle stores write VU1_CYCLE_REG (x21) directly, skipping
// the `Ldr x4, [VU1_BASE, cycle_off]` that C1 emitted.
//
// Scratch: w4/x4, w5/x5, x6, x7. Matches emitFMACAddPair's scratch usage.
static void emitLowerNonFMACAdd(const _VURegsNum& lregs)
{
	switch (lregs.pipe)
	{
		case VUPIPE_FDIV:
			if (lregs.VIwrite & (1u << REG_Q))
			{
				const int64_t q_off          = (int64_t)offsetof(VURegs, q);
				const int64_t fdiv_off       = (int64_t)offsetof(VURegs, fdiv);
				const int64_t d_enable       = (int64_t)offsetof(fdivPipe, enable);
				const int64_t d_reg          = (int64_t)offsetof(fdivPipe, reg);
				const int64_t d_sCycle       = (int64_t)offsetof(fdivPipe, sCycle);
				const int64_t d_Cycle        = (int64_t)offsetof(fdivPipe, Cycle);
				const int64_t d_statusflag   = (int64_t)offsetof(fdivPipe, statusflag);

				// enable = 1
				armAsm->Mov(w4, 1);
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_enable));
				// sCycle (u64) = VU->cycle (cached in x21)
				armAsm->Str(VU1_CYCLE_REG, MemOperand(VU1_BASE_REG, fdiv_off + d_sCycle));
				// Cycle = lregs.cycles (compile-time)
				armAsm->Mov(w4, static_cast<u32>(lregs.cycles));
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_Cycle));
				// reg.F = VU->q.F (first 4 bytes of the REG_VI union)
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, q_off));
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_reg));
				// statusflag from pinned VU1_STATUSFLAG_REG (w20) — no memory load.
				armAsm->Str(VU1_STATUSFLAG_REG, MemOperand(VU1_BASE_REG, fdiv_off + d_statusflag));
			}
			break;

		case VUPIPE_EFU:
			if (lregs.VIwrite & (1u << REG_P))
			{
				const int64_t p_off    = (int64_t)offsetof(VURegs, p);
				const int64_t efu_off  = (int64_t)offsetof(VURegs, efu);
				const int64_t e_enable = (int64_t)offsetof(efuPipe, enable);
				const int64_t e_reg    = (int64_t)offsetof(efuPipe, reg);
				const int64_t e_sCycle = (int64_t)offsetof(efuPipe, sCycle);
				const int64_t e_Cycle  = (int64_t)offsetof(efuPipe, Cycle);

				armAsm->Mov(w4, 1);
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, efu_off + e_enable));
				armAsm->Str(VU1_CYCLE_REG, MemOperand(VU1_BASE_REG, efu_off + e_sCycle));
				armAsm->Mov(w4, static_cast<u32>(lregs.cycles));
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, efu_off + e_Cycle));
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, p_off));
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, efu_off + e_reg));
			}
			break;

		case VUPIPE_IALU:
			if (lregs.cycles != 0)
			{
				const int64_t ialu_off      = (int64_t)offsetof(VURegs, ialu);
				const int64_t ialucount_off = (int64_t)offsetof(VURegs, ialucount);
				const int64_t i_reg         = (int64_t)offsetof(ialuPipe, reg);
				const int64_t i_sCycle      = (int64_t)offsetof(ialuPipe, sCycle);
				const int64_t i_Cycle       = (int64_t)offsetof(ialuPipe, Cycle);

				// Stage C3: ialuwritepos is held live in x25/w25 —
				// x25 is the zero-extended 64-bit view (every write to
				// w25 in this file is 32-bit, which zeros the top half).
				// x5 = wpos * 24 = (wpos * 3) << 3
				armAsm->Add(x5, x25, Operand(x25, LSL, 1));
				armAsm->Lsl(x5, x5, 3);
				// x7 = VU1_BASE + wpos*24
				armAsm->Add(x7, VU1_BASE_REG, x5);

				// sCycle (u64) = VU->cycle (cached in x21)
				armAsm->Str(VU1_CYCLE_REG, MemOperand(x7, ialu_off + i_sCycle));
				// Cycle = lregs.cycles (compile-time)
				armAsm->Mov(w6, static_cast<u32>(lregs.cycles));
				armAsm->Str(w6, MemOperand(x7, ialu_off + i_Cycle));
				// reg = lregs.VIwrite (compile-time)
				armAsm->Mov(w6, lregs.VIwrite);
				armAsm->Str(w6, MemOperand(x7, ialu_off + i_reg));

				// Stage C3: ialuwritepos = (wpos + 1) & 3 — in-register,
				// no memory store. Block-end epilogue flushes x25 back.
				armAsm->Add(VU1_IALU_WPOS_REG, VU1_IALU_WPOS_REG, 1);
				armAsm->And(VU1_IALU_WPOS_REG, VU1_IALU_WPOS_REG, 3);

				// ialucount++ (stays memory-resident — bumped by us,
				// decremented by vu1_TestPipes_VU1 / vu1EbitDone).
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, ialucount_off));
				armAsm->Add(w4, w4, 1);
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, ialucount_off));
			}
			break;

		default:
			break;
	}
}

// Destroy every cached variant in every slot. Called on cache-full reset,
// Shutdown, and Reset. The compiled code buffer itself is reclaimed by the
// caller — this only frees the per-variant heap storage (snapshot + entry).
static void deleteAllVariants()
{
	for (u32 i = 0; i < VU1_NUM_SLOTS; i++)
	{
		for (VU1BlockEntry* blk : s_variants[i])
		{
			delete[] blk->snapshot;
			delete blk;
		}
		s_variants[i].clear();
		s_waitingForSlot[i].clear();
	}
}

// VU1_PROFILE_OPS scaffolding (toggle in arm64/InterpFlags.h). Begin/End
// pair captures the emit cursor before/after a per-pair section and
// registers the range with simpleperf. When the toggle is off both macros
// expand to no-ops — zero compile-time and zero runtime cost.
#ifdef VU1_PROFILE_OPS
	#define VU1_PERF_BEGIN(varname) const u8* varname = armGetCurrentCodePointer()
	#define VU1_PERF_END(varname, fmt, ...) do { \
		const u8* _vu1_pe_end = armGetCurrentCodePointer(); \
		if (_vu1_pe_end > (varname)) { \
			char _vu1_pe_name[64]; \
			std::snprintf(_vu1_pe_name, sizeof(_vu1_pe_name), fmt, ##__VA_ARGS__); \
			Perf::vu1.Register((varname), \
				static_cast<size_t>(_vu1_pe_end - (varname)), _vu1_pe_name); \
		} \
	} while (0)
#else
	#define VU1_PERF_BEGIN(varname) ((void)0)
	#define VU1_PERF_END(varname, fmt, ...) ((void)0)
#endif

static u8* CompileBlock(u32 startPC, u32 numPairs, VU1BlockEntry* out_block)
{
	// VF cache: clear the compile-time tracker before any pair emit. The
	// previous block compile leaked state into the tracker; without reset
	// here, the first FMAC of the new block would emit `Mov scratch, vN`
	// thinking some VF was already resident, but the runtime NEON state of
	// the new block has none of that.
	vfCacheReset();
	// VI cache: same lifecycle as VF cache — empty at every block compile.
	viCacheReset();
	// FMAC opt #16: broadcast operand cache is per-block — v1 is caller-saved
	// per AAPCS64, so it doesn't survive across the prior block's epilogue
	// Ret + Execute's outer dispatch back into our codeEntry prologue.
	vu1BroadcastCacheReset();
	// FMAC opt #17: same-VF different-lane batching state. Even though d10
	// is callee-saved, the deferred state must not leak across block
	// boundaries — Pass 1 only marks pairs within the current block.
	vu1BatchCacheReset();
	// #18 deep-dive: fmaccount Add-batching state. Reset per block since the
	// pinned w26 is reloaded fresh in the prologue.
	s_vu1_deferred_fmaccount = 0;

	// --- Size check ---
	const size_t data_size    = numPairs * 2 * sizeof(_VURegsNum);
	const size_t code_worst   = static_cast<size_t>(numPairs) * 512 + 64;
	const size_t total_needed = data_size + code_worst;

	if (static_cast<size_t>(s_code_end - s_code_write) < total_needed)
	{
		DEV_LOG("VU1 JIT: code buffer full, resetting");
		// The incoming out_block is brand-new, not yet registered in any
		// deque — wipe the rest of the cache and let the caller continue
		// emitting into the reclaimed buffer. out_block survives intact.
		deleteAllVariants();
		s_code_write = s_code_base;
		s_pool.Reset();
	}

	// --- Data section: pre-computed uregs/lregs for every pair ---
	// Layout: [uregs[0..N-1]] [lregs[0..N-1]] (before the code, in JIT buffer)
	u8* const data_base = s_code_write;
	_VURegsNum* const uregs_data = reinterpret_cast<_VURegsNum*>(data_base);
	_VURegsNum* const lregs_data = uregs_data + numPairs;

	// Zero the entire data section first — some regs functions don't set all
	// fields (e.g., branch regs don't set 'cycles'). The interpreter does
	// 'lregs.cycles = 0' before calling the regs function; we match that by
	// zeroing the whole array.
	std::memset(data_base, 0, data_size);

	// _VURegsNum pre-walk: decode each pair's upper/lower into the data
	// section so subsequent passes have field-level operand info without
	// re-decoding. This walk used to also accumulate block_has_* flags
	// inline; that work has moved into microIR Pass 1 below (single source
	// of truth — see iVU1IR_arm64.h).
	{
		u32 pc = startPC;
		for (u32 i = 0; i < numPairs; i++)
		{
			const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
			const u32 lower = *reinterpret_cast<const u32*>(VU1.Micro + pc);
			const bool ibit = ((upper >> 31) & 1) != 0;

			VU1.code = upper;
			VU1regs_UPPER_OPCODE[upper & 0x3f](&uregs_data[i]);

			if (!ibit)
			{
				// Non-I-bit: lower field is an instruction.
				VU1.code = lower;
				VU1regs_LOWER_OPCODE[lower >> 25](&lregs_data[i]);
			}
			// I-bit pairs: lregs_data[i] stays zeroed (no lower instruction).

			pc = (pc + 8) & (VU1_PROGSIZE - 1);
		}
	}

	// microIR Pass 1 — derive per-pair `microOp` overlay from the just-
	// populated _VURegsNum arrays. Also computes block-level summary flags
	// (ir.has_ebit / has_branch / has_dbit_or_tbit / has_ibxx / has_vi_backup_set
	// / has_xgkick) that were previously accumulated inline in the pre-walk
	// above. Single source of truth for both per-pair and block-level
	// derived state.
	//
	// Block-level flags drive per-emit gating below:
	//   has_ebit / has_dbit_or_tbit : step 13 countdown.
	//   has_branch : step 12 countdown. Mirrors the step-13 gate shape — VU1
	//                has no per-pair budget abort, so any branch set in a
	//                prior block was countdowned to 0 in that same block
	//                before exit. "VU->branch == 0 at entry" therefore holds
	//                for any block whose own pre-walk sees no VUPIPE_BRANCH
	//                lower. Hazard fallback is invariant-preserving: vu1Exec
	//                interprets the whole pair (countdown included), so the
	//                native body resumes with coherent state.
	//   has_ibxx : only IBxx reads VIBackupCycles via emitHazardVIRead
	//              (JR/JALR skip the hazard path despite `VIread != 0`).
	//              Opcodes per VUops.cpp LOWER_OPCODE[128]: IBEQ=0x28,
	//              IBNE=0x29, IBLTZ=0x2C, IBGTZ=0x2D, IBLEZ=0x2E, IBGEZ=0x2F.
	//   has_vi_backup_set : any lower whose emitter calls emitBackupVI
	//              (IADD/ISUB/IALU-imm/IAND/IOR/LQD/LQI/SQD/SQI/ILWR/MTIR).
	//              Overapproximated as "writes VI[0..15] and pipe != BRANCH"
	//              — that includes FSAND/FMAND/FCAND/FCGET (flag-test ops
	//              that write VI but never touch VIBackupCycles). False
	//              positives only keep step 6b's decrement emits, so the
	//              overapproximation is soundness-safe. BAL/JALR are the
	//              intentional exclusions (write VI without emitBackupVI).
	armvu1ir::microOp ir_info[VU1_MAX_BLOCK_PAIRS];
	armvu1ir::microIR ir;
	ir.info = ir_info;
	armvu1ir::mvu1AnalyzeBlock(startPC, numPairs, uregs_data, lregs_data, ir);

	// Step 6b (VIBackupCycles decrement) is observable only when some pair
	// in the block reads VIBackupCycles — i.e., has an IBxx. If no IBxx,
	// the per-pair decrement is dead within this block; the only concern
	// is cross-block state leaking. Two cases:
	//
	//   (1) !ir.has_ibxx && !ir.has_vi_backup_set:
	//       No writes in this block either. Entry VIBackupCycles is at most
	//       2 (max value set by emitBackupVI). numPairs >= 2 (AnalyzeBlock
	//       always includes a delay-slot pair, and stalls can only increase
	//       elapsed cycles — never shrink them), so the natural per-pair
	//       decrement would reach 0 before block exit. Eliding the
	//       decrements and clamping VIBackupCycles to 0 at block end is
	//       equivalent behavior for any downstream block.
	//
	//   (2) !ir.has_ibxx && ir.has_vi_backup_set:
	//       In-block write sets VIBackupCycles=2 at some pair. A clamp-to-0
	//       at exit would drop a still-live hazard for the next block's
	//       IBxx. Keep the per-pair decrement.
	//
	// So we elide only in case (1).
	const bool skip_vibackup_decrement = !ir.has_ibxx && !ir.has_vi_backup_set;

	// --- Flag-deferral analysis ---
	// For each FMAC pair, determine whether its MAC/STATUS flag updates are
	// observable. Two reasons to keep them:
	//   (a) Some later same-block pair reads MAC/STATUS/CLIP via FMxxx/FSxxx/
	//       FCxxx (detected via lregs.VIread bits).
	//   (b) The pair is one of the LAST 4 FMAC ops in the block — the FMAC
	//       pipe has 4 slots and ~4-cycle latency, so these writes have not
	//       reached VI[FLAG] before the block ends; the next block's
	//       _vuTestPipes will flush them.
	//
	// When neither holds, the FMAC arithmetic emitters skip BL vu1_fmac_writeback
	// entirely and emit a NEON clamp + store instead — typically 5-7 instructions
	// instead of a function call doing per-lane flag math.
	//
	// Audit item #7 — vuFlagHack speedhack honoring: this elision is exactly
	// what the upstream microVU vuFlagHack toggle controls. When the user
	// disables the speedhack (`EmuConfig.Speedhacks.vuFlagHack == false`),
	// they're asking for exact flag computation regardless of observability.
	// We honor that by forcing pair_needs_flags[i] = true for every pair.
	// When the toggle is on (default), the elision logic runs as before.
	bool pair_needs_flags[VU1_MAX_BLOCK_PAIRS];
	const bool flagHackOn = EmuConfig.Speedhacks.vuFlagHack;
	// block_reads_uo: any in-block op reads MAC_FLAG or STATUS_FLAG. Used to
	// gate emitFmacInlineWriteback's U/O ladder. CLIP_FLAG is excluded —
	// FCxxx ops read CLIP, never MAC/STATUS, so they don't pull U/O.
	bool block_reads_uo = false;
	{
		constexpr u32 FLAG_READ_MASK = (1u << REG_MAC_FLAG)
		                              | (1u << REG_STATUS_FLAG)
		                              | (1u << REG_CLIP_FLAG);
		constexpr u32 UO_READ_MASK = (1u << REG_MAC_FLAG)
		                            | (1u << REG_STATUS_FLAG);
		bool sawFlagReader = false; // any pair > current reads flags
		int  fmacFromEnd   = 0;     // count of FMAC pairs at indices > current
		for (int i = static_cast<int>(numPairs) - 1; i >= 0; i--)
		{
			const _VURegsNum& uregs = uregs_data[i];
			const _VURegsNum& lregs = lregs_data[i];
			const bool isFmacPair = (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC);

			bool needsFlags = false;
			if (isFmacPair)
			{
				if (!flagHackOn || fmacFromEnd < 4 || sawFlagReader)
					needsFlags = true;
				fmacFromEnd++;
			}
			pair_needs_flags[i] = needsFlags;

			const u32 readsCombined = (uregs.VIread | lregs.VIread);
			// Update sawFlagReader for the NEXT (earlier) iteration. The
			// current pair's own flag read does not pull its own flag write
			// — pipe latency means a same-pair FMxxx reads VI[FLAG] from
			// 4+ cycles ago, not the upper FMAC's just-now-written value.
			if (readsCombined & FLAG_READ_MASK)
				sawFlagReader = true;
			if (readsCombined & UO_READ_MASK)
				block_reads_uo = true;
		}
	}

	// Set the U/O computation gate once per block. True when:
	//   - vuFlagHack off (exact mode forces full computation), OR
	//   - any in-block op reads MAC/STATUS (FMxxx/FSxxx + CFC2 of vi16/vi17), OR
	//   - the overflow gamefix is on (clamp path needs the inf mask).
	// Cross-block readers of MAC/STATUS get the same OLD-equivalent semantic
	// gap upstream's CHECK_VUOVERFLOWHACK gating produces (rare; matches
	// upstream's compatibility tradeoff).
	g_vu1NeedsUOFlags = !flagHackOn || block_reads_uo || CHECK_VU_OVERFLOW(1);

	// Precompute link_info here for the exit-selector emit below. An earlier
	// version also used this to gate step 2 (TPC write) per pair, but the
	// gating caused missing geometry (BIOS menu pillars / memcard icons)
	// that couldn't be root-caused to a specific reader within the audit
	// window. Step 2 now emits unconditionally. link_info's computation and
	// placement here are kept because the exit selector still needs it.
	const BlockLinkExits link_info = computeBlockLinkExits(startPC, numPairs);

	// --- Compile-time pipeline state tracking (Stages A+B) ---
	// Pre-walk the block to decide which stall-check / TestPipes BLs can be
	// proven unnecessary at compile time. Tracks the four VU pipes:
	//   FMAC (4-slot ring, Cycle=4 fixed)
	//   IALU (4-slot ring, per-slot Cycle)
	//   FDIV (single slot, per-slot Cycle, max 13)
	//   EFU  (single slot, per-slot Cycle, max 54)
	//
	// Soundness: ct_cycle advances by exactly 1 per pair (no stall-induced
	// bumps). Runtime cycle can only LEAD ours (stalls advance runtime but
	// not our model), so for any slot, (runtime_cycle - runtime_sCycle) >=
	// (ct_cycle - ct_sCycle). "Slot absent in our model" implies "slot
	// absent at runtime" — elision is one-way safe.
	//
	// Carry-in: runtime's rings may hold entries at block entry that our
	// model can't see. Each pipe has a "carry-in gate" — ct_cycle threshold
	// past which all possible carry-in is guaranteed retired:
	//   FMAC: > 3  (max Cycle=4, latest delta-sCycle=-1 matures at ct_cycle=3)
	//   IALU: > 3  (max Cycle=4)
	//   FDIV: > 12 (max Cycle=13)
	//   EFU : > 54 (max Cycle=54 for EATAN family)
	// Elision only fires once the relevant gate is cleared.
	struct CTFmacSlot
	{
		u8 regupper, xyzwupper;
		u8 reglower, xyzwlower;
		int sCycle;
		bool valid;
		// Phase 1 (microVU static-stall port): track which VI flag registers
		// this FMAC slot will commit (mirrors fmacPipe.flagreg in VU.h —
		// runtime _vuTestPipes commits macflag/statusflag/clipflag based on
		// these bits).
		u32 flagreg;
		// Maturity cycle = sCycle + Cycle. For FMAC slots this is always 4
		// per VUops.cpp:350 (`VU->fmac[i].Cycle = 4`); kept as a field for
		// future variant tracking.
		int Cycle;
	};
	CTFmacSlot ct_fmac[4] = {};
	int ct_fmac_wpos = 0, ct_fmac_rpos = 0, ct_fmac_count = 0;

	struct CTIaluSlot
	{
		u32 reg;    // VIwrite bits
		int sCycle;
		int cycles;
		bool valid;
	};
	CTIaluSlot ct_ialu[4] = {};
	int ct_ialu_wpos = 0, ct_ialu_rpos = 0, ct_ialu_count = 0;

	bool ct_fdiv_pending = false;
	int  ct_fdiv_sCycle  = 0;
	int  ct_fdiv_cycles  = 0;

	bool ct_efu_pending = false;
	int  ct_efu_sCycle  = 0;
	int  ct_efu_cycles  = 0;

	constexpr int CARRY_IN_GATE_FMAC = 3;
	constexpr int CARRY_IN_GATE_IALU = 3;
	constexpr int CARRY_IN_GATE_FDIV = 12;
	constexpr int CARRY_IN_GATE_EFU  = 54;

	struct PerPairSkip
	{
		bool skipUpperFMACStall0;
		bool skipUpperFMACStall1;
		bool skipLowerFMACStall0;
		bool skipLowerFMACStall1;
		bool skipLowerFDIVWait;
		bool skipLowerEFUWait;
		bool skipLowerALUStall;
		bool skipTestPipes;
		// Phase 1 (microVU static-stall port) — compute-only fields filled
		// in by the per-pair static-stall block after skipTestPipes is set.
		//
		// `fmac_stall` = cycles the runtime FMAC stall helpers
		// (`vu1_TestFMACStallReg{,2}`) would add to VU->cycle on top of the
		// natural per-pair `cycle++`. Computed from FMAC alias scans only —
		// these helpers have no side effects beyond the cycle bump, so the
		// delta can be replaced with a single inline Add at the JIT site
		// under Phase 2's VU1_STATIC_STALL_EMIT toggle.
		//
		// `pipe_stall` = cycles the FDIV/EFU/IALU stall helpers would add on
		// top of `fmac_stall`. These helpers have side effects
		// (`vu1_TestEFUPipeWait` decrements `efu.Cycle`) so they CANNOT be
		// replaced with a bare Add — Phase 2 leaves those BLs in place.
		//
		// `static_stall = fmac_stall + pipe_stall`, used to bump our
		// compile-time `ct_cycle` so subsequent slot adds + retirement
		// see the same cycle the runtime will at this point in the block.
		int  fmac_stall;
		int  pipe_stall;
		int  static_stall;
		// True when this pair reads MAC/CLIP/STATUS or REG_Q/REG_P AND
		// there is an in-flight FMAC slot whose `flagreg` matches the
		// read (or a pending FDIV/EFU completion). Held here for
		// diagnostic use; Phase 2 itself doesn't elide TestPipes BLs.
		bool mustCommitForFlagReader;
		// Phase 2 (VU1_STATIC_STALL_EMIT) safety gate. True only when
		// `fmac_carry_safe` is true at this pair (`ct_cycle > 3`), meaning
		// any cross-block FMAC carry-in slot the CT model can't see is
		// guaranteed mature at runtime — runtime FMAC stall helper would
		// `continue` past it without bumping cycle. Within that window,
		// CT's `fmac_stall` is the authoritative stall value and the
		// FMAC stall BL can be suppressed in favour of an inline Add.
		//
		// Outside that window (carry-unsafe pairs at block entry), the
		// runtime ring may have non-mature carry-in slots the CT misses.
		// Suppressing the BL there leads to under-counted stalls — this
		// was the GoW graphical-glitch root cause. So in carry-unsafe
		// pairs the BL stays, no inline Add is emitted, and Phase 2
		// reverts to upstream behaviour for that pair.
		bool useStaticFmacStall;
	};
	PerPairSkip skip_info[VU1_MAX_BLOCK_PAIRS] = {};

	// Stage A+B pre-walk (re-enabled after root-causing the Crazy Taxi bug
	// from commit 9a68eba8).
	//
	// Original bug: `skipTestPipes` relied on a maturity check using
	// `ct_delta` (pair-indexed, no stall tracking). Runtime cycle can be
	// LARGER than ct_cycle due to stall tests bumping runtime_cycle, which
	// means a slot with `ct_delta < Cycle` (our model: not mature) can
	// have `runtime_delta >= Cycle` (runtime: hardware-mature). Skipping
	// `_vuTestPipes` in that window leaves the mature slot unfed through
	// `_vuFMACflush` / `_vuFDIVflush` / `_vuEFUflush` — so VI[FLAG_*] /
	// VI[REG_Q] / VI[REG_P] don't get committed. Games that read those
	// (FCAND/FCGET/FMxxx/FSxxx, MFIR from Q/P, ADDq, MULq) then see stale
	// values.
	//
	// Fix: `skipTestPipes` now requires ALL pipe rings to be empty in our
	// model AND all carry-in gates cleared. That rules out the staleness
	// window entirely — if nothing's in our ring, nothing's pending at
	// runtime that could have matured stall-bumped either (by the same
	// monotonicity argument that underpins stall-check soundness).
	//
	// Stall-check skips (skipUpperFMACStall0/1, skipLowerFMACStall0/1,
	// skipLowerFDIVWait, skipLowerEFUWait, skipLowerALUStall) remain
	// active — they're sound. Their soundness argument is DIFFERENT:
	// they're based on "no matching slot in our ring" not "no mature
	// slot". Runtime's stall tests inline `if (delta >= Cycle) continue`
	// which treats stall-matured slots as retired WITHOUT needing
	// TestPipes to run. So our ring ⊇ runtime's effective (non-retired-
	// by-inline) ring, and "no match in our ring" → "no match in
	// runtime's effective ring" → safe to skip.
	{
		int ct_cycle = 0;
		u32 pc_walk = startPC;
		for (u32 i = 0; i < numPairs; i++)
		{
			const u32 upper = *reinterpret_cast<const u32*>(VU1.Micro + pc_walk + 4);
			const bool ibit = (upper >> 31) & 1;
			const _VURegsNum& uregs = uregs_data[i];
			const _VURegsNum& lregs = lregs_data[i];

			// step 1: cycle++
			ct_cycle++;

			const bool fmac_carry_safe = (ct_cycle > CARRY_IN_GATE_FMAC);
			const bool ialu_carry_safe = (ct_cycle > CARRY_IN_GATE_IALU);
			const bool fdiv_carry_safe = (ct_cycle > CARRY_IN_GATE_FDIV);
			const bool efu_carry_safe  = (ct_cycle > CARRY_IN_GATE_EFU);

			auto aliasFmac = [&](u8 reg, u8 xyzw) -> bool {
				if (reg == 0)
					return false;
				int idx = ct_fmac_rpos;
				for (int n = 0; n < ct_fmac_count; n++)
				{
					const CTFmacSlot& slot = ct_fmac[idx];
					if (slot.valid)
					{
						if (slot.regupper == reg && (slot.xyzwupper & xyzw))
							return true;
						if (slot.reglower == reg && (slot.xyzwlower & xyzw))
							return true;
					}
					idx = (idx + 1) & 3;
				}
				return false;
			};

			// Phase 1 (microVU static-stall port): find max maturity cycle
			// (slot.sCycle + slot.Cycle) over all FMAC slots aliased by this
			// (reg, xyzw) read. Returns 0 if no match. Mirrors the runtime
			// `vu1_TestFMACStallReg` body, which bumps VU->cycle to
			// max(cycle, sCycle+Cycle) per match — the max-over-matches is
			// the post-helper cycle. Caller subtracts the pre-helper cycle
			// to get the stall delta.
			auto aliasFmacMaxReady = [&](u8 reg, u8 xyzw) -> int {
				if (reg == 0)
					return 0;
				int max_ready = 0;
				int idx = ct_fmac_rpos;
				for (int n = 0; n < ct_fmac_count; n++)
				{
					const CTFmacSlot& slot = ct_fmac[idx];
					if (slot.valid)
					{
						const bool hit_upper = (slot.regupper == reg && (slot.xyzwupper & xyzw));
						const bool hit_lower = (slot.reglower == reg && (slot.xyzwlower & xyzw));
						if (hit_upper || hit_lower)
						{
							const int ready = slot.sCycle + slot.Cycle;
							if (ready > max_ready)
								max_ready = ready;
						}
					}
					idx = (idx + 1) & 3;
				}
				return max_ready;
			};

			// True if any in-flight FMAC slot's flagreg bits intersect mask.
			auto fmacHasPendingFlagBits = [&](u32 mask) -> bool {
				if (mask == 0)
					return false;
				int idx = ct_fmac_rpos;
				for (int n = 0; n < ct_fmac_count; n++)
				{
					const CTFmacSlot& slot = ct_fmac[idx];
					if (slot.valid && (slot.flagreg & mask) != 0)
						return true;
					idx = (idx + 1) & 3;
				}
				return false;
			};

			auto aliasIalu = [&](u32 VIread) -> bool {
				if (VIread == 0)
					return false;
				int idx = ct_ialu_rpos;
				for (int n = 0; n < ct_ialu_count; n++)
				{
					const CTIaluSlot& slot = ct_ialu[idx];
					if (slot.valid && (slot.reg & VIread))
						return true;
					idx = (idx + 1) & 3;
				}
				return false;
			};

			// step 5 upper stalls — FMAC only
			if (fmac_carry_safe && uregs.pipe == VUPIPE_FMAC)
			{
				skip_info[i].skipUpperFMACStall0 = !aliasFmac(uregs.VFread0, uregs.VFr0xyzw);
				skip_info[i].skipUpperFMACStall1 = !aliasFmac(uregs.VFread1, uregs.VFr1xyzw);
			}

			// step 5b lower stalls — FMAC/FDIV/EFU do FMAC checks first, plus
			// their respective wait helpers. BRANCH does an ALU stall check.
			if (!ibit)
			{
				const bool lowerDoesFMACCheck =
					(lregs.pipe == VUPIPE_FMAC) ||
					(lregs.pipe == VUPIPE_FDIV) ||
					(lregs.pipe == VUPIPE_EFU);
				if (lowerDoesFMACCheck && fmac_carry_safe)
				{
					skip_info[i].skipLowerFMACStall0 = !aliasFmac(lregs.VFread0, lregs.VFr0xyzw);
					skip_info[i].skipLowerFMACStall1 = !aliasFmac(lregs.VFread1, lregs.VFr1xyzw);
				}

				switch (lregs.pipe)
				{
					case VUPIPE_FDIV:
						// Elide wait BL when FDIV definitely not pending:
						// no in-block add AND carry-in gate cleared.
						skip_info[i].skipLowerFDIVWait = !ct_fdiv_pending && fdiv_carry_safe;
						break;
					case VUPIPE_EFU:
						// vu1_TestEFUPipeWait has a mandatory `efu.Cycle -= 1`
						// side effect when enable!=0, so elision is only safe
						// when we know enable=0 for certain (no in-block add
						// AND carry-in retired — gate 54).
						skip_info[i].skipLowerEFUWait = !ct_efu_pending && efu_carry_safe;
						break;
					case VUPIPE_BRANCH:
						if (lregs.VIread != 0 && ialu_carry_safe)
							skip_info[i].skipLowerALUStall = !aliasIalu(lregs.VIread);
						break;
					default:
						break;
				}
			}

			// step 6 TestPipes: decide elision. Must require ALL pipe rings
			// empty in our model AND all carry-in gates cleared — see the
			// pre-walk header comment for the soundness argument. Checking
			// maturity (the old approach) is UNSAFE: stall bumps can make
			// runtime slots mature ahead of our model, and those slots have
			// VI-visible writes (flag/Q/P) that need _vuTestPipes to commit.
			skip_info[i].skipTestPipes =
				ct_fmac_count == 0   && fmac_carry_safe
				&& !ct_fdiv_pending  && fdiv_carry_safe
				&& !ct_efu_pending   && efu_carry_safe
				&& ct_ialu_count == 0 && ialu_carry_safe;

			// ----------------------------------------------------------------
			// Phase 1 (microVU static-stall port): compute the stall this
			// pair's runtime stall-checks would add, and split into the
			// side-effect-free FMAC portion (`fmac_stall`, replaceable with
			// an inline Add under Phase 2 VU1_STATIC_STALL_EMIT) and the
			// pipe portion (`pipe_stall`, FDIV/EFU/IALU — BLs kept because
			// of side effects). Also compute `mustCommitForFlagReader` for
			// future Phase-3 TestPipes elision decisions (currently held
			// for diagnostics only).
			{
				// FMAC stall scan (no side effects, JIT-inlineable).
				int fmac_candidate = ct_cycle;
				if (uregs.pipe == VUPIPE_FMAC)
				{
					if (!skip_info[i].skipUpperFMACStall0 && uregs.VFread0 != 0)
					{
						const int r = aliasFmacMaxReady(uregs.VFread0, uregs.VFr0xyzw);
						if (r > fmac_candidate) fmac_candidate = r;
					}
					if (!skip_info[i].skipUpperFMACStall1 && uregs.VFread1 != 0)
					{
						const int r = aliasFmacMaxReady(uregs.VFread1, uregs.VFr1xyzw);
						if (r > fmac_candidate) fmac_candidate = r;
					}
				}
				if (!ibit)
				{
					const bool lowerDoesFMACCheck =
						(lregs.pipe == VUPIPE_FMAC) ||
						(lregs.pipe == VUPIPE_FDIV) ||
						(lregs.pipe == VUPIPE_EFU);
					if (lowerDoesFMACCheck)
					{
						if (!skip_info[i].skipLowerFMACStall0 && lregs.VFread0 != 0)
						{
							const int r = aliasFmacMaxReady(lregs.VFread0, lregs.VFr0xyzw);
							if (r > fmac_candidate) fmac_candidate = r;
						}
						if (!skip_info[i].skipLowerFMACStall1 && lregs.VFread1 != 0)
						{
							const int r = aliasFmacMaxReady(lregs.VFread1, lregs.VFr1xyzw);
							if (r > fmac_candidate) fmac_candidate = r;
						}
					}
				}

				// Pipe stall scan (FDIV/EFU/IALU — BLs kept due to side
				// effects). Tracked here for CT cycle modelling.
				int pipe_candidate = fmac_candidate;
				if (!ibit)
				{
					if (lregs.pipe == VUPIPE_FDIV && ct_fdiv_pending
						&& !skip_info[i].skipLowerFDIVWait)
					{
						const int r = ct_fdiv_sCycle + ct_fdiv_cycles;
						if (r > pipe_candidate) pipe_candidate = r;
					}
					if (lregs.pipe == VUPIPE_EFU && ct_efu_pending
						&& !skip_info[i].skipLowerEFUWait)
					{
						// vu1_TestEFUPipeWait does `efu.Cycle -= 1` before
						// comparing, per VUops.cpp:269.
						const int r = ct_efu_sCycle + ct_efu_cycles - 1;
						if (r > pipe_candidate) pipe_candidate = r;
					}
					if (lregs.pipe == VUPIPE_BRANCH && lregs.VIread != 0
						&& !skip_info[i].skipLowerALUStall)
					{
						int idx = ct_ialu_rpos;
						for (int n = 0; n < ct_ialu_count; n++)
						{
							const CTIaluSlot& slot = ct_ialu[idx];
							if (slot.valid && (slot.reg & lregs.VIread))
							{
								const int r = slot.sCycle + slot.cycles;
								if (r > pipe_candidate) pipe_candidate = r;
							}
							idx = (idx + 1) & 3;
						}
					}
				}

				const int candidate_cycle = pipe_candidate;
				skip_info[i].fmac_stall   = fmac_candidate - ct_cycle;
				skip_info[i].pipe_stall   = pipe_candidate - fmac_candidate;
				skip_info[i].static_stall = candidate_cycle - ct_cycle;

				// Detect flag-committing sink. Pair reads MAC/CLIP/STATUS or
				// Q/P AND there's a pending slot/pipe that would commit
				// that value via vu1_TestPipes_VU1.
				const u32 STATUS_BIT = 1u << REG_STATUS_FLAG;
				const u32 MAC_BIT    = 1u << REG_MAC_FLAG;
				const u32 CLIP_BIT   = 1u << REG_CLIP_FLAG;
				const u32 Q_BIT      = 1u << REG_Q;
				const u32 P_BIT      = 1u << REG_P;
				const u32 flag_mask  = STATUS_BIT | MAC_BIT | CLIP_BIT;
				const u32 reads_flag = (uregs.VIread | (ibit ? 0 : lregs.VIread))
					& flag_mask;
				const u32 reads_qp   = (uregs.VIread | (ibit ? 0 : lregs.VIread))
					& (Q_BIT | P_BIT);
				bool must_commit = false;
				if (reads_flag && fmacHasPendingFlagBits(reads_flag))
					must_commit = true;
				if ((reads_qp & Q_BIT) && ct_fdiv_pending)
					must_commit = true;
				if ((reads_qp & P_BIT) && ct_efu_pending)
					must_commit = true;
				skip_info[i].mustCommitForFlagReader = must_commit;

				// Phase 2 safety gate. The carry-safe gate `ct_cycle > 3`
				// guarantees any cross-block carry-in FMAC slot the CT
				// model can't see is mature at runtime (the helper would
				// `continue` past it). Within that window CT's
				// `fmac_stall` is sound and we can inline the Add and
				// suppress the BL. Outside that window the BL stays.
				skip_info[i].useStaticFmacStall = fmac_carry_safe;

				// Intentionally DO NOT mutate `ct_cycle` here. An earlier
				// version of this block did `ct_cycle = candidate_cycle` to
				// keep our CT model "in lockstep with runtime after stall
				// bumps." That broke the pre-walk's retire/skipTestPipes
				// soundness: shadow-verify caught a VI[REG_STATUS_FLAG]
				// divergence at pc=0x0648 (slot at fmac[1] with flagreg &
				// REG_STATUS_FLAG, statusflag=0xC0). The bumped ct_cycle
				// caused the retire loop to pop the slot in the CT model
				// before the runtime had retired it, which made
				// skipTestPipes evaluate true → BL elided → flagreg never
				// committed → JIT VI[16] stuck at pre-pair 0x40 while
				// interp landed on 0xC0.
				//
				// fmac_stall is a DELTA (slot.sCycle + 4 − ct_cycle); both
				// terms are in pair-count space (Phase 1 doesn't bump
				// either), so the delta is the same as if we'd bumped
				// both. Phase 2's inline `Add VU1_CYCLE_REG, ..., fmac_stall`
				// correctly accounts for the stall at runtime. Keeping
				// ct_cycle in pair-count space leaves the CT retire/
				// skipTestPipes decisions conservative (slot stays in our
				// ring until 4 pairs have passed, matching the
				// pre-Phase-1 strict criterion), which is the soundness
				// argument the original skipTestPipes design rested on.
				(void)candidate_cycle; // computed for fmac_stall/pipe_stall above
			}

			// Retire mature slots in the CT model.
			while (ct_fmac_count > 0)
			{
				CTFmacSlot& head = ct_fmac[ct_fmac_rpos];
				if (!head.valid || (ct_cycle - head.sCycle) < 4)
					break;
				head.valid = false;
				ct_fmac_rpos = (ct_fmac_rpos + 1) & 3;
				ct_fmac_count--;
			}
			while (ct_ialu_count > 0)
			{
				CTIaluSlot& head = ct_ialu[ct_ialu_rpos];
				if (!head.valid || (ct_cycle - head.sCycle) < head.cycles)
					break;
				head.valid = false;
				ct_ialu_rpos = (ct_ialu_rpos + 1) & 3;
				ct_ialu_count--;
			}
			if (ct_fdiv_pending && (ct_cycle - ct_fdiv_sCycle) >= ct_fdiv_cycles)
				ct_fdiv_pending = false;
			if (ct_efu_pending && (ct_cycle - ct_efu_sCycle) >= ct_efu_cycles)
				ct_efu_pending = false;

			// step 11 adds
			{
				const bool uFMAC = (uregs.pipe == VUPIPE_FMAC);
				const bool lFMAC = !ibit && (lregs.pipe == VUPIPE_FMAC);
				if (uFMAC || lFMAC)
				{
					CTFmacSlot& slot = ct_fmac[ct_fmac_wpos];
					slot.regupper  = uFMAC ? uregs.VFwrite : 0;
					slot.xyzwupper = uFMAC ? uregs.VFwxyzw : 0;
					slot.reglower  = lFMAC ? lregs.VFwrite : 0;
					slot.xyzwlower = lFMAC ? lregs.VFwxyzw : 0;
					slot.sCycle    = ct_cycle;
					slot.Cycle     = 4; // VUops.cpp:350 — FMAC pipe = 4 cycles
					// flagreg union mirrors VUops.cpp:356/362 layering
					// (upper VIwrite | lower VIwrite).
					slot.flagreg   = (uFMAC ? uregs.VIwrite : 0)
						| (lFMAC ? lregs.VIwrite : 0);
					slot.valid     = true;
					ct_fmac_wpos = (ct_fmac_wpos + 1) & 3;
					if (ct_fmac_count < 4)
						ct_fmac_count++;
				}
			}

			if (!ibit)
			{
				switch (lregs.pipe)
				{
					case VUPIPE_FDIV:
						if (lregs.VIwrite & (1u << REG_Q))
						{
							ct_fdiv_pending = true;
							ct_fdiv_sCycle  = ct_cycle;
							ct_fdiv_cycles  = lregs.cycles;
						}
						break;
					case VUPIPE_EFU:
						if (lregs.VIwrite & (1u << REG_P))
						{
							ct_efu_pending = true;
							ct_efu_sCycle  = ct_cycle;
							ct_efu_cycles  = lregs.cycles;
						}
						break;
					case VUPIPE_IALU:
						if (lregs.cycles != 0)
						{
							CTIaluSlot& slot = ct_ialu[ct_ialu_wpos];
							slot.reg    = lregs.VIwrite;
							slot.sCycle = ct_cycle;
							slot.cycles = lregs.cycles;
							slot.valid  = true;
							ct_ialu_wpos = (ct_ialu_wpos + 1) & 3;
							if (ct_ialu_count < 4)
								ct_ialu_count++;
						}
						break;
					default:
						break;
				}
			}

			pc_walk = (pc_walk + 8) & (VU1_PROGSIZE - 1);
		}
	}

	// Code section starts after data, 4-byte aligned.
	u8* code_start = data_base + data_size;
	code_start = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(code_start) + 3) & ~3ULL);

	armSetAsmPtr(code_start, static_cast<size_t>(s_code_end - code_start), &s_pool);
	u8* const entry = armStartBlock();

	// Forward-declared label for the cycle-budget check at linkEntry — bound
	// just before the register-flush path at block end so a budget-exceeded
	// entry skips past the per-pair body AND the exit selector.
	Label budget_exceeded_exit;

	// --- Prologue: save callee-saved regs, pin VU1_BASE_REG = &VU1 ---
	// 96-byte frame (Phase-7 expanded from 80 — adds x19/x20/x28 pinning
	// for macflag/statusflag/clipflag):
	//   [sp+0..7]   = x29 (fp)
	//   [sp+8..15]  = x30 (lr)
	//   [sp+16..23] = x21 (VU1_CYCLE_REG — Stage C2 cached VU->cycle)
	//   [sp+24..31] = x22 (cyclesBefore scratch)
	//   [sp+32..39] = x23 (VU1_BASE_REG)
	//   [sp+40..47] = x24 (VU1_FMAC_WPOS_REG — Stage C3 cached fmacwritepos)
	//   [sp+48..55] = x25 (VU1_IALU_WPOS_REG — Stage C3 cached ialuwritepos)
	//   [sp+56..63] = x26 (VU1_FMACCOUNT_REG — Phase-9b cached VU->fmaccount;
	//                      formerly cycle-limit gate addr, dropped to free x26)
	//   [sp+64..71] = x27 (VU1_TERM_ADDR_REG — opt #1 pinned gate addr)
	//   [sp+72..79] = x19 (VU1_MACFLAG_REG    — Phase-7 cached VU->macflag)
	//   [sp+80..87] = x20 (VU1_STATUSFLAG_REG — Phase-7 cached VU->statusflag)
	//   [sp+88..95] = x28 (VU1_CLIPFLAG_REG   — Phase-7 cached VU->clipflag)
	armAsm->Stp(x29, x30, MemOperand(sp, -96, PreIndex));
	armAsm->Stp(VU1_CYCLE_REG, x22, MemOperand(sp, 16));
	armAsm->Stp(VU1_BASE_REG, x24, MemOperand(sp, 32));
	armAsm->Stp(x25, x26, MemOperand(sp, 48));
	armAsm->Stp(VU1_TERM_ADDR_REG, x19, MemOperand(sp, 64));
	armAsm->Stp(x20, x28, MemOperand(sp, 80));
	armAsm->Mov(x29, sp);
	armMoveAddressToReg(VU1_BASE_REG, &VU1);
	// Opt #1: pin the termination gate address. Loaded once per block;
	// every codeEntry+linkEntry pair amortizes the cost across all linked
	// entries within the chain. The cycle-limit half was dropped in
	// Phase-9b; that address now materializes fresh inside the gate.
	if (THREAD_VU1)
		armMoveAddressToReg(VU1_TERM_ADDR_REG, &s_vu1_program_ended);
	else
		armMoveAddressToReg(VU1_TERM_ADDR_REG, &VU0);

	// Compile-time constants for field offsets used throughout the loop.
	const int64_t cycle_off      = (int64_t)offsetof(VURegs, cycle);
	const int64_t code_off       = (int64_t)offsetof(VURegs, code);
	const int64_t branch_off     = (int64_t)offsetof(VURegs, branch);
	const int64_t branchpc_off   = (int64_t)offsetof(VURegs, branchpc);
	const int64_t ebit_off       = (int64_t)offsetof(VURegs, ebit);
	const int64_t tpc_off        = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_TPC * (int64_t)sizeof(REG_VI));
	const int64_t regi_off       = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_I   * (int64_t)sizeof(REG_VI));
	const int64_t fmacwpos_off   = (int64_t)offsetof(VURegs, fmacwritepos);
	const int64_t ialuwpos_off   = (int64_t)offsetof(VURegs, ialuwritepos);
	const int64_t fmaccount_off  = (int64_t)offsetof(VURegs, fmaccount);
	const int64_t vibackup_off   = (int64_t)offsetof(VURegs, VIBackupCycles);
	const int64_t macflag_off    = (int64_t)offsetof(VURegs, macflag);
	const int64_t statusflag_off = (int64_t)offsetof(VURegs, statusflag);
	const int64_t clipflag_off   = (int64_t)offsetof(VURegs, clipflag);
	const int64_t acc_off        = (int64_t)offsetof(VURegs, ACC);
	const int64_t micro_off      = (int64_t)offsetof(VURegs, Micro);

	// IbitHack forces per-op immediate decode from live micro memory (mirrors
	// x86 microVU's ptr32[&curI] reads). When on, VU->code is loaded from
	// VU->Micro[pc] at runtime instead of the JIT-baked instruction word so
	// subsequent native ops + C wrappers pick up any post-compile patches.
	// Natively-emitted IADDI/IADDIU/ISUBIU/LQ/SQ/ILW/ISW and PC-relative
	// branches (B/BAL/IBxx) consult EmuConfig directly and emit runtime-decode
	// paths of their own. Matches the VU0 Lower D-3 fix.
	const bool use_ibit_hack = EmuConfig.Gamefixes.IbitHack;

	// Stage C2: prime the pinned cycle register from memory. Every subsequent
	// step 1 in the per-pair loop bumps x21 in place and does NOT store back
	// to memory; the block-end flush (pre-epilogue) writes it out once.
	armAsm->Ldr(VU1_CYCLE_REG, MemOperand(VU1_BASE_REG, cycle_off));

	// Stage C3: prime the pinned FMAC/IALU write-position registers from
	// memory. Every FMAC-pipe pair reads x24 directly for the slot address
	// math (no per-pair memory load) and step 14 bumps w24 in place without
	// touching memory. Same for w25 / IALU. The block-end flush (pre-
	// epilogue) writes both back in a single Str pair.
	emitReloadWposRegs(fmacwpos_off, ialuwpos_off);

	// Phase-9b: prime the pinned VU->fmaccount register. emitFMACAddPair
	// bumps w26 in place every FMAC pair; flushed before BLs that read
	// fmaccount and reloaded after BLs that may decrement it.
	emitReloadFmaccountReg(fmaccount_off);

	// Phase-7: prime the pinned flag registers from memory. emitFMACAddPair
	// reads all three per pair (captures into the fmac pipe slot) and the
	// FMAC arith writeback + FDIV/SQRT/RSQRT + FSSET/FCSET/CLIP read/write
	// them — all now routed through pinned regs instead of memory.
	emitReloadFlagRegs(macflag_off, statusflag_off, clipflag_off);

	// Phase-8: prime the pinned ACC register from memory. Every FMAC
	// transform chain reads+writes ACC 4× — pinning gives us 1 Ldr here
	// and 1 Str at epilogue instead of 8 memory ops per chain.
	emitReloadAccReg(acc_off);

	// Block-linking (Phase 1+): record the address of the first instruction
	// past the prologue. Linked predecessors B here directly, skipping the
	// prologue. At this point x21/x23/x24/x25 are live — caller's regs
	// trusted (same ABI as the fall-through from codeEntry above).
	out_block->linkEntry = armGetCurrentCodePointer();

	// VU1_PROFILE_BLOCKS: bump per-block exec counter. Placed at linkEntry so
	// both the prologue fall-through (first dispatch) and direct-linked
	// predecessor B's get counted. x4/x5 are scratch here — the entry gate
	// below uses x5, so we pick the same pair (both are caller-saved and
	// unused across the gate's Ldr/Cmp/B sequence).
	// Guarded by the VU1_PROFILE_BLOCKS #define in arm64/InterpFlags.h — the
	// entire emit disappears in shipping builds.
#ifdef VU1_PROFILE_BLOCKS
	{
		armMoveAddressToReg(x4, &out_block->execCount);
		armAsm->Ldr(x5, MemOperand(x4));
		armAsm->Add(x5, x5, 1);
		armAsm->Str(x5, MemOperand(x4));
	}
#endif

	// Entry-gate checks (Phase 3 + Phase 5). Every block entry — whether
	// via the codeEntry fall-through after prologue, or via a direct `B`
	// from a predecessor's linked exit — runs through this gate BEFORE
	// the per-pair body. Two failure modes jump to budget_exceeded_exit
	// which tail-falls to flushes+epilogue+Ret, returning to Execute's
	// outer loop.
	//
	//   1. Cycle budget. Tight VU1 loops (IBNE loop:, B -8) now link
	//      their conditional taken-exit back to the block's own
	//      linkEntry — if we don't yield when the per-Execute cycle
	//      budget is exhausted, a loop with no ebit runs forever.
	//      Compare cached cycle (x21) against s_vu1_cycle_limit (set by
	//      recArmVU1::Execute on entry).
	//
	//   2. Termination. External termination signals (FBRST reset from
	//      EE thread under non-MTVU; vu1EbitDone's s_vu1_program_ended
	//      under MTVU) need to interrupt linked-chain execution —
	//      otherwise we'd keep running blocks until cycle budget runs
	//      out, potentially thousands of wasted cycles. Moving the
	//      check here means linked chains respect the same termination
	//      rules as Execute's outer loop did pre-Phase-5.
	//
	//      The check flavor is chosen at compile time from THREAD_VU1:
	//        - MTVU  : read s_vu1_program_ended (set by vu1EbitDone)
	//        - !MTVU : test VU0.VI[REG_VPU_STAT] bit 0x100 (clear =
	//                  VU1 stopped; cleared by vu1EbitDone or FBRST reset)
	//
	//      Matches the old `stopped` computation in Execute's while body.
	//
	// Opt #1: termination address is pre-pinned in VU1_TERM_ADDR_REG (x27)
	// by the prologue. The cycle-limit address half was dropped in Phase-9b
	// to free x26 for VU1_FMACCOUNT_REG; materialize fresh in scratch x5
	// here (+1 insn per gate execution; amortized across all linked-chain
	// entries — net win when the block has any FMAC pairs).
	{
		// 1. Cycle budget.
		// VUSyncHack honoring (gamefix #15): when set, fire the gate if the
		// upcoming block WOULD overshoot the limit, instead of only when we
		// already have. Mirrors x86 microVU_Compile.inl:481-484 — that path
		// does `eax = cycles - block_size` then jumps if negative; we
		// equivalently compare `current + numPairs >= limit`. numPairs is a
		// safe upper bound on the block's actual cycle cost (1 cycle/pair
		// + stalls; block size bounded by VU1_MAX_BLOCK_PAIRS = 256, well
		// within Add's 12-bit immediate range). FullVU0SyncHack is VU0-
		// specific and intentionally ignored here.
		armMoveAddressToReg(x5, &s_vu1_cycle_limit);
		armAsm->Ldr(x5, MemOperand(x5));
		if (EmuConfig.Gamefixes.VUSyncHack)
		{
			armAsm->Add(x6, VU1_CYCLE_REG, numPairs);
			armAsm->Cmp(x6, x5);
		}
		else
		{
			armAsm->Cmp(VU1_CYCLE_REG, x5);
		}
		armAsm->B(&budget_exceeded_exit, hs);

		// 2. Termination.
		if (THREAD_VU1)
		{
			armAsm->Ldrb(w5, MemOperand(VU1_TERM_ADDR_REG));
			armAsm->Cbnz(w5, &budget_exceeded_exit);
		}
		else
		{
			const int64_t vpu_stat_off = (int64_t)offsetof(VURegs, VI)
				+ REG_VPU_STAT * (int64_t)sizeof(REG_VI);
			armAsm->Ldr(w5, MemOperand(VU1_TERM_ADDR_REG, vpu_stat_off));
			armAsm->Tst(w5, 0x100);
			armAsm->B(&budget_exceeded_exit, eq);
		}
	}

	// CHECK_XGKICKHACK (C-1): read once at block compile and bake into the
	// emitted code. recArmVU1::Reset() flushes all variants on gamefix
	// toggle (via VMManager::ApplySettings), so a block's hackmode binding
	// is stable for the block's cached lifetime.
	//
	// Under hackmode the scratch-based pending_xgkick_fire mechanism is
	// disabled entirely — both JIT and interp agree on VU1.xgkick* state
	// management (see the C-1 comment block in iVU1Lower_arm64.cpp), so
	// the step 8a / step 15 / block-end / hazard-capture-from-interp
	// paths below are all guarded with `!xgkickhack`.
	const bool xgkickhack = CHECK_XGKICKHACK;

	// Hackmode pre-walk: compute per-pair kickcycles (cycles to sync into
	// the paced XGKICK transfer at this pair's memwrite boundary).
	// Accumulates `1 + mVUstall` per pair post-XGKICK, commits + resets
	// on memwrite pairs. Mirrors mVUregs.xgkickcycles / mVUlow.kickcycles
	// accumulation at microVU_Compile.inl:779-786.
	//
	// mVUstall is reconstructed by simulating upstream's pipeline counters
	// (mVUregs.VF[reg].{x,y,z,w}, mVUregs.VI[reg], mVUregs.q, mVUregs.p).
	// Matches upstream analyzeReg1 / analyzeVIreg1 / analyzeQreg / analyzePreg
	// + mVUincCycles + mVUsetCycles behaviour at cycle granularity:
	//
	//   - FMAC writes set a VF lane to 4 (analyzeReg2).
	//   - Any VI write (lower IALU + LQ/SQ autoincrement + branch link)
	//     installs 1-cycle latency (matches analyzeVIreg2(..., 1) at
	//     every upstream call site).  The interp's _vuRegs* sets
	//     _VURegsNum.cycles=0 for IALU, so we can't use that field —
	//     we install 1 unconditionally for any op whose VIwrite hits a
	//     real VI slot (bits 0..15; flag bits 16+ are masked out).
	//   - FDIV writes Q with _VURegsNum.cycles (7 for DIV/SQRT, 13 for RSQRT).
	//   - EFU writes P with _VURegsNum.cycles (12..54 depending on op).
	//   - Reads stall on max remaining cycles across read lanes /
	//     VI slots / pipelines.  P-read stall is (p - 1), matching
	//     analyzePreg's off-by-one.
	//   - xgkick pipeline (analyzeXGkick1) skipped: only active under
	//     !CHECK_XGKICKHACK which disables this pre-walk entirely.
	//   - R pipeline skipped: analyzeRreg never contributes to mVUstall
	//     (it only stages mVUregsTemp.r, never reads mVUregs.r).
	u32 kick_cycles_sync[VU1_MAX_BLOCK_PAIRS] = {};
	if (xgkickhack)
	{
		// xyzw bit layout (matches VUops.cpp:25 _XYZW): bit 3=X, bit 2=Y,
		// bit 1=Z, bit 0=W. Per-lane index for our pl_vf is natural 0..3
		// (X/Y/Z/W), so lane index = 3 - bit_position.
		u8 pl_vf[32][4] = {};
		u8 pl_vi[16]    = {};
		u8 pl_q         = 0;
		u8 pl_p         = 0;

		auto decLaneArr = [](u8* arr, u32 n, u32 len) {
			for (u32 k = 0; k < len; k++)
				arr[k] = (arr[k] > n) ? static_cast<u8>(arr[k] - n) : 0;
		};
		auto decU8 = [](u8& v, u32 n) {
			v = (v > n) ? static_cast<u8>(v - n) : 0;
		};

		u32 accum = 0;
		u32 pc_walk = startPC;
		for (u32 i = 0; i < numPairs; i++)
		{
			const u32 upper_w = *reinterpret_cast<const u32*>(VU1.Micro + pc_walk + 4);
			const u32 lower_w = *reinterpret_cast<const u32*>(VU1.Micro + pc_walk);
			const bool ibit_w = (upper_w >> 31) & 1;
			const _VURegsNum& uregs_i = uregs_data[i];
			const _VURegsNum& lregs_i = lregs_data[i];

			// Step 1: baseline +1 cycle decrement (mVUincCycles(mVU, 1)).
			decLaneArr(&pl_vf[0][0], 1, 32 * 4);
			decLaneArr(&pl_vi[0],    1, 16);
			decU8(pl_q, 1);
			decU8(pl_p, 1);

			// Step 2: compute mVUstall from this pair's reads. Upper is
			// always decoded (even on I-bit pairs, since upper still runs
			// — the I-bit only suppresses the lower slot). Lower is only
			// decoded when !ibit.
			u32 stall = 0;
			auto readVF = [&](u8 reg, u8 xyzw) {
				if (reg == 0 || xyzw == 0)
					return;
				for (int b = 0; b < 4; b++)
				{
					if (xyzw & (1u << b))
					{
						const int lane = 3 - b;
						if (pl_vf[reg][lane] > stall)
							stall = pl_vf[reg][lane];
					}
				}
			};
			auto readVI = [&](u32 mask) {
				mask &= 0xFFFFu;
				while (mask)
				{
					const u32 r = __builtin_ctz(mask);
					if (pl_vi[r] > stall)
						stall = pl_vi[r];
					mask &= mask - 1;
				}
			};

			readVF(uregs_i.VFread0, uregs_i.VFr0xyzw);
			readVF(uregs_i.VFread1, uregs_i.VFr1xyzw);
			readVI(uregs_i.VIread);

			if (!ibit_w)
			{
				readVF(lregs_i.VFread0, lregs_i.VFr0xyzw);
				readVF(lregs_i.VFread1, lregs_i.VFr1xyzw);
				readVI(lregs_i.VIread);

				// Q read stall (analyzeQreg): FDIV ops back-to-back.
				// analyzeQreg is only emitted by mVUanalyzeFDIV, and FDIV
				// ops always have pipe==VUPIPE_FDIV.  Upper FMAC ops that
				// use Q as src (ADDq/MADDq/...) do NOT call analyzeQreg in
				// upstream x86 — matching that (slight inaccuracy but
				// upstream-truth).
				if (lregs_i.pipe == VUPIPE_FDIV && pl_q > stall)
					stall = pl_q;

				// P read stall (analyzePreg): EFU ops back-to-back.  The
				// analyzePreg macro uses `(p ? p - 1 : 0)` — off by one
				// vs Q/VF stalls (upstream quirk).
				if (lregs_i.pipe == VUPIPE_EFU && pl_p > 0)
				{
					const u32 p_stall = pl_p - 1u;
					if (p_stall > stall)
						stall = p_stall;
				}
			}

			// Step 3: advance by stall (mVUsetCycles -> mVUincCycles(stall)).
			if (stall)
			{
				decLaneArr(&pl_vf[0][0], stall, 32 * 4);
				decLaneArr(&pl_vi[0],    stall, 16);
				decU8(pl_q, stall);
				decU8(pl_p, stall);
			}

			// Step 4: install writes from this pair.  tCycles(old, new)
			// = max(old, new); since we just decremented to 0 on any
			// lane/slot being written, a straight "set if larger" suffices.
			auto writeVF = [&](u8 reg, u8 xyzw, u8 cyc) {
				if (reg == 0 || xyzw == 0)
					return;
				for (int b = 0; b < 4; b++)
				{
					if (xyzw & (1u << b))
					{
						const int lane = 3 - b;
						if (pl_vf[reg][lane] < cyc)
							pl_vf[reg][lane] = cyc;
					}
				}
			};
			auto writeVI = [&](u32 mask, u8 cyc) {
				if (cyc == 0)
					return;
				mask &= 0xFFFFu;
				while (mask)
				{
					const u32 r = __builtin_ctz(mask);
					if (pl_vi[r] < cyc)
						pl_vi[r] = cyc;
					mask &= mask - 1;
				}
			};

			// Upper FMAC VF writes: 4-cycle pipeline latency.
			if (uregs_i.pipe == VUPIPE_FMAC)
				writeVF(uregs_i.VFwrite, uregs_i.VFwxyzw, 4);

			if (!ibit_w)
			{
				// Lower VF writes (LQ/LQI/LQD/MOVE/MR32/MFIR family, all
				// pipe==VUPIPE_FMAC in _vuRegs*): same 4-cycle latency.
				if (lregs_i.pipe == VUPIPE_FMAC)
					writeVF(lregs_i.VFwrite, lregs_i.VFwxyzw, 4);

				// Lower VI writes: upstream analyzeVIreg2 always uses
				// aCycles=1, regardless of opcode.  Install for every
				// op that sets VIwrite on a real VI slot (0..15).
				// Covers IALU (IADD/ISUB/IADDI/IAND/IOR), LQI/LQD/SQI/SQD
				// autoincrement, MTIR, branch link registers.
				// FMAC ops that set VIwrite for flag bits (REG_ACC_FLAG=16+)
				// get filtered by writeVI's `mask & 0xFFFFu`.
				if (lregs_i.VIwrite)
					writeVI(lregs_i.VIwrite, 1);

				// Q write (FDIV): _VURegsNum.cycles holds latency (7/13).
				if (lregs_i.pipe == VUPIPE_FDIV && lregs_i.cycles > 0)
				{
					const u8 cyc = static_cast<u8>(lregs_i.cycles);
					if (pl_q < cyc)
						pl_q = cyc;
				}

				// P write (EFU): _VURegsNum.cycles holds latency (12..54).
				if (lregs_i.pipe == VUPIPE_EFU && lregs_i.cycles > 0)
				{
					const u8 cyc = static_cast<u8>(lregs_i.cycles);
					if (pl_p < cyc)
						pl_p = cyc;
				}
			}

			// Step 5: accumulate per-pair kickcycles (microVU_Compile.inl:781).
			if (!ibit_w && isXgkickOp(lower_w))
			{
				// XGKICK: reset to 1 (VUops.cpp:1934 — kick itself is 1 cycle).
				accum = 1;
			}
			else if (!ibit_w && isMemWriteOp(lower_w))
			{
				accum += 1 + stall;
				kick_cycles_sync[i] = accum;
				accum = 0;
			}
			else
			{
				accum += 1 + stall;
			}

			pc_walk = (pc_walk + 8) & (VU1_PROGSIZE - 1);
		}

		// Block-end residual commit (Bug #2 — matches microVU_Compile.inl
		// :812-816 / :835-839 / :845-849). When block ends on a non-memwrite
		// pair, upstream commits the running xgkickcycles onto the last
		// pair's mVUlow.kickcycles so the 2nd-pass sync (line 897) drains
		// them before the block exits. Without this, cycles accumulated
		// after the last in-block memwrite are dropped at block end and
		// the next block's pre-walk restarts at 0 — starving the paced
		// XGKICK of drain budget across block boundaries for patterns
		// like `XGKICK; <arith>; branch` (common in Crash Twinsanity's
		// render loop). Folding into the last pair's existing sync site
		// keeps the drain ordered before the exit selector, which mirrors
		// upstream's before-exec placement.
		if (accum > 0 && numPairs > 0)
			kick_cycles_sync[numPairs - 1] += accum;
	}

	// --- Per-pair code emission ---
	// XGKICK cycle-delay tracking (mirrors microVU mVUinfo.doXGKICK).
	// When a pair captures an XGKICK (vu1_XGKICK stashes the addr in
	// s_vu1_pending_xgkick_addr), the *next* pair fires the deferred
	// transfer AFTER its own opcodes so any store on that pair has
	// committed before GIF walks VU1.Mem. If the next pair is itself an
	// XGKICK, the prior kick is fired *before* that pair's lower emit
	// (see step 8a), so pair k's kick always reaches GIF before pair k+1
	// overwrites the scratch with its own captured addr.
	//
	// Unused when xgkickhack=true — see pre-walk above.
	bool pending_xgkick_fire = false;

	// Track whether the previous pair executed a branch op — feeds the
	// "is this pair a branch delay slot?" predicate for D/T bit suppression
	// at step 11b. Mirrors mVUinfo.isBdelay in x86 microVU_Compile.inl:901.
	// Same pattern as the VU0 C-4 fix.
	bool prev_was_branch = false;

	// Tracks whether the previous pair had its E-bit set — feeds the
	// "branch in E-bit delay slot?" predicate for branch suppression in
	// the lower emit path. Mirrors x86 microVU_Compile.inl branchWarning
	// which sets mVUlow.isNOP when mVUup.eBit && mVUbranch. Same pattern
	// as the VU0 C-5 fix.
	bool prev_was_ebit = false;

#if defined(VU1_BLOCK_SHADOW_VERIFY) && defined(VU1_SHADOW_VERIFY)
	// Block-level shadow snapshot. Emitted after the linkEntry gate (so
	// budget-aborted entries don't snapshot) and before the per-pair body.
	// Captures the pre-block state. Companion verify call lands right
	// before the epilogue Ldp/Ret — by then vfCacheFlushAndInvalidate has
	// committed all deferred VF writes to memory, so the snapshot-post is
	// JIT's authoritative end-of-block state.
	//
	// Skip if any pair in the block is an XGKICK — the interp re-run would
	// duplicate GS packet writes (same constraint as per-pair). Detected
	// at compile time via ir.info[].isKick.
	bool block_has_xgkick = false;
	for (u32 ki = 0; ki < numPairs; ki++)
	{
		if (ir.info[ki].isKick) { block_has_xgkick = true; break; }
	}
	if (!block_has_xgkick)
	{
		// Flush pinned VU1 state + caches so the snapshot reads coherent
		// memory. Mirrors the per-pair shadow snapshot site (line ~4970).
		emitFlushCycleReg(cycle_off);
		emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
		emitFlushFmaccountReg(fmaccount_off);
		emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
		emitFlushAccReg(acc_off);
		vfCacheFlushAndInvalidate();
		viCacheInvalidateAll();
		armEmitCall(reinterpret_cast<const void*>(vu1_block_shadow_snapshot));
	}
#endif

	u32 pc = startPC;
	for (u32 i = 0; i < numPairs; i++)
	{
		// Per-pair info is now sourced from microIR Pass 1: `ir_op` is the
		// authoritative pre-decoded view of this pair (raw upper/lower words,
		// e/i/m/t/d bits, branch kind, hazard summaries). The `upper` /
		// `lower` / `ibit` / etc. locals below alias into ir_op so the
		// existing emit body doesn't need to be retouched line-by-line.
		const armvu1ir::microOp& ir_op = ir.info[i];
		const u32  upper    = ir_op.upper;
		const u32  lower    = ir_op.lower;
		const bool ibit     = ir_op.iBit;
		const bool ebit_set = ir_op.eBit;
		const bool dbit_set = ir_op.dBit;
		const bool tbit_set = ir_op.tBit;
		const _VURegsNum& uregs = uregs_data[i];
		const _VURegsNum& lregs = lregs_data[i];

		// Hoisted up-front — consumed by both step 8 branch-in-ebit-delay
		// suppression and step 11b D/T branch-context suppression. Equivalent
		// to the original `!ibit && (lregs.pipe == VUPIPE_BRANCH)` test:
		// Pass 1's classifyBranch returns BR_NONE for any I-bit pair (which
		// has no lower instruction) and for non-branch lower opcodes.
		const bool branch_pipe = ir_op.branch != armvu1ir::BR_NONE;

		// Detect every VF/VI hazard that _vu1Exec (VU1microInterp.cpp:108-163)
		// resolves via save/restore or discard. The native machinery does
		// neither, so all four cases must fall back to vu1Exec:
		//
		//   VF: upper writes vfX, lower also writes vfX        -> discard lower
		//   VF: upper writes vfX, lower reads  vfX             -> save/restore VF
		//   CLIP: upper writes CLIP, lower writes CLIP         -> discard lower
		//   CLIP: upper writes CLIP, lower reads  CLIP         -> save/restore CLIP
		//
		// The TPC at this point already equals `pc` (set by the previous pair),
		// so vu1Exec can run directly without adjustment.
		//
		// Without the discard cases, the JIT runs upper then lower
		// sequentially and lower's write silently clobbers upper's FMAC
		// result whenever both target the same VF.
		//
		// Hazard detection moved to microIR Pass 1 (mvu1AnalyzeBlock). Single
		// source of truth lets follow-on optimizations land at the IR layer
		// instead of duplicating the gate here.
		//
		// doSwapOp native fast-path (audit item #2): vf_read_after_write
		// pairs no longer fall back to vu1Exec. Native emit runs upper-then-
		// lower; lower reads the just-written vfX value (lanes that overlap)
		// instead of the original. This DIVERGES from vu1Exec's save/restore
		// semantics but matches the old port-in-place's behavior — that port
		// also emits upper-then-lower without backup for non-flag-reader
		// lowers, accepting the same divergence. The IR's XYZW-aware
		// refinement (Pass 1) ensures we only divert pairs where the lanes
		// actually conflict; lane-disjoint pairs never reached the fallback.
		//
		// vf_write_collision still falls back: native emit would have lower
		// CLOBBER upper's write (different from both vu1Exec's "discard
		// lower" and old port's "noWriteVF/isNOP suppress"). Supporting it
		// natively requires the lower emitter to skip its VF writeback —
		// invasive change deferred to a later pass.
		//
		// CLIP cases (clip_*) still fall back: CLIP has no XYZW lanes (single
		// 24-bit flag), and an XGKICK with _Is_==REG_CLIP_FLAG (vi18) gets
		// caught by clip_read_after_write — keeping that on the fallback
		// path preserves the existing XGKICK-from-interp capture handling.
		//
		// Guard kept as a static const so the divert can be toggled in one
		// place if a regression turns up.
		//
		// 2026-04-25: was reverted to false after GoW2 menu showed FMAC
		// pipeline divergence with naive upper-then-lower (lower read
		// upper's just-written vfX). Re-enabled with the deferred-
		// writeback path below: upper's FMAC computes v5 + updates pinned
		// flag regs but SPILLS v5 to g_vu1DeferredVfStash instead of
		// committing to VF[X]; lower then emits and reads original VF[X]
		// from memory/broadcast-cache; after lower we reload from stash
		// and call vfCacheStore — matches interp's save/restore semantics.
		// Eliminates the BL-into-vu1Exec roundtrip for transform-heavy
		// pairs (MUL/MADD upper writes vfX, SQ/MTIR/MR32 lower reads vfX).
		static constexpr bool kAllowReadAfterWriteNative = true;
		const bool vf_hazard = ir_op.vf_write_collision ||
			(!kAllowReadAfterWriteNative && ir_op.vf_read_after_write);
		const bool vi_hazard = ir_op.clip_write_collision || ir_op.clip_read_after_write;
		// Pair is eligible for the deferred-writeback fast path iff the only
		// hazard flag set is vf_read_after_write. write_collision still falls
		// back (lower must NOT write VF[X] — would clobber upper's deferred
		// result), and CLIP hazards have separate (rarer) semantics.
		// ACC writers and dead writes don't need deferral (no VF[X] write to
		// commit) — those follow the naive native path and the writeback emit
		// itself skips the stash via the ACC/dead-VF early returns.
		const bool defer_vf_writeback = kAllowReadAfterWriteNative
			&& ir_op.vf_read_after_write
			&& !ir_op.vf_write_collision
			&& !vi_hazard
			&& !ir_op.isKick;

		if (vf_hazard || vi_hazard)
		{
			// Full interpreter fallback for this pair. vu1Exec runs a complete
			// interpreter pair, including the _vuTest*/_vuAdd* pipeline helpers
			// which read AND write VU->cycle — flush x21 first, reload after.
			// Stage C3: vu1Exec's inner driver loop (_vu1Exec in
			// VU1microInterp.cpp) also advances fmacwritepos AND _vuAddIALUStalls
			// advances ialuwritepos, so x24/x25 must be flushed+reloaded
			// across this BL too.
			// Phase-7: the interp pair may also write VU->macflag / statusflag
			// (via VU_MAC_UPDATE / VU_STAT_UPDATE) and VU->clipflag (if a CLIP
			// upper is being interpreted), so flush+reload the pinned flag
			// regs (w19/w20/w28) around the BL too.
			// Phase-8: the interp pair may read AND write VU->ACC (any FMAC
			// upper running through interp will update it), so flush q16 in
			// and reload q16 out across the BL.
			emitFlushCycleReg(cycle_off);
			emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			// Phase-9b: vu1Exec runs a full pair through interp, which may
			// call _vuTestPipes / _vuFMACflush and mutate fmaccount.
			emitFlushFmaccountReg(fmaccount_off);
			emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitFlushAccReg(acc_off);
			armAsm->Mov(x0, VU1_BASE_REG);
			emitVu1Call(reinterpret_cast<const void*>(vu1Exec));
			emitReloadCycleReg(cycle_off);
			emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
			emitReloadFmaccountReg(fmaccount_off);
			emitReloadFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitReloadAccReg(acc_off);
			// Non-hack path only: honor pending XGKICK fire from prior pair
			// and translate interp-captured XGKICK state into the JIT's
			// scratch. Under xgkickhack, VU1.xgkick* is managed by both
			// interp and JIT directly — the interp's _vuXGKICK that runs
			// inside vu1Exec already set xgkickenable, and our per-pair
			// sync ticks below will advance the paced transfer. No
			// translation or scratch-firing needed.
			//
			// vf_hazard can only fire when lower reads/writes a VF that
			// upper wrote, and XGKICK has VFwrite=VFread0=VFread1=0, so
			// vf_hazard pairs are never XGKICK. vi_hazard, however, can
			// fire when lower reads CLIP (= VI18) and upper writes CLIP;
			// XGKICK's _vuRegsXGKICK sets VIread = (1 << _Is_), so an
			// XGKICK with _Is_ == REG_CLIP_FLAG (vi18) matches. Rare but
			// legal — handled below under non-hack mode.
			if (!xgkickhack)
			{
				if (pending_xgkick_fire)
				{
					armAsm->Mov(x0, VU1_BASE_REG);
					emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
					pending_xgkick_fire = false;
				}
				// If this pair is an XGKICK that reached the interp via the
				// vi_hazard path, vu1Exec's _vuXGKICK left VU1.xgkickenable=true
				// + xgkickaddr + xgkickcyclecount=1 + VPU_STAT bit 12 set.
				// None of the non-hack JIT paths manage xgkickenable;
				// leaving it set would trip a later hazard fallback's
				// _vuTestPipes into the broken _vuXGKICKTransfer
				// (iVU1Lower_arm64.cpp:540-548). Translate into the JIT's
				// pending-fire scratch and clear the interp-side state.
				// The actual kick fires one pair later via step 15 (or
				// step 8a on back-to-back), matching the 1-pair delay
				// semantics used by the normal XGKICK capture path.
				if (ir_op.isKick)
				{
					armAsm->Mov(x0, VU1_BASE_REG);
					emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_capture_from_interp));
					pending_xgkick_fire = true;
				}
			}
			pc = (pc + 8) & (VU1_PROGSIZE - 1);
			continue;
		}

#ifdef VU1_SHADOW_VERIFY
		// Snapshot VU1 state before the native pair runs. Skipped for XGKICK
		// pairs (re-running interp would duplicate GIF packet writes); the
		// matching verify hook below applies the same gate. THREAD_VU1
		// runtime check lives inside vu1_shadow_snapshot itself.
		if (!ir_op.isKick)
		{
			// Flush ALL pinned VU1 state to memory BEFORE the snapshot —
			// the snapshot helper reads VU1 fields via memcpy from memory.
			// Pinned regs (cycle x21, fmac/ialu wpos x24/x25, fmaccount w26,
			// flags w19/w20/w28, ACC q16) are stale in memory until flushed.
			// Without these flushes, the snapshot captures pre-pair state
			// from STALE memory: the verify-side interp run starts from
			// outdated values (e.g. cycle off by N from the real RCYCLE),
			// and the JIT vs interp compare reports a phantom diff that
			// only reflects the snapshot staleness, not real JIT behavior.
			//
			// Mirror the verify site's flush set (line ~5301). VF (NEON
			// cache) and VI (GPR cache) also flushed because prior pairs'
			// writes may still be deferred in q-regs / w9..w15.
			emitFlushCycleReg(cycle_off);
			emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			emitFlushFmaccountReg(fmaccount_off);
			emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitFlushAccReg(acc_off);
			vfCacheFlushAndInvalidate();
			viCacheInvalidateAll();
			armEmitCall(reinterpret_cast<const void*>(vu1_shadow_snapshot));
			// No reloads — pinned regs (x19-x28, q8-q15) are callee-saved
			// per AAPCS64, BL preserves them. Memory is in sync now too.
		}
#endif

		// 1. VU->cycle++ — Stage C2 uses the cached VU1_CYCLE_REG (x21).
		//    x22 latches "cycle before this pair" for the VIBackupCycles
		//    decrement at step 6b. Both x21 and x22 are callee-saved and
		//    already saved/restored in our prologue/epilogue. No memory
		//    store here; the block-end flush writes x21 to VU->cycle once.
		//
		//    The latch into x22 is dead code when step 6b is elided block-
		//    wide (skip_vibackup_decrement: no IBxx reader and no
		//    emitBackupVI-triggering writer). Gating on the same flag saves
		//    1 insn per pair across every block that doesn't carry a
		//    VIBackupCycles dependency.
		if (!skip_vibackup_decrement)
			armAsm->Mov(x22, VU1_CYCLE_REG);
		armAsm->Add(VU1_CYCLE_REG, VU1_CYCLE_REG, 1);

		// 2. Advance TPC to next pair (compile-time constant).
		// Emitted every pair. An earlier optimization attempted to gate
		// this on "pair_needs_tpc[i]" (next pair is hazard / last pair of
		// conditional-or-indirect block / max-size block) but caused
		// missing geometry in BIOS menu rendering that couldn't be
		// root-caused to a specific TPC reader within the audit window.
		// The 2 insns/pair cost is small, so revert to unconditional
		// emission until a repro pins down the missed reader.
		{
			const u32 new_tpc = (pc + 8) & VU1_PROGMASK;
			armAsm->Mov(w4, new_tpc);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, tpc_off));
		}

		// 3. E-bit: set VU->ebit = 2 (bit 30 of upper — compile-time known).
		if (ebit_set)
		{
			armAsm->Mov(w4, 2u);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, ebit_off));
		}

		// 5. Test upper stalls — compile-time-specialized inline. Most upper
		//    instructions are non-FMAC and emit zero work here. Stage A uses
		//    skip_info[i] to elide FMAC stall-check BLs when the compile-time
		//    ring buffer proves no alias exists.
		VU1_PERF_BEGIN(_pp_s5);
		emitTestUpperStalls(uregs,
			skip_info[i].skipUpperFMACStall0,
			skip_info[i].skipUpperFMACStall1);
		VU1_PERF_END(_pp_s5, "VU1_TestUpper_0x%04x", pc);

		// 5b. Test lower stalls BEFORE TestPipes (non-I-bit only).
		//     TestLowerStalls may advance VU->cycle (FDIV/EFU/ALU stalls);
		//     TestPipes needs to see the updated cycle to flush FMAC correctly.
		//     Stage B adds FDIV/EFU/ALU wait skip flags.
		VU1_PERF_BEGIN(_pp_s5b);
		if (!ibit)
			emitTestLowerStalls(lregs,
				skip_info[i].skipLowerFMACStall0,
				skip_info[i].skipLowerFMACStall1,
				skip_info[i].skipLowerFDIVWait,
				skip_info[i].skipLowerEFUWait,
				skip_info[i].skipLowerALUStall);
		VU1_PERF_END(_pp_s5b, "VU1_TestLower_0x%04x", pc);

		// === SH2 / VI[16] divergence probe at pc=0x0648 (2026-05-06) ===
		// Compile-time: log the pre-walk's skipTestPipes decision + the
		// stall-skip flags + lregs/uregs pipe classifications.
		// Runtime: BL into vu1_probe_pc0x648 to dump fmac[fmacreadpos] state
		// at the moment the JIT would otherwise gate TestPipes. If the
		// runtime slot has matured but skipTestPipes is true, we have the
		// smoking gun for the skipTestPipes-unsoundness class.
		// Disabled. Leave the helper in place for quick re-enable when
		// chasing a similar pipe-state divergence — flip to `if 1` to fire.
#if 0
		if (pc == 0x0648u)
		{
			static bool s_logged_compile = false;
			if (!s_logged_compile)
			{
				s_logged_compile = true;
				Console.WriteLn("VU1 PROBE pc=0x0648 COMPILE-TIME: "
					"skipTestPipes=%d skipLowerFMACStall0=%d skipLowerFMACStall1=%d "
					"skipLowerFDIVWait=%d skipLowerEFUWait=%d skipLowerALUStall=%d "
					"skipUpperFMACStall0=%d skipUpperFMACStall1=%d "
					"uregs.pipe=%u lregs.pipe=%u "
					"upper=0x%08x lower=0x%08x "
					"ir.has_branch=%d ir.has_ebit=%d ir_op.iBit=%d ir_op.isKick=%d "
					"numPairs=%u i=%u startPC=0x%04x",
					(int)skip_info[i].skipTestPipes,
					(int)skip_info[i].skipLowerFMACStall0,
					(int)skip_info[i].skipLowerFMACStall1,
					(int)skip_info[i].skipLowerFDIVWait,
					(int)skip_info[i].skipLowerEFUWait,
					(int)skip_info[i].skipLowerALUStall,
					(int)skip_info[i].skipUpperFMACStall0,
					(int)skip_info[i].skipUpperFMACStall1,
					(u32)uregs.pipe, (u32)lregs.pipe,
					upper, lower,
					(int)ir.has_branch, (int)ir.has_ebit,
					(int)ir_op.iBit, (int)ir_op.isKick,
					numPairs, i, startPC);
			}
			// Runtime probe — full pinned-reg flush/reload around the BL.
			// q16 (ACC) and the flag regs are caller-saved per AAPCS64; even
			// though our helper is read-only, the C++ vfprintf path in
			// Console.WriteLn may clobber them via varargs handling.
			emitFlushCycleReg(cycle_off);
			emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			emitFlushFmaccountReg(fmaccount_off);
			emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitFlushAccReg(acc_off);
			armEmitCall(reinterpret_cast<const void*>(vu1_probe_pc0x648));
			emitReloadCycleReg(cycle_off);
			emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
			emitReloadFmaccountReg(fmaccount_off);
			emitReloadFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitReloadAccReg(acc_off);
		}
#endif

		// 6. Test pipes (after lower stalls for non-I-bit). Uses the VU1-
		//    specialized helper that skips the XGKICK block and the do-while
		//    retry loop — see vu1_TestPipes_VU1 definition above. Stage B
		//    elides the BL entirely when the pre-walk proved nothing matures
		//    at this pair AND all pipes' carry-in gates have cleared. Stage
		//    C2 flushes the cached cycle register before the BL so the
		//    helper's flush checks read the up-to-date value; it does not
		//    write cycle so no reload is needed.
		if (!skip_info[i].skipTestPipes)
		{
			VU1_PERF_BEGIN(_pp_s6);
			// Refactored: cycle (x21) AND fmaccount (w26) passed as args
			// (x2 / w1) → no Str+Ldr round-trip through memory. Helper
			// returns new fmaccount in w0; cycle is read-only so no reload.
			emitDrainFmaccountReg();

			// Runtime fast-path gate. simpleperf (Futurama main menu,
			// 2026-05-07) showed `vu1_TestPipes_VU1` at 31.94% of total
			// CPU on MTVU — the existing all-empty gate (fmaccount==0
			// AND others==0) almost never fires on FMAC-busy code, so
			// every pair was paying the BL + helper-prologue cost.
			//
			// Two-tier gate:
			//   1. All four counters zero → skip BL.
			//   2. fmaccount > 0 AND fdiv/efu/ialu all zero → check FMAC
			//      front slot maturity inline. If (cycle - sCycle) <
			//      Cycle the slot hasn't matured yet, helper would no-op
			//      → skip BL. This is the dominant case in transform/
			//      skinning loops where FMAC ops fire every pair and the
			//      pipe is full of in-flight slots that haven't had the
			//      4-cycle latency yet.
			//   3. Anything else → BL (helper has actual drain work).
			//
			// Maturity check uses the runtime cycle (VU1_CYCLE_REG = x21,
			// already kept up-to-date with stall bumps), so it doesn't
			// suffer the static-analysis "stall bumps make slots mature
			// ahead of our model" pitfall that vetoes compile-time
			// maturity-based elision (see skipTestPipes pre-walk note).
			Label call_helper, skip_helper;
			const int64_t fdiv_enable_off = (int64_t)offsetof(VURegs, fdiv)
				+ (int64_t)offsetof(fdivPipe, enable);
			const int64_t efu_enable_off  = (int64_t)offsetof(VURegs, efu)
				+ (int64_t)offsetof(efuPipe, enable);
			const int64_t ialucount_off   = (int64_t)offsetof(VURegs, ialucount);

			// Load all three non-FMAC counters once; OR them to test
			// "any non-FMAC pipe active." Loads are CSE'd with whatever
			// the BL helper would do anyway (same cache lines).
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, fdiv_enable_off));
			armAsm->Ldr(w5, MemOperand(VU1_BASE_REG, efu_enable_off));
			armAsm->Ldr(w6, MemOperand(VU1_BASE_REG, ialucount_off));
			armAsm->Orr(w7, w4, w5);
			armAsm->Orr(w7, w7, w6);

			// Tier 1: all four counters zero → skip BL.
			armAsm->Orr(w8, w7, VU1_FMACCOUNT_REG);
			armAsm->Cbz(w8, &skip_helper);

			// Some pipe is non-empty. If any non-FMAC pipe → BL (helper
			// might drain FDIV/EFU result or pop matured IALU).
			armAsm->Cbnz(w7, &call_helper);

			// Tier 2: fmaccount > 0, fdiv/efu/ialu empty. Maturity check
			// on the FMAC front slot. (slot.sCycle is u64 at offset 24,
			// slot.Cycle is u32 at offset 32 — see fmacPipe layout in
			// VU.h.) (pos*3)<<4 = pos*48 = sizeof(fmacPipe) trick mirrors
			// the emitFMACAddPair address-compute pattern.
			const int64_t fmacreadpos_off = (int64_t)offsetof(VURegs, fmacreadpos);
			const int64_t fmac_off        = (int64_t)offsetof(VURegs, fmac);
			const int64_t fmac_sCycle_off = (int64_t)offsetof(fmacPipe, sCycle);
			const int64_t fmac_Cycle_off  = (int64_t)offsetof(fmacPipe, Cycle);

			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, fmacreadpos_off));
			armAsm->Add(x5, x4, Operand(x4, LSL, 1));            // x5 = pos*3
			armAsm->Add(x5, VU1_BASE_REG, Operand(x5, LSL, 4));  // x5 = base + pos*48
			armAsm->Ldr(x6, MemOperand(x5, fmac_off + fmac_sCycle_off));
			armAsm->Ldr(w4, MemOperand(x5, fmac_off + fmac_Cycle_off));
			armAsm->Sub(x6, VU1_CYCLE_REG, x6);                  // diff = cycle - sCycle (u64)
			armAsm->Cmp(x6, x4);                                 // vs Cycle (u32 zero-ext)
			armAsm->B(lo, &skip_helper);                         // not mature → skip BL
			// fall through to call_helper

			armAsm->Bind(&call_helper);
			armAsm->Mov(w1, VU1_FMACCOUNT_REG);
			armAsm->Mov(x2, VU1_CYCLE_REG);
			armAsm->Mov(x0, VU1_BASE_REG);
			// vu1_TestPipes_VU1 is NEON-free (see emitVu1CallNeonFree
			// doc). Skipping vfCacheFlushAndInvalidate alone saves 3-6
			// Strs per call — and this BL fires every pair on transform-
			// heavy blocks. Was 19.49% of total CPU on GoW2 in-game.
			emitVu1CallNeonFree(reinterpret_cast<const void*>(vu1_TestPipes_VU1));
			armAsm->Mov(VU1_FMACCOUNT_REG, w0);

			armAsm->Bind(&skip_helper);
			VU1_PERF_END(_pp_s6, "VU1_TestPipes_0x%04x", pc);
		}

		// 6b. Decrement VIBackupCycles (needed for correct VI backup reads
		//     in branch instructions). x22 holds cycle value before this pair.
		//     Inlined: common case is VIBackupCycles==0 → CBZ skips entire block.
		//     Elided entirely when the pre-walk proves no IBxx reader AND
		//     no emitBackupVI-triggering write lives in this block — a
		//     block-end Strb(wzr) clamp substitutes for the decrements.
		if (!skip_vibackup_decrement)
			emitDecrementVIBackup(cycle_off, vibackup_off);

		// 6c. CHECK_XGKICKHACK periodic sync tick (C-1). If the pre-walk
		//     committed accumulated kickcycles to this pair (only happens
		//     at memwrite pairs), advance the paced XGKICK transfer
		//     BEFORE this pair's memwrite emits its store — so the transfer
		//     reads pre-store VU1.Mem state. Mirrors mVU_XGKICK_SYNC(..., false)
		//     at microVU_Compile.inl:895.
		//
		//     No flush/reload needed: _vuXGKICKTransfer(flush=false) reads
		//     and writes only VU1.xgkick* fields (all memory-only, not
		//     cached in x21/x24/x25). vu1_XGKICK_hack_sync is a no-op when
		//     xgkickenable=false (early return inside _vuXGKICKTransfer).
		if (xgkickhack && kick_cycles_sync[i] > 0)
		{
			armAsm->Mov(x0, VU1_BASE_REG);
			armAsm->Mov(w1, kick_cycles_sync[i]);
			emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_hack_sync));
		}

		// 7. Execute upper instruction.
		//    Set VU->code at runtime (interpreter reads it for register fields).
		//    Set VU1.code at compile time so the rec emitter resolves the correct
		//    interpreter function pointer via VU1_UPPER_OPCODE[code & 0x3f].
		if (use_ibit_hack)
		{
			// IbitHack: live read from micro memory so post-compile patches
			// are visible to C wrappers + runtime-decode native paths.
			armAsm->Ldr(x5, MemOperand(VU1_BASE_REG, micro_off));
			armAsm->Ldr(w4, MemOperand(x5, (pc + 4)));
		}
		else
		{
			armAsm->Mov(w4, upper);
		}
		armAsm->Str(w4, MemOperand(VU1_BASE_REG, code_off));
		VU1.code = upper; // compile-time context for the rec emitter
		g_vu1NeedsFlags = pair_needs_flags[i]; // flag-deferral hint for FMAC emitters
		// analyzeBranchVI: gate VI backup BL on whether any in-block
		// branch within 4 pairs reads this writer's VI (or this pair is
		// in the cross-block conservative tail).
		g_vu1NeedsVIBackup = ir_op.needs_vi_backup;
		// FMAC opt #14: gate VF writeback's cache store on Pass 1's
		// dead-write analysis for THIS pair's UPPER op.
#ifdef VU1_SHADOW_VERIFY
		// Dead-write elision is by-design correct under live execution but
		// makes per-pair VF state diverge from interp. Force-disable under
		// the shadow harness so JIT writes match interp at every pair.
		g_vu1DeadVFWrite = false;
#else
		g_vu1DeadVFWrite = ir_op.dead_vf_write_upper;
#endif
		// FMAC opt #17: same-VF different-lane batching gates.
#ifdef VU1_SHADOW_VERIFY
		// Batching defers pair K's VF writeback so pair K+1 emits the
		// combined Stp. Per-pair shadow scope verifies AFTER pair K — at
		// that point K's VF is still in a stash (callee-saved q-reg) and
		// memory holds the pre-K value. Interp eagerly writes per pair.
		// Force-disable batching under shadow so each pair commits its
		// own VF writeback, matching interp's per-pair memory state.
		g_vu1BatchWithNext = false;
		g_vu1BatchFromPrev = false;
#else
		g_vu1BatchWithNext = ir_op.batch_with_next;
		g_vu1BatchFromPrev = ir_op.batch_from_prev;
#endif
		// FMAC opt #4: ABS-of-known-positive gate.
		g_vu1AbsSrcKnownNonNeg = ir_op.abs_src_known_non_neg;

		// vf_read_after_write deferred writeback gate. When set, the FMAC
		// writeback emit spills v5 to g_vu1DeferredVfStash instead of
		// committing to VF[X] — keeps lower's read of VF[X] pointed at the
		// original memory value. Cleared immediately after the upper emit
		// so subsequent emits (lower, step 9 FMAC pipe add, etc.) don't
		// accidentally trigger the stash path.
		//
		// Under batching (opt #17) the writeback already routes through
		// d10 — disable batching for this pair so the defer path wins.
		// vf_read_after_write hazards are rare and batching detection
		// (adjacent X-then-Y to same VF) is unlikely to coexist with a
		// VF-read-by-lower in the same pair anyway.
		if (defer_vf_writeback)
		{
			g_vu1BatchWithNext      = false;
			g_vu1BatchFromPrev      = false;
			g_vu1DeferVfWriteback   = true;
			g_vu1DeferredVfIdx      = -1;
		}

		VU1_PERF_BEGIN(_pp_s7);
		emitVU1Upper(upper); // switch dispatch — emits native ARM64 for this op
		VU1_PERF_END(_pp_s7, "VU1_U_%02x_0x%04x", upper & 0x3f, pc);

		// Clear defer flag — only the upper writeback above should consult
		// it. Lower's emit (next) and step 9's FMAC-pipe-add must use the
		// normal cache-store paths.
		g_vu1DeferVfWriteback = false;

		// 8. Lower instruction handling.
		// NOP the lower when this pair is a branch AND the previous pair
		// set E-bit — "branch in E-bit delay slot" is ISA-undefined.
		// Matches x86 microVU_Compile.inl branchWarning which flags
		// mVUlow.isNOP when the pair is both in an E-bit delay slot and
		// contains a branch. Upper still executes; we just skip the
		// branch rec emission so VU->branch / branchpc stay untouched.
		// Same pattern as VU0 C-5 fix.
		const bool suppress_branch = !ibit && branch_pipe && prev_was_ebit;
		// Also elide the entire lower emit scaffold (VU->code store +
		// dispatch) when the lower is a known-NOP on VU1 (WAITP/WAITQ) —
		// matches x86 microVU's mVUlow.isNOP pass1 optimization. Saves
		// ~3 emitted instructions per NOP op plus the switch dispatch.
		// Same pattern as VU0 C-6 fix.
		const bool lower_is_nop = !ibit && isVU1LowerNOP(lower);
		if (ibit)
		{
			// I-bit: lower field is a float immediate — load into VI[REG_I].
			armAsm->Mov(w4, lower);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, regi_off));
		}
		else if (!lower_is_nop)
		{
			// 8a. (non-hack) Back-to-back XGKICK sequencing. If the prior
			//     pair captured an XGKICK and this pair's lower is also an
			//     XGKICK, fire the prior kick NOW — before vu1_XGKICK
			//     clobbers the scratch with the new addr. Pair k+1's upper
			//     has already emitted above (step 7) and upper ops don't
			//     write VU1.Mem, so firing here doesn't race with any
			//     pending store. Under xgkickhack the scratch is unused
			//     (hack path writes VU1.xgkick* directly and the capture
			//     helper flushes prior via _vuXGKICKTransfer(0, true) itself).
			if (!xgkickhack && pending_xgkick_fire && ir_op.isKick)
			{
				armAsm->Mov(x0, VU1_BASE_REG);
				emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
				pending_xgkick_fire = false;
			}
			// Execute lower instruction (stalls already tested above).
			if (use_ibit_hack)
			{
				// IbitHack: live read from micro memory.
				armAsm->Ldr(x5, MemOperand(VU1_BASE_REG, micro_off));
				armAsm->Ldr(w4, MemOperand(x5, pc));
			}
			else
			{
				armAsm->Mov(w4, lower);
			}
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, code_off));
			VU1.code = lower; // compile-time context
			g_vu1CurrentPC = pc; // compile-time PC for native branches
			// FMAC opt #14: gate VF writeback's cache store on Pass 1's
			// dead-write analysis for THIS pair's LOWER op. Lower FMACs
			// are rare (most lowers are non-FMAC pipes), but lower MOVE/
			// MR32/MFIR/MFP also write VFs through vfCacheStore directly
			// — those don't currently consult the gate, only FMAC paths
			// (emitFmac{InlineWriteback,StoreMasked,NoFlagWriteback}) do.
#ifdef VU1_SHADOW_VERIFY
			g_vu1DeadVFWrite = false;
#else
			g_vu1DeadVFWrite = ir_op.dead_vf_write_lower;
#endif
			// FMAC opt #17: lower never participates in upper-write batching.
			g_vu1BatchWithNext = false;
			g_vu1BatchFromPrev = false;

			// Hackmode XGKICK: recVU1_XGKICK emits a BL to
			// vu1_XGKICK_hack_capture which internally runs
			// _vuXGKICKTransfer(0, true). That writes VU1.cycle (adds
			// transfersize/8 per iteration under flush) and invokes
			// _vuTestPipes (reads/writes FMAC + IALU pipeline state), so
			// flush x21/x24/x25 before the BL and reload after. Non-hack
			// XGKICK (vu1_XGKICK) only writes s_vu1_pending_xgkick_addr,
			// so no flush is needed there.
			const bool hack_xgkick_here = xgkickhack && ir_op.isKick;
			if (hack_xgkick_here)
			{
				emitFlushCycleReg(cycle_off);
				emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
				// Phase-9b: hack-mode XGKICK capture eventually runs
				// _vuTestPipes inside _vuXGKICKTransfer(0, true), which
				// reads + decrements fmaccount.
				emitFlushFmaccountReg(fmaccount_off);
			}
			VU1_PERF_BEGIN(_pp_s8);
			if (!suppress_branch)
				emitVU1Lower(lower); // switch dispatch — emits native ARM64 for this op
			VU1_PERF_END(_pp_s8, "VU1_L_%02x_0x%04x", lower >> 25, pc);
			if (hack_xgkick_here)
			{
				emitReloadCycleReg(cycle_off);
				emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
				emitReloadFmaccountReg(fmaccount_off);
			}
		}

		// vf_read_after_write deferred-writeback commit. Upper spilled v5
		// to g_vu1DeferredVfStash (idx + xyzw captured by the writeback
		// emit); lower above ran with the original VF[X] in memory. Now
		// reload and call vfCacheStore to commit upper's result — same
		// final-state as the eager writeback path, just sequenced so lower
		// sees pre-upper VF[X].
		//
		// Idx <= 0 means the upper writeback emit hit an early return
		// (skip_dst_write for VF[0] / to_acc redirect / dead-VF-write
		// elision) — nothing to commit. The vf_read_after_write hazard
		// from Pass 1 implies upper.VFwrite != 0, but a covering forward
		// write can still mark this pair's upper as dead.
		if (defer_vf_writeback && g_vu1DeferredVfIdx > 0)
		{
			armMoveAddressToReg(x4, &g_vu1DeferredVfStash[0]);
			armAsm->Ldr(v5.Q(), MemOperand(x4));
			// vfCacheStore already calls vu1BroadcastCacheNoteVfWritten —
			// no separate invalidate needed.
			vfCacheStore(g_vu1DeferredVfIdx, v5, g_vu1DeferredXyzw);
			g_vu1DeferredVfIdx = -1;
		}

		// 9-11. FMAC clear + AddUpperStalls + AddLowerStalls fused.
		//       emitFMACAddPair handles ClearFMAC + the FMAC sides of
		//       AddUpper/AddLowerStalls in a single BL (skipped entirely
		//       when neither side is FMAC). emitLowerNonFMACAdd handles
		//       FDIV/EFU/IALU adds for non-FMAC lower pipes.
		//       For I-bit pairs lregs is all-zero (pipe == VUPIPE_NONE),
		//       so passing it directly is safe — both helpers no-op on it.
		VU1_PERF_BEGIN(_pp_s9);
		emitFMACAddPair(uregs, lregs);
		if (!ibit)
			emitLowerNonFMACAdd(lregs);
		VU1_PERF_END(_pp_s9, "VU1_PipeAdd_0x%04x", pc);

		// 11b. D/T bits — depend on VU0 FBRST (runtime). Only emit when
		//      actually set. Runs AFTER the op so the pair's side effects
		//      on VPU_STAT / VI mem-mapped regs happen before the VPU_STAT
		//      bit is set, matching x86 microVU_Compile.inl:900-910 which
		//      calls mVUDoDBit/mVUDoTBit after mVUexecuteInstruction. D/T
		//      → ebit=1 is picked up by step 13's ebit countdown below in
		//      the same pair. Same placement as VU0 C-1 fix.
		//
		//      Suppressed when the current pair is itself a branch or is in
		//      a branch delay slot — matches x86's `!mVUinfo.isBdelay &&
		//      !mVUlow.branch` guard (ISA undefined behavior for D/T in
		//      these contexts). Same pattern as VU0 C-4 fix.
		//      `branch_pipe` is computed at the top of the per-pair body
		//      (needed by the step 8 branch-in-ebit-delay suppression too).
		if ((dbit_set || tbit_set) && !branch_pipe && !prev_was_branch)
		{
			armAsm->Mov(w0, upper);
			emitVu1Call(reinterpret_cast<const void*>(vu1CheckDTBits));
		}

		// 12. Branch countdown (inline).
		//
		// Gated on ir.has_branch: if no pair in this block has a
		// VUPIPE_BRANCH lower, VU->branch is never written here, and any
		// prior block's branch was countdowned to 0 in that block (VU1 has
		// no per-pair budget abort; every block runs to natural completion).
		// Same correctness shape as step 13's ebit gate.
		if (ir.has_branch)
		{
			Label skip_branch;
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, branch_off));
			armAsm->Cbz(w4, &skip_branch);        // branch == 0: nothing to do
			armAsm->Subs(w4, w4, 1);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, branch_off));
			armAsm->B(&skip_branch, ne);           // still > 0: keep counting
			// branch just reached 0: set TPC = branchpc
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, branchpc_off));
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, tpc_off));
			// Inline of vu1HandleDelayBranch (4-line C body that doesn't read
			// or write VU->cycle and doesn't touch VF/VI memory). Saves the
			// BL+vfCacheFlushAndInvalidate+viCacheInvalidateAll cost — the
			// cache tracker reset matters too: keeping the trackers live lets
			// downstream emit reuse cached VF/VI values across the branch
			// handoff. Hits whenever a delay-slot branch retires.
			//
			// C reference (iVU1micro_arm64.cpp:987):
			//   if (VU->takedelaybranch) {
			//       VU->branch          = 1;
			//       VU->branchpc        = VU->delaybranchpc;
			//       VU->takedelaybranch = false;
			//   }
			const int64_t takedelaybranch_off = (int64_t)offsetof(VURegs, takedelaybranch);
			const int64_t delaybranchpc_off   = (int64_t)offsetof(VURegs, delaybranchpc);
			Label hdb_skip;
			armAsm->Ldrb(w4, MemOperand(VU1_BASE_REG, takedelaybranch_off));
			armAsm->Cbz(w4, &hdb_skip);
			armAsm->Mov(w4, 1);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, branch_off));
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, delaybranchpc_off));
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, branchpc_off));
			armAsm->Strb(wzr, MemOperand(VU1_BASE_REG, takedelaybranch_off));
			armAsm->Bind(&hdb_skip);
			armAsm->Bind(&skip_branch);
		}

		// 13. Ebit countdown (inline). vu1EbitDone calls _vuFlushAll which
		//     writes VU->cycle (pipeline drain can advance the cycle to
		//     retire still-pending slots), so flush/reload the cached
		//     cycle register around the BL.
		//
		// Gated on block-level flags: if no pair in this block has E-bit
		// set (step 3 writes ebit=2) and no pair has D/T-bit set (step 4's
		// vu1CheckDTBits writes ebit=1 on fire), then VU->ebit stays 0
		// throughout the block and the countdown is always a no-op.
		//
		// Cross-block carryover: a prior block that ran its own ebit
		// countdown to 0 calls vu1EbitDone → sets s_vu1_program_ended /
		// clears VPU_STAT running bit; the current block's linkEntry gate
		// catches termination before reaching this per-pair body. So
		// "ebit=0 at block entry" holds for any block that actually
		// executes its per-pair loop.
		if (ir.has_ebit || ir.has_dbit_or_tbit)
		{
			// #18 fix: drain any deferred fmaccount Adds into x26 BEFORE the
			// runtime Cbz/B.ne. emitFlushFmaccountReg below would otherwise
			// emit `Add x26, x26, N` inside the runtime conditional — when
			// the skip path is taken (ebit was 0, or still > 1 after
			// decrement) the Add never executes, but compile-time resets
			// the deferred counter to 0. Subsequent FMAC pairs would then
			// accumulate from 0 against an x26 that's silently short by N,
			// and a future _vuFMACflush would walk the wrong number of ring
			// slots — visible as missing flag/clip writes and corrupted
			// vertex output (FFX residual flicker after the link-exit drain).
			if (s_vu1_deferred_fmaccount > 0)
			{
				armAsm->Add(VU1_FMACCOUNT_REG, VU1_FMACCOUNT_REG, s_vu1_deferred_fmaccount);
				s_vu1_deferred_fmaccount = 0;
			}
			Label skip_ebit;
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, ebit_off));
			armAsm->Cbz(w4, &skip_ebit);          // ebit == 0: nothing to do
			armAsm->Subs(w4, w4, 1);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, ebit_off));
			armAsm->B(&skip_ebit, ne);             // still > 0: keep counting
			// ebit just reached 0: end of microprogram
			emitFlushCycleReg(cycle_off);
			// Phase-9b: vu1EbitDone calls _vuFlushAll, which drains the
			// FMAC pipe via _vuFMACflush (decrements fmaccount per slot).
			// (Deferred Adds already drained above; this just emits Str.)
			emitFlushFmaccountReg(fmaccount_off);
			armAsm->Mov(x0, VU1_BASE_REG);
			emitVu1Call(reinterpret_cast<const void*>(vu1EbitDone));
			emitReloadCycleReg(cycle_off);
			emitReloadFmaccountReg(fmaccount_off);
			armAsm->Bind(&skip_ebit);
		}

		// 14. FMAC write-position advance (wraps mod 4). Stage C3: hoisted
		// into w24 — no memory load/store here; the block-end flush writes
		// the final value back in one store.
		if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		{
			armAsm->Add(VU1_FMAC_WPOS_REG, VU1_FMAC_WPOS_REG, 1);
			armAsm->And(VU1_FMAC_WPOS_REG, VU1_FMAC_WPOS_REG, 3);
		}

		// 15. (non-hack) XGKICK deferred fire. A pending kick from the
		//     prior pair is emitted here — AFTER this pair's opcodes so
		//     any store has committed before GIF walks VU1.Mem. Back-to-
		//     back XGKICK was already handled at step 8a, so if we reach
		//     here with pending set, this pair's lower is guaranteed to
		//     be non-XGKICK. Skipped under xgkickhack: the scratch
		//     mechanism is disabled and pending_xgkick_fire stays false.
		if (!xgkickhack)
		{
			if (pending_xgkick_fire)
			{
				armAsm->Mov(x0, VU1_BASE_REG);
				emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
				pending_xgkick_fire = false;
			}
			// Re-arm for the next pair if this one captured an XGKICK.
			// (ir_op.isKick is already gated on !iBit by Pass 1.)
			if (ir_op.isKick)
				pending_xgkick_fire = true;
		}

		// Step 16 (FMAC opt #20): software-pipeline preload of next pair's
		// VF reads. ARM Cortex/Oryon out-of-order cores hide a 4-6 cycle
		// Ldr latency behind independent FMUL/FADD/integer work. Pass 1
		// already populated the next pair's _VURegsNum; emitting the Ldrs
		// HERE — at the runtime tail of this pair, before the loop iterates
		// to the next pair's prologue/compute — means the Ldrs issue while
		// step 12/13/15 conditional BLs (if any) have already executed,
		// step 14 fmacwpos++ has retired, and step 1-6 of the NEXT pair
		// (cycle++, TPC store, etc.) provide the cycle gap that hides the
		// Ldr's load-to-use latency. The next pair's vfCacheLoadInto then
		// hits the cache and emits a single Mov instead of an Ldr.
		//
		// Restricted to "clean-entry" next pairs only: any BL in the next
		// pair's pre-compute path goes through emitVu1Call →
		// vfCacheFlushAndInvalidate, which drops the cache tracker and
		// (at runtime) clobbers caller-saved NEON regs. A speculative
		// preload through such a BL is pure waste — the runtime data is
		// destroyed and the tracker reset means the JIT re-Ldrs anyway.
		//
		// xgkickhack mode is excluded entirely: step 6c xgkickhack sync
		// and step 8's hack_xgkick_here both emit unconditional BLs in
		// configurations that's enabled, and the per-pair gating would
		// have to consider kick_cycles_sync[] which adds noise. Most
		// users keep xgkickhack off; we forfeit the optimization for the
		// rest.
		if (!xgkickhack && i + 1 < numPairs)
		{
			const u32 next_i = i + 1;
			const armvu1ir::microOp& nextOp = ir.info[next_i];
			const _VURegsNum& uregs_n = uregs_data[next_i];
			const _VURegsNum& lregs_n = lregs_data[next_i];
			const PerPairSkip& sk = skip_info[next_i];

			// Upper-stalls BL gate. emitTestUpperStalls only emits a BL
			// when uregs.pipe == VUPIPE_FMAC AND a non-skipped VFread is
			// non-zero. Match that exactly.
			const bool upper_clean =
				(uregs_n.pipe != VUPIPE_FMAC)
				|| ((sk.skipUpperFMACStall0 || uregs_n.VFread0 == 0)
				    && (sk.skipUpperFMACStall1 || uregs_n.VFread1 == 0));

			// Lower-stalls BL gate. I-bit pairs have no lower work; for
			// non-I-bit, the BL gating depends on the lower's pipe class.
			bool lower_clean = false;
			if (nextOp.iBit)
			{
				lower_clean = true;
			}
			else
			{
				const bool fmac_stalls_clean =
					(sk.skipLowerFMACStall0 || lregs_n.VFread0 == 0)
					&& (sk.skipLowerFMACStall1 || lregs_n.VFread1 == 0);
				switch (lregs_n.pipe)
				{
					case VUPIPE_FMAC:   lower_clean = fmac_stalls_clean; break;
					case VUPIPE_FDIV:   lower_clean = fmac_stalls_clean && sk.skipLowerFDIVWait; break;
					case VUPIPE_EFU:    lower_clean = fmac_stalls_clean && sk.skipLowerEFUWait; break;
					case VUPIPE_BRANCH: lower_clean = (sk.skipLowerALUStall || lregs_n.VIread == 0); break;
					default:            lower_clean = true; break;
				}
			}

			const bool clean_entry =
				!nextOp.vf_read_after_write && !nextOp.vf_write_collision
				&& !nextOp.clip_read_after_write && !nextOp.clip_write_collision
				&& upper_clean && lower_clean
				&& sk.skipTestPipes
				// step 8a back-to-back XGKICK fire BL: both pairs are XGKICK.
				&& !(ir_op.isKick && nextOp.isKick);

			if (clean_entry)
			{
				u32 preloaded[4] = {0, 0, 0, 0};
				int n_preloaded = 0;
				auto try_preload = [&](u32 vf) {
					// VF[0] is constant {0,0,0,1} — load helpers handle it
					// inline without a cache slot, so preloading wastes a
					// slot and an Ldr.
					if (vf == 0)
						return;
					for (int k = 0; k < n_preloaded; k++)
						if (preloaded[k] == vf)
							return;
					preloaded[n_preloaded++] = vf;
					(void)vfCacheLoadResident(static_cast<int>(vf));
				};
				try_preload(uregs_n.VFread0);
				try_preload(uregs_n.VFread1);
				try_preload(lregs_n.VFread0);
				try_preload(lregs_n.VFread1);
			}
		}

		// Track branch for next pair's D/T bit suppression (step 11b).
		// Updated on every pair that emits the native body; hazard-fallback
		// pairs use `continue` above so they DON'T update this — their
		// branch-pipe-ness is invisible to the native code. But since the
		// pair after a hazard-fallback pair is still a valid next-pair
		// context, leaving prev_was_branch at its stale value is acceptable:
		// the hazard fallback emitted vu1Exec for the whole pair, and any
		// D/T bit on the next pair's native path honors the previous
		// NATIVELY-EMITTED branch correctly.
		prev_was_branch = branch_pipe;

		// Track E-bit for next pair's branch suppression (step 8).
		// Same reasoning: delay-slot context applies regardless of path.
		prev_was_ebit = ebit_set;

#ifdef VU1_SHADOW_VERIFY
		// Run interp on the snapshot, compare to JIT result, halt+abort on
		// first divergence. Same gate as snapshot above. Pinned regs need
		// to be flushed/reloaded around the BL since the interp re-run
		// inside vu1_shadow_verify mutates VU1.cycle / fmaccount / wpos /
		// flags / ACC and our pinned views would otherwise drift.
		if (!ir_op.isKick)
		{
			emitFlushCycleReg(cycle_off);
			emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			emitFlushFmaccountReg(fmaccount_off);
			emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitFlushAccReg(acc_off);
			// Flush VF (NEON cache) + VI (GPR cache) so vu1_shadow_verify
			// sees the JIT's post-pair VF/VI state in memory. Without this,
			// dirty cache slots from FMAC writebacks linger in q-regs and
			// the harness reads stale pre-pair memory → phantom
			// "JIT didn't write VF[X]" divergences.
			vfCacheFlushAndInvalidate();
			viCacheInvalidateAll();
			armAsm->Mov(w0, pc);
			armEmitCall(reinterpret_cast<const void*>(vu1_shadow_verify));
			emitReloadCycleReg(cycle_off);
			emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
			emitReloadFmaccountReg(fmaccount_off);
			emitReloadFlagRegs(macflag_off, statusflag_off, clipflag_off);
			emitReloadAccReg(acc_off);
		}
#endif

		pc = (pc + 8) & (VU1_PROGSIZE - 1);
	}

	// Block-end XGKICK drain (non-hack only). If the last pair was XGKICK
	// we never got a chance to emit the deferred fire inside the loop —
	// drain here so the scratch (s_vu1_pending_xgkick_addr) never carries
	// state into the next compiled block. The file-local static assumption
	// in iVU1Lower_arm64.cpp depends on this drain firing on every exit
	// path. Skipped under xgkickhack: VU1.xgkickenable carries across
	// blocks intentionally — the hack's pacing spans block boundaries via
	// sync ticks in subsequent blocks.
	if (!xgkickhack && pending_xgkick_fire)
	{
		armAsm->Mov(x0, VU1_BASE_REG);
		emitVu1Call(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
	}

	// Step 6b block-end clamp. Elision of per-pair VIBackupCycles decrement
	// requires this single Strb(wzr) at the fall-through exit to match the
	// natural decrement's end state. Only fires on normal block completion —
	// the budget_exceeded_exit path (linkEntry cycle/termination gate)
	// bypasses the per-pair body entirely and correctly preserves
	// VIBackupCycles untouched, matching "did not execute any pair" semantics.
	if (skip_vibackup_decrement)
		armAsm->Strb(wzr, MemOperand(VU1_BASE_REG, vibackup_off));

	// #18 fix: drain any deferred fmaccount Adds into the pinned w26 BEFORE
	// the exit selector. Linked exits (B → successor's linkEntry) and the
	// JR/JALR indirect dispatch path BOTH skip the epilogue flush below;
	// without this drain, the successor inherits a stale x26 (deferred
	// counter not added in), every subsequent emitFMACAddPair fires on the
	// stale value, and FMAC pipeline state corrupts. Symptom: vertex
	// transform output collapses to flat textures at camera position
	// (FFX battle scenes regression seen during testing).
	if (s_vu1_deferred_fmaccount > 0)
	{
		armAsm->Add(VU1_FMACCOUNT_REG, VU1_FMACCOUNT_REG, s_vu1_deferred_fmaccount);
		s_vu1_deferred_fmaccount = 0;
	}

	// Block-linking scaffolding (Phase 1): record the address immediately
	// after the block-end XGKICK drain and immediately before the register
	// flushes + epilogue. Phase 2 emits a patch slot here for linkable
	// blocks — a `B` instruction that either falls through to the epilogue
	// below (initially, and whenever unlinked) or jumps directly to a
	// successor's linkEntry (when patched).
	out_block->returnExit = armGetCurrentCodePointer();

	// Phase 2+3+4: emit the link-exit selector.
	//
	// Each patch slot (for static-target links) is a 4-byte unconditional
	// `B` that initially jumps to `fallthrough` (the flush+epilogue code
	// below). Linking rewrites a slot's target to a successor block's
	// linkEntry, which trusts the caller's cached x21/x23/x24/x25.
	//
	// Layout depends on the computeBlockLinkExits result:
	//
	//   num_exits == 0, indirect == false  (ebit) — no selector; falls
	//                   straight into the flush+epilogue.
	//
	//   num_exits == 1  (B/BAL/max-size fall-through):
	//       [patch] B <fallthrough>          ← exits[0].patch_site
	//       flushes...                        ← exits[0].fallthrough = patch+4
	//
	//   num_exits == 2  (conditional IBxx): both patch slots are
	//       UNCONDITIONAL B instructions — a hardcoded B.ne (NOT a patch
	//       slot) steers execution. armEmitJmpPtr only encodes
	//       unconditional B, so patching a B.eq in place would clobber
	//       the condition bits — the indirection through B.ne avoids
	//       that entirely.
	//       Ldr w4, [tpc_off]
	//       Mov w5, <taken_target>
	//       Cmp w4, w5
	//       B.ne use_not_taken_path
	//       [patch_taken]     B <target or flushes>
	//       [patch_not_taken] B <target or flushes>
	//       flushes...
	//
	//   indirect == true  (JR/JALR): target is runtime-computed from
	//       VI[_Is_]; no compile-time patch slot. Emit an inline BL to
	//       vu1_indirect_dispatch which returns the target block's
	//       linkEntry (or nullptr). On non-null, Br tail-jumps to it;
	//       on null, falls through to flushes+Ret and Execute's outer
	//       loop dispatches normally (compiling the target if needed).
	//       Ldr w4, [tpc_off]
	//       Mov w0, w4
	//       Bl vu1_indirect_dispatch
	//       Cbz x0, fall_through
	//       Br x0
	//       fall_through: flushes...
	//
	// w4/w5/x0 are caller-saved and clobbered freely here — past the per-
	// pair loop, before the register-flush path. Cached regs
	// x21/x23/x24/x25 stay untouched and are live across any linked jump.
	// link_info was computed earlier (step 2 TPC-gating pre-walk above).
	out_block->num_exits = link_info.num_exits;
	for (u32 e = 0; e < 2; e++)
	{
		out_block->exits[e].target_pc      = link_info.target_pcs[e];
		out_block->exits[e].patch_site     = nullptr;
		out_block->exits[e].fallthrough    = nullptr;
		out_block->exits[e].current_target = nullptr;
	}

	// Phase 2 CRITICAL: emit the VF cache flush HERE, before the patch_site
	// B is written, so the runtime flush runs in BOTH the unlinked path
	// (B falls through to flushes-local → budget_exceeded_exit) AND the
	// linked path (B patched to successor's linkEntry, which SKIPS the
	// budget_exceeded_exit flush entirely). The successor block's compile-
	// time tracker is independent of ours; it expects to find any deferred
	// writes already in VU1.VF[] memory at entry. Without this pre-link
	// flush, every linked exit silently drops dirty cache slots — visible
	// as missing geometry uploads (XGKICK transfers stale VU memory) while
	// VU itself runs at 100% (the work happens, the writes vanish).
	//
	// VI cache: write-through so nothing to flush — but invalidate the
	// tracker so the LRU clock resets cleanly (defensive; not strictly
	// required since the next block compile resets on entry).
	//
	// Indirect (JR/JALR) goes through emitVu1Call(vu1_indirect_dispatch),
	// which already flushes inside the wrapper — no extra work here.
	if (link_info.num_exits > 0 || link_info.indirect)
	{
		vfCacheFlushAndInvalidate();
		viCacheInvalidateAll();
	}

	if (link_info.num_exits == 1)
	{
		u8* patch = armGetCurrentCodePointer();
		Label flushes;
		armAsm->B(&flushes);
		armAsm->Bind(&flushes);
		out_block->exits[0].patch_site     = patch;
		out_block->exits[0].fallthrough    = patch + 4;
		out_block->exits[0].current_target = patch + 4;
	}
	else if (link_info.num_exits == 2)
	{
		// Conditional layout — CRITICAL: both patch slots must be
		// unconditional `B` instructions. armEmitJmpPtr encodes a bare
		// `B imm26` (opcode 0x14000000) and would clobber the condition
		// bits if we tried to patch a `B.eq` in place. Instead we emit
		// a hardcoded `B.ne` (NOT a patch slot) that steers to whichever
		// unconditional-B patch slot matches the runtime TPC.
		//
		//   pos 0:  Ldr w4, [tpc_off]
		//   pos 4:  Mov w5, <taken_target>
		//   pos 8:  Cmp w4, w5
		//   pos 12: B.ne use_not_taken_path     (hardcoded steering)
		//   pos 16: [patch_taken]  B <target or flushes>   (exits[1])
		//   pos 20: [patch_not_taken] B <target or flushes> (exits[0])
		//   pos 24: flushes...
		//
		// TPC == taken_target  → B.ne not taken → falls through to
		//                        patch_taken → jumps per link state.
		// TPC != taken_target  → B.ne fires to pos 20 → patch_not_taken.
		armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, tpc_off));
		armAsm->Mov(w5, link_info.target_pcs[1]);
		armAsm->Cmp(w4, w5);

		Label use_not_taken_path;
		Label flushes;
		armAsm->B(&use_not_taken_path, ne);

		u8* patch_taken = armGetCurrentCodePointer();
		armAsm->B(&flushes);               // exits[1] (taken)

		armAsm->Bind(&use_not_taken_path);
		u8* patch_not_taken = armGetCurrentCodePointer();
		armAsm->B(&flushes);               // exits[0] (not-taken)

		armAsm->Bind(&flushes);

		// exits[0] = NOT-TAKEN. Fallthrough = patch_not_taken + 4 = flushes.
		out_block->exits[0].patch_site     = patch_not_taken;
		out_block->exits[0].fallthrough    = patch_not_taken + 4;
		out_block->exits[0].current_target = patch_not_taken + 4;

		// exits[1] = TAKEN. Fallthrough = patch_taken + 8 skips
		// patch_not_taken's `B` and lands on the same flushes address.
		out_block->exits[1].patch_site     = patch_taken;
		out_block->exits[1].fallthrough    = patch_taken + 8;
		out_block->exits[1].current_target = patch_taken + 8;
	}
	else if (link_info.indirect)
	{
		// Phase 4: JR/JALR runtime dispatch. No static patch slot; the
		// dispatcher helper content-matches live VU1.Micro against the
		// target slot's variant deque (findVariant) and returns the
		// matching linkEntry or nullptr. A nullptr return falls through
		// to Execute's outer dispatch, which compiles or re-picks a
		// variant against live micro.
		//
		// x0 is caller-saved — we use it for both the call arg and the
		// return value. BL preserves x21/x23/x24/x25 (all x19-x28 are
		// callee-saved per AAPCS64), so cached regs survive across the
		// helper call and remain live for a tail-Br into linkEntry.
		armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, tpc_off));
		armAsm->Mov(w0, w4);
		emitVu1Call(reinterpret_cast<const void*>(vu1_indirect_dispatch));
		Label indirect_fall_through;
		armAsm->Cbz(x0, &indirect_fall_through);
		armAsm->Br(x0);
		armAsm->Bind(&indirect_fall_through);
	}

	// Budget-exceeded entries from the cycle check at linkEntry land here,
	// skipping both the per-pair loop body and the exit selector. Falls
	// straight into the flush+epilogue+Ret path below.
	armAsm->Bind(&budget_exceeded_exit);

	// Phase 2: flush deferred VF writes to memory before the epilogue. The
	// next block (entered via Execute's outer loop or a linked B from
	// elsewhere) won't share our compile-time slot map, so any cached-but-
	// unflushed values must hit VU1.VF[] now. Drops tracker afterwards.
	vfCacheFlushAndInvalidate();
	// VI cache: write-through so nothing to flush; just reset the tracker.
	viCacheInvalidateAll();

	// Stage C2: flush the cached cycle register to memory before restoring
	// the caller's x21. From here on VU->cycle is authoritative again.
	emitFlushCycleReg(cycle_off);
	// Stage C3: flush the cached FMAC/IALU write-position registers to
	// memory before restoring the caller's x24/x25.
	emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
	// Phase-9b: flush the cached fmaccount before restoring caller's x26.
	emitFlushFmaccountReg(fmaccount_off);
	// Phase-7: flush the cached flag regs before restoring x19/x20/x28.
	emitFlushFlagRegs(macflag_off, statusflag_off, clipflag_off);
	// Phase-8: flush the pinned ACC reg to memory before Ret. q16 is
	// caller-saved so no stack restore is needed — the block-scope
	// contract is "ACC memory-authoritative outside compiled blocks".
	emitFlushAccReg(acc_off);

#if defined(VU1_BLOCK_SHADOW_VERIFY) && defined(VU1_SHADOW_VERIFY)
	// Block-level shadow verify. By this point vfCacheFlushAndInvalidate
	// has flushed all deferred VF writes; the pinned-state flushes above
	// have committed cycle / wpos / fmaccount / flags / ACC. Memory is
	// JIT's authoritative end-of-block state. Now invoke the verify
	// helper which captures it, restores from snapshot, replays interp,
	// and compares. Skipped at compile time when any pair was XGKICK.
	if (!block_has_xgkick)
	{
		armAsm->Mov(w0, startPC);
		armAsm->Mov(w1, numPairs);
		armEmitCall(reinterpret_cast<const void*>(vu1_block_shadow_verify));
	}
#endif

	// --- Epilogue (96-byte frame; mirrors the prologue layout above) ---
	armAsm->Ldp(x20, x28, MemOperand(sp, 80));
	armAsm->Ldp(VU1_TERM_ADDR_REG, x19, MemOperand(sp, 64));
	armAsm->Ldp(x25, x26, MemOperand(sp, 48));
	armAsm->Ldp(VU1_BASE_REG, x24, MemOperand(sp, 32));
	armAsm->Ldp(VU1_CYCLE_REG, x22, MemOperand(sp, 16));
	armAsm->Ldp(x29, x30, MemOperand(sp, 96, PostIndex));
	armAsm->Ret();

	u8* end = armEndBlock();
	s_code_write = end;

	// Register the compiled VU1 block with simpleperf/perfetto so the JIT'd
	// code shows up as `VU1_<startPC>` in profiler reports instead of
	// "unknown unknown". Cost: one map insert per block compile.
	Perf::vu1.RegisterPC(entry, static_cast<size_t>(end - entry), startPC);

	return entry;
}

// ============================================================================
//  recArmVU1
// ============================================================================

recArmVU1::recArmVU1()
{
	m_Idx = 1;
	IsInterpreter = false;
}

void recArmVU1::Reserve()
{
	u8* const buf     = SysMemory::GetVU1Rec();
	u8* const buf_end = SysMemory::GetVU1RecEnd();

	s_pool.Init(buf, POOL_SIZE);
	s_code_base  = buf + POOL_SIZE;
	s_code_write = s_code_base;
	s_code_end   = buf_end;

	// s_variants[] default-constructs as empty deques — no explicit init.
}

#ifdef VU1_PROFILE_BLOCKS
static void DumpTopBlocks()
{
	struct BlockStat { u32 pc; u32 pairs; u64 execs; const u8* bytes; };
	std::vector<BlockStat> stats;
	stats.reserve(64);

	u64 total_execs = 0;
	u64 total_pair_execs = 0;
	u32 active_blocks = 0;
	// Walk every variant in every slot — a hot slot may have multiple
	// variants (different bytecode uploaded to the same PC over time),
	// and each has its own execCount.
	for (u32 i = 0; i < VU1_NUM_SLOTS; i++)
	{
		for (const VU1BlockEntry* blk : s_variants[i])
		{
			if (blk->execCount == 0)
				continue;
			active_blocks++;
			total_execs += blk->execCount;
			total_pair_execs += static_cast<u64>(blk->execCount) * blk->numPairs;
			stats.push_back(BlockStat{ i * 8u, blk->numPairs, blk->execCount, blk->snapshot });
		}
	}

	if (stats.empty())
	{
		INFO_LOG("VU1 JIT profile: no blocks executed since last reset");
		return;
	}

	std::sort(stats.begin(), stats.end(),
		[](const BlockStat& a, const BlockStat& b) {
			// Sort by pair-execs (pairs * execs — a rough proxy for time spent).
			const u64 ka = static_cast<u64>(a.execs) * a.pairs;
			const u64 kb = static_cast<u64>(b.execs) * b.pairs;
			if (ka != kb)
				return ka > kb;
			return a.execs > b.execs;
		});

	INFO_LOG("VU1 JIT top-20 hottest blocks (of {} active variants, total entries={}, total pair-execs={})",
		active_blocks, total_execs, total_pair_execs);
	const size_t limit = std::min<size_t>(20, stats.size());
	for (size_t i = 0; i < limit; i++)
	{
		const BlockStat& s = stats[i];
		const u64 pair_execs = static_cast<u64>(s.execs) * s.pairs;
		const double pct = total_pair_execs > 0
			? (100.0 * static_cast<double>(pair_execs) / static_cast<double>(total_pair_execs))
			: 0.0;
		INFO_LOG("  #{}: pc=0x{:04x} pairs={:3} execs={:12} pair-execs={:14} ({:5.2f}%)",
			i + 1, s.pc, s.pairs, s.execs, pair_execs, pct);

		// Disassemble each pair from the variant's private snapshot, NOT
		// live VU1.Micro — live may have been Cleared and overwritten
		// since this variant executed, and the point of the dump is to
		// show what each variant actually contains.
		//
		// disVU1MicroUF / disVU1MicroLF return a pointer into a shared
		// static buffer (DisVU1Micro.cpp:7 `static char ostr`), so the
		// upper disassembly must be copied into a std::string before
		// calling the lower disassembly — otherwise the second call
		// overwrites the first's result.
		u32 pc = s.pc;
		for (u32 p = 0; p < s.pairs; p++)
		{
			const u32 upper = *reinterpret_cast<const u32*>(s.bytes + p * 8 + 4);
			const u32 lower = *reinterpret_cast<const u32*>(s.bytes + p * 8);
			const bool ibit = (upper >> 31) & 1;
			const bool ebit = (upper >> 30) & 1;

			// Flag suffix: compact marker for E/I bits so the reader can
			// spot program-end and immediate-slot pairs at a glance.
			const char* flag_suffix =
				(ibit && ebit) ? " [EI]" :
				 ibit          ? " [I]"  :
				 ebit          ? " [E]"  : "";

			const std::string upper_s = disVU1MicroUF(upper, pc);
			if (ibit)
			{
				// I-bit: lower field is a 32-bit float immediate, not an opcode.
				float imm_f;
				std::memcpy(&imm_f, &lower, sizeof(imm_f));
				INFO_LOG("      [0x{:04x}] {:32} | IMM={:g} (0x{:08x}){}",
					pc, upper_s, imm_f, lower, flag_suffix);
			}
			else
			{
				const std::string lower_s = disVU1MicroLF(lower, pc);
				INFO_LOG("      [0x{:04x}] {:32} | {}{}",
					pc, upper_s, lower_s, flag_suffix);
			}

			pc = (pc + 8) & (VU1_PROGSIZE - 1);
		}
	}
}
#endif // VU1_PROFILE_BLOCKS

void recArmVU1::Shutdown()
{
	// VU1_PROFILE_BLOCKS dump is invoked from VMManager::Shutdown via
	// DumpProfile() — that fires on overlay Exit AND precedes the full
	// app-exit path that lands here, so dumping again would duplicate it.
	deleteAllVariants();
	s_pool.Destroy();
	s_code_base  = nullptr;
	s_code_write = nullptr;
	s_code_end   = nullptr;
}

void recArmVU1::DumpProfile()
{
#ifdef VU1_PROFILE_BLOCKS
	DumpTopBlocks();
#endif
}

void recArmVU1::Reset()
{
	VU1.fmacwritepos = 0;
	VU1.fmacreadpos  = 0;
	VU1.fmaccount    = 0;
	VU1.ialuwritepos = 0;
	VU1.ialureadpos  = 0;
	VU1.ialucount    = 0;

	deleteAllVariants();
	if (s_code_base)
		s_code_write = s_code_base;
	s_pool.Reset();
}

void recArmVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void recArmVU1::Step()
{
	VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
	vu1Exec(&VU1);
}

void recArmVU1::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

	VU1.VI[REG_TPC].UL <<= 3;
	const u64 startcycles = VU1.cycle;
	// Publish the cycle limit for the per-linkEntry budget check. Must be
	// set BEFORE the first block runs on this Execute call.
	s_vu1_cycle_limit   = startcycles + cycles;
	s_vu1_program_ended = false;

	// Phase 5 note on termination:
	//
	// Compiled blocks now check termination at linkEntry (see the gate in
	// CompileBlock right after the prologue). That catches ebit-done or
	// external FBRST clears mid-linked-chain and jumps straight to
	// budget_exceeded_exit, bypassing the rest of the block. The outer
	// loop's is_stopped() check still runs as the post-Ret re-entry gate
	// — without it a terminated block would Ret and we'd just dispatch it
	// again, since the cycle budget hasn't advanced past limit. Folding
	// both checks into the while condition keeps the body tight.
	auto is_stopped = [] {
		return THREAD_VU1
			? s_vu1_program_ended
			: !(VU0.VI[REG_VPU_STAT].UL & 0x100);
	};

	while ((VU1.cycle - startcycles) < cycles && !is_stopped())
	{
		const u32 pc   = VU1.VI[REG_TPC].UL & (VU1_PROGSIZE - 1);
		const u32 slot = pc / 8;

		// Content-keyed lookup: scan the slot's deque for a variant whose
		// snapshot matches live VU1.Micro at `pc`. A hit bubbles the variant
		// to deque front (MRU) so subsequent dispatches find it first.
		VU1BlockEntry* blk = findVariant(pc);

		if (!blk)
		{
			// Miss — compile a new variant. Allocate first so
			// CompileBlock has a stable out_block pointer for embedding
			// &out_block->execCount into the emitted code.
			const u32 numPairs = AnalyzeBlock(pc);
			blk            = new VU1BlockEntry{};
			blk->numPairs  = numPairs;

			// Snapshot the bytecode this variant will compile against, so
			// future dispatches can content-match against it even after
			// Clear() rewrites live VU1.Micro.
			const u32 snap_bytes = numPairs * 8;
			blk->snapshot = new u8[snap_bytes];
			std::memcpy(blk->snapshot, VU1.Micro + pc, snap_bytes);

			// CompileBlock populates blk->linkEntry, blk->returnExit, and
			// the Phase 2 link_* fields via the out_block pointer, then
			// returns the prologue address for blk->codeEntry.
			blk->codeEntry = CompileBlock(pc, numPairs, blk);

			// Cap the per-slot deque. Evict the LRU (back) before pushing
			// the new variant; destroyVariant unpatches any predecessors
			// that were linked to the evicted variant's linkEntry (they
			// fall through, then patchWaitingPredecessors below re-links
			// them to the new variant if the target_pc still matches).
			//
			// Eviction must happen BEFORE indexVariantExits(blk) so the
			// destroyVariant walk of s_waitingForSlot[slot] doesn't see
			// the new variant as a predecessor candidate of itself.
			auto& deque = s_variants[slot];
			if (deque.size() >= kVariantCapPerSlot)
			{
				VU1BlockEntry* victim = deque.back();
				deque.pop_back();
				destroyVariant(victim, slot);
			}

			deque.push_front(blk);
			indexVariantExits(blk);

			// Phase 2 block linking:
			//   1. Forward link — if this block's static exit target has
			//      a live (content-matching) variant, patch to it.
			//   2. Waiter patching — any previously-compiled blocks
			//      whose static target is THIS block's PC (and that are
			//      still falling through because no matching variant
			//      existed yet) get patched to jump to our linkEntry.
			// Batched icache flush: a freshly-compiled block at a hot PC
			// can have many waiters; per-patch flush would dominate the
			// post-compile cost.
			VU1IcacheBatch compile_batch;
			tryForwardLink(*blk, &compile_batch);
			patchWaitingPredecessors(pc, blk->linkEntry, &compile_batch);
			compile_batch.flush();
		}
		else if (blk->needsRelink)
		{
			// The variant survived a Clear() that unpatched its incoming
			// exit edges — re-wire the graph lazily on the first dispatch
			// post-Clear so repeated hits pay this cost only once. Batch
			// the icache flush across both relink calls and the eventual
			// codeEntry call below — must flush before the dispatch.
			VU1IcacheBatch flush_batch;
			tryForwardLink(*blk, &flush_batch);
			patchWaitingPredecessors(pc, blk->linkEntry, &flush_batch);
			flush_batch.flush();
			blk->needsRelink = false;
		}

		using BlockFn = void (*)();
		reinterpret_cast<BlockFn>(blk->codeEntry)();
	}

	// If termination interrupted us with a branch countdown pending
	// (transient branch==1 state between pair K-1 setting branch=2 and
	// pair K's step 12 decrementing it to 0), commit the pending branch
	// target to TPC so a future Execute resumes at the correct PC. In
	// the JIT path this is dead in practice — blocks always run to
	// completion once dispatched, and step 12 of the delay slot always
	// decrements branch to 0 — but we keep the fix-up for safety in
	// case of an external interrupt path that breaks the invariant.
	if (VU1.branch == 1)
	{
		VU1.VI[REG_TPC].UL = VU1.branchpc;
		VU1.branch         = 0;
	}

	VU1.VI[REG_TPC].UL >>= 3;
	VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}

void recArmVU1::Clear(u32 addr, u32 size)
{
	const u32 first        = addr / 8;
	const u32 last         = (addr + size + 7) / 8;
	const u32 clamped_last = std::min(last, VU1_NUM_SLOTS);

	if (first >= VU1_NUM_SLOTS)
		return;

	// Block linking invalidation: walk only the variants whose exits
	// target slots in the cleared range, via the reverse index. Without
	// this, a predecessor would still hold a dangling `B <freed_code>`
	// to a variant whose bytecode is about to be overwritten and take
	// it on next execution — executing code compiled against stale micro.
	//
	// Phase 3: each predecessor may have up to 2 active exits (conditional
	// branches link both taken and not-taken). Iterate pred->exits[] up
	// to num_exits and re-check target_pc per-exit, since a multi-exit
	// variant indexed under one cleared slot may still have a second exit
	// pointing OUTSIDE [first, clamped_last) which must not be unpatched.
	//
	// Batched icache flush: a wide Clear() (e.g., VIF MPG re-uploading the
	// whole micro) can touch hundreds of patch sites — per-patch flush
	// would pay 3 barriers each (~ms-scale lag). Defer + batch.
	VU1IcacheBatch flush_batch;
	for (u32 ts = first; ts < clamped_last; ts++)
	{
		for (VU1BlockEntry* pred : s_waitingForSlot[ts])
		{
			for (u32 e = 0; e < pred->num_exits; e++)
			{
				LinkExit& exit = pred->exits[e];
				if (!exit.patch_site || exit.target_pc == LINK_TARGET_NONE)
					continue;
				const u32 target_slot = exit.target_pc / 8;
				if (target_slot >= first && target_slot < clamped_last)
					unpatchLinkSite(exit, &flush_batch);
			}
		}
	}
	flush_batch.flush();

	// Mark variants in the cleared range as needing relink on next dispatch.
	// We deliberately do NOT delete them: if the EE re-uploads identical
	// bytes later (the common GOW2 thrash pattern), findVariant will match
	// against the preserved snapshot and reuse the compiled code without
	// re-emitting. On the first post-Clear dispatch the `needsRelink` flag
	// triggers tryForwardLink + patchWaitingPredecessors to re-wire exits.
	for (u32 i = first; i < clamped_last; i++)
	{
		for (VU1BlockEntry* blk : s_variants[i])
			blk->needsRelink = true;
	}
}
