// Mupen64Plus PS5: where the emulation thread's time goes (n64/core/perf.c), shown with the FPS counter
// and logged every 10 s, to tell a CPU-bound game from an RSP- or RDP-bound one.
//
// It runs around every RSP task and every RDP list (thousands per frame in some games), so it must cost
// next to nothing: the CPU's time-stamp counter (rdtsc, no system call; 0.2 used clock_gettime and slowed
// GoldenEye down) and plain counters (everything is counted on the emulation thread).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	N64PS5_PERF_RSP, // cxd4 running microcode (DoRspCycles), without the RDP lists it submits meanwhile
	N64PS5_PERF_RDP, // angrylion's RDP command lists, incl. waiting for its workers (ProcessRDPList)
	N64PS5_PERF_VI, // angrylion's VI filter (UpdateScreen, without the frontend's part)
	N64PS5_PERF_WAIT, // the frontend waiting for the presenter (vsync pacing) or for audio room
	N64PS5_PERF_COUNT
};

// totals in time-stamp-counter ticks (n64/core/perf.c)
extern uint64_t n64ps5_perf_total[N64PS5_PERF_COUNT];

static inline uint64_t n64ps5_perf_now(void)
{
	return __builtin_ia32_rdtsc();
}

// Emulation thread only.
static inline void n64ps5_perf_add(int slot, uint64_t ticks)
{
	n64ps5_perf_total[slot] += ticks;
}

// The totals since the last call (and resets them).
void n64ps5_perf_take(uint64_t out[N64PS5_PERF_COUNT]);

#ifdef __cplusplus
}
#endif
