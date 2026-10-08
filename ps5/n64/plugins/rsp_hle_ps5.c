// Mupen64Plus PS5: HLE audio in front of the cxd4 RSP (rsp_cxd4_ps5.c), the way mupen64plus-rsp-hle runs with
// cxd4 as its "RspFallback".
//
// rsp-hle's core (third_party/rsp-hle/src, without its plugin.c) recognises the game's audio microcode and does
// its work in C, much faster than cxd4 running the microcode instruction by instruction; the same goes for the
// MP3 (Conker, Perfect Dark), JPEG and video tasks it knows. Everything else, graphics above all (paraLLEl-RDP
// and angrylion are low-level RDPs: they need the RSP to run the game's own graphics microcode), is forwarded
// to cxd4 unchanged. Setting "Audio processing" (n64ps5_rsp_set_hle) turns it off: then cxd4 runs every task.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "api/m64p_plugin.h"
#include "api/m64p_types.h"
#include "../../third_party/rsp-hle/src/hle.h"
#include "../../third_party/rsp-hle/src/hle_external.h"

#include "n64_bridge.h"
#include "rsp_hle_ps5.h"

unsigned int cxd4_DoRspCycles(unsigned int cycles);

static struct hle_t g_hle;
static void (*l_CheckInterrupts)(void);
static void (*l_DebugCallback)(void*, int, const char*);
static void* l_DebugContext;
static bool l_Enabled = true;

// which tasks went where, logged once per microcode (rsp-hle picks a handler once per microcode too)
enum { kSeenMax = 32 };
static struct
{
	uint32_t type, ucode;
} l_Seen[kSeenMax];
static int l_SeenCount;
static bool l_Forwarding;

static void Log(int level, const char* fmt, ...)
{
	if (!l_DebugCallback)
		return;
	char msg[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	l_DebugCallback(l_DebugContext, level, msg);
}

// the current task's type and microcode address (the OSTask in DMEM at 0xfc0), read before it runs
static uint32_t l_Type, l_Ucode;

// first time a microcode is seen: say whether HLE took it or cxd4 runs it
static void NoteTask(bool forwarded)
{
	const uint32_t type = l_Type, ucode = l_Ucode;
	for (int i = 0; i < l_SeenCount; i++)
		if (l_Seen[i].type == type && l_Seen[i].ucode == ucode)
			return;
	if (l_SeenCount < kSeenMax)
	{
		l_Seen[l_SeenCount].type = type;
		l_Seen[l_SeenCount].ucode = ucode;
		l_SeenCount++;
	}
	const char* const how = forwarded ? "LLE (cxd4)" : "HLE";
	// no OSTask in DMEM (e.g. the CIC 6105 boot code): the "type" is whatever is there
	if (type == 0 || type > 7)
	{
		Log(M64MSG_INFO, "RSP code without a task: %s", how);
		return;
	}
	static const char* const kTypes[] = {"?", "graphics", "audio"};
	Log(M64MSG_INFO, "task type %u (%s), microcode at %06x: %s", type, type <= 2 ? kTypes[type] : "other", ucode, how);
}

void n64ps5_rsp_set_hle(bool on)
{
	l_Enabled = on;
}

void n64ps5_hle_startup(void* context, void (*debug_callback)(void*, int, const char*))
{
	l_DebugContext = context;
	l_DebugCallback = debug_callback;
}

void n64ps5_hle_init(const RSP_INFO* info)
{
	hle_init(&g_hle, info->RDRAM, info->DMEM, info->IMEM, info->MI_INTR_REG, info->SP_MEM_ADDR_REG,
		info->SP_DRAM_ADDR_REG, info->SP_RD_LEN_REG, info->SP_WR_LEN_REG, info->SP_STATUS_REG, info->SP_DMA_FULL_REG,
		info->SP_DMA_BUSY_REG, info->SP_PC_REG, info->SP_SEMAPHORE_REG, info->DPC_START_REG, info->DPC_END_REG,
		info->DPC_CURRENT_REG, info->DPC_STATUS_REG, info->DPC_CLOCK_REG, info->DPC_BUFBUSY_REG,
		info->DPC_PIPEBUSY_REG, info->DPC_TMEM_REG, NULL);
	g_hle.hle_gfx = 0; // graphics go to cxd4 (HleForwardTask)
	g_hle.hle_aud = 0; // audio is done here, not sent to the audio plugin
	l_CheckInterrupts = info->CheckInterrupts;
	Log(M64MSG_INFO, "audio processing: %s", l_Enabled ? "HLE (rsp-hle), other tasks on cxd4" : "LLE (cxd4)");
}

void n64ps5_hle_rom_closed(void)
{
	g_hle.cached_ucodes.count = 0;
	l_SeenCount = 0;
}

bool n64ps5_hle_enabled(void)
{
	return l_Enabled;
}

void n64ps5_hle_execute(void)
{
	const uint32_t* task = (const uint32_t*)(g_hle.dmem + 0xfc0);
	l_Type = task[0];
	l_Ucode = task[4] & 0x00ffffff;
	l_Forwarding = false;
	hle_execute(&g_hle);
	if (!l_Forwarding)
		NoteTask(false);
}

// ---- what rsp-hle's core expects from its user (hle_external.h) -----------------------------------------
static void HleLog(int level, const char* message, va_list args)
{
	if (!l_DebugCallback)
		return;
	char msg[256];
	vsnprintf(msg, sizeof(msg), message, args);
	l_DebugCallback(l_DebugContext, level, msg);
}

void HleVerboseMessage(void* user_defined, const char* message, ...)
{
	(void)user_defined;
	(void)message;
}

void HleInfoMessage(void* user_defined, const char* message, ...)
{
	(void)user_defined;
	va_list args;
	va_start(args, message);
	HleLog(M64MSG_INFO, message, args);
	va_end(args);
}

void HleErrorMessage(void* user_defined, const char* message, ...)
{
	(void)user_defined;
	va_list args;
	va_start(args, message);
	HleLog(M64MSG_ERROR, message, args);
	va_end(args);
}

void HleWarnMessage(void* user_defined, const char* message, ...)
{
	(void)user_defined;
	va_list args;
	va_start(args, message);
	HleLog(M64MSG_WARNING, message, args);
	va_end(args);
}

void HleCheckInterrupts(void* user_defined)
{
	(void)user_defined;
	if (l_CheckInterrupts)
		l_CheckInterrupts();
}

// hle_gfx and hle_aud are off, so these are never called
void HleProcessDlistList(void* user_defined)
{
	(void)user_defined;
}

void HleProcessAlistList(void* user_defined)
{
	(void)user_defined;
}

void HleProcessRdpList(void* user_defined)
{
	(void)user_defined;
}

void HleShowCFB(void* user_defined)
{
	(void)user_defined;
}

// a task rsp-hle doesn't know: cxd4 runs the game's microcode
int HleForwardTask(void* user_defined)
{
	(void)user_defined;
	l_Forwarding = true;
	NoteTask(true);
	cxd4_DoRspCycles(0xffffffffu);
	return 0;
}
