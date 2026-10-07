// Mupen64Plus PS5: the audio plugin, straight to libSceAudioOut (ProsperoAudio.h).
//
// The N64's audio interface (AI) plays 16-bit stereo samples from RDRAM at a rate the game picks
// (VI clock / (AI_DACRATE + 1): 32 kHz, 22.05 kHz, 44.1 kHz...). Each time the game queues a buffer
// (AI_LEN written) the samples are read, resampled to 48 kHz and pushed into the AudioOut ring.
//
// The console's 60 Hz flips pace the emulation (fe_emu.cpp); the N64 does not run at exactly 60 Hz, so the
// resampler stretches the sound by up to 0.5% to keep the ring at a target level (the latency). The
// correction is proportional plus integral: 0.4 had the proportional part only, which settled wherever it
// balanced the clock drift (~5800 frames, ~120 ms on the console); the integral part removes that offset.
// The target follows the game: the ring rises by a whole AI buffer at a time, so it is kept at the largest
// recent buffer plus a margin, between 67 ms (small buffers) and 125 ms (games that queue big ones).
// When the ring is full anyway (a PAL game, the flips not blocking), the plugin waits for room, which paces
// the game on the audio clock instead.
//
// SPDX-License-Identifier: MIT

#include "api/m64p_plugin.h"
#include "api/m64p_types.h"

#include "ProsperoAudio.h"
#include "n64_bridge.h"
#include "perf.h"
#include "static_dynlib.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace
{
constexpr int kVersion = 0x010000;
constexpr int kApiVersion = 0x020000;
constexpr double kMaxRateAdjust = 0.005; // the most the sound is stretched or squeezed (0.5%)
constexpr int kTargetMin = 3200; // 67 ms at 48 kHz
constexpr int kTargetMax = 6000; // 125 ms
constexpr int kTargetMargin = 1280; // over the largest recent buffer: a few AudioOut grains
constexpr double kIntegralStep = 0.00002; // per buffer (30-60 a second): settles in seconds, not frames

void (*g_debug)(void*, int, const char*) = nullptr;
void* g_debug_ctx = nullptr;
bool g_init = false;
AUDIO_INFO g_info;
m64p_system_type g_system = SYSTEM_NTSC;
std::atomic<bool> g_mute{false};
std::atomic<bool> g_fast_forward{false};
std::atomic<int> g_rate{0};
int g_volume = 100;

// resampler state
double g_pos = 0.0; // position between g_prev and the next input sample
double g_integral = 0.0; // the rate control's integral part (a rate adjustment, clamped)
double g_chunk_peak = 0.0; // the largest recent buffer in 48 kHz frames, decaying
std::atomic<int> g_target{3200}; // the ring level aimed at (frames)
int16_t g_prev[2] = {0, 0};
std::vector<int16_t> g_out;

void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Log(int level, const char* fmt, ...)
{
	if (!g_debug)
		return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_debug(g_debug_ctx, level, buf);
}

unsigned ViClock(m64p_system_type type)
{
	switch (type)
	{
		case SYSTEM_PAL: return 49656530u;
		case SYSTEM_MPAL: return 48628316u;
		default: return 48681812u;
	}
}

// Push, waiting (up to ~100 ms) for room unless fast-forwarding.
void PushOut(const int16_t* stereo, int frames)
{
	int done = 0;
	const uint64_t t0 = n64ps5_perf_now();
	bool waited = false;
	for (int tries = 0; done < frames; tries++)
	{
		done += ps5audio::Push(stereo + done * 2, frames - done);
		if (done >= frames || g_fast_forward.load(std::memory_order_relaxed) || tries >= 100)
			break;
		usleep(1000);
		waited = true;
	}
	if (waited)
		n64ps5_perf_add(N64PS5_PERF_WAIT, n64ps5_perf_now() - t0);
}

// ---- the plugin API -------------------------------------------------------------------------------------
m64p_error PluginStartup(m64p_dynlib_handle, void* context, void (*debug)(void*, int, const char*))
{
	if (g_init)
		return M64ERR_ALREADY_INIT;
	g_debug = debug;
	g_debug_ctx = context;
	g_init = true;
	return M64ERR_SUCCESS;
}

m64p_error PluginShutdown()
{
	if (!g_init)
		return M64ERR_NOT_INIT;
	g_init = false;
	return M64ERR_SUCCESS;
}

m64p_error PluginGetVersion(m64p_plugin_type* type, int* version, int* api, const char** name, int* caps)
{
	if (type)
		*type = M64PLUGIN_AUDIO;
	if (version)
		*version = kVersion;
	if (api)
		*api = kApiVersion;
	if (name)
		*name = "PS5 AudioOut";
	if (caps)
		*caps = 0;
	return M64ERR_SUCCESS;
}

int InitiateAudio(AUDIO_INFO info)
{
	g_info = info;
	return 1;
}

int RomOpen()
{
	g_pos = 0.0;
	g_integral = 0.0;
	g_chunk_peak = 0.0;
	g_prev[0] = g_prev[1] = 0;
	g_rate.store(0);
	return 1;
}

void RomClosed()
{
	g_rate.store(0);
}

void AiDacrateChanged(int system_type)
{
	g_system = m64p_system_type(system_type);
	const unsigned dacrate = g_info.AI_DACRATE_REG ? *g_info.AI_DACRATE_REG : 0;
	const int rate = int(ViClock(g_system) / (dacrate + 1));
	if (rate != g_rate.load())
		Log(M64MSG_INFO, "audio: game rate %d Hz (%s)", rate, g_system == SYSTEM_PAL ? "PAL" : g_system == SYSTEM_MPAL ? "MPAL" : "NTSC");
	g_rate.store(rate);
}

void AiLenChanged()
{
	if (!g_info.RDRAM || !g_info.AI_LEN_REG || !g_info.AI_DRAM_ADDR_REG)
		return;
	const uint32_t len = *g_info.AI_LEN_REG & 0x3fff8u; // bytes, 4 per stereo sample
	uint32_t addr = *g_info.AI_DRAM_ADDR_REG & 0xfffff8u;
	const int rate = g_rate.load();
	if (len == 0 || rate <= 0)
		return;
	if (addr + len > 0x800000u)
		return; // past RDRAM: a broken pointer, not sound
	if (g_mute.load(std::memory_order_relaxed))
		return;

	const uint32_t* words = reinterpret_cast<const uint32_t*>(g_info.RDRAM + addr);
	const int in_frames = int(len / 4);

	// Dynamic rate control: a little more output per input sample when the ring is below the target, less
	// when above. Proportional part (instant) + integral part (cancels a steady clock drift).
	const double chunk = in_frames * double(ps5audio::kRate) / rate;
	g_chunk_peak = chunk > g_chunk_peak ? chunk : g_chunk_peak * 0.999; // forgets over a few hundred buffers
	int target = int(g_chunk_peak) + kTargetMargin;
	target = target < kTargetMin ? kTargetMin : target > kTargetMax ? kTargetMax : target;
	g_target.store(target, std::memory_order_relaxed);
	double err = double(target - ps5audio::Queued()) / target;
	if (err > 1.0)
		err = 1.0;
	if (err < -1.0)
		err = -1.0;
	if (!g_fast_forward.load(std::memory_order_relaxed)) // the ring is full on purpose then: don't wind up
		g_integral += err * kIntegralStep;
	if (g_integral > kMaxRateAdjust)
		g_integral = kMaxRateAdjust;
	if (g_integral < -kMaxRateAdjust)
		g_integral = -kMaxRateAdjust;
	double adjust = kMaxRateAdjust * err + g_integral;
	if (adjust > kMaxRateAdjust)
		adjust = kMaxRateAdjust;
	if (adjust < -kMaxRateAdjust)
		adjust = -kMaxRateAdjust;
	const double step = double(rate) / (double(ps5audio::kRate) * (1.0 + adjust));

	const int max_out = int(in_frames / step) + 4;
	if (int(g_out.size()) < max_out * 2)
		g_out.resize(size_t(max_out) * 2);
	int16_t* out = g_out.data();
	int n = 0;
	const int vol = g_volume;
	// Linear interpolation between g_prev (position 0) and each next sample (position 1).
	for (int i = 0; i < in_frames; i++)
	{
		// each 32-bit word (host order) holds the left sample in its high half, the right in its low half
		const uint32_t w = words[i];
		const int16_t cur[2] = {int16_t(w >> 16), int16_t(w & 0xffffu)};
		while (g_pos < 1.0 && n < max_out)
		{
			for (int c = 0; c < 2; c++)
			{
				int v = int(g_prev[c] + (cur[c] - g_prev[c]) * g_pos);
				if (vol != 100)
					v = v * vol / 100;
				out[n * 2 + c] = int16_t(v);
			}
			n++;
			g_pos += step;
		}
		g_pos -= 1.0;
		g_prev[0] = cur[0];
		g_prev[1] = cur[1];
	}
	PushOut(out, n);
}

void ProcessAList()
{
}

void SetSpeedFactor(int)
{
}

void VolumeMute()
{
	g_mute.store(!g_mute.load());
}

void VolumeUp()
{
	g_volume = g_volume + 10 > 100 ? 100 : g_volume + 10;
}

void VolumeDown()
{
	g_volume = g_volume - 10 < 0 ? 0 : g_volume - 10;
}

int VolumeGetLevel()
{
	return g_mute.load() ? 0 : g_volume;
}

void VolumeSetLevel(int level)
{
	g_volume = level < 0 ? 0 : level > 100 ? 100 : level;
}

const char* VolumeGetString()
{
	static char text[32];
	if (g_mute.load())
		snprintf(text, sizeof(text), "Mute");
	else
		snprintf(text, sizeof(text), "%d%%", g_volume);
	return text;
}

#define SYM(name) {#name, reinterpret_cast<m64p_function>(&name)}
const m64ps5_symbol kSymbols[] = {
	SYM(PluginStartup),
	SYM(PluginShutdown),
	SYM(PluginGetVersion),
	SYM(InitiateAudio),
	SYM(RomOpen),
	SYM(RomClosed),
	SYM(AiDacrateChanged),
	SYM(AiLenChanged),
	SYM(ProcessAList),
	SYM(SetSpeedFactor),
	SYM(VolumeMute),
	SYM(VolumeUp),
	SYM(VolumeDown),
	SYM(VolumeGetLevel),
	SYM(VolumeSetLevel),
	SYM(VolumeGetString),
	{nullptr, nullptr},
};
} // namespace

extern "C" const m64ps5_library m64ps5_audio_lib = {"ps5-audioout", kSymbols};

extern "C" void n64ps5_audio_set_mute(bool mute)
{
	g_mute.store(mute);
}

extern "C" void n64ps5_audio_set_fast_forward(bool on)
{
	g_fast_forward.store(on);
}

extern "C" int n64ps5_audio_game_rate(void)
{
	return g_rate.load();
}

extern "C" int n64ps5_audio_target(void)
{
	return g_target.load(std::memory_order_relaxed);
}
