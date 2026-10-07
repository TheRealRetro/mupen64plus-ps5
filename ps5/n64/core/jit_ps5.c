// Mupen64Plus PS5: executable memory for the new dynarec (jit_ps5.h).
//
// The x86-64 dynarec writes its code into g_dev.r4300.extra_memory (32 MiB inside the core's state, so the
// code reaches the emulator's variables with 32-bit RIP-relative addresses) after one call,
// mprotect(extra_memory, 32 MiB, RWX). new_dynarec.c is compiled with -Dmprotect=n64ps5_jit_mprotect, so
// that call lands here and the core stays unmodified.
//
// A PS5 title's memory isn't executable just because it asks. Two ways are tried, once, by
// n64ps5_jit_probe() at start-up, on the code cache itself (writing `mov eax, 0x1234; ret` there and calling
// it, with the faults caught):
//   1. mprotect() itself;
//   2. direct memory mapped with CPU read/write/execute over the same addresses (MAP_FIXED): what
//      RetroArch's PS5 port does for its recompilers ("executable direct memory"). The cache isn't
//      whole 16 KiB PS5 pages (it sits 8 KiB into one), so the mapping covers the pages around it and
//      the core state sharing the first and last page is copied out and back in.
// The code cache gets whichever worked; with neither, the frontend keeps the cached interpreter.
//
// SPDX-License-Identifier: MIT

#include "jit_ps5.h"

#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "device/device.h"
#include "main/main.h"

#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#define PS5_PAGE 0x4000u

#ifdef __PROSPERO__
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, off_t* phys);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, off_t phys, size_t align);
#define SCE_PROT_RWX 0x07 // CPU read | write | execute
#define SCE_MAP_FIXED 0x10
#define SCE_WB_ONION 0 // write-back, cached: CPU memory
#endif

static int MapExecutableDirect(void* addr, size_t len);

// Executable direct memory over [addr, addr + len), which needn't be page aligned: whatever else lives in
// the first and last page keeps its contents. Only for memory nobody else touches meanwhile.
static int MapExecutableRange(void* addr, size_t len)
{
	const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(PS5_PAGE - 1);
	const uintptr_t end = ((uintptr_t)addr + len + PS5_PAGE - 1) & ~(uintptr_t)(PS5_PAGE - 1);
	const size_t head = (uintptr_t)addr - start;
	const size_t tail = end - ((uintptr_t)addr + len);
	uint8_t* saved_head = head ? (uint8_t*)malloc(head) : NULL;
	uint8_t* saved_tail = tail ? (uint8_t*)malloc(tail) : NULL;
	if ((head && !saved_head) || (tail && !saved_tail))
	{
		free(saved_head);
		free(saved_tail);
		return -3;
	}
	if (head)
		memcpy(saved_head, (void*)start, head);
	if (tail)
		memcpy(saved_tail, (uint8_t*)addr + len, tail);
	const int r = MapExecutableDirect((void*)start, end - start);
	if (r == 0)
	{
		if (head)
			memcpy((void*)start, saved_head, head);
		memset(addr, 0, len);
		if (tail)
			memcpy((uint8_t*)addr + len, saved_tail, tail);
	}
	free(saved_head);
	free(saved_tail);
	return r;
}

static int l_Method = -1; // -1 not probed, 0 none, 1 mprotect, 2 direct memory
static char l_Why[160];

static int MapExecutableDirect(void* addr, size_t len)
{
#ifdef __PROSPERO__
	off_t phys = 0;
	int r = sceKernelAllocateMainDirectMemory(len, PS5_PAGE, SCE_WB_ONION, &phys);
	if (r != 0)
		return r;
	void* where = addr;
	r = sceKernelMapDirectMemory(&where, len, SCE_PROT_RWX, SCE_MAP_FIXED, phys, PS5_PAGE);
	if (r != 0)
		return r;
	return where == addr ? 0 : -2;
#else
	// host build: the same replacement of the pages, with an anonymous RWX mapping (tests the range logic)
	void* p = mmap(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == addr ? 0 : -1;
#endif
}

// ---- calling generated code with faults caught ----------------------------------------------------------------
static sigjmp_buf l_Jump;

static void OnFault(int sig)
{
	siglongjmp(l_Jump, sig);
}

static int TryExecute(void* page)
{
	static const uint8_t code[] = {0xb8, 0x34, 0x12, 0x00, 0x00, 0xc3}; // mov eax, 0x1234; ret
	struct sigaction sa, old_segv, old_bus, old_ill;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = OnFault;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_segv);
	sigaction(SIGBUS, &sa, &old_bus);
	sigaction(SIGILL, &sa, &old_ill);
	volatile int result = -1;
	const int sig = sigsetjmp(l_Jump, 1);
	if (sig == 0)
	{
		memcpy(page, code, sizeof(code));
		__builtin___clear_cache((char*)page, (char*)page + sizeof(code));
		int (*fn)(void) = (int (*)(void))page;
		result = fn();
	}
	else
		result = -100 - sig;
	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);
	sigaction(SIGILL, &old_ill, NULL);
	return result;
}

int n64ps5_jit_probe(void)
{
	if (l_Method >= 0)
		return l_Method;
	l_Method = 0;

	// The test runs on the real code cache, before any game: what the dynarec will use is what gets proven
	// (its own mprotect call ignores failures, so it must not be left to find out).
	uint8_t* cache = (uint8_t*)g_dev.r4300.extra_memory;
	const size_t len = sizeof(g_dev.r4300.extra_memory);
	const uintptr_t start = (uintptr_t)cache & ~(uintptr_t)(PS5_PAGE - 1);
	const uintptr_t end = ((uintptr_t)cache + len + PS5_PAGE - 1) & ~(uintptr_t)(PS5_PAGE - 1);

	// host tests: N64PS5_JIT=direct skips mprotect (the direct-memory way), =none refuses both
	const char* force = getenv("N64PS5_JIT");
	if (force && strcmp(force, "none") == 0)
	{
		snprintf(l_Why, sizeof(l_Why), "turned off (N64PS5_JIT=none)");
		return l_Method;
	}
	const int skip_mprotect = force && strcmp(force, "direct") == 0;

	// 1. mprotect over the whole pages around the cache (the neighbouring core state just becomes executable)
	const int r = skip_mprotect ? -1 : mprotect((void*)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC);
	const int x = r == 0 ? TryExecute(cache) : -1;
	memset(cache, 0, 16);
	if (x == 0x1234)
	{
		l_Method = 1;
		snprintf(l_Why, sizeof(l_Why), "mprotect RWX works (cache %p)", (void*)cache);
		return l_Method;
	}
	// 2. executable direct memory over those pages (their other contents copied back)
	const int r2 = MapExecutableRange(cache, len);
	const int x2 = r2 == 0 ? TryExecute(cache) : -1;
	memset(cache, 0, 16);
	if (x2 == 0x1234)
	{
		l_Method = 2;
		snprintf(l_Why, sizeof(l_Why), "executable direct memory works (cache %p; mprotect: %d, ran %d)", (void*)cache, r, x);
		return l_Method;
	}
	snprintf(l_Why, sizeof(l_Why), "no executable memory: mprotect %d (ran %d), direct memory %x (ran %d), cache %p", r, x,
		(unsigned)r2, x2, (void*)cache);
	return l_Method;
}

const char* n64ps5_jit_probe_result(void)
{
	return l_Why;
}

int n64ps5_jit_mprotect(void* addr, size_t len, int prot)
{
	const int is_cache = addr == (void*)g_dev.r4300.extra_memory && len <= sizeof(g_dev.r4300.extra_memory);
	if (!is_cache)
		return mprotect(addr, len, prot);
	// The probe made the code cache executable once for the whole run, and it stays that way: the dynarec's
	// cleanup at the end of each game asks for read/write only, and passing that through left the next game
	// running from non-executable memory (0.4: SIGSEGV at the cache on the second game of a session).
	if (n64ps5_jit_probe() != 0)
		return 0;
	if (!(prot & PROT_EXEC))
		return mprotect(addr, len, prot);
	DebugMessage(M64MSG_ERROR, "dynarec: no executable memory for %p (%s)", addr, l_Why);
	return -1;
}
