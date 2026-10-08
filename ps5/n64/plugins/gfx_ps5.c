// Mupen64Plus PS5: the video plugin, angrylion's RDP Plus (third_party/angrylion-rdp-plus) without OpenGL.
//
// It runs on the CPU at the N64's own resolution; builds with Vulkan also have the GPU one (gfx_parallel_ps5.cpp).
// angrylion's renderer is a software, pixel-accurate RDP + VI: it rasterises the RDP command lists that
// the low-level RSP plugin (cxd4) produces straight into RDRAM, and its VI emulation turns the frame
// buffer into a picture. It spreads the work over several threads, which the PS5's eight Zen 2 cores have.
//
// This file replaces the plugin's mupen64plus glue (src/plugin/mupen64plus/*.c) and its OpenGL output
// (src/output/*): on every video interrupt the picture goes to the frontend (n64ps5_on_vi), which scales
// it into the VideoOut surface.
//
// SPDX-License-Identifier: MIT

#include "api/m64p_plugin.h"
#include "api/m64p_types.h"

#include "core/msg.h"
#include "core/n64video.h"

#include "n64_bridge.h"
#include "perf.h"
#include "static_dynlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GFXPS5_PLUGIN_VERSION 0x010600
#define GFXPS5_API_VERSION 0x020200

static void (*l_DebugCallback)(void*, int, const char*);
static void* l_DebugContext;
static bool l_Initialized;
static bool l_RomOpen;
static bool l_WarnedHle;
static GFX_INFO l_Gfx;
static void (*l_RenderCallback)(int);
static n64ps5_gfx_options l_Options = {0, false, 6, 1, 1, true};
static struct n64video_config l_Config;
static struct n64video_frame_buffer l_LastFb; // for ReadScreen2

// ---- angrylion's message hooks (core/msg.h) -------------------------------------------------------------
static void Message(int level, const char* fmt, va_list ap)
{
	if (!l_DebugCallback)
		return;
	char buf[256];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	l_DebugCallback(l_DebugContext, level, buf);
}

// The desktop plugin calls exit() here; a PS5 title must never exit() (SIGSYS). Every call site either
// clamps the bad value afterwards or is harmless, so log and carry on.
void msg_error(const char* err, ...)
{
	va_list ap;
	va_start(ap, err);
	Message(M64MSG_ERROR, err, ap);
	va_end(ap);
}

void msg_warning(const char* err, ...)
{
	va_list ap;
	va_start(ap, err);
	Message(M64MSG_WARNING, err, ap);
	va_end(ap);
}

void msg_debug(const char* err, ...)
{
	va_list ap;
	va_start(ap, err);
	Message(M64MSG_VERBOSE, err, ap);
	va_end(ap);
}

void n64ps5_gfx_set_options(const n64ps5_gfx_options* opt)
{
	if (opt)
		l_Options = *opt;
}

// ---- the plugin API -------------------------------------------------------------------------------------
static m64p_error gfxps5_PluginStartup(m64p_dynlib_handle core, void* context, void (*debug)(void*, int, const char*))
{
	(void)core;
	if (l_Initialized)
		return M64ERR_ALREADY_INIT;
	l_DebugCallback = debug;
	l_DebugContext = context;
	l_Initialized = true;
	return M64ERR_SUCCESS;
}

static m64p_error gfxps5_PluginShutdown(void)
{
	if (!l_Initialized)
		return M64ERR_NOT_INIT;
	l_Initialized = false;
	return M64ERR_SUCCESS;
}

static m64p_error gfxps5_PluginGetVersion(m64p_plugin_type* type, int* version, int* api, const char** name, int* caps)
{
	if (type)
		*type = M64PLUGIN_GFX;
	if (version)
		*version = GFXPS5_PLUGIN_VERSION;
	if (api)
		*api = GFXPS5_API_VERSION;
	if (name)
		*name = "angrylion's RDP Plus (PS5)";
	if (caps)
		*caps = 0;
	return M64ERR_SUCCESS;
}

static int gfxps5_InitiateGFX(GFX_INFO info)
{
	l_Gfx = info;
	return 1;
}

static void gfxps5_MoveScreen(int x, int y)
{
	(void)x;
	(void)y;
}

static void gfxps5_ProcessDList(void)
{
	// only reached when the RSP plugin is set up for HLE graphics, which cxd4 isn't here
	if (!l_WarnedHle)
	{
		msg_warning("HLE display list received: angrylion needs the LLE RSP (cxd4)");
		l_WarnedHle = true;
	}
}

static void gfxps5_ProcessRDPList(void)
{
	if (!l_RomOpen)
		return;
	const uint64_t t0 = n64ps5_perf_now();
	n64video_process_list();
	n64ps5_perf_add(N64PS5_PERF_RDP, n64ps5_perf_now() - t0);
}

static int gfxps5_RomOpen(void)
{
	n64video_config_init(&l_Config);
	l_Config.parallel = l_Options.num_workers != 1;
	l_Config.num_workers = l_Options.num_workers > 0 ? (uint32_t)l_Options.num_workers : 0;
	l_Config.busyloop = false;
	l_Config.vi.mode = l_Options.vi_mode == 1 ? VI_MODE_COLOR : VI_MODE_NORMAL;
	l_Config.vi.interp = VI_INTERP_NEAREST; // unused: the frontend scales the picture
	l_Config.vi.widescreen = false;
	l_Config.vi.hide_overscan = l_Options.hide_overscan;
	l_Config.vi.integer_scaling = false;
	l_Config.vi.vsync = false;
	l_Config.dp.compat = (enum dp_compat_profile)(l_Options.dp_compat < 0 ? 0 : l_Options.dp_compat > 2 ? 2 : l_Options.dp_compat);

	l_Config.gfx.rdram = l_Gfx.RDRAM;
	l_Config.gfx.rdram_size = (l_Gfx.version >= 2 && l_Gfx.RDRAM_SIZE) ? *l_Gfx.RDRAM_SIZE : RDRAM_MAX_SIZE;
	l_Config.gfx.dmem = l_Gfx.DMEM;
	l_Config.gfx.mi_intr_reg = (uint32_t*)l_Gfx.MI_INTR_REG;
	l_Config.gfx.mi_intr_cb = l_Gfx.CheckInterrupts;
	l_Config.gfx.vi_reg = (uint32_t**)&l_Gfx.VI_STATUS_REG;
	l_Config.gfx.dp_reg = (uint32_t**)&l_Gfx.DPC_START_REG;

	n64video_init(&l_Config);
	memset(&l_LastFb, 0, sizeof(l_LastFb));
	l_RomOpen = true;
	if (l_DebugCallback)
	{
		char msg[128];
		snprintf(msg, sizeof(msg), "angrylion: %s VI, %u render thread(s), compat %d",
			l_Config.vi.mode == VI_MODE_NORMAL ? "filtered" : "unfiltered", l_Config.parallel ? l_Config.num_workers : 1u,
			(int)l_Config.dp.compat);
		l_DebugCallback(l_DebugContext, M64MSG_INFO, msg);
	}
	return 1;
}

static void gfxps5_RomClosed(void)
{
	if (!l_RomOpen)
		return;
	n64video_close();
	l_RomOpen = false;
	memset(&l_LastFb, 0, sizeof(l_LastFb));
}

static void gfxps5_ShowCFB(void)
{
}

static void gfxps5_UpdateScreen(void)
{
	if (!l_RomOpen)
		return;
	struct n64video_frame_buffer fb;
	memset(&fb, 0, sizeof(fb));
	const uint64_t t0 = n64ps5_perf_now();
	n64video_update_screen(&fb);
	n64ps5_perf_add(N64PS5_PERF_VI, n64ps5_perf_now() - t0);
	if (fb.valid && fb.pixels && fb.width > 0 && fb.height > 0)
	{
		l_LastFb = fb;
		const n64ps5_frame frame = {(const uint32_t*)fb.pixels, (int)fb.width, (int)fb.height, (int)fb.pitch};
		n64ps5_on_vi(&frame);
	}
	else
		n64ps5_on_vi(NULL);
	if (l_RenderCallback)
		l_RenderCallback(1);
}

static void gfxps5_ViStatusChanged(void)
{
}

static void gfxps5_ViWidthChanged(void)
{
}

static void gfxps5_ChangeWindow(void)
{
}

// The core asks for the picture (screenshots, the GB camera); give the last one, 24-bit RGB rows.
static void gfxps5_ReadScreen2(void* dest, int* width, int* height, int front)
{
	(void)front;
	if (width)
		*width = (int)l_LastFb.width;
	if (height)
		*height = (int)l_LastFb.height;
	if (!dest || !l_LastFb.pixels)
		return;
	uint8_t* out = (uint8_t*)dest;
	// bottom-up rows, as the OpenGL plugins return them
	for (uint32_t y = 0; y < l_LastFb.height; y++)
	{
		const struct n64video_pixel* row = l_LastFb.pixels + (size_t)(l_LastFb.height - 1 - y) * l_LastFb.pitch;
		for (uint32_t x = 0; x < l_LastFb.width; x++)
		{
			*out++ = row[x].r;
			*out++ = row[x].g;
			*out++ = row[x].b;
		}
	}
}

static void gfxps5_SetRenderingCallback(void (*callback)(int))
{
	l_RenderCallback = callback;
}

static void gfxps5_ResizeVideoOutput(int width, int height)
{
	(void)width;
	(void)height;
}

static void gfxps5_FBRead(unsigned int addr)
{
	(void)addr;
}

static void gfxps5_FBWrite(unsigned int addr, unsigned int size)
{
	(void)addr;
	(void)size;
}

static void gfxps5_FBGetFrameBufferInfo(void* info)
{
	(void)info;
}

#define SYM(name) {#name, (m64p_function)gfxps5_##name}
static const m64ps5_symbol l_GfxSymbols[] = {
	SYM(PluginStartup),
	SYM(PluginShutdown),
	SYM(PluginGetVersion),
	SYM(ChangeWindow),
	SYM(InitiateGFX),
	SYM(MoveScreen),
	SYM(ProcessDList),
	SYM(ProcessRDPList),
	SYM(RomClosed),
	SYM(RomOpen),
	SYM(ShowCFB),
	SYM(UpdateScreen),
	SYM(ViStatusChanged),
	SYM(ViWidthChanged),
	SYM(ReadScreen2),
	SYM(SetRenderingCallback),
	SYM(ResizeVideoOutput),
	SYM(FBRead),
	SYM(FBWrite),
	SYM(FBGetFrameBufferInfo),
	{NULL, NULL},
};

const m64ps5_library m64ps5_gfx_lib = {"angrylion-rdp-plus", l_GfxSymbols};
