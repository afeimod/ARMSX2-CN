// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0
//
// ARM64 VU1 microIR — derived per-pair info layered on top of the existing
// _VURegsNum arrays the recompiler already populates per CompileBlock.
//
// Foundation for follow-on optimizations that need cross-pair analysis:
//   - doSwapOp / VF-VF hazard native fast-paths (eliminates the vu1Exec
//     fallback for pairs the old port-in-place handled natively)
//   - Cycle pre-summation, doDivFlag exact maturity
//   - analyzeBranchVI (VI backup detection)
//   - constJump (JR/JALR target const-prop)
//   - flag pipeline rewrite (s/m/c flag instance walks)
//
// Design choice: this is a thin overlay on the existing _VURegsNum data,
// not a wholesale port of the old port's microIR<pSize>. The arm64 emit
// loop already populates uregs_data[] / lregs_data[] in CompileBlock; the
// IR derives the higher-level "what should the emitter do for this pair"
// flags from those plus the raw upper/lower instruction words.
//
// Lifecycle: reset at CompileBlock entry, populated by mvu1AnalyzeBlock
// once the _VURegsNum arrays are filled, consumed by the emit pass.

#pragma once

#include "VUmicro.h"

namespace armvu1ir
{

// Branch encoding mirrors the old microLowerOp::branch field. Keeps a single
// vocabulary for "what kind of branch is this lower" across all consumers.
enum BranchKind : u8
{
	BR_NONE  = 0,
	BR_B     = 1,  // unconditional B
	BR_BAL   = 2,  // unconditional with link
	BR_IBEQ  = 3,
	BR_IBNE  = 4,
	BR_IBLTZ = 5,
	BR_IBGTZ = 6,
	BR_IBLEZ = 7,
	BR_IBGEZ = 8,
	BR_JR    = 9,  // indirect (target = VI[Is])
	BR_JALR  = 10, // indirect with link
};

// Per-pair derived info. Compact layout — fits in one cacheline so the
// emit loop can prefetch ahead cheaply.
struct alignas(16) microOp
{
	// Raw context — duplicated from the encoded instructions so the emit
	// loop doesn't have to reach back into VU1.Micro for fast checks.
	u32  upper;
	u32  lower;
	u32  pc;            // Block-relative PC of this pair (== the lower's PC).
	BranchKind branch;  // BR_NONE if no branch in lower.

	// E/I/M/T/D bits split out from the upper word.
	bool eBit;
	bool iBit;
	bool mBit;
	bool tBit;
	bool dBit;

	// Block position flags.
	bool isEOB;     // Last pair in the block.
	bool isBdelay;  // This pair is the delay slot of an earlier branch.

	// Lower-pipe classification. These mirror the old microLowerOp bool
	// fields where useful for compile-time decisions:
	bool isNOP;       // Lower decoded to a NOP (or an I-bit's missing lower).
	bool isFSSET;     // Lower is FSSET (writes status flag with literal mask).
	bool isFlagRead;  // Lower is FSAND/FSEQ/FSOR/FMAND/FMEQ/FMOR/FCAND/FCEQ/FCOR/FCGET.
	bool isMemWrite;  // Lower is SQ/SQI/SQD (store to VU memory).
	bool isKick;      // Lower is XGKICK.

	// Hazard summary derived from upper/lower _VURegsNum overlap. Exposed
	// for the emit loop's hazard-fallback gate. The underlying _VURegsNum
	// arrays remain authoritative — these are convenience flags that
	// match the existing vf_hazard / vi_hazard expressions in CompileBlock.
	bool vf_write_collision;     // upper.VFwrite == lower.VFwrite (both write same VF)
	bool vf_read_after_write;    // lower.VFread{0,1} == upper.VFwrite
	bool clip_write_collision;   // upper writes CLIP AND lower writes CLIP
	bool clip_read_after_write;  // upper writes CLIP AND lower reads CLIP

	// swapOps mirrors the old microOp::swapOps. Set when the lower is a
	// flag-reader op writing a non-zero VI target — those need to read
	// the s/m/c flag instance from BEFORE upper writes its new flag.
	// (In the current arm64 port we don't yet have flag instance pipelining,
	//  but we record this for the doSwapOp native fast-path which can use
	//  it to emit lower-then-upper natively when there's no VF backup
	//  requirement on top.)
	bool swapOps;

	// noWriteVF / backupVF are not yet computed — placeholders for when
	// the arm64 port grows native handling of those cases. Reserved here
	// so consumers can switch on them without re-walking later.
	bool noWriteVF;
	bool backupVF;

	// analyzeBranchVI (audit item #12): set when this pair writes a VI
	// register that some downstream branch (within 4 pairs forward) reads.
	// The VI-writing emit must call emitBackupVI to snapshot the OLD value
	// into VIOldValue before overwriting, so the branch's evaluation reads
	// the pre-write value. Conservatively also true for all VI writers in
	// the last 4 pairs of the block (next block may branch in its first
	// 4 pairs reading our late writes — without cross-block info we can't
	// rule it out). When false, the emitBackupVI BL is elided entirely.
	bool needs_vi_backup;
};

// Block-level IR. Lives alongside the existing skip_info[] / pair_needs_flags[]
// arrays in CompileBlock. We size the inline storage to VU1_MAX_BLOCK_PAIRS
// to avoid a heap alloc on the hot compile path.
//
// VU1_MAX_BLOCK_PAIRS is defined in iVU1micro_arm64.cpp. We don't import it
// here to keep the header dependency-free; the consumer in the .cpp passes
// an externally-sized array via the analyze entry point.
struct microIR
{
	microOp* info;   // Caller-owned storage, sized to numPairs.
	u32      count;  // == numPairs for the block.
	u32      startPC;

	// Block-level summary flags (mirror the existing block_has_* locals
	// in CompileBlock — exposed here so future passes can consume them).
	bool has_ebit;
	bool has_branch;
	bool has_dbit_or_tbit;
	bool has_ibxx;
	bool has_vi_backup_set;
	bool has_xgkick;
};

// ============================================================================
//  Pass 1 — analyze
// ============================================================================
//
// Populates `ir.info[0..numPairs-1]` from the already-filled _VURegsNum
// arrays (uregs_data / lregs_data) plus the raw VU1.Micro words. Also
// populates the block-level summary flags.
//
// The arm64 CompileBlock today walks the block twice for analysis (once for
// the block_has_* flags, once for skip_info). This entry point is a third
// independent walk for now — it's cheap (~256 pairs max) and additive. A
// future cleanup can fold the three walks together once all consumers are
// migrated to the IR.

void mvu1AnalyzeBlock(
	u32 startPC,
	u32 numPairs,
	const _VURegsNum* uregs_data,
	const _VURegsNum* lregs_data,
	microIR& ir);

} // namespace armvu1ir
