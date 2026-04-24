// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0
//
// ARM64 VU1 Recompiler — Class declaration.
// Initially wraps the VU1 interpreter; individual instructions are gradually
// replaced with native ARM64 codegen via per-instruction ISTUB toggles.

#pragma once

#include "VUmicro.h"

// ============================================================================
//  recArmVU1 — ARM64 VU1 recompiler
// ============================================================================

class recArmVU1 final : public BaseVUmicroCPU
{
public:
	recArmVU1();
	~recArmVU1() override { Shutdown(); }

	const char* GetShortName() const override { return "armVU1"; }
	const char* GetLongName() const override { return "ARM64 VU1 Recompiler"; }

	void Reserve();
	void Shutdown() override;
	void Reset() override;
	void SetStartPC(u32 startPC) override;
	void Step() override;
	void Execute(u32 cycles) override;
	void Clear(u32 addr, u32 size) override;
	void ResumeXGkick() override {}
};

extern recArmVU1 CpuArmVU1;

// ISTUB helper — emits flush of all VU1 pinned regs (ACC q16, flags
// w19/w20/w28, cycle x21, fmac/ialu wpos x24/x25), BL to interp_fn,
// and reload of the same regs. Called by the REC_VU1_*_INTERP macros in
// iVU1Upper_arm64.cpp / iVU1Lower_arm64.cpp when any INTERP_VU_* harness
// flag routes an op to the C interpreter. Keeping this as a single
// helper prevents the hybrid-harness pin-skew bug (ISTUB emits a bare BL
// and the interp sees stale state).
void emitVU1InterpBL(const void* interp_fn);
