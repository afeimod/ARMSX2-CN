// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#pragma once

// Bisect: Commented = native
// Un-comment a group to find a general problem area
// Un-comment indiviudal ops to narrow it down to a specific op
//
// These flags are also consumed by shared headers (Config.h) to keep config
// macros like REC_VU1 / THREAD_VU1 consistent with the JIT bisect state.

//EE
//#define INTERP_EE        // Master
//#define INTERP_BRANCH    // BEQ, BNE, J, JAL, JR, JALR, SYSCALL, BREAK, etc.
//#define INTERP_MOVE      // LUI, MFHI/LO, MTHI/LO, MOVZ, MOVN, MFSA, MTSA, etc.
//#define INTERP_COP0      // MFC0, MTC0, BC0x, TLB*, ERET, EI, DI
//#define INTERP_COP1      // MFC1, MTC1, CFC1, CTC1, BC1x, FPU arith/cmp/cvt
//#define INTERP_COP2      // QMFC2/QMTC2 (native 128b), CFC2/CTC2, BC2x, all VU0 macro math ops
//#define INTERP_ALU       // ADDU, SUBU, ADDIU, DADDU, DSUBU, DADDIU, AND/OR/XOR/NOR, SLT/U, etc.
//#define INTERP_SHIFT     // SLL, SRL, SRA, SLLV, SRLV, SRAV, DSLL/DSRL/DSRA + 32 variants
//#define INTERP_MMI       // All packed SIMD ops: PADD*/PSUB*, PCGT*, PMAX/MIN*, PCEQ*, PABS*, PSxx shifts, etc.
//#define INTERP_LOAD      // LB, LBU, LH, LHU, LW, LWU, LD, LQ, LWL/R, LDL/R, LWC1, LQC2
//#define INTERP_STORE     // SB, SH, SW, SD, SQ, SWL/R, SDL/R, SWC1, SQC2
//#define INTERP_TRAP      // TGEI, TGEIU, TLTI, TLTIU, TEQI, TNEI, TGE, TGEU, TLT, TLTU, TEQ, TNE
//#define INTERP_MULTDIV

//VU0
// Pair-level bisect: comment out a line to force that class of pairs to fall back to vu0Exec.
//#define INTERP_VU0            // Master
//#define INTERP_VU0_PAIR       // Force every pair to fall back to vu0Exec (kills all per-pair native machinery)
//#define INTERP_VU0_HAZARD     // (Dormant — VF/CLIP hazards always fall back; native save/restore not yet implemented)
//#define INTERP_VU0_MBIT       // Fall back to vu0Exec when M-bit (bit 29) is set on the upper instruction
//#define INTERP_VU0_DTBITS     // Fall back to vu0Exec when D-bit or T-bit is set
//#define INTERP_VU0_EBIT       // Fall back to vu0Exec when E-bit is set
//#define INTERP_VU0_BRANCH     // Fall back to vu0Exec when the pair contains a branch lower op


//#define INTERP_VU0_UPPER     // FMAC arith (ADD/SUB/MUL/MADD/MSUB xyzwqi), accum, MAX/MINI, ABS, CLIP, FTOI/ITOF, NOP
//#define INTERP_VU0_LOWER_FDIV       // DIV, SQRT, RSQRT, WAITQ, WAITP
//#define INTERP_VU0_LOWER_IALU       // IADD, ISUB, IADDI, IADDIU, ISUBIU, IAND, IOR
//#define INTERP_VU0_LOWER_LOADSTORE  // LQ, LQD, LQI, SQ, SQD, SQI, ILW, ISW, ILWR, ISWR
//#define INTERP_VU0_LOWER_BRANCH     // B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ
//#define INTERP_VU0_LOWER_MISC       // MOVE, MR32, MFIR, MTIR, MFP, flag ops, random, EFU, XITOP, XTOP, XGKICK

//VU1
//#define INTERP_VU1            // Master
//#define INTERP_VU_UPPER      // FMAC arith (ADD/SUB/MUL/MADD/MSUB xyzwqi), accum, MAX/MINI, ABS, CLIP, FTOI/ITOF, NOP
//#define INTERP_VU_FDIV       // DIV, SQRT, RSQRT, WAITQ, WAITP
//#define INTERP_VU_IALU       // IADD, ISUB, IADDI, IADDIU, ISUBIU, IAND, IOR
//#define INTERP_VU_LOADSTORE  // LQ, LQD, LQI, SQ, SQD, SQI, ILW, ISW, ILWR, ISWR
//#define INTERP_VU_BRANCH     // B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ
//#define INTERP_VU_MISC       // MOVE, MR32, MFIR, MTIR, MFP, flag ops, random, EFU, XITOP, XTOP, XGKICK

// Diagnostic: emit per-block exec counter at VU1 block linkEntry and dump the
// top-5 hottest blocks (with per-pair disassembly) on recArmVU1::Shutdown —
// fires when the Compose Stop button is pressed. Adds ~5 insns per block
// entry (measurable cost on linked-chain entries), so keep off unless profiling.
//#define VU1_PROFILE_BLOCKS

// Diagnostic: shadow-compare every native VU0 pair against a parallel
// _vu0Exec interp run on a snapshot of VU0 state. Logs the first divergent
// field per pair (jit value vs interp value + pc) to Console.Error. Fires
// only on the native pair path (fallback pairs use vu0Exec themselves and
// would always agree). Slows VU0 emulation drastically — debug-only.
// Use this to localize "JIT pair body produces wrong VF/ACC/macflag"
// bugs that don't show up in INTERP_VU0_PAIR mode.
//#define VU0_SHADOW_VERIFY

// Diagnostic: gate the shadow-verify comparison to a cycle window so the
// harness doesn't tank perf during long boot sequences. Uncomment ONE
// (or both) to limit when the comparison fires. Snapshots/verify
// emit ALWAYS at compile time; the GATE is at runtime in vu0_shadow_verify
// — out-of-window pairs skip the vu0Exec call + memcmp entirely.
//
//   FROM: skip until VU->cycle reaches this value. Use to bypass init / BIOS.
//   TO:   stop after VU->cycle exceeds this value. Use to abort once past
//         the area of interest.
//
// Find your target cycle via the OSD (no exact cycle counter shown there)
// or by enabling shadow-verify with a permissive window first, noting the
// cycle in the divergence log, then narrowing on the next run.
//#define VU0_SHADOW_VERIFY_FROM_CYCLE 24148000000ULL
//#define VU0_SHADOW_VERIFY_TO_CYCLE   24148999999ULL

// Diagnostic: TPC-range fallback bisection. Pairs whose pc falls in
// [INTERP_VU0_PC_LOW, INTERP_VU0_PC_HIGH] route to vu0Exec; others go
// through the native JIT body. Use to BINARY-SEARCH a buggy pair when
// INTERP_VU0_PAIR fixes the bug but per-pair shadow-verify is silent.
//
// Workflow:
//   1. Set range = whole 4KB program (LOW=0, HIGH=0xFFF). Verify physics
//      are correct → equivalent to INTERP_VU0_PAIR.
//   2. Halve the range. Re-run. If physics break, the bug is in the OTHER
//      half. Otherwise it's in this half.
//   3. Repeat until you've narrowed to ~8 bytes (one pair).
//   4. Disable VU0_SHADOW_VERIFY and INTERP_VU0_PC range; manually inspect
//      the JIT emit for that pair against x86 microVU.
//
// The bug is in a pair the harness can't observe — likely a hazard pair
// fallback (the harness skips fallback pairs entirely) or external state
// the per-pair compare doesn't capture (vif0Regs, EE memory, etc.).
// Symptom #1 (MGS2 ledge teleport + back-facing invisible walls): pinned
// to pc=0x4D8 — keep this range stable across symptom #2 bisect rounds.
//#define INTERP_VU0_PC_LOW  0x04D8u
//#define INTERP_VU0_PC_HIGH 0x04DFu

// Symptom #2 (MGS2 bullets hit invisible walls, cannot descend stairs):
// INTERACTING-PAIR bug at pc=0x168 + pc=0x170. Routing either pair alone
// is NOT enough — both must take the vu0Exec fallback simultaneously.
// Bisect cascade summary:
//   0x140-0x17F OK; 0x140-0x15F BROKEN; 0x160-0x17F OK
//   0x160-0x16F BROKEN; 0x170-0x17F BROKEN; 0x168-0x17F OK
//   0x168-0x177 OK (covers both 0x168 + 0x170 pairs, drops 0x178)
// → both 0x168 and 0x170 pairs are critical and interact. Likely one
//   writes a VF/VI/ACC value the other reads, with a JIT-emit bug at the
//   data handoff. To investigate: disassemble VU0 micro at:
//     pc=0x168: lower at Micro[0x168], upper at Micro[0x16C]
//     pc=0x170: lower at Micro[0x170], upper at Micro[0x174]
//   and audit the JIT emit vs x86 microVU.
//
// Range 2 below pins both pairs through interp until the JIT bug is fixed.
// Range1 (above) pins symptom #1 at 0x4D8.
//#define INTERP_VU0_PC_LOW2  0x0168u
//#define INTERP_VU0_PC_HIGH2 0x0177u

// Diagnostic: per-op JIT symbol registration with simpleperf. When defined,
// each emitted op is registered as a separate symbol (e.g. `EE_OP_lui_0x123`,
// `VU1_U_05_0x0040`). The next simpleperf trace then attributes samples
// directly to specific MIPS/VU op emits inside hot blocks.
//
// Cost: one snprintf + one Perf::Register call per op AT COMPILE TIME (not
// per execution). For typical workloads this is a few thousand calls per
// block-cache-fill, negligible compared to actual emit cost. The runtime
// JIT'd code is unchanged. Each Register call appends a line to the
// /data/local/tmp/perf-<PID>.map file (~50 bytes/op), so a long session
// can grow that file to several MB — flush the map when starting a new
// trace if needed.
//
// Recommended workflow: enable one or two of these at a time, rebuild,
// record perf.data, analyze, disable. Don't ship with these on.
//#define EE_PROFILE_OPS
//#define IOP_PROFILE_OPS
//#define VU0_PROFILE_OPS
//#define VU1_PROFILE_OPS

//DMAC
//#define INTERP_DMAC          // VIF0, VIF1, GIF, IPU0/1, SIF0/1/2, SPR0/1 + interrupt handlers

//IOP
//#define INTERP_IOP             // Master
//#define INTERP_IOP_ALU         // BISECT: uncommented → per-instruction ISTUBs in iR3000Atables_arm64.cpp
//#define INTERP_IOP_BRANCH      // BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL/J/JAL/JR/JALR
//#define INTERP_IOP_SHIFT       // SLL/SRL/SRA/SLLV/SRLV/SRAV
//#define INTERP_IOP_MULTDIV     // MULT/MULTU/DIV/DIVU/MFHI/MTHI/MFLO/MTLO
//#define INTERP_IOP_MOVE        // MFHI/MTHI/MFLO/MTLO
//#define INTERP_IOP_LOADSTORE   // LB/LBU/LH/LHU/LW/LWL/LWR/SB/SH/SW/SWL/SWR
//#define INTERP_IOP_COP0        // MFC0/MTC0/CFC0/CTC0/RFE
#define INTERP_IOP_COP2        // All GTE (keep stubbed)
#define INTERP_IOP_SYSTEM      // SYSCALL/BREAK
