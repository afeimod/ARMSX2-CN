// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "Common.h"
#include "Config.h"
#include "Gif_Unit.h"
#include "MTGS.h"
#include "R3000A.h"
#include "VMManager.h"

#ifdef __APPLE__
#include "common/Darwin/ApplePlatform.h"
#endif

#include <list>

alignas(16) u8 g_RealGSMem[Ps2MemSize::GSregs];
static bool s_GSRegistersWritten = false;

#if ARMSX2_APPLE_MAC_RUNTIME
extern "C" void LogUnified(const char* fmt, ...);

static const char* MacGSRegName(u32 mem)
{
	switch (mem & ~0xf)
	{
		case GS_PMODE: return "PMODE";
		case GS_SMODE1: return "SMODE1";
		case GS_SMODE2: return "SMODE2";
		case GS_SRFSH: return "SRFSH";
		case GS_SYNCH1: return "SYNCH1";
		case GS_SYNCH2: return "SYNCH2";
		case GS_SYNCV: return "SYNCV";
		case GS_DISPFB1: return "DISPFB1";
		case GS_DISPLAY1: return "DISPLAY1";
		case GS_DISPFB2: return "DISPFB2";
		case GS_DISPLAY2: return "DISPLAY2";
		case GS_EXTBUF: return "EXTBUF";
		case GS_EXTDATA: return "EXTDATA";
		case GS_EXTWRITE: return "EXTWRITE";
		case GS_BGCOLOR: return "BGCOLOR";
		case GS_CSR: return "CSR";
		case GS_IMR: return "IMR";
		case GS_BUSDIR: return "BUSDIR";
		case GS_SIGLBLID: return "SIGLBLID";
		default: return "GS_REG";
	}
}

static bool MacGSRegIsDisplayCritical(u32 mem)
{
	switch (mem & ~0xf)
	{
		case GS_PMODE:
		case GS_SMODE1:
		case GS_SMODE2:
		case GS_DISPFB1:
		case GS_DISPLAY1:
		case GS_DISPFB2:
		case GS_DISPLAY2:
		case GS_BGCOLOR:
		case GS_CSR:
		case GS_BUSDIR:
			return true;
		default:
			return false;
	}
}

static void MacTraceGSWrite(const char* width, u32 mem, u64 lo, u64 hi = 0)
{
	static u32 s_count = 0;
	const u32 count = s_count++;
	const bool critical = MacGSRegIsDisplayCritical(mem);
	if (count >= 192 && (!critical || (count % 60) != 0))
		return;

	LogUnified("@@MAC_GS_REG_WRITE@@ count=%u width=%s mem=%08x reg=%s lo=%016llx hi=%016llx ee_pc=%08x iop_pc=%08x ee_cycle=%u gs_written=%d csr=%08x pmode=%016llx dispfb1=%016llx display1=%016llx dispfb2=%016llx display2=%016llx\n",
		count, width, mem, MacGSRegName(mem), static_cast<unsigned long long>(lo),
		static_cast<unsigned long long>(hi), cpuRegs.pc, psxRegs.pc, cpuRegs.cycle,
		s_GSRegistersWritten ? 1 : 0, CSRreg._u32,
		*reinterpret_cast<u64*>(PS2GS_BASE(GS_PMODE)),
		*reinterpret_cast<u64*>(PS2GS_BASE(GS_DISPFB1)),
		*reinterpret_cast<u64*>(PS2GS_BASE(GS_DISPLAY1)),
		*reinterpret_cast<u64*>(PS2GS_BASE(GS_DISPFB2)),
		*reinterpret_cast<u64*>(PS2GS_BASE(GS_DISPLAY2)));
}
#endif

void gsSetVideoMode(GS_VideoMode mode)
{
	gsVideoMode = mode;
	UpdateVSyncRate(false);
}

// Make sure framelimiter options are in sync with GS capabilities.
void gsReset()
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET) {
		GUNIT_WARN("GUNIT_WARN: csr.RESET");
		//Console.Warning( "csr.RESET" );
		//gifUnit.Reset(true); // Don't think gif should be reset...
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		// Privilage registers also reset.
		std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR.reset();
		CSRreg.Reset();
		MTGS::ResetGS(false);
	}

	if(csr.FLUSH)
	{
		// Our emulated GS has no FIFO, but if it did, it would flush it here...
		//Console.WriteLn("GS_CSR FLUSH GS fifo: %x (CSRr=%x)", value, GSCSRr);
	}

	if(csr.SIGNAL)
	{
		const bool resume = CSRreg.SIGNAL;
		// SIGNAL : What's not known here is whether or not the SIGID register should be updated
		//  here or when the IMR is cleared (below).
		GUNIT_LOG("csr.SIGNAL");
		if (gifUnit.gsSIGNAL.queued) {
			//DevCon.Warning("Firing pending signal");
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK) gsIrq();
			CSRreg.SIGNAL  = true; // Just to be sure :p
		}
		else CSRreg.SIGNAL = false;
		gifUnit.gsSIGNAL.queued = false;

		if (resume)
			gifUnit.Execute(false, true); // Resume paused transfers
	}

	if (csr.FINISH)	{
		CSRreg.FINISH = false;
		gifUnit.gsFINISH.gsFINISHFired = false; //Clear the previously fired FINISH (YS, Indiecar 2005, MGS3)
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	CSRreg.HSINT	= false;
	if(csr.VSINT)	CSRreg.VSINT	= false;
	if(csr.EDWINT)	CSRreg.EDWINT	= false;
}

static __fi void IMRwrite(u32 value)
{
	GUNIT_LOG("IMRwrite()");

	if (CSRreg.GetInterruptMask() & (~value & GSIMR._u32) >> 8)
		gsIrq();

	GSIMR._u32 = (value & 0x1f00)|0x6000;
}

__fi void gsWrite8(u32 mem, u8 value)
{
#if ARMSX2_APPLE_MAC_RUNTIME
	MacTraceGSWrite("8", mem, value);
#endif
	switch (mem)
	{
		// CSR 8-bit write handlers.
		// I'm quite sure these would just write the CSR portion with the other
		// bits set to 0 (no action).  The previous implementation masked the 8-bit
		// write value against the previous CSR write value, but that really doesn't
		// make any sense, given that the real hardware's CSR circuit probably has no
		// real "memory" where it saves anything.  (for example, you can't write to
		// and change the GS revision or ID portions -- they're all hard wired.) --air

		case GS_CSR: // GS_CSR
			gsCSRwrite( tGS_CSR((u32)value) );			break;
		case GS_CSR + 1: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) <<  8) );	break;
		case GS_CSR + 2: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );	break;
		case GS_CSR + 3: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) << 24) );	break;

		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
	GIF_LOG("GS write 8 at %8.8lx with data %8.8lx", mem, value);
}

//////////////////////////////////////////////////////////////////////////
// GS Write 16 bit

__fi void gsWrite16(u32 mem, u16 value)
{
#if ARMSX2_APPLE_MAC_RUNTIME
	MacTraceGSWrite("16", mem, value);
#endif
	GIF_LOG("GS write 16 at %8.8lx with data %8.8lx", mem, value);

	switch (mem)
	{
		// See note above about CSR 8 bit writes, and handling them as zero'd bits
		// for all but the written parts.

		case GS_CSR:
			gsCSRwrite( tGS_CSR((u32)value) );
		return; // do not write to MTGS memory

		case GS_CSR+2:
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );
		return; // do not write to MTGS memory

		case GS_IMR:
			IMRwrite(value);
		return; // do not write to MTGS memory
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 32 bit

__fi void gsWrite32(u32 mem, u32 value)
{
	pxAssume( (mem & 3) == 0 );
#if ARMSX2_APPLE_MAC_RUNTIME
	MacTraceGSWrite("32", mem, value);
#endif
	GIF_LOG("GS write 32 at %8.8lx with data %8.8lx", mem, value);

	switch (mem)
	{
		case GS_CSR:
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
			IMRwrite(value);
		return;
	}

	*(u32*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 64 bit

void gsWrite64_generic( u32 mem, u64 value )
{
#if ARMSX2_APPLE_MAC_RUNTIME
	MacTraceGSWrite("64", mem, value);
#endif
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	std::memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

void gsWrite64_page_00( u32 mem, u64 value )
{
	s_GSRegistersWritten |= (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE);
	bool reqUpdate = false;
	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			reqUpdate = true;
	}

	gsWrite64_generic( mem, value );

	if (reqUpdate)
		UpdateVSyncRate(false);
}

void gsWrite64_page_01( u32 mem, u64 value )
{
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	switch( mem )
	{
		case GS_BUSDIR:

			gifUnit.stat.DIR = static_cast<u32>(value) & 1;
			if (gifUnit.stat.DIR) {      // Assume will do local->host transfer
				gifUnit.stat.OPH = true; // Should we set OPH here?
				gifUnit.FlushToMTGS();   // Send any pending GS Primitives to the GS
				GUNIT_LOG("Busdir - GS->EE Download");
			}
			else {
				GUNIT_LOG("Busdir - EE->GS Upload");
			}

			gsWrite64_generic( mem, value );
		return;

		case GS_CSR:
#if ARMSX2_APPLE_MAC_RUNTIME
			MacTraceGSWrite("64", mem, value);
#endif
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
#if ARMSX2_APPLE_MAC_RUNTIME
			MacTraceGSWrite("64", mem, value);
#endif
			IMRwrite(static_cast<u32>(value));
		return;
	}

	gsWrite64_generic( mem, value );
}

//////////////////////////////////////////////////////////////////////////
// GS Write 128 bit

void TAKES_R128 gsWrite128_page_00( u32 mem, r128 value )
{
	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_page_01( u32 mem, r128 value )
{
	switch( mem )
	{
		case GS_CSR:
#if ARMSX2_APPLE_MAC_RUNTIME
			{
				const u128 uvalue = r128_to_u128(value);
				MacTraceGSWrite("128", mem, uvalue._u64[0], uvalue._u64[1]);
			}
#endif
			gsCSRwrite(r128_to_u32(value));
		return;

		case GS_IMR:
#if ARMSX2_APPLE_MAC_RUNTIME
			{
				const u128 uvalue = r128_to_u128(value);
				MacTraceGSWrite("128", mem, uvalue._u64[0], uvalue._u64[1]);
			}
#endif
			IMRwrite(r128_to_u32(value));
		return;
	}

	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_generic( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
#if ARMSX2_APPLE_MAC_RUNTIME
	MacTraceGSWrite("128", mem, uvalue._u64[0], uvalue._u64[1]);
#endif
	GIF_LOG("GS Write128 at %8.8lx with data %8.8x_%8.8x_%8.8x_%8.8x", mem,
		uvalue._u32[3], uvalue._u32[2], uvalue._u32[1], uvalue._u32[0]);

	r128_store(PS2GS_BASE(mem), value);
}

__fi u8 gsRead8(u32 mem)
{
	GIF_LOG("GS read 8 from %8.8lx  value: %8.8lx", mem, *(u8*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u8*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
	}
}

__fi u16 gsRead16(u32 mem)
{
	GIF_LOG("GS read 16 from %8.8lx  value: %8.8lx", mem, *(u16*)PS2GS_BASE(mem));
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u16*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
	}
}

__fi u32 gsRead32(u32 mem)
{
	GIF_LOG("GS read 32 from %8.8lx  value: %8.8lx", mem, *(u32*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u32*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
	}
}

__fi u64 gsRead64(u32 mem)
{
	// fixme - PS2GS_BASE(mem+4) = (g_RealGSMem+(mem + 4 & 0x13ff))
	GIF_LOG("GS read 64 from %8.8lx  value: %8.8lx_%8.8lx", mem, *(u32*)PS2GS_BASE(mem+4), *(u32*)PS2GS_BASE(mem) );

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u64*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
	}
}

__fi u128 gsNonMirroredRead(u32 mem)
{
	return *(u128*)PS2GS_BASE(mem);
}

void gsIrq() {
	hwIntcIrq(INTC_GS);
}

//These are done at VSync Start.  Drawing is done when VSync is off, then output the screen when Vsync is on
//The GS needs to be told at the start of a vsync else it loses half of its picture (could be responsible for some halfscreen issues)
//We got away with it before i think due to our awful GS timing, but now we have it right (ish)
void gsPostVsyncStart()
{
	//gifUnit.FlushToMTGS();  // Needed for some (broken?) homebrew game loaders

	const bool registers_written = s_GSRegistersWritten;
	s_GSRegistersWritten = false;
	MTGS::PostVsyncStart(registers_written);
}

bool SaveStateBase::gsFreeze()
{
	FreezeMem(PS2MEM_GS, 0x2000);
	Freeze(gsVideoMode);
	return IsOkay();
}
