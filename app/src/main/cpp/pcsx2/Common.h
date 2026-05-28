// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/VixlHelpers.h"
#include "common/Pcsx2Defs.h"

static const u32 BIAS = 2;				// Bus is half of the actual ps2 speed
static const u32 PS2CLK = 294912000;	//hz	/* 294.912 mhz */
extern s64 PSXCLK;	/* 36.864 Mhz */


#include "Memory.h"
#include "R5900.h"
#include "Hw.h"
#include "Dmac.h"

#include "SaveState.h"
#include "DebugTools/Debug.h"

#include <cstdlib>
#include <cstring>
#include <string>

inline const char* ARMSX2_GetRuntimeEnv(const char* name)
{
	if (!name || !name[0])
		return nullptr;

	const char* value = std::getenv(name);
	if (value && value[0])
		return value;

	if (std::strncmp(name, "ARMSX2_", 7) != 0)
		return nullptr;

	std::string child_name = "SIMCTL_CHILD_";
	child_name += name;
	value = std::getenv(child_name.c_str());
	return (value && value[0]) ? value : nullptr;
}

inline bool ARMSX2_GetRuntimeEnvBool(const char* name, bool default_value = false)
{
	const char* value = ARMSX2_GetRuntimeEnv(name);
	if (!value)
		return default_value;
	return (value[0] == '1' && value[1] == '\0');
}

inline bool ARMSX2_IsDebugVerbose()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = ARMSX2_GetRuntimeEnvBool("ARMSX2_DEBUG_VERBOSE", false) ? 1 : 0;
	return (s_cached == 1);
}

extern std::string ShiftJIS_ConvertString( const char* src );
extern std::string ShiftJIS_ConvertString( const char* src, int maxlen );
