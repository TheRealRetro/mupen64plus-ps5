// Mupen64Plus PS5: the RSP plugin, mupen64plus-rsp-cxd4 (third_party/rsp-cxd4, CC0) built into the app.
//
// cxd4 is a low-level RSP interpreter: it runs the game's own microcode, so graphics tasks end up as RDP
// command lists for angrylion's renderer (gfx_ps5.c) and audio tasks write samples to RDRAM for the audio
// plugin. Every plugin exports the same names (PluginStartup, RomOpen...), and cxd4 keeps its pointers to
// the core's config functions in globals named like the core's functions, so they are all renamed here
// and the whole plugin is compiled as one unit (its own lto.c).
//
// SPDX-License-Identifier: MIT

#define PluginStartup cxd4_PluginStartup
#define PluginShutdown cxd4_PluginShutdown
#define PluginGetVersion cxd4_PluginGetVersion
#define RomOpen cxd4_RomOpen
#define RomClosed cxd4_RomClosed
#define InitiateRSP cxd4_InitiateRSP
#define DoRspCycles cxd4_DoRspCycles
#define GetDllInfo cxd4_GetDllInfo
#define ConfigOpenSection cxd4_ConfigOpenSection
#define ConfigDeleteSection cxd4_ConfigDeleteSection
#define ConfigSetParameter cxd4_ConfigSetParameter
#define ConfigGetParameter cxd4_ConfigGetParameter
#define ConfigSetDefaultFloat cxd4_ConfigSetDefaultFloat
#define ConfigSetDefaultBool cxd4_ConfigSetDefaultBool
#define ConfigGetParamBool cxd4_ConfigGetParamBool
#define CoreDoCommand cxd4_CoreDoCommand

#include "lto.c"

#undef PluginStartup
#undef PluginShutdown
#undef PluginGetVersion
#undef RomOpen
#undef RomClosed
#undef InitiateRSP
#undef DoRspCycles

#include "perf.h"
#include "static_dynlib.h"

// cxd4's time, without the RDP lists it hands angrylion while it runs
static unsigned int timed_DoRspCycles(unsigned int cycles)
{
	const uint64_t rdp0 = n64ps5_perf_total[N64PS5_PERF_RDP];
	const uint64_t t0 = n64ps5_perf_now();
	const unsigned int r = cxd4_DoRspCycles(cycles);
	const uint64_t t1 = n64ps5_perf_now();
	const uint64_t spent = t1 - t0;
	const uint64_t rdp = n64ps5_perf_total[N64PS5_PERF_RDP] - rdp0;
	n64ps5_perf_add(N64PS5_PERF_RSP, spent > rdp ? spent - rdp : 0);
	return r;
}

static const m64ps5_symbol l_RspSymbols[] = {
	{"PluginStartup", (m64p_function)cxd4_PluginStartup},
	{"PluginShutdown", (m64p_function)cxd4_PluginShutdown},
	{"PluginGetVersion", (m64p_function)cxd4_PluginGetVersion},
	{"RomClosed", (m64p_function)cxd4_RomClosed},
	{"InitiateRSP", (m64p_function)cxd4_InitiateRSP},
	{"DoRspCycles", (m64p_function)timed_DoRspCycles},
	{NULL, NULL},
};

const m64ps5_library m64ps5_rsp_lib = {"mupen64plus-rsp-cxd4", l_RspSymbols};
