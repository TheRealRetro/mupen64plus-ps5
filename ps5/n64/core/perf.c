// Mupen64Plus PS5: per-part timing of the emulation thread (perf.h).
// SPDX-License-Identifier: MIT

#include "perf.h"

uint64_t n64ps5_perf_total[N64PS5_PERF_COUNT];

void n64ps5_perf_take(uint64_t out[N64PS5_PERF_COUNT])
{
	for (int i = 0; i < N64PS5_PERF_COUNT; i++)
	{
		out[i] = n64ps5_perf_total[i];
		n64ps5_perf_total[i] = 0;
	}
}
