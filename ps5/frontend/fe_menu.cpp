// Mupen64Plus PS5 frontend: the game list and the menus.
//
// Everything is drawn on the CPU into the 1920x1080 surface with the UI fonts (fe_text.h), and
// shown with ps5video::Present (which waits for vsync, so the menus run at 60 Hz).
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_emu.h"
#include "fe_settings.h"
#include "fe_text.h"

#include "OrbisPaths.h"
#include "ProsperoInput.h"
#include "ProsperoSce.h"
#include "ProsperoVideo.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#ifndef N64PS5_VERSION
#define N64PS5_VERSION "dev"
#endif

namespace fe
{
namespace
{
using ps5video::Rgb;
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

const uint32_t kBg = Rgb(14, 14, 28);
const uint32_t kPanel = Rgb(26, 24, 48);
const uint32_t kAccent = Rgb(150, 125, 255);
const uint32_t kSel = Rgb(78, 62, 170);
const uint32_t kText = Rgb(235, 235, 245);
const uint32_t kDim = Rgb(150, 150, 175);
const uint32_t kFolder = Rgb(255, 210, 110);

double Now()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---- input with key repeat ---------------------------------------------------------------------------
struct Nav
{
	bool up = false, down = false, left = false, right = false;
	bool pgup = false, pgdn = false;
	bool ok = false, back = false, options = false, triangle = false, menu = false;
};

class NavReader
{
public:
	NavReader()
	{
		ps5input::Poll();
		m_prev = ps5input::Pad(0).buttons; // buttons still held from before don't count
	}
	Nav Read()
	{
		ps5input::Poll();
		const uint32_t cur = ps5input::Pad(0).buttons;
		const uint32_t down = cur & ~m_prev;
		m_prev = cur;
		const double now = Now();
		Nav n;
		n.up = Repeat(cur, down, SCE_PAD_BUTTON_UP, now, 0);
		n.down = Repeat(cur, down, SCE_PAD_BUTTON_DOWN, now, 1);
		n.left = Repeat(cur, down, SCE_PAD_BUTTON_LEFT, now, 2);
		n.right = Repeat(cur, down, SCE_PAD_BUTTON_RIGHT, now, 3);
		n.pgup = Repeat(cur, down, SCE_PAD_BUTTON_L1, now, 4);
		n.pgdn = Repeat(cur, down, SCE_PAD_BUTTON_R1, now, 5);
		n.ok = down & SCE_PAD_BUTTON_CROSS;
		n.back = down & SCE_PAD_BUTTON_CIRCLE;
		n.options = down & SCE_PAD_BUTTON_OPTIONS;
		n.triangle = down & SCE_PAD_BUTTON_TRIANGLE;
		const uint32_t combo = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
		n.menu = (cur & combo) == combo && (down & combo);
		return n;
	}

private:
	bool Repeat(uint32_t cur, uint32_t down, uint32_t bit, double now, int i)
	{
		if (down & bit)
		{
			m_next[i] = now + 0.40;
			return true;
		}
		if ((cur & bit) && now >= m_next[i])
		{
			m_next[i] = now + 0.07;
			return true;
		}
		return false;
	}
	uint32_t m_prev = 0;
	double m_next[6] = {};
};

void Present()
{
	ps5video::Present(0, 0, 0, 0, true);
}

void Header(const char* subtitle)
{
	ps5video::FillRect(0, 0, W, H, kBg);
	ps5video::FillRect(0, 0, W, 150, kPanel);
	ps5video::FillRect(0, 150, W, 4, kAccent);
	DrawText(80, 34, "Mupen64Plus PS5", 6, kAccent);
	const char* ver = "mupen64plus-core 2.6.0 - PS5 " N64PS5_VERSION;
	DrawText(W - 80 - TextWidth(ver, 2), 40, ver, 2, kDim);
	if (subtitle && *subtitle)
		DrawText(84, 104, FitText(subtitle, 3, W - 168).c_str(), 3, kDim);
}

void Footer(const char* help)
{
	ps5video::FillRect(0, H - 80, W, 80, kPanel);
	DrawText(80, H - 60, help, 3, kDim);
}

bool HasRomExtension(const char* name)
{
	static const char* const exts[] = {".z64", ".n64", ".v64", ".rom", ".zip"};
	const size_t n = strlen(name);
	for (const char* e : exts)
	{
		const size_t m = strlen(e);
		if (n > m && strcasecmp(name + n - m, e) == 0)
			return true;
	}
	return false;
}

struct Entry
{
	std::string name; // shown
	std::string path; // full path
	bool dir = false;
};

std::vector<Entry> ListDir(const std::string& dir)
{
	std::vector<Entry> out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		Entry en;
		en.path = dir + "/" + e->d_name;
		en.dir = OrbisIsDir(en.path);
		if (!en.dir && !HasRomExtension(e->d_name))
			continue;
		en.name = e->d_name;
		if (!en.dir)
		{
			const size_t dot = en.name.find_last_of('.');
			if (dot != std::string::npos && dot > 0)
				en.name.resize(dot);
		}
		out.push_back(std::move(en));
	}
	closedir(d);
	std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
		if (a.dir != b.dir)
			return a.dir;
		return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
	});
	return out;
}

std::string RootLabel(const std::string& root)
{
	if (root == OrbisDir("roms"))
		return "Console  (" + root + ")";
	if (root.rfind("/mnt/usb", 0) == 0)
		return "USB " + root.substr(8, 1) + "  (" + root + ")";
	return root;
}

// A box of label/value rows (pause menu, settings).
struct Row
{
	std::string label;
	std::string value; // "" = an action
	bool enabled = true;
};

void DrawOptionBox(const char* title, const std::vector<Row>& rows, int sel, bool over_game)
{
	const int scale = 3;
	const int row_h = 44;
	const int bw = 1000;
	const int bh = 120 + int(rows.size()) * row_h + 30;
	const int bx = (W - bw) / 2;
	const int by = std::max(20, (H - bh) / 2);
	if (!over_game)
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
	ps5video::FillRect(bx, by, bw, bh, kPanel);
	ps5video::FillRect(bx, by + 86, bw, 3, kAccent);
	DrawText(bx + 40, by + 24, title, 5, kAccent);
	for (size_t i = 0; i < rows.size(); i++)
	{
		const int y = by + 110 + int(i) * row_h;
		if (int(i) == sel)
			ps5video::FillRect(bx + 16, y - 6, bw - 32, row_h, kSel);
		const uint32_t col = rows[i].enabled ? kText : kDim;
		DrawText(bx + 40, y, rows[i].label.c_str(), scale, col);
		if (!rows[i].value.empty())
		{
			const std::string v = (int(i) == sel ? "< " + rows[i].value + " >" : rows[i].value);
			DrawText(bx + bw - 40 - TextWidth(v.c_str(), scale), y, v.c_str(), scale, int(i) == sel ? kText : kAccent);
		}
	}
}

const char* YesNo(bool b)
{
	return b ? "On" : "Off";
}

// The settings shared by the game list (Triangle) and the pause menu. Returns true if 'idx' was a setting
// and 'dir' (-1/+1) changed it.
enum SettingRow
{
	S_ASPECT,
	S_SMOOTH,
	S_VI_FILTER,
	S_OVERSCAN,
	S_FPS,
	S_AUDIO,
#ifdef N64PS5_VULKAN
	S_RENDERER,
	S_UPSCALE,
	S_GPU_SYNC,
#endif
	S_CPU,
	S_THREADS,
	S_PAK,
	S_DEADZONE,
	S_COVERS,
	S_UPDATES,
	S_COUNT
};

// The settings marked * are read when a game starts (the renderer and the controllers are set up then).
std::string SettingLabel(int s)
{
	switch (s)
	{
		case S_ASPECT: return "Aspect ratio";
		case S_SMOOTH: return "Smooth scaling";
		case S_VI_FILTER: return "N64 video filter *";
		case S_OVERSCAN: return "Hide overscan *";
		case S_FPS: return "Show FPS";
		case S_AUDIO: return "Sound";
#ifdef N64PS5_VULKAN
		case S_RENDERER: return "Renderer *";
		case S_UPSCALE: return "Internal resolution *";
		case S_GPU_SYNC: return "GPU sync *";
#endif
		case S_CPU: return "CPU core *";
		case S_THREADS: return "Render threads *";
		case S_PAK: return "Controller pak *";
		case S_DEADZONE: return "Stick dead zone";
		case S_COVERS: return "Download covers";
		case S_UPDATES: return "Check for updates";
		default: return "";
	}
}

const char* PakName(int pak)
{
	switch (pak)
	{
		case 0: return "None";
		case 2: return "Rumble Pak";
		default: return "Controller Pak";
	}
}

std::string SettingValue(int s)
{
	const Settings& c = Config();
	char buf[32];
	switch (s)
	{
		case S_ASPECT: return ps5video::AspectName(ps5video::Aspect(c.aspect));
		case S_SMOOTH: return YesNo(c.smooth);
		case S_VI_FILTER: return YesNo(c.vi_filter);
		case S_OVERSCAN: return YesNo(c.hide_overscan);
		case S_FPS: return YesNo(c.show_fps);
		case S_AUDIO: return YesNo(c.audio);
#ifdef N64PS5_VULKAN
		case S_RENDERER: return c.gpu ? "GPU (paraLLEl-RDP)" : "CPU (angrylion)";
		case S_UPSCALE:
			if (!c.gpu)
				return "GPU only";
			snprintf(buf, sizeof(buf), c.upscale == 1 ? "1x (native)" : "%dx", c.upscale);
			return buf;
		case S_GPU_SYNC: return !c.gpu ? "GPU only" : c.gpu_sync ? "Accurate" : "Fast";
#endif
		case S_CPU: return c.dynarec ? "Dynarec" : "Interpreter";
		case S_THREADS: snprintf(buf, sizeof(buf), "%d", c.render_threads); return buf;
		case S_PAK: return PakName(c.pak);
		case S_DEADZONE: snprintf(buf, sizeof(buf), "%d%%", c.deadzone); return buf;
		case S_COVERS: return YesNo(c.covers_download);
		case S_UPDATES: return YesNo(c.updates);
		default: return "";
	}
}

int Step(int value, int dir, const int* steps, int count)
{
	int i = 0;
	while (i < count - 1 && steps[i] < value)
		i++;
	i = std::max(0, std::min(count - 1, i + dir));
	return steps[i];
}

void ChangeSetting(int s, int dir)
{
	Settings& c = Config();
	const int n = int(ps5video::Aspect::Count);
	switch (s)
	{
		case S_ASPECT: c.aspect = (c.aspect + dir + n) % n; break;
		case S_SMOOTH: c.smooth = !c.smooth; break;
		case S_VI_FILTER: c.vi_filter = !c.vi_filter; break;
		case S_OVERSCAN: c.hide_overscan = !c.hide_overscan; break;
		case S_FPS: c.show_fps = !c.show_fps; break;
		case S_AUDIO: c.audio = !c.audio; break;
#ifdef N64PS5_VULKAN
		case S_RENDERER: c.gpu = !c.gpu; break;
		case S_UPSCALE:
		{
			static const int steps[] = {1, 2, 4, 8};
			if (c.gpu)
				c.upscale = Step(c.upscale, dir, steps, 4);
			break;
		}
		case S_GPU_SYNC:
			if (c.gpu)
				c.gpu_sync = !c.gpu_sync;
			break;
#endif
		case S_CPU: c.dynarec = !c.dynarec; break;
		case S_THREADS:
		{
			static const int steps[] = {1, 2, 4, 6, 8, 10, 12};
			c.render_threads = Step(c.render_threads, dir, steps, 7);
			break;
		}
		case S_PAK: c.pak = (c.pak + dir + 3) % 3; break;
		case S_DEADZONE:
		{
			static const int steps[] = {0, 4, 8, 12, 16, 20, 25, 30};
			c.deadzone = Step(c.deadzone, dir, steps, 8);
			break;
		}
		case S_COVERS: c.covers_download = !c.covers_download; break;
		case S_UPDATES: c.updates = !c.updates; break;
	}
	emu::ApplySettings();
}

} // namespace

void SettingsMenu()
{
	NavReader nav;
	int sel = 0;
	for (;;)
	{
		std::vector<Row> rows;
		for (int s = 0; s < S_COUNT; s++)
			rows.push_back({SettingLabel(s), SettingValue(s)});
		rows.push_back({"Back", ""});
		Header("Settings");
		DrawOptionBox("Settings", rows, sel, false);
		Footer((std::string(icon::Cross) + " Toggle     " + icon::DpadLeftRight + " Adjust     " + icon::Circle + " Back      * when a game starts").c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(rows.size());
		if (n.up)
			sel = (sel + count - 1) % count;
		if (n.down)
			sel = (sel + 1) % count;
		if (sel < S_COUNT && (n.left || n.right || n.ok))
			ChangeSetting(sel, n.left ? -1 : 1);
		if (n.back || n.options || (n.ok && sel == S_COUNT))
		{
			Config().Save();
			return;
		}
	}
}

namespace
{
std::string DirTitle(const std::string& dir)
{
	return dir.empty() ? "Choose where your games are" : dir;
}
} // namespace

// =========================================================================================================
std::string RomBrowser()
{
	Settings& cfg = Config();
	NavReader nav;

	std::vector<std::string> roots = OrbisRomRoots();
	std::string dir;
	// Reopen the last folder if it is still there and under one of the roots.
	if (!cfg.last_dir.empty() && OrbisIsDir(cfg.last_dir))
	{
		for (const std::string& r : roots)
			if (cfg.last_dir.rfind(r, 0) == 0)
				dir = cfg.last_dir;
	}
	if (dir.empty() && roots.size() == 1)
		dir = roots[0];

	std::vector<Entry> list;
	int sel = 0, top = 0;
	auto reload = [&](const std::string& select_path) {
		roots = OrbisRomRoots();
		if (dir.empty())
		{
			list.clear();
			for (const std::string& r : roots)
				list.push_back({RootLabel(r), r, true});
		}
		else
			list = ListDir(dir);
		sel = 0;
		for (size_t i = 0; i < list.size(); i++)
			if (list[i].path == select_path)
				sel = int(i);
		top = 0;
	};
	reload(cfg.last_rom);

	const int scale = 3;
	const int row_h = 42;
	const int list_y = 190;
	const int rows_visible = (H - 100 - list_y) / row_h;
	double next_rescan = Now() + 3.0;

	for (;;)
	{
		// USB drives come and go: look again every few seconds while on the root list.
		if (dir.empty() && Now() >= next_rescan)
		{
			const std::string keep = list.empty() ? "" : list[sel].path;
			reload(keep);
			next_rescan = Now() + 3.0;
		}

		Header(DirTitle(dir).c_str());
		if (list.empty())
		{
			const char* lines[] = {
				"No games found here.",
				"",
				"Copy your N64 ROMs (.z64 .n64 .v64 .zip) to:",
				"    /data/mupen64plus/roms",
				"or, on a USB drive, to the folder  mupen64plus/roms",
			};
			int y = 330;
			for (const char* l : lines)
			{
				DrawText(160, y, l, 4, kText);
				y += 60;
			}
		}
		else
		{
			if (sel < top)
				top = sel;
			if (sel >= top + rows_visible)
				top = sel - rows_visible + 1;
			for (int i = 0; i < rows_visible && top + i < int(list.size()); i++)
			{
				const Entry& e = list[top + i];
				const int y = list_y + i * row_h;
				if (top + i == sel)
					ps5video::FillRect(60, y - 6, W - 140, row_h, kSel);
				const std::string label = (e.dir ? "> " : "  ") + e.name;
				DrawText(80, y, FitText(label, scale, W - 260).c_str(), scale, e.dir ? kFolder : kText);
			}
			// scrollbar
			if (int(list.size()) > rows_visible)
			{
				const int track = rows_visible * row_h;
				const int thumb = std::max(30, track * rows_visible / int(list.size()));
				const int ty = list_y + (track - thumb) * top / std::max(1, int(list.size()) - rows_visible);
				ps5video::FillRect(W - 60, list_y, 8, track, kPanel);
				ps5video::FillRect(W - 60, ty, 8, thumb, kAccent);
			}
			char count[64];
			snprintf(count, sizeof(count), "%d / %d", sel + 1, int(list.size()));
			DrawText(W - 80 - TextWidth(count, 3), 104, count, 3, kDim);
		}
		Footer((std::string(icon::Cross) + " Open     " + icon::Circle + " Back     " + icon::Triangle + " Settings     " + icon::Options + " Quit     " + icon::L1 + " " + icon::R1 + " Page").c_str());
		Present();

		const Nav n = nav.Read();
		const int count = int(list.size());
		if (count > 0)
		{
			if (n.up)
				sel = (sel + count - 1) % count;
			if (n.down)
				sel = (sel + 1) % count;
			if (n.pgup || n.left)
				sel = std::max(0, sel - rows_visible);
			if (n.pgdn || n.right)
				sel = std::min(count - 1, sel + rows_visible);
		}
		if (n.triangle)
		{
			SettingsMenu();
			nav = NavReader();
		}
		if (n.options)
			return "";
		if (n.back && !dir.empty())
		{
			// up one level, or back to the list of drives
			bool at_root = false;
			for (const std::string& r : roots)
				at_root |= (dir == r);
			const std::string from = dir;
			if (at_root)
				dir = roots.size() == 1 ? dir : "";
			else
				dir = dir.substr(0, dir.find_last_of('/'));
			if (dir != from)
				reload(from);
		}
		if (n.ok && count > 0)
		{
			const Entry e = list[sel];
			if (e.dir)
			{
				dir = e.path;
				reload("");
			}
			else
			{
				cfg.last_dir = dir;
				cfg.last_rom = e.path;
				cfg.Save();
				return e.path;
			}
		}
	}
}

PauseAction PauseMenu()
{
	Settings& cfg = Config();
	NavReader nav;
	int sel = 0;
	enum Item
	{
		I_RESUME,
		I_SAVE,
		I_LOAD,
		I_SLOT,
		I_SPEED,
		I_SET0, // the S_* settings follow
		I_RESET = I_SET0 + S_COUNT,
		I_LIST,
		I_QUIT,
		I_COUNT
	};
	std::string toast;
	double toast_until = 0;

	for (;;)
	{
		// the paused game, darkened, under the box
		ps5video::FillRect(0, 0, W, H, Rgb(0, 0, 0));
		emu::RedrawLastFrame();
		ps5video::DarkenRect(0, 0, W, H);
		ps5video::DarkenRect(0, 0, W, H);

		char slot[64];
		std::vector<Row> rows(I_COUNT);
		rows[I_RESUME] = {"Resume", ""};
		snprintf(slot, sizeof(slot), "Save state (slot %d)", cfg.state_slot);
		rows[I_SAVE] = {slot, ""};
		snprintf(slot, sizeof(slot), "Load state (slot %d)", cfg.state_slot);
		rows[I_LOAD] = {slot, "", emu::StateExists(cfg.state_slot)};
		snprintf(slot, sizeof(slot), "%d%s", cfg.state_slot, emu::StateExists(cfg.state_slot) ? " (used)" : " (empty)");
		rows[I_SLOT] = {"State slot", slot};
		rows[I_SPEED] = {"Speed", emu::FastForward() ? "Fast forward" : "Normal"};
		for (int s = 0; s < S_COUNT; s++)
			rows[I_SET0 + s] = {SettingLabel(s), SettingValue(s)};
		rows[I_RESET] = {"Reset game", ""};
		rows[I_LIST] = {"Back to the game list", ""};
		rows[I_QUIT] = {"Quit Mupen64Plus", ""};

		DrawOptionBox(FitText(emu::GameName(), 5, 900).c_str(), rows, sel, true);
		if (!toast.empty() && Now() < toast_until)
		{
			const int tw = TextWidth(toast.c_str(), 3);
			ps5video::FillRect((W - tw) / 2 - 30, H - 110, tw + 60, 70, kSel);
			DrawText((W - tw) / 2, H - 90, toast.c_str(), 3, kText);
		}
		Present();

		const Nav n = nav.Read();
		if (n.up)
			sel = (sel + I_COUNT - 1) % I_COUNT;
		if (n.down)
			sel = (sel + 1) % I_COUNT;
		if (n.back || n.options || n.menu)
			return PauseAction::Resume;

		if (sel == I_SLOT && (n.left || n.right || n.ok))
			cfg.state_slot = (cfg.state_slot + (n.left ? 9 : 1)) % 10;
		else if (sel == I_SPEED && (n.left || n.right || n.ok))
			emu::SetFastForward(!emu::FastForward());
		else if (sel >= I_SET0 && sel < I_SET0 + S_COUNT && (n.left || n.right || n.ok))
		{
			ChangeSetting(sel - I_SET0, n.left ? -1 : 1);
			cfg.Save();
		}
		else if (n.ok)
		{
			switch (sel)
			{
				case I_RESUME: return PauseAction::Resume;
				case I_SAVE:
					toast = emu::SaveState(cfg.state_slot) ? "State saved." : "Could not save the state.";
					toast_until = Now() + 2.0;
					cfg.Save();
					break;
				case I_LOAD:
					if (emu::LoadState(cfg.state_slot))
					{
						cfg.Save();
						emu::Osd("State loaded from slot %d", cfg.state_slot);
						return PauseAction::Resume;
					}
					toast = "This slot is empty.";
					toast_until = Now() + 2.0;
					break;
				case I_RESET:
					emu::Reset();
					return PauseAction::Resume;
				case I_LIST: return PauseAction::BackToList;
				case I_QUIT: return PauseAction::Quit;
				default: break;
			}
		}
	}
}

namespace
{
// Text broken into lines of at most max_px: '\n' starts a new line; Markdown list marks become bullets and
// heading marks go; blank lines are kept once.
std::vector<std::string> WrapText(const std::string& text, int scale, int max_px)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		start = end + 1;
		line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
		while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
			line.erase(0, 1);
		while (!line.empty() && line[0] == '#')
			line.erase(0, 1);
		if (line.size() >= 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ')
			line = "\xE2\x80\xA2 " + line.substr(2);
		while (!line.empty() && line[0] == ' ')
			line.erase(0, 1);
		// no Markdown emphasis, and no emoji (4-byte UTF-8: the fonts have none)
		std::string clean;
		for (size_t k = 0; k < line.size(); k++)
		{
			const unsigned char c = (unsigned char)line[k];
			if (c >= 0xF0)
			{
				k += 3;
				continue;
			}
			if (c != '*' && c != '`')
				clean += char(c);
		}
		while (!clean.empty() && clean.back() == ' ')
			clean.pop_back();
		if (clean.empty())
		{
			if (!out.empty() && !out.back().empty())
				out.push_back("");
			continue;
		}
		std::string cur;
		size_t i = 0;
		while (i < clean.size())
		{
			size_t sp = clean.find(' ', i);
			if (sp == std::string::npos)
				sp = clean.size();
			const std::string word = clean.substr(i, sp - i);
			const std::string next = cur.empty() ? word : cur + " " + word;
			if (!cur.empty() && TextWidth(next.c_str(), scale) > max_px)
			{
				out.push_back(cur);
				cur = word;
			}
			else
				cur = next;
			i = sp + 1;
		}
		out.push_back(FitText(cur, scale, max_px));
	}
	while (!out.empty() && out.back().empty())
		out.pop_back();
	return out;
}
} // namespace

bool Confirm(const std::string& title, const std::string& text, const char* yes, const char* no)
{
	const int bw = 1400, max_lines = 14, line_h = 38;
	std::vector<std::string> lines = WrapText(text, 3, bw - 80);
	if (int(lines.size()) > max_lines)
	{
		lines.resize(max_lines);
		lines.back() = "...";
	}
	const int bh = 130 + int(lines.size()) * line_h + 100;
	NavReader nav;
	for (;;)
	{
		Header("");
		const int bx = (W - bw) / 2, by = std::max(170, (H - bh) / 2);
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
		ps5video::FillRect(bx, by, bw, bh, kPanel);
		DrawText(bx + 40, by + 30, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
		for (size_t i = 0; i < lines.size(); i++)
			DrawText(bx + 40, by + 120 + int(i) * line_h, lines[i].c_str(), 3, kText);
		const std::string help = std::string(icon::Cross) + " " + yes + "      " + icon::Circle + " " + no;
		DrawText(bx + 40, by + bh - 70, help.c_str(), 3, kDim);
		Present();
		const Nav n = nav.Read();
		if (n.ok)
			return true;
		if (n.back || n.options)
			return false;
	}
}

void ShowBusy(const std::string& text)
{
	for (int i = 0; i < 2; i++) // both buffers
	{
		Header("");
		DrawText((W - TextWidth(text.c_str(), 5)) / 2, H / 2 - 25, text.c_str(), 5, kAccent);
		Present();
	}
}

void MessageBox(const std::string& title, const std::string& text)
{
	// laid out as Confirm: the text wrapped over as many lines as it needs
	const int bw = 1400, max_lines = 14, line_h = 38;
	std::vector<std::string> lines = WrapText(text, 3, bw - 80);
	if (int(lines.size()) > max_lines)
	{
		lines.resize(max_lines);
		lines.back() = "...";
	}
	const int bh = 130 + int(lines.size()) * line_h + 100;
	NavReader nav;
	for (;;)
	{
		Header("");
		const int bx = (W - bw) / 2, by = std::max(170, (H - bh) / 2);
		ps5video::FillRect(bx - 4, by - 4, bw + 8, bh + 8, kAccent);
		ps5video::FillRect(bx, by, bw, bh, kPanel);
		DrawText(bx + 40, by + 30, FitText(title, 5, bw - 80).c_str(), 5, kAccent);
		for (size_t i = 0; i < lines.size(); i++)
			DrawText(bx + 40, by + 120 + int(i) * line_h, lines[i].c_str(), 3, kText);
		DrawText(bx + 40, by + bh - 70, (std::string(icon::Cross) + " OK").c_str(), 3, kDim);
		Present();
		const Nav n = nav.Read();
		if (n.ok || n.back || n.options)
			return;
	}
}
} // namespace fe
