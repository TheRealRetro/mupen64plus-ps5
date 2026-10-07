// Mupen64Plus PS5: sound through libSceAudioOut.
//
// The port opens the main output on the system user (255), S16 stereo, 48 kHz, 256-frame grains, and a
// thread feeds it: sceAudioOutOutput blocks until the previous grain was consumed, so the thread runs at
// exactly the console's audio clock. The emulation thread pushes into a single-producer single-consumer
// ring; when the ring runs dry the thread plays silence (counted in Underruns) instead of blocking.
//
// SPDX-License-Identifier: MIT

#include "ProsperoAudio.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <atomic>
#include <cstring>
#include <thread>

namespace ps5audio
{
namespace
{
struct State
{
	int handle = -1;
	std::thread thread;
	std::atomic<bool> quit{false};
	alignas(64) std::atomic<uint32_t> head{0}; // written by the producer (frames, wraps)
	alignas(64) std::atomic<uint32_t> tail{0}; // written by the audio thread
	std::atomic<uint64_t> underruns{0};
	int16_t ring[kCapacity * 2];
};
State* g = nullptr;

void Run()
{
	alignas(64) int16_t grain[kGrain * 2];
	while (!g->quit.load(std::memory_order_relaxed))
	{
		const uint32_t tail = g->tail.load(std::memory_order_relaxed);
		const uint32_t head = g->head.load(std::memory_order_acquire);
		const uint32_t avail = head - tail;
		if (avail >= uint32_t(kGrain))
		{
			for (int i = 0; i < kGrain; i++)
			{
				const uint32_t pos = (tail + i) % kCapacity;
				grain[i * 2] = g->ring[pos * 2];
				grain[i * 2 + 1] = g->ring[pos * 2 + 1];
			}
			g->tail.store(tail + kGrain, std::memory_order_release);
		}
		else
		{
			memset(grain, 0, sizeof(grain));
			g->underruns.fetch_add(1, std::memory_order_relaxed);
		}
		sceAudioOutOutput(g->handle, grain);
	}
}
} // namespace

bool Init()
{
	if (g)
		return true;
	const int r = sceAudioOutInit();
	OrbisLog("[audio] sceAudioOutInit -> %x", r);
	const int handle = sceAudioOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, SCE_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrain,
		kRate, SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
	OrbisLog("[audio] sceAudioOutOpen(48 kHz, S16 stereo, %d) -> %d", kGrain, handle);
	if (handle <= 0)
		return false;
	g = new State();
	g->handle = handle;
	g->thread = std::thread(Run);
	return true;
}

void Shutdown()
{
	if (!g)
		return;
	g->quit.store(true);
	if (g->thread.joinable())
		g->thread.join();
	sceAudioOutClose(g->handle);
	OrbisLog("[audio] closed, %llu underruns", (unsigned long long)g->underruns.load());
	delete g;
	g = nullptr;
}

int Free()
{
	if (!g)
		return kCapacity;
	return kCapacity - int(g->head.load(std::memory_order_relaxed) - g->tail.load(std::memory_order_acquire));
}

int Queued()
{
	return kCapacity - Free();
}

int Push(const int16_t* stereo, int frames)
{
	if (!g || frames <= 0)
		return 0;
	const int n = frames < Free() ? frames : Free();
	const uint32_t head = g->head.load(std::memory_order_relaxed);
	for (int i = 0; i < n; i++)
	{
		const uint32_t pos = (head + i) % kCapacity;
		g->ring[pos * 2] = stereo[i * 2];
		g->ring[pos * 2 + 1] = stereo[i * 2 + 1];
	}
	g->head.store(head + n, std::memory_order_release);
	return n;
}

uint64_t Underruns()
{
	return g ? g->underruns.load(std::memory_order_relaxed) : 0;
}
} // namespace ps5audio
