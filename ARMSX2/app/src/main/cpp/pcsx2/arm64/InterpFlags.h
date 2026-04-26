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
#define VU1_PROFILE_BLOCKS

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
