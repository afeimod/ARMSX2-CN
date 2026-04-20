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
#include "GS.h"
#include "Gif_Unit.h"
#include "Memory.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "VUops.h"
#include "arm64/AsmHelpers.h"
#include "arm64/iVU1micro_arm64.h"

#include <cfenv>
#include <cstring>

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

// ============================================================================
//  Rec emitter dispatch tables (defined in iVU1Upper/Lower_arm64.cpp)
// ============================================================================

using VU1RecFn = void (*)();
extern VU1RecFn recVU1_UpperTable[64];
extern VU1RecFn recVU1_LowerTable[128];

// Flag-deferral state owned by iVU1Upper_arm64.cpp. Set per-pair before
// dispatching the upper emitter — when false, FMAC arithmetic emitters
// skip the BL vu1_fmac_writeback and inline a NEON clamp + store instead.
extern bool g_vu1NeedsFlags;
extern u32 g_vu1CurrentPC;

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
};

static VU1BlockEntry s_blocks[VU1_NUM_SLOTS];
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
// No XGKICK drain here: step 13 (this call) runs before step 14/15 within
// the same pair, so a pair-local pending kick is always drained by step 15
// or by the block-end drain in CompileBlock. And since vu1_XGKICK no longer
// touches VU1.xgkickenable, there's no legacy interpreter-style pending
// state to clean up on our behalf.
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
static void vu1_TestFMACStallReg(VURegs* VU, u32 reg, u32 xyzw)
{
	u32 i = 0;
	for (int currentpipe = VU->fmacreadpos; i < VU->fmaccount;
	     currentpipe = (currentpipe + 1) & 3, i++)
	{
		if ((VU->cycle - VU->fmac[currentpipe].sCycle) >= VU->fmac[currentpipe].Cycle)
			continue;

		if ((VU->fmac[currentpipe].regupper == reg && (VU->fmac[currentpipe].xyzwupper & xyzw))
			|| (VU->fmac[currentpipe].reglower == reg && (VU->fmac[currentpipe].xyzwlower & xyzw)))
		{
			u64 newCycle = VU->fmac[currentpipe].Cycle + VU->fmac[currentpipe].sCycle;
			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
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
static void vu1_TestPipes_VU1(VURegs* VU)
{
	// --- FMAC flush ---
	for (int i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
			break;

		if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

		if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30)
				| (VU->fmac[i].statusflag & 0xFC0)
				| (VU->fmac[i].statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0)
				| (VU->fmac[i].statusflag & 0xF)
				| ((VU->fmac[i].statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

		VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;
		VU->fmaccount--;
	}

	// --- FDIV flush ---
	if (VU->fdiv.enable != 0
		&& (VU->cycle - VU->fdiv.sCycle) >= VU->fdiv.Cycle)
	{
		VU->fdiv.enable = 0;
		VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
		VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF)
			| (VU->fdiv.statusflag & 0xC30);
	}

	// --- EFU flush ---
	if (VU->efu.enable != 0
		&& (VU->cycle - VU->efu.sCycle) >= VU->efu.Cycle)
	{
		VU->efu.enable = 0;
		VU->VI[REG_P].UL = VU->efu.reg.UL;
	}

	// --- IALU flush (pop only, no flag writes) ---
	for (int i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
			break;
		VU->ialureadpos = (VU->ialureadpos + 1) & 3;
		VU->ialucount--;
	}
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
	VU1regs_LOWER_OPCODE[lower >> 25](&lregs);
	return lregs.pipe == VUPIPE_BRANCH;
}

static u32 AnalyzeBlock(u32 startPC)
{
	u32 pairs = 0;
	u32 pc    = startPC;

	while (pairs < VU1_MAX_BLOCK_PAIRS)
	{
		const bool ebit   = PairHasEbit(pc);
		const bool branch = PairHasBranch(pc);

		pairs++;
		pc = (pc + 8) & (VU1_PROGSIZE - 1);

		if (ebit || branch)
		{
			// Include the one delay-slot pair then stop.
			pairs++;
			break;
		}
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

// Rewrite a single LinkExit's patch site to jump to `target` (typically
// another block's linkEntry, or the exit's own fallthrough address for
// unpatching). No-op if the slot already points at `target`. Handles
// I-cache coherency via armEmitJmpPtr's internal FlushInstructionCache.
//
// The single-thread assumption: this is called from recArmVU1::Execute
// (block compile path) or recArmVU1::Clear (invalidation). Both run on the
// same thread that eventually executes compiled blocks — either the EE
// thread (non-MTVU) or the MTVU thread (MTVU via Clear from EE needs
// external serialization, same as the pre-existing codeEntry=nullptr
// invalidation). Intra-thread patching is safe with armEmitJmpPtr's
// FlushInstructionCache call; we don't add extra barriers here.
static void patchLinkSite(LinkExit& exit, u8* target)
{
	if (!exit.patch_site)
		return;
	if (exit.current_target == target)
		return;
	armEmitJmpPtr(exit.patch_site, target, true);
	exit.current_target = target;
}

// Restore an exit's patch site to its unlinked fallthrough target.
// For num_exits==1 (Phase 2 unconditional), fallthrough is the flush
// path immediately after the B. For num_exits==2 (Phase 3 conditional)
// both exits' fallthrough is ALSO the flush path — exits[1]'s B.eq
// fallthrough skips past exits[0]'s B to reach it.
static void unpatchLinkSite(LinkExit& exit)
{
	if (!exit.patch_site)
		return;
	patchLinkSite(exit, exit.fallthrough);
}

// Called right after a block compiles: for each active exit, if its static
// target is already compiled, patch that exit's slot to jump directly.
static void tryForwardLink(VU1BlockEntry& block)
{
	for (u32 e = 0; e < block.num_exits; e++)
	{
		LinkExit& exit = block.exits[e];
		if (exit.target_pc == LINK_TARGET_NONE)
			continue;
		const u32 target_slot = exit.target_pc / 8;
		const VU1BlockEntry& target = s_blocks[target_slot];
		if (target.linkEntry)
			patchLinkSite(exit, target.linkEntry);
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
// This is the full "cache" — s_blocks itself IS the lookup table. No
// per-block jumpCache state is needed: the inline lookup at every
// indirect exit reads the live s_blocks[slot].linkEntry, which stays in
// sync with invalidation (Clear zeroes linkEntry → helper returns null →
// falls through to dispatcher).
static u8* vu1_indirect_dispatch(u32 tpc)
{
	const u32 slot = (tpc & VU1_PROGMASK) >> 3;
	return s_blocks[slot].linkEntry;
}

// Called right after a block compiles at `my_pc` with `my_linkEntry`: scan
// all blocks for predecessors that have an exit whose static target is
// `my_pc` and whose patch site is still pointing at its fallthrough (not
// yet linked to us). Patch them forward.
//
// Complexity: O(VU1_NUM_SLOTS * max_exits) per compile — 2048 * 2 ≤ 4096
// pointer-compares per new block. Compilation is amortized (each block
// compiles at most once between Clear()s) so total cost is bounded. A
// target_pc → predecessor_list multimap would reduce this to O(1) per
// link but complicates Clear semantics; defer until profile-driven.
static void patchWaitingPredecessors(u32 my_pc, u8* my_linkEntry)
{
	if (!my_linkEntry)
		return;
	for (u32 i = 0; i < VU1_NUM_SLOTS; i++)
	{
		VU1BlockEntry& pred = s_blocks[i];
		for (u32 e = 0; e < pred.num_exits; e++)
		{
			LinkExit& exit = pred.exits[e];
			if (exit.patch_site != nullptr
			    && exit.target_pc == my_pc
			    && exit.current_target != my_linkEntry)
			{
				patchLinkSite(exit, my_linkEntry);
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

// Phase-6 opt #1: pinned pointers to the linkEntry gate globals. Both are
// callee-saved (x19-x28 per AAPCS64). Loaded once at block prologue,
// restored at epilogue. Collapses each linkEntry budget+termination check
// from ~9 instructions (two adrp+add+ldr sequences) down to ~5 (direct
// Ldr via the pinned base). Since every linked-chain block entry runs
// this gate, the compounded savings are significant.
//
//   x26 → &s_vu1_cycle_limit  (u64)
//   x27 → &s_vu1_program_ended (THREAD_VU1) OR &VU0 (!THREAD_VU1,
//         used with vpu_stat_off for the VPU_STAT 0x100 test)
static const auto VU1_CYCLE_LIMIT_ADDR_REG = x26;
static const auto VU1_TERM_ADDR_REG        = x27;

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
static void emitFMACStallChecks(const _VURegsNum& regs, bool skip0, bool skip1)
{
	const int64_t cycle_off = (int64_t)offsetof(VURegs, cycle);

	if (!skip0 && regs.VFread0 != 0)
	{
		emitFlushCycleReg(cycle_off);
		armAsm->Mov(x0, VU1_BASE_REG);
		armAsm->Mov(w1, regs.VFread0);
		armAsm->Mov(w2, regs.VFr0xyzw);
		armEmitCall(reinterpret_cast<const void*>(vu1_TestFMACStallReg));
		emitReloadCycleReg(cycle_off);
	}
	if (!skip1 && regs.VFread1 != 0)
	{
		emitFlushCycleReg(cycle_off);
		armAsm->Mov(x0, VU1_BASE_REG);
		armAsm->Mov(w1, regs.VFread1);
		armAsm->Mov(w2, regs.VFr1xyzw);
		armEmitCall(reinterpret_cast<const void*>(vu1_TestFMACStallReg));
		emitReloadCycleReg(cycle_off);
	}
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
				emitFlushCycleReg(cycle_off);
				armAsm->Mov(x0, VU1_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(vu1_TestFDIVPipeWait));
				emitReloadCycleReg(cycle_off);
			}
			break;
		case VUPIPE_EFU:
			emitFMACStallChecks(lregs, skipFMACStall0, skipFMACStall1);
			if (!skipEFUWait)
			{
				emitFlushCycleReg(cycle_off);
				armAsm->Mov(x0, VU1_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(vu1_TestEFUPipeWait));
				emitReloadCycleReg(cycle_off);
			}
			break;
		case VUPIPE_BRANCH:
			// Unconditional B/BAL have VIread == 0; the ALU stall test
			// would be a no-op, so skip the BL entirely.
			if (!skipALUStall && lregs.VIread != 0)
			{
				emitFlushCycleReg(cycle_off);
				armAsm->Mov(x0, VU1_BASE_REG);
				armAsm->Mov(w1, lregs.VIread);
				armEmitCall(reinterpret_cast<const void*>(vu1_TestALUStallReg));
				emitReloadCycleReg(cycle_off);
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
	const int64_t fmaccount_off  = (int64_t)offsetof(VURegs, fmaccount);
	const int64_t macflag_off    = (int64_t)offsetof(VURegs, macflag);
	const int64_t statusflag_off = (int64_t)offsetof(VURegs, statusflag);
	// clipflag is loaded paired with statusflag via Ldp, so no dedicated
	// offset is needed — the field layout consistency is asserted below.
	static_assert(offsetof(VURegs, clipflag) == offsetof(VURegs, statusflag) + 4,
		"Ldp(statusflag, clipflag) requires adjacent u32 layout in VURegs");
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

	// Cycle(const 4) + macflag(loaded from VU->macflag) → one Stp.
	armAsm->Mov(w5, 4);
	armAsm->Ldr(w6, MemOperand(VU1_BASE_REG, macflag_off));
	armAsm->Stp(w5, w6, MemOperand(x7, f_Cycle));

	// statusflag + clipflag: paired via Ldp + Stp. VURegs lays out macflag,
	// statusflag, clipflag as consecutive u32 (see VU.h), so Ldp from
	// &VU->statusflag yields statusflag + clipflag.
	//
	// statusflag_off is ~VU offset 1188 which is outside Ldp's imm7 range
	// (±256 for 32-bit pair), so we compute a base scratch first.
	armAsm->Add(x8, VU1_BASE_REG, statusflag_off);
	armAsm->Ldp(w5, w6, MemOperand(x8));
	armAsm->Stp(w5, w6, MemOperand(x7, f_statusflag));

	// fmaccount++
	armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, fmaccount_off));
	armAsm->Add(w4, w4, 1);
	armAsm->Str(w4, MemOperand(VU1_BASE_REG, fmaccount_off));
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
				const int64_t statusflag_off = (int64_t)offsetof(VURegs, statusflag);
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
				// statusflag = VU->statusflag
				armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, statusflag_off));
				armAsm->Str(w4, MemOperand(VU1_BASE_REG, fdiv_off + d_statusflag));
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

static u8* CompileBlock(u32 startPC, u32 numPairs, VU1BlockEntry* out_block)
{
	// --- Size check ---
	const size_t data_size    = numPairs * 2 * sizeof(_VURegsNum);
	const size_t code_worst   = static_cast<size_t>(numPairs) * 512 + 64;
	const size_t total_needed = data_size + code_worst;

	if (static_cast<size_t>(s_code_end - s_code_write) < total_needed)
	{
		DEV_LOG("VU1 JIT: code buffer full, resetting");
		std::memset(s_blocks, 0, sizeof(s_blocks));
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

	// Block-level flags driving per-emit gating below. All zero-cost to
	// accumulate here — we already read `upper` / decode `lregs` for the
	// regs pre-walk.
	//   block_has_ebit         / block_has_dbit_or_tbit : step 13 countdown.
	//   block_has_ibxx         : only IBxx reads VIBackupCycles via
	//                            emitHazardVIRead (JR/JALR skip the hazard
	//                            path despite `VIread != 0`). Opcodes per
	//                            VUops.cpp LOWER_OPCODE[128]: IBEQ=0x28,
	//                            IBNE=0x29, IBLTZ=0x2C, IBGTZ=0x2D,
	//                            IBLEZ=0x2E, IBGEZ=0x2F.
	//   block_has_vi_backup_set: any lower whose emitter calls emitBackupVI
	//                            (IADD/ISUB/IALU-imm/IAND/IOR/LQD/LQI/SQD/
	//                            SQI/ILWR/MTIR). Overapproximated as
	//                            "writes VI[0..15] and pipe != BRANCH" —
	//                            that includes FSAND/FMAND/FCAND/FCGET
	//                            (flag-test ops that write VI but never
	//                            touch VIBackupCycles). False positives
	//                            only keep step 6b's decrement emits, so
	//                            the overapproximation is soundness-safe.
	//                            BAL/JALR are the intentional exclusions
	//                            (write VI without emitBackupVI).
	bool block_has_ebit           = false;
	bool block_has_dbit_or_tbit   = false;
	bool block_has_ibxx           = false;
	bool block_has_vi_backup_set  = false;
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

				const u32 lopc = lower >> 25;
				const bool is_IBxx =
					lopc == 0x28u || lopc == 0x29u ||          // IBEQ, IBNE
					(lopc >= 0x2Cu && lopc <= 0x2Fu);           // IBLTZ/GTZ/LEZ/GEZ
				block_has_ibxx |= is_IBxx;

				const _VURegsNum& lregs_i = lregs_data[i];
				if ((lregs_i.VIwrite & 0xFFFFu) != 0u && lregs_i.pipe != VUPIPE_BRANCH)
					block_has_vi_backup_set = true;
			}
			// I-bit pairs: lregs_data[i] stays zeroed (no lower instruction).

			block_has_ebit         |= ((upper >> 30) & 1) != 0;
			block_has_dbit_or_tbit |= ((upper >> 28) & 1) != 0;
			block_has_dbit_or_tbit |= ((upper >> 27) & 1) != 0;

			pc = (pc + 8) & (VU1_PROGSIZE - 1);
		}
	}

	// Step 6b (VIBackupCycles decrement) is observable only when some pair
	// in the block reads VIBackupCycles — i.e., has an IBxx. If no IBxx,
	// the per-pair decrement is dead within this block; the only concern
	// is cross-block state leaking. Two cases:
	//
	//   (1) !block_has_ibxx && !block_has_vi_backup_set:
	//       No writes in this block either. Entry VIBackupCycles is at most
	//       2 (max value set by emitBackupVI). numPairs >= 2 (AnalyzeBlock
	//       always includes a delay-slot pair, and stalls can only increase
	//       elapsed cycles — never shrink them), so the natural per-pair
	//       decrement would reach 0 before block exit. Eliding the
	//       decrements and clamping VIBackupCycles to 0 at block end is
	//       equivalent behavior for any downstream block.
	//
	//   (2) !block_has_ibxx && block_has_vi_backup_set:
	//       In-block write sets VIBackupCycles=2 at some pair. A clamp-to-0
	//       at exit would drop a still-live hazard for the next block's
	//       IBxx. Keep the per-pair decrement.
	//
	// So we elide only in case (1).
	const bool skip_vibackup_decrement = !block_has_ibxx && !block_has_vi_backup_set;

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
	bool pair_needs_flags[VU1_MAX_BLOCK_PAIRS];
	{
		constexpr u32 FLAG_READ_MASK = (1u << REG_MAC_FLAG)
		                              | (1u << REG_STATUS_FLAG)
		                              | (1u << REG_CLIP_FLAG);
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
				if (fmacFromEnd < 4 || sawFlagReader)
					needsFlags = true;
				fmacFromEnd++;
			}
			pair_needs_flags[i] = needsFlags;

			// Update sawFlagReader for the NEXT (earlier) iteration. The
			// current pair's own flag read does not pull its own flag write
			// — pipe latency means a same-pair FMxxx reads VI[FLAG] from
			// 4+ cycles ago, not the upper FMAC's just-now-written value.
			if ((uregs.VIread | lregs.VIread) & FLAG_READ_MASK)
				sawFlagReader = true;
		}
	}

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
	// 64-byte frame (Stage C3 expanded from 48):
	//   [sp+0..7]   = x29 (fp)
	//   [sp+8..15]  = x30 (lr)
	//   [sp+16..23] = x21 (VU1_CYCLE_REG — Stage C2 cached VU->cycle)
	//   [sp+24..31] = x22 (cyclesBefore scratch)
	//   [sp+32..39] = x23 (VU1_BASE_REG)
	//   [sp+40..47] = x24 (VU1_FMAC_WPOS_REG — Stage C3 cached fmacwritepos)
	//   [sp+48..55] = x25 (VU1_IALU_WPOS_REG — Stage C3 cached ialuwritepos)
	//   [sp+56..63] = x26 (VU1_CYCLE_LIMIT_ADDR_REG — opt #1 pinned gate addr)
	//   [sp+64..71] = x27 (VU1_TERM_ADDR_REG           — opt #1 pinned gate addr)
	//   [sp+72..79] = unused pad for 16-byte alignment
	armAsm->Stp(x29, x30, MemOperand(sp, -80, PreIndex));
	armAsm->Stp(VU1_CYCLE_REG, x22, MemOperand(sp, 16));
	armAsm->Stp(VU1_BASE_REG, x24, MemOperand(sp, 32));
	armAsm->Stp(x25, VU1_CYCLE_LIMIT_ADDR_REG, MemOperand(sp, 48));
	armAsm->Str(VU1_TERM_ADDR_REG, MemOperand(sp, 64));
	armAsm->Mov(x29, sp);
	armMoveAddressToReg(VU1_BASE_REG, &VU1);
	// Opt #1: pin the linkEntry gate addresses. Loaded once per block;
	// every codeEntry+linkEntry pair amortizes the cost across all linked
	// entries within the chain.
	armMoveAddressToReg(VU1_CYCLE_LIMIT_ADDR_REG, &s_vu1_cycle_limit);
	if (THREAD_VU1)
		armMoveAddressToReg(VU1_TERM_ADDR_REG, &s_vu1_program_ended);
	else
		armMoveAddressToReg(VU1_TERM_ADDR_REG, &VU0);

	// Compile-time constants for field offsets used throughout the loop.
	const int64_t cycle_off     = (int64_t)offsetof(VURegs, cycle);
	const int64_t code_off      = (int64_t)offsetof(VURegs, code);
	const int64_t branch_off    = (int64_t)offsetof(VURegs, branch);
	const int64_t branchpc_off  = (int64_t)offsetof(VURegs, branchpc);
	const int64_t ebit_off      = (int64_t)offsetof(VURegs, ebit);
	const int64_t tpc_off       = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_TPC * (int64_t)sizeof(REG_VI));
	const int64_t regi_off      = (int64_t)((int64_t)offsetof(VURegs, VI) + REG_I   * (int64_t)sizeof(REG_VI));
	const int64_t fmacwpos_off  = (int64_t)offsetof(VURegs, fmacwritepos);
	const int64_t ialuwpos_off  = (int64_t)offsetof(VURegs, ialuwritepos);
	const int64_t vibackup_off  = (int64_t)offsetof(VURegs, VIBackupCycles);

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

	// Block-linking (Phase 1+): record the address of the first instruction
	// past the prologue. Linked predecessors B here directly, skipping the
	// prologue. At this point x21/x23/x24/x25 are live — caller's regs
	// trusted (same ABI as the fall-through from codeEntry above).
	out_block->linkEntry = armGetCurrentCodePointer();

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
	// Opt #1: both gate-target addresses are pre-pinned in
	// VU1_CYCLE_LIMIT_ADDR_REG (x26) and VU1_TERM_ADDR_REG (x27) by the
	// prologue. linkEntry collapses to Ldr + Cmp + B per check.
	{
		// 1. Cycle budget.
		armAsm->Ldr(x5, MemOperand(VU1_CYCLE_LIMIT_ADDR_REG));
		armAsm->Cmp(VU1_CYCLE_REG, x5);
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
	// emitted code. recArmVU1::Reset() flushes s_blocks on gamefix toggle
	// (via VMManager::ApplySettings), so a block's hackmode binding is
	// stable for the block's cached lifetime.
	//
	// Under hackmode the scratch-based pending_xgkick_fire mechanism is
	// disabled entirely — both JIT and interp agree on VU1.xgkick* state
	// management (see the C-1 comment block in iVU1Lower_arm64.cpp), so
	// the step 8a / step 15 / block-end / hazard-capture-from-interp
	// paths below are all guarded with `!xgkickhack`.
	const bool xgkickhack = CHECK_XGKICKHACK;

	// Hackmode pre-walk: compute per-pair kickcycles (cycles to sync into
	// the paced XGKICK transfer at this pair's memwrite boundary).
	// Accumulates 1 per pair post-XGKICK, commits + resets on memwrite
	// pairs. Mirrors mVUregs.xgkickcycles / mVUlow.kickcycles accumulation
	// at microVU_Compile.inl:779-786.
	//
	// Note: we count 1 cycle per pair rather than upstream's `1 + stall`
	// — exact stall counts aren't tracked per-pair in this rec and
	// under-counting is the conservative choice (sync fires slightly too
	// early rather than missing bytes). Games relying on this hack tend
	// not to stall heavily in XGKICK-adjacent code.
	u32 kick_cycles_sync[VU1_MAX_BLOCK_PAIRS] = {};
	if (xgkickhack)
	{
		u32 accum = 0;
		u32 pc_walk = startPC;
		for (u32 i = 0; i < numPairs; i++)
		{
			const u32 upper_w = *reinterpret_cast<const u32*>(VU1.Micro + pc_walk + 4);
			const u32 lower_w = *reinterpret_cast<const u32*>(VU1.Micro + pc_walk);
			const bool ibit_w = (upper_w >> 31) & 1;

			if (!ibit_w && isXgkickOp(lower_w))
			{
				// XGKICK op handles its own sync internally (the helper
				// calls _vuXGKICKTransfer(0, true) to drain any prior kick
				// before installing new state). Reset accumulator to 1:
				// the XGKICK itself counts as 1 cycle toward the NEW
				// kick's pacing — matches VUops.cpp:1934.
				accum = 1;
			}
			else if (!ibit_w && isMemWriteOp(lower_w))
			{
				accum += 1;
				kick_cycles_sync[i] = accum;
				accum = 0;
			}
			else
			{
				accum += 1;
			}

			pc_walk = (pc_walk + 8) & (VU1_PROGSIZE - 1);
		}
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
	u32 pc = startPC;
	for (u32 i = 0; i < numPairs; i++)
	{
		const u32 upper     = *reinterpret_cast<const u32*>(VU1.Micro + pc + 4);
		const u32 lower     = *reinterpret_cast<const u32*>(VU1.Micro + pc);
		const bool ibit     = (upper >> 31) & 1;
		const bool ebit_set = (upper >> 30) & 1;
		const bool dbit_set = (upper >> 28) & 1;
		const bool tbit_set = (upper >> 27) & 1;
		const _VURegsNum& uregs = uregs_data[i];
		const _VURegsNum& lregs = lregs_data[i];

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
		const bool vf_hazard = !ibit && uregs.VFwrite != 0 &&
			(lregs.VFwrite == uregs.VFwrite ||
			 lregs.VFread0 == uregs.VFwrite ||
			 lregs.VFread1 == uregs.VFwrite);
		const bool vi_hazard = !ibit &&
			(uregs.VIwrite & (1u << REG_CLIP_FLAG)) &&
			((lregs.VIwrite & (1u << REG_CLIP_FLAG)) ||
			 (lregs.VIread  & (1u << REG_CLIP_FLAG)));

		if (vf_hazard || vi_hazard)
		{
			// Full interpreter fallback for this pair. vu1Exec runs a complete
			// interpreter pair, including the _vuTest*/_vuAdd* pipeline helpers
			// which read AND write VU->cycle — flush x21 first, reload after.
			// Stage C3: vu1Exec's inner driver loop (_vu1Exec in
			// VU1microInterp.cpp) also advances fmacwritepos AND _vuAddIALUStalls
			// advances ialuwritepos, so x24/x25 must be flushed+reloaded
			// across this BL too.
			emitFlushCycleReg(cycle_off);
			emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			armAsm->Mov(x0, VU1_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(vu1Exec));
			emitReloadCycleReg(cycle_off);
			emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
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
					armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
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
				if (isXgkickOp(lower))
				{
					armAsm->Mov(x0, VU1_BASE_REG);
					armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_capture_from_interp));
					pending_xgkick_fire = true;
				}
			}
			pc = (pc + 8) & (VU1_PROGSIZE - 1);
			continue;
		}

		// 1. VU->cycle++ — Stage C2 uses the cached VU1_CYCLE_REG (x21).
		//    x22 latches "cycle before this pair" for the VIBackupCycles
		//    decrement at step 6b. Both x21 and x22 are callee-saved and
		//    already saved/restored in our prologue/epilogue. No memory
		//    store here; the block-end flush writes x21 to VU->cycle once.
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

		// 4. D/T bits: depend on VU0 FBRST (runtime). Only emit when actually set.
		if (dbit_set || tbit_set)
		{
			armAsm->Mov(w0, upper);
			armEmitCall(reinterpret_cast<const void*>(vu1CheckDTBits));
		}

		// 5. Test upper stalls — compile-time-specialized inline. Most upper
		//    instructions are non-FMAC and emit zero work here. Stage A uses
		//    skip_info[i] to elide FMAC stall-check BLs when the compile-time
		//    ring buffer proves no alias exists.
		emitTestUpperStalls(uregs,
			skip_info[i].skipUpperFMACStall0,
			skip_info[i].skipUpperFMACStall1);

		// 5b. Test lower stalls BEFORE TestPipes (non-I-bit only).
		//     TestLowerStalls may advance VU->cycle (FDIV/EFU/ALU stalls);
		//     TestPipes needs to see the updated cycle to flush FMAC correctly.
		//     Stage B adds FDIV/EFU/ALU wait skip flags.
		if (!ibit)
			emitTestLowerStalls(lregs,
				skip_info[i].skipLowerFMACStall0,
				skip_info[i].skipLowerFMACStall1,
				skip_info[i].skipLowerFDIVWait,
				skip_info[i].skipLowerEFUWait,
				skip_info[i].skipLowerALUStall);

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
			emitFlushCycleReg(cycle_off);
			armAsm->Mov(x0, VU1_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(vu1_TestPipes_VU1));
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
			armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_hack_sync));
		}

		// 7. Execute upper instruction.
		//    Set VU->code at runtime (interpreter reads it for register fields).
		//    Set VU1.code at compile time so the rec emitter resolves the correct
		//    interpreter function pointer via VU1_UPPER_OPCODE[code & 0x3f].
		armAsm->Mov(w4, upper);
		armAsm->Str(w4, MemOperand(VU1_BASE_REG, code_off));
		VU1.code = upper; // compile-time context for the rec emitter
		g_vu1NeedsFlags = pair_needs_flags[i]; // flag-deferral hint for FMAC emitters
		recVU1_UpperTable[upper & 0x3f](); // emits BL to specific interpreter fn

		// 8. Lower instruction handling.
		if (ibit)
		{
			// I-bit: lower field is a float immediate — load into VI[REG_I].
			armAsm->Mov(w4, lower);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, regi_off));
		}
		else
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
			if (!xgkickhack && pending_xgkick_fire && isXgkickOp(lower))
			{
				armAsm->Mov(x0, VU1_BASE_REG);
				armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
				pending_xgkick_fire = false;
			}
			// Execute lower instruction (stalls already tested above).
			armAsm->Mov(w4, lower);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, code_off));
			VU1.code = lower; // compile-time context
			g_vu1CurrentPC = pc; // compile-time PC for native branches

			// Hackmode XGKICK: recVU1_XGKICK emits a BL to
			// vu1_XGKICK_hack_capture which internally runs
			// _vuXGKICKTransfer(0, true). That writes VU1.cycle (adds
			// transfersize/8 per iteration under flush) and invokes
			// _vuTestPipes (reads/writes FMAC + IALU pipeline state), so
			// flush x21/x24/x25 before the BL and reload after. Non-hack
			// XGKICK (vu1_XGKICK) only writes s_vu1_pending_xgkick_addr,
			// so no flush is needed there.
			const bool hack_xgkick_here = xgkickhack && isXgkickOp(lower);
			if (hack_xgkick_here)
			{
				emitFlushCycleReg(cycle_off);
				emitFlushWposRegs(fmacwpos_off, ialuwpos_off);
			}
			recVU1_LowerTable[lower >> 25](); // emits BL to specific interpreter fn
			if (hack_xgkick_here)
			{
				emitReloadCycleReg(cycle_off);
				emitReloadWposRegs(fmacwpos_off, ialuwpos_off);
			}
		}

		// 9-11. FMAC clear + AddUpperStalls + AddLowerStalls fused.
		//       emitFMACAddPair handles ClearFMAC + the FMAC sides of
		//       AddUpper/AddLowerStalls in a single BL (skipped entirely
		//       when neither side is FMAC). emitLowerNonFMACAdd handles
		//       FDIV/EFU/IALU adds for non-FMAC lower pipes.
		//       For I-bit pairs lregs is all-zero (pipe == VUPIPE_NONE),
		//       so passing it directly is safe — both helpers no-op on it.
		emitFMACAddPair(uregs, lregs);
		if (!ibit)
			emitLowerNonFMACAdd(lregs);

		// 12. Branch countdown (inline).
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
			armAsm->Mov(x0, VU1_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(vu1HandleDelayBranch));
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
		if (block_has_ebit || block_has_dbit_or_tbit)
		{
			Label skip_ebit;
			armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, ebit_off));
			armAsm->Cbz(w4, &skip_ebit);          // ebit == 0: nothing to do
			armAsm->Subs(w4, w4, 1);
			armAsm->Str(w4, MemOperand(VU1_BASE_REG, ebit_off));
			armAsm->B(&skip_ebit, ne);             // still > 0: keep counting
			// ebit just reached 0: end of microprogram
			emitFlushCycleReg(cycle_off);
			armAsm->Mov(x0, VU1_BASE_REG);
			armEmitCall(reinterpret_cast<const void*>(vu1EbitDone));
			emitReloadCycleReg(cycle_off);
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
				armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
				pending_xgkick_fire = false;
			}
			// Re-arm for the next pair if this one captured an XGKICK.
			if (!ibit && isXgkickOp(lower))
				pending_xgkick_fire = true;
		}

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
		armEmitCall(reinterpret_cast<const void*>(vu1_XGKICK_fire_deferred));
	}

	// Step 6b block-end clamp. Elision of per-pair VIBackupCycles decrement
	// requires this single Strb(wzr) at the fall-through exit to match the
	// natural decrement's end state. Only fires on normal block completion —
	// the budget_exceeded_exit path (linkEntry cycle/termination gate)
	// bypasses the per-pair body entirely and correctly preserves
	// VIBackupCycles untouched, matching "did not execute any pair" semantics.
	if (skip_vibackup_decrement)
		armAsm->Strb(wzr, MemOperand(VU1_BASE_REG, vibackup_off));

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
		// dispatcher helper returns the live s_blocks[tpc/8].linkEntry
		// which self-heals through Clear() (invalidation zeroes linkEntry
		// and the next miss falls through to Execute's outer dispatch).
		//
		// x0 is caller-saved — we use it for both the call arg and the
		// return value. BL preserves x21/x23/x24/x25 (all x19-x28 are
		// callee-saved per AAPCS64), so cached regs survive across the
		// helper call and remain live for a tail-Br into linkEntry.
		armAsm->Ldr(w4, MemOperand(VU1_BASE_REG, tpc_off));
		armAsm->Mov(w0, w4);
		armEmitCall(reinterpret_cast<const void*>(vu1_indirect_dispatch));
		Label indirect_fall_through;
		armAsm->Cbz(x0, &indirect_fall_through);
		armAsm->Br(x0);
		armAsm->Bind(&indirect_fall_through);
	}

	// Budget-exceeded entries from the cycle check at linkEntry land here,
	// skipping both the per-pair loop body and the exit selector. Falls
	// straight into the flush+epilogue+Ret path below.
	armAsm->Bind(&budget_exceeded_exit);

	// Stage C2: flush the cached cycle register to memory before restoring
	// the caller's x21. From here on VU->cycle is authoritative again.
	emitFlushCycleReg(cycle_off);
	// Stage C3: flush the cached FMAC/IALU write-position registers to
	// memory before restoring the caller's x24/x25.
	emitFlushWposRegs(fmacwpos_off, ialuwpos_off);

	// --- Epilogue (80-byte frame; mirrors the prologue layout above) ---
	armAsm->Ldr(VU1_TERM_ADDR_REG, MemOperand(sp, 64));
	armAsm->Ldp(x25, VU1_CYCLE_LIMIT_ADDR_REG, MemOperand(sp, 48));
	armAsm->Ldp(VU1_BASE_REG, x24, MemOperand(sp, 32));
	armAsm->Ldp(VU1_CYCLE_REG, x22, MemOperand(sp, 16));
	armAsm->Ldp(x29, x30, MemOperand(sp, 80, PostIndex));
	armAsm->Ret();

	u8* end = armEndBlock();
	s_code_write = end;
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

	std::memset(s_blocks, 0, sizeof(s_blocks));
}

void recArmVU1::Shutdown()
{
	s_pool.Destroy();
	s_code_base  = nullptr;
	s_code_write = nullptr;
	s_code_end   = nullptr;
	std::memset(s_blocks, 0, sizeof(s_blocks));
}

void recArmVU1::Reset()
{
	VU1.fmacwritepos = 0;
	VU1.fmacreadpos  = 0;
	VU1.fmaccount    = 0;
	VU1.ialuwritepos = 0;
	VU1.ialureadpos  = 0;
	VU1.ialucount    = 0;

	std::memset(s_blocks, 0, sizeof(s_blocks));
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

		VU1BlockEntry& blk = s_blocks[slot];

		if (!blk.codeEntry)
		{
			const u32 numPairs = AnalyzeBlock(pc);
			blk.numPairs  = numPairs;
			// CompileBlock populates blk.linkEntry, blk.returnExit, and
			// the Phase 2 link_* fields via the out_block pointer, then
			// returns the prologue address for blk.codeEntry.
			blk.codeEntry = CompileBlock(pc, numPairs, &blk);

			// Phase 2 block linking:
			//   1. Forward link — if this block's static exit target
			//      is already compiled, patch our slot to jump there.
			//   2. Waiter patching — any previously-compiled blocks
			//      whose static target is THIS block's PC (and that
			//      are still falling through because we weren't
			//      compiled yet) get patched to jump to our linkEntry.
			tryForwardLink(blk);
			patchWaitingPredecessors(pc, blk.linkEntry);
		}

		using BlockFn = void (*)();
		reinterpret_cast<BlockFn>(blk.codeEntry)();
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

	// Block linking invalidation: before zeroing each cleared block's
	// codeEntry/linkEntry, walk the full block table and un-patch any
	// predecessor exit that currently jumps into the about-to-be-freed
	// linkEntry. Without this, a predecessor would still hold a dangling
	// `B <freed_code>` and take it on next execution.
	//
	// Phase 3: each predecessor may have up to 2 active exits (conditional
	// branches link both taken and not-taken). Iterate pred.exits[] up to
	// num_exits.
	for (u32 i = 0; i < VU1_NUM_SLOTS; i++)
	{
		VU1BlockEntry& pred = s_blocks[i];
		for (u32 e = 0; e < pred.num_exits; e++)
		{
			LinkExit& exit = pred.exits[e];
			if (!exit.patch_site || exit.target_pc == LINK_TARGET_NONE)
				continue;
			const u32 target_slot = exit.target_pc / 8;
			if (target_slot >= first && target_slot < clamped_last)
				unpatchLinkSite(exit);
		}
	}

	for (u32 i = first; i < clamped_last; i++)
	{
		VU1BlockEntry& blk = s_blocks[i];
		blk.codeEntry  = nullptr;
		blk.linkEntry  = nullptr;
		blk.returnExit = nullptr;
		blk.num_exits  = 0;
		for (u32 e = 0; e < 2; e++)
		{
			blk.exits[e].target_pc      = LINK_TARGET_NONE;
			blk.exits[e].patch_site     = nullptr;
			blk.exits[e].fallthrough    = nullptr;
			blk.exits[e].current_target = nullptr;
		}
	}
}
