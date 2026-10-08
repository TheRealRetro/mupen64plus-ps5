// Mupen64Plus PS5 frontend: mupen64plus-core and its built-in plugins (fe_emu.h).
//
// The core is linked into eboot.bin with its four plugins (static_dynlib.h): the cxd4 RSP, angrylion's
// software RDP (gfx_ps5.c), AudioOut (audio_ps5.cpp) and DualSense input (input_ps5.cpp). A game runs
// inside CoreDoCommand(M64CMD_EXECUTE) on the frontend's thread (8 MiB stack, main-boot.cpp); once per
// video interrupt the video plugin calls n64ps5_on_vi(), and that is where this file does its work:
//   - the pads are read (ps5input::Poll), L3+R3 or the touchpad opens the pause menu (fe_menu.cpp), which
//     runs right there, so the emulation simply waits while it is open;
//   - the picture goes to a presenter thread, which scales it (DrawN64), draws the OSD and flips it. The
//     emulation of the next frame overlaps with that, and Submit() waits while the previous picture is
//     still queued, so the console's 60 Hz flips pace NTSC games (the core's speed limiter is off);
//   - PAL games (50 Hz) keep the core's speed limiter and don't wait for the flips;
//   - fast forward (pause menu): no limiter, no waiting, one picture in four shown, sound dropped when the
//     ring is full.
//
// SPDX-License-Identifier: MIT

#include "fe_emu.h"

#include "fe_games.h"
#include "fe_menu.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoAudio.h"
#include "ProsperoCrash.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoThread.h"
#include "ProsperoVideo.h"

#include "jit_ps5.h"
#include "n64_bridge.h"
#include "perf.h"
#include "static_dynlib.h"

#define M64P_CORE_PROTOTYPES 1
#include "api/m64p_common.h"
#include "api/m64p_config.h"
#include "api/m64p_frontend.h"
#include "api/m64p_types.h"

#include "unzip.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// mupen64plus.ini (the core's ROM catalog: save types, players, per-game fixes) built into the ELF and
// written to /data/mupen64plus/data/ when it is missing or different.
#define FE_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

FE_INCBIN(n64ps5_rom_catalog, M64P_INI_PATH)

namespace
{
constexpr int kFrontendApiVersion = 0x020106;
constexpr size_t kMaxRomSize = 64u << 20; // the biggest N64 cartridges are 64 MiB

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- the presenter thread ---------------------------------------------------------------------------------
class Presenter
{
public:
	void Start()
	{
		std::lock_guard<std::mutex> l(m_lock);
		m_quit = false;
		m_paused = false;
		m_has_pending = false;
		m_busy = false;
		m_thread = ps5::BigThread([this] { Run(); }, 512 * 1024);
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> l(m_lock);
			m_quit = true;
		}
		m_cv.notify_all();
		m_thread.join();
	}

	// Emulation thread: queue a picture (copied), after the previous one has been taken.
	void Submit(const uint32_t* px, int w, int h, int pitch, bool wait_vsync)
	{
		std::unique_lock<std::mutex> l(m_lock);
		const uint64_t t0 = n64ps5_perf_now();
		m_cv.wait(l, [&] { return !m_has_pending || m_quit || m_paused; });
		n64ps5_perf_add(N64PS5_PERF_WAIT, n64ps5_perf_now() - t0);
		if (m_quit)
			return;
		m_pending.resize(size_t(w) * h);
		for (int y = 0; y < h; y++)
			memcpy(&m_pending[size_t(y) * w], px + size_t(y) * pitch, size_t(w) * 4);
		m_pw = w;
		m_ph = h;
		m_pending_vsync = wait_vsync;
		m_has_pending = true;
		l.unlock();
		m_cv.notify_all();
	}

	// The pause menu takes the screen: wait until the thread is done with it.
	void Pause()
	{
		std::unique_lock<std::mutex> l(m_lock);
		m_paused = true;
		m_cv.wait(l, [&] { return !m_busy; });
	}

	void Resume()
	{
		{
			std::lock_guard<std::mutex> l(m_lock);
			m_paused = false;
		}
		m_cv.notify_all();
	}

	// Only while paused or stopped.
	void RedrawCurrent()
	{
		const fe::Settings& cfg = fe::Config();
		if (!m_current.empty())
			ps5video::DrawN64(m_current.data(), m_cw, m_cw, m_ch, ps5video::Aspect(cfg.aspect), cfg.smooth);
	}

	void Forget()
	{
		m_current.clear();
		m_pending.clear();
	}

private:
	void Run()
	{
		std::unique_lock<std::mutex> l(m_lock);
		for (;;)
		{
			m_cv.wait(l, [&] { return m_quit || (m_has_pending && !m_paused); });
			if (m_quit)
				break;
			m_current.swap(m_pending);
			m_cw = m_pw;
			m_ch = m_ph;
			const bool vsync = m_pending_vsync;
			m_has_pending = false;
			m_busy = true;
			l.unlock();
			m_cv.notify_all();
			Draw(vsync);
			l.lock();
			m_busy = false;
			m_cv.notify_all();
		}
	}

	void Draw(bool vsync);

	std::mutex m_lock;
	std::condition_variable m_cv;
	ps5::BigThread m_thread;
	bool m_quit = false, m_paused = false, m_has_pending = false, m_busy = false, m_pending_vsync = true;
	std::vector<uint32_t> m_pending, m_current;
	int m_pw = 0, m_ph = 0, m_cw = 0, m_ch = 0;
};

// ---- state ----------------------------------------------------------------------------------------------
struct State
{
	bool core_ok = false;
	bool running = false;
	bool stopping = false;
	std::string rom_path;
	std::string file_base;
	std::string title;
	bool pal = false;
	std::atomic<bool> fast_forward{false};
	bool limiter_set = false;
	bool limiter_on = true;
	emu::ExitReason exit = emu::ExitReason::BackToList;
	uint32_t vi = 0;
	uint32_t prev_buttons = 0;
	bool block_until_release = false;
	int pending_save_slot = -1;
	// FPS counter
	double fps_t0 = 0;
	uint64_t fps_tsc0 = 0;
	uint32_t fps_vi0 = 0;
	// OSD
	std::mutex osd_lock;
	std::string osd;
	double osd_until = 0;
	char fps_text[48] = "";
	char perf_text[64] = "";
};
State g;
Presenter g_presenter;

const m64ps5_library* const kPlugins[] = {&m64ps5_gfx_lib, &m64ps5_audio_lib, &m64ps5_input_lib, &m64ps5_rsp_lib};
const m64p_plugin_type kPluginTypes[] = {M64PLUGIN_GFX, M64PLUGIN_AUDIO, M64PLUGIN_INPUT, M64PLUGIN_RSP};

void Presenter::Draw(bool vsync)
{
	N64_STAGE(Pool, "present");
	const fe::Settings& cfg = fe::Config();
	ps5video::Rect r = ps5video::DrawN64(m_current.data(), m_cw, m_cw, m_ch, ps5video::Aspect(cfg.aspect), cfg.smooth);
	const ps5video::Rect pic = ps5video::LastN64Rect();
	std::string osd;
	char fps[48] = "";
	char perf[64] = "";
	{
		std::lock_guard<std::mutex> l(g.osd_lock);
		if (!g.osd.empty() && Now() < g.osd_until)
			osd = g.osd;
		if (cfg.show_fps)
		{
			memcpy(fps, g.fps_text, sizeof(fps));
			memcpy(perf, g.perf_text, sizeof(perf));
		}
	}
	if (!osd.empty())
		fe::DrawText(pic.x + 24, pic.y + 20, fe::FitText(osd, 3, pic.w - 48).c_str(), 3, ps5video::Rgb(255, 255, 255));
	if (fps[0])
		fe::DrawText(pic.x + pic.w - 24 - fe::TextWidth(fps, 3), pic.y + 20, fps, 3, ps5video::Rgb(255, 255, 120));
	if (perf[0])
		fe::DrawText(pic.x + pic.w - 24 - fe::TextWidth(perf, 2), pic.y + 60, perf, 2, ps5video::Rgb(255, 255, 120));
	ps5video::Present(r.x, r.y, r.w, r.h, vsync);
}

void CoreLog(void* context, int level, const char* message)
{
	const char* who = static_cast<const char*>(context);
	static const char* const kLevel[] = {"", "error", "warning", "info", "status", "verbose"};
	if (level == M64MSG_VERBOSE)
		return;
	OrbisLog("[%s %s] %s", who ? who : "m64p", (level >= 1 && level <= 5) ? kLevel[level] : "?", message);
}

void CoreState(void*, m64p_core_param param, int value)
{
	if (param == M64CORE_STATE_SAVECOMPLETE || param == M64CORE_STATE_LOADCOMPLETE)
	{
		const bool save = param == M64CORE_STATE_SAVECOMPLETE;
		OrbisLog("[emu] state %s %s", save ? "save" : "load", value ? "done" : "failed");
		if (save)
			g.pending_save_slot = -1;
		if (!value)
			emu::Osd(save ? "Could not save the state" : "Could not load the state");
	}
}

void SetCoreString(m64p_handle section, const char* key, const std::string& value)
{
	ConfigSetParameter(section, key, M64TYPE_STRING, value.c_str());
}

void SetCoreInt(m64p_handle section, const char* key, int value)
{
	ConfigSetParameter(section, key, M64TYPE_INT, &value);
}

void SetCoreBool(m64p_handle section, const char* key, bool value)
{
	int v = value ? 1 : 0;
	ConfigSetParameter(section, key, M64TYPE_BOOL, &v);
}

// The ROM catalog in <data>/mupen64plus.ini, rewritten when it differs from the built-in copy.
void InstallRomCatalog(const std::string& data_dir)
{
	const size_t size = size_t(n64ps5_rom_catalog_end - n64ps5_rom_catalog_begin);
	const std::string path = data_dir + "/mupen64plus.ini";
	struct stat st = {};
	if (stat(path.c_str(), &st) == 0 && size_t(st.st_size) == size)
		return;
	const std::string tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
	{
		OrbisLog("[emu] can't write %s", tmp.c_str());
		return;
	}
	const bool ok = fwrite(n64ps5_rom_catalog_begin, 1, size, f) == size;
	fclose(f);
	if (ok && rename(tmp.c_str(), path.c_str()) == 0)
		OrbisLog("[emu] ROM catalog written to %s (%zu bytes)", path.c_str(), size);
	else
		unlink(tmp.c_str());
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string* error)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
	{
		*error = "Can't open the file";
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size_t(size) > kMaxRomSize)
	{
		fclose(f);
		*error = "Not an N64 ROM (wrong size)";
		return false;
	}
	out.resize(size_t(size));
	const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	if (!ok)
		*error = "Read error";
	return ok;
}

// The biggest file in the archive is the ROM.
bool ReadZippedRom(const std::string& path, std::vector<uint8_t>& out, std::string* error)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
	{
		*error = "Can't open the zip file";
		return false;
	}
	uLong best = 0;
	char best_name[512] = "";
	for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
	{
		unz_file_info info;
		char name[512];
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) == UNZ_OK &&
			info.uncompressed_size > best)
		{
			best = info.uncompressed_size;
			snprintf(best_name, sizeof(best_name), "%s", name);
		}
	}
	bool ok = false;
	if (best == 0 || best > kMaxRomSize)
		*error = "No N64 ROM in the zip file";
	else if (unzLocateFile(z, best_name, 0) != UNZ_OK || unzOpenCurrentFile(z) != UNZ_OK)
		*error = "Can't read the zip file";
	else
	{
		out.resize(best);
		size_t got = 0;
		int n;
		while (got < out.size() && (n = unzReadCurrentFile(z, out.data() + got, unsigned(out.size() - got))) > 0)
			got += size_t(n);
		unzCloseCurrentFile(z);
		ok = got == out.size();
		if (!ok)
			*error = "The zip file is damaged";
	}
	unzClose(z);
	return ok;
}

bool IsPalCountry(uint8_t code)
{
	switch (code)
	{
		case 0x44: case 0x46: case 0x49: case 0x50: case 0x53: case 0x55: case 0x58: case 0x59: return true;
		default: return false;
	}
}

// "Super Mario 64 (U) [!]" -> "Super Mario 64"
std::string CleanGoodName(const char* good)
{
	std::string s = good;
	const size_t cut = s.find_first_of("([");
	if (cut != std::string::npos)
		s.resize(cut);
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s;
}

// The core names a game's saves "<GoodName (32 chars)>-<MD5 (8)>.eep/.sra/.fla/.mpk" in saves/.
void LogSaves(const char* when, const char* goodname, const char* md5)
{
	char prefix[64];
	snprintf(prefix, sizeof(prefix), "%.32s-%.8s.", goodname, md5);
	const std::string dir = OrbisDir("saves");
	int found = 0;
	if (DIR* d = opendir(dir.c_str()))
	{
		while (dirent* e = readdir(d))
		{
			if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
				continue;
			struct stat st = {};
			stat((dir + "/" + e->d_name).c_str(), &st);
			char t[32];
			const time_t mt = st.st_mtime;
			strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", localtime(&mt));
			OrbisLog("[saves] %s: %s, %lld bytes, written %s", when, e->d_name, (long long)st.st_size, t);
			found++;
		}
		closedir(d);
	}
	if (!found)
		OrbisLog("[saves] %s: none yet for \"%s\" in %s", when, prefix, dir.c_str());
}

std::string StatePath(int slot)
{
	return OrbisDir("states") + "/" + g.file_base + ".st" + std::to_string(slot);
}

void SetLimiter(bool on)
{
	if (g.limiter_set && g.limiter_on == on)
		return;
	int v = on ? 1 : 0;
	CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &v);
	g.limiter_set = true;
	g.limiter_on = on;
}

void StopGame(emu::ExitReason why)
{
	if (g.stopping)
		return;
	g.exit = why;
	g.stopping = true;
	CoreDoCommand(M64CMD_STOP, 0, nullptr);
}

void OpenPauseMenu()
{
	N64_STAGE(Emu, "pause menu");
	g_presenter.Pause();
	n64ps5_input_block(true);
	for (int i = 0; i < ps5input::kMaxPads; i++)
		ps5input::SetRumble(i, 0, 0);
	const fe::PauseAction action = fe::PauseMenu();
	ps5video::FillRect(0, 0, ps5video::kWidth, ps5video::kHeight, ps5video::Rgb(0, 0, 0));
	ps5video::InvalidateN64();
	emu::ApplySettings();
	g_presenter.Resume();
	// the button that closed the menu must not reach the game
	g.block_until_release = true;
	switch (action)
	{
		case fe::PauseAction::Resume: break;
		case fe::PauseAction::BackToList: StopGame(emu::ExitReason::BackToList); break;
		case fe::PauseAction::Quit: StopGame(emu::ExitReason::Quit); break;
	}
	N64_STAGE(Emu, "running");
}

void UpdateFps()
{
	const double now = Now();
	if (g.fps_t0 == 0)
	{
		g.fps_t0 = now;
		g.fps_tsc0 = n64ps5_perf_now();
		g.fps_vi0 = g.vi;
		uint64_t discard[N64PS5_PERF_COUNT];
		n64ps5_perf_take(discard);
		return;
	}
	if (now - g.fps_t0 < 1.0)
		return;
	const double secs = now - g.fps_t0;
	const double vis = (g.vi - g.fps_vi0) / secs;
	const double expected = g.pal ? 50.0 : 60.0;
	// where the emulation thread's time went: the rest (CPU) is what the other parts didn't take
	uint64_t ns[N64PS5_PERF_COUNT];
	n64ps5_perf_take(ns);
	const uint64_t tsc = n64ps5_perf_now();
	const double wall = double(tsc - g.fps_tsc0);
	g.fps_tsc0 = tsc;
	auto pct = [&](uint64_t v) { return int(v * 100.0 / wall + 0.5); };
	const int rsp = pct(ns[N64PS5_PERF_RSP]), rdp = pct(ns[N64PS5_PERF_RDP]), vi = pct(ns[N64PS5_PERF_VI]);
	const int wait = pct(ns[N64PS5_PERF_WAIT]);
	const int cpu = std::max(0, 100 - rsp - rdp - vi - wait);
	std::lock_guard<std::mutex> l(g.osd_lock);
	snprintf(g.fps_text, sizeof(g.fps_text), "%.0f VI/s %.0f%%", vis, vis * 100.0 / expected);
	snprintf(g.perf_text, sizeof(g.perf_text), "cpu %d  rsp %d  rdp %d  vi %d  idle %d", cpu, rsp, rdp, vi, wait);
	g.fps_t0 = now;
	g.fps_vi0 = g.vi;
}
} // namespace

// =========================================================================================================
// Called by the video plugin on every video interrupt (emulation thread).
// =========================================================================================================
extern "C" void n64ps5_on_vi(const n64ps5_frame* frame)
{
	g.vi++;
	if (g.vi == 1)
		N64_STAGE(Emu, "running");
	const bool ff = g.fast_forward.load();
	SetLimiter(g.pal && !ff);

	ps5input::Poll();
	const uint32_t raw = ps5input::Pad(0).raw_buttons;
	const uint32_t pressed = raw & ~g.prev_buttons;
	g.prev_buttons = raw;
	if (g.block_until_release && raw == 0)
	{
		g.block_until_release = false;
		n64ps5_input_block(false);
	}

	const uint32_t combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
	const bool menu = (pressed & SCE_PAD_BUTTON_TOUCH_PAD) || ((raw & combo) == combo && (pressed & combo));
	if (menu && !g.stopping)
	{
		OpenPauseMenu();
		g.prev_buttons = ps5input::Pad(0).raw_buttons;
	}

	UpdateFps();
	if (g.vi == 120 || g.vi % 600 == 0)
		OrbisLog("[emu] VI %u: %s (%s), audio queued %d (target %d), underruns %llu", g.vi, g.fps_text, g.perf_text,
			ps5audio::Queued(), n64ps5_audio_target(),
			(unsigned long long)ps5audio::Underruns());

	if (frame && !g.stopping && (!ff || (g.vi % 4) == 0))
		g_presenter.Submit(frame->pixels, frame->width, frame->height, frame->pitch, !ff && !g.pal);
}

// =========================================================================================================
// emu::
// =========================================================================================================
namespace emu
{
bool InitCore()
{
	if (g.core_ok)
		return true;
	const std::string config_dir = OrbisDir("config");
	const std::string data_dir = OrbisDir("data");
	OrbisMkdirs(config_dir);
	OrbisMkdirs(data_dir);
	InstallRomCatalog(data_dir);

	m64p_error r = CoreStartup(kFrontendApiVersion, config_dir.c_str(), data_dir.c_str(), (void*)"core", CoreLog,
		nullptr, CoreState);
	if (r != M64ERR_SUCCESS)
	{
		OrbisLog("[emu] CoreStartup -> %d (%s)", int(r), CoreErrorMessage(r));
		return false;
	}

	// what the port needs whatever mupen64plus.cfg says
	m64p_handle core = nullptr;
	if (ConfigOpenSection("Core", &core) == M64ERR_SUCCESS && core)
	{
		SetCoreInt(core, "R4300Emulator", 1); // cached interpreter until a game starts (RunGame picks)
		SetCoreBool(core, "OnScreenDisplay", false);
		SetCoreBool(core, "AutoStateSlotIncrement", false);
		SetCoreString(core, "SaveSRAMPath", OrbisDir("saves"));
		SetCoreString(core, "SaveStatePath", OrbisDir("states"));
		SetCoreString(core, "ScreenshotPath", OrbisDir("screenshots"));
		SetCoreString(core, "SharedDataPath", data_dir);
	}
	m64p_handle rsp = nullptr;
	const m64p_dynlib_handle core_handle = M64PS5_HANDLE(m64ps5_core_lib);
	static const char* const kNames[] = {"video", "audio", "input", "rsp"};
	for (int i = 0; i < 4; i++)
	{
		auto startup = reinterpret_cast<ptr_PluginStartup>(m64ps5_getproc(kPlugins[i], "PluginStartup"));
		r = startup ? startup(core_handle, (void*)kNames[i], CoreLog) : M64ERR_INPUT_INVALID;
		OrbisLog("[emu] %s plugin (%s): PluginStartup -> %d", kNames[i], kPlugins[i]->lib_name, int(r));
		if (r != M64ERR_SUCCESS && r != M64ERR_ALREADY_INIT)
			return false;
	}
	// cxd4 is our RSP: graphics and audio tasks run as microcode (LLE), for angrylion and the AI
	if (ConfigOpenSection("rsp-cxd4", &rsp) == M64ERR_SUCCESS && rsp)
	{
		SetCoreBool(rsp, "DisplayListToGraphicsPlugin", false);
		SetCoreBool(rsp, "AudioListToAudioPlugin", false);
	}
	ConfigSaveFile();
	// can this process run generated code? (the dynarec needs it; tested once, on a page of its own)
	const int jit = n64ps5_jit_probe();
	OrbisLog("[emu] dynarec: %s -> %s", n64ps5_jit_probe_result(), jit ? "available" : "cached interpreter only");
	g.core_ok = true;
	ApplySettings();
	OrbisLog("[emu] mupen64plus-core ready (config %s, data %s)", config_dir.c_str(), data_dir.c_str());
	return true;
}

void DeinitCore()
{
	if (!g.core_ok)
		return;
	for (int i = 0; i < 4; i++)
		if (auto shutdown = reinterpret_cast<ptr_PluginShutdown>(m64ps5_getproc(kPlugins[i], "PluginShutdown")))
			shutdown();
	CoreShutdown();
	g.core_ok = false;
}

ExitReason RunGame(const std::string& path, std::string* error)
{
	std::string dummy;
	if (!error)
		error = &dummy;
	if (!g.core_ok)
	{
		*error = "The emulator is not running";
		return ExitReason::Error;
	}

	N64_STAGE(Emu, "load rom");
	OrbisLog("[emu] loading %s", path.c_str());
	std::vector<uint8_t> rom;
	const size_t dot = path.find_last_of('.');
	const std::string ext = dot == std::string::npos ? "" : Lower(path.substr(dot));
	if (!(ext == ".zip" ? ReadZippedRom(path, rom, error) : ReadWholeFile(path, rom, error)))
	{
		OrbisLog("[emu] %s", error->c_str());
		return ExitReason::Error;
	}
	m64p_error r = CoreDoCommand(M64CMD_ROM_OPEN, int(rom.size()), rom.data());
	rom.clear();
	rom.shrink_to_fit();
	if (r != M64ERR_SUCCESS)
	{
		*error = std::string("Not an N64 ROM (") + CoreErrorMessage(r) + ")";
		OrbisLog("[emu] M64CMD_ROM_OPEN -> %d", int(r));
		return ExitReason::Error;
	}

	m64p_rom_header header = {};
	m64p_rom_settings settings = {};
	CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(header), &header);
	CoreDoCommand(M64CMD_ROM_GET_SETTINGS, sizeof(settings), &settings);

	g.rom_path = path;
	const size_t slash = path.find_last_of('/');
	g.file_base = path.substr(slash == std::string::npos ? 0 : slash + 1);
	if (const size_t d = g.file_base.find_last_of('.'); d != std::string::npos && d > 0)
		g.file_base.resize(d);
	g.pal = IsPalCountry(header.Country_code);
	// the title: as on the shelf (No-Intro name), else the catalog's GoodName, else the file name
	uint32_t crc = 0;
	std::string name;
	if (fe::RomHeaderCrc(path, &crc))
		name = fe::gamedb::ByCrc(crc);
	g.title = !name.empty() ? fe::gamedb::Title(name)
			  : (settings.goodname[0] && !strstr(settings.goodname, "(unknown rom)")) ? CleanGoodName(settings.goodname)
																					   : g.file_base;
	OrbisLog("[emu] \"%s\": %s, country %02x (%s), save type %d, players %d, MD5 %s", g.title.c_str(), settings.goodname,
		header.Country_code, g.pal ? "PAL" : "NTSC", int(settings.savetype), int(settings.players), settings.MD5);

	// the renderer and the controllers are set up from the settings now
	const fe::Settings& cfg = fe::Config();
	// the CPU core: the dynarec when the setting asks for it and the console allows executable memory
	const bool use_dynarec = cfg.dynarec && n64ps5_jit_probe() != 0;
	m64p_handle core_cfg = nullptr;
	if (ConfigOpenSection("Core", &core_cfg) == M64ERR_SUCCESS && core_cfg)
		SetCoreInt(core_cfg, "R4300Emulator", use_dynarec ? 2 : 1);
	OrbisLog("[emu] CPU: %s", use_dynarec ? "dynamic recompiler" : "cached interpreter");
	static const char* const kPaks[] = {"none", "Controller Pak", "Rumble Pak"};
	OrbisLog("[emu] controller pak: %s", kPaks[cfg.pak < 0 || cfg.pak > 2 ? 1 : cfg.pak]);
	LogSaves("at start", settings.goodname, settings.MD5);
	const n64ps5_gfx_options gfx = {cfg.vi_filter ? 0 : 1, cfg.hide_overscan, cfg.render_threads, cfg.dp_compat,
		cfg.upscale, cfg.gpu_sync};
	n64ps5_gfx_set_options(&gfx);
	// the renderer: paraLLEl-RDP on the GPU when the setting asks for it and Vulkan works, else angrylion
	const m64ps5_library* gfx_lib = &m64ps5_gfx_lib;
#ifdef N64PS5_VULKAN
	if (cfg.gpu)
	{
		if (n64ps5_gpu_available())
		{
			n64ps5_gpu_set_options(&gfx);
			gfx_lib = &m64ps5_gfx_parallel_lib;
		}
		else
			emu::Osd("The GPU renderer is not available: using the CPU renderer");
	}
#endif
	OrbisLog("[emu] renderer: %s", gfx_lib == &m64ps5_gfx_lib ? "angrylion (CPU)" : "paraLLEl-RDP (GPU)");
	ApplySettings();

	static const char* const kNames[] = {"video", "audio", "input", "rsp"};
	for (int i = 0; i < 4; i++)
	{
		r = CoreAttachPlugin(kPluginTypes[i], M64PS5_HANDLE(i == 0 ? *gfx_lib : *kPlugins[i]));
		if (r != M64ERR_SUCCESS)
		{
			OrbisLog("[emu] CoreAttachPlugin(%s) -> %d (%s)", kNames[i], int(r), CoreErrorMessage(r));
			*error = std::string("The ") + kNames[i] + " plugin failed to start";
			for (int j = 0; j < i; j++)
				CoreDetachPlugin(kPluginTypes[j]);
			CoreDoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
			return ExitReason::Error;
		}
	}

	g.running = true;
	g.stopping = false;
	g.exit = ExitReason::BackToList;
	g.vi = 0;
	g.prev_buttons = ps5input::Pad(0).raw_buttons;
	g.block_until_release = true; // Cross from the shelf
	n64ps5_input_block(true);
	g.limiter_set = false;
	g.fast_forward.store(false);
	g.fps_t0 = 0;
	g.pending_save_slot = -1;
	g.fps_text[0] = 0;
	ps5video::FillRect(0, 0, ps5video::kWidth, ps5video::kHeight, ps5video::Rgb(0, 0, 0));
	ps5video::InvalidateN64();
	g_presenter.Forget();
	Osd("%s", g.title.c_str());

	g_presenter.Start();
	N64_STAGE(Emu, "execute");
	OrbisLog("[emu] running");
	r = CoreDoCommand(M64CMD_EXECUTE, 0, nullptr);
	OrbisLog("[emu] stopped (%d) after %u VIs", int(r), g.vi);
	LogSaves("at stop", settings.goodname, settings.MD5);
	g_presenter.Stop();

	for (int i = 3; i >= 0; i--)
		CoreDetachPlugin(kPluginTypes[i]);
	CoreDoCommand(M64CMD_ROM_CLOSE, 0, nullptr);
	for (int i = 0; i < ps5input::kMaxPads; i++)
		ps5input::SetRumble(i, 0, 0);
	n64ps5_input_block(false);
	g.running = false;
	N64_STAGE(Emu, "idle");
	if (r != M64ERR_SUCCESS && g.vi == 0)
	{
		*error = std::string("The game did not start (") + CoreErrorMessage(r) + ")";
		return ExitReason::Error;
	}
	return g.exit;
}

bool GameLoaded()
{
	return g.running;
}

std::string GameName()
{
	return g.title;
}

void ApplySettings()
{
	const fe::Settings& cfg = fe::Config();
	n64ps5_audio_set_mute(!cfg.audio);
	n64ps5_audio_set_fast_forward(g.fast_forward.load());
	const n64ps5_input_options in = {cfg.deadzone, 80, cfg.pak};
	n64ps5_input_set_options(&in);
	ps5video::InvalidateN64();
}

bool SaveState(int slot)
{
	if (!g.running)
		return false;
	const std::string path = StatePath(slot);
	const bool ok = CoreDoCommand(M64CMD_STATE_SAVE, 1, (void*)path.c_str()) == M64ERR_SUCCESS;
	if (ok)
		g.pending_save_slot = slot;
	OrbisLog("[emu] save state %d -> %s: %s", slot, path.c_str(), ok ? "queued" : "refused");
	return ok;
}

bool LoadState(int slot)
{
	if (!g.running || !StateExists(slot))
		return false;
	const std::string path = StatePath(slot);
	const bool ok = CoreDoCommand(M64CMD_STATE_LOAD, 0, (void*)path.c_str()) == M64ERR_SUCCESS;
	OrbisLog("[emu] load state %d <- %s: %s", slot, path.c_str(), ok ? "queued" : "refused");
	return ok;
}

bool StateExists(int slot)
{
	return g.running && (g.pending_save_slot == slot || OrbisIsFile(StatePath(slot)));
}

void Reset()
{
	if (g.running)
		CoreDoCommand(M64CMD_RESET, 0, nullptr); // soft reset, like the console's button
}

bool FastForward()
{
	return g.fast_forward.load();
}

void SetFastForward(bool on)
{
	g.fast_forward.store(on);
	n64ps5_audio_set_fast_forward(on);
	Osd(on ? "Fast forward" : "Normal speed");
}

void Osd(const char* fmt, ...)
{
	char text[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	{
		std::lock_guard<std::mutex> l(g.osd_lock);
		g.osd = text;
		g.osd_until = Now() + 3.0;
	}
	OrbisLog("[osd] %s", text);
}

void RedrawLastFrame()
{
	g_presenter.RedrawCurrent();
}
} // namespace emu
