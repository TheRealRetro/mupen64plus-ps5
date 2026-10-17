// Mupen64Plus PS5 frontend: the game list and the menus.
//
// Everything is drawn on the CPU into the 1920x1080 surface with the UI fonts (fe_text.h), and
// shown with ps5video::Present (which waits for vsync, so the menus run at 60 Hz).
//
// SPDX-License-Identifier: MIT

#include "fe_menu.h"

#include "fe_draw.h"
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
	bool ok = false, back = false, options = false, triangle = false, square = false, touchpad = false, menu = false;
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
		n.square = down & SCE_PAD_BUTTON_SQUARE;
		n.touchpad = down & SCE_PAD_BUTTON_TOUCH_PAD;
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
	S_VOLUME,
#ifdef N64PS5_VULKAN
	S_RENDERER,
	S_UPSCALE,
	S_GPU_SYNC,
#endif
	S_CPU,
	S_OVERCLOCK,
	S_RSP_AUDIO,
	S_THREADS,
	S_PAK,
	S_DEADZONE,
	S_INVERT,
	S_INVERT_C,
	S_LAYOUT,
	S_RESUME,
	S_VIEW,
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
		case S_VOLUME: return "Volume";
#ifdef N64PS5_VULKAN
		case S_RENDERER: return "Renderer *";
		case S_UPSCALE: return "Internal resolution *";
		case S_GPU_SYNC: return "GPU sync *";
#endif
		case S_CPU: return "CPU core *";
		case S_OVERCLOCK: return "CPU overclock *";
		case S_RSP_AUDIO: return "Audio processing *";
		case S_THREADS: return "Render threads *";
		case S_PAK: return "Controller pak *";
		case S_DEADZONE: return "Stick dead zone";
		case S_INVERT: return "Invert left stick";
		case S_INVERT_C: return "Invert right stick";
		case S_LAYOUT: return "Controller layout";
		case S_RESUME: return "Continue where you left off";
		case S_VIEW: return "Game list view";
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
		case S_VOLUME: snprintf(buf, sizeof(buf), "%d%%", c.volume); return buf;
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
		case S_OVERCLOCK:
		{
			static const char* const kSpeeds[] = {"Off", "2x", "4x"};
			return kSpeeds[std::clamp(c.overclock, 0, 2)];
		}
		case S_RSP_AUDIO: return c.hle_audio ? "Fast (HLE)" : "Accurate (LLE)";
		case S_THREADS: snprintf(buf, sizeof(buf), "%d", c.render_threads); return buf;
		case S_PAK: return PakName(c.pak);
		case S_DEADZONE: snprintf(buf, sizeof(buf), "%d%%", c.deadzone); return buf;
		case S_INVERT:
		case S_INVERT_C:
		{
			static const char* const kInvert[] = {"Off", "X (left/right)", "Y (up/down)", "X and Y"};
			return kInvert[(s == S_INVERT ? c.invert_stick : c.invert_c_stick) & 3];
		}
		case S_LAYOUT:
			if (!emu::GameLoaded())
				return c.layout == Layout() ? "N64 pad (edit)" : "Your own (edit)";
			return emu::GameHasOwnLayout() ? "This game's own" : "Same as all games";
		case S_RESUME: return c.resume ? "Ask" : "Off";
		case S_VIEW:
		{
			static const char* const kViews[] = {"Shelf (3D)", "Grid", "List"};
			return kViews[std::clamp(c.game_view, 0, 2)];
		}
		case S_COVERS: return YesNo(c.covers_download);
		case S_UPDATES: return YesNo(c.updates);
		default: return "";
	}
}

// ---- the menus' look (fe_draw.h): a panel over the paused game or the game list, rows of settings
namespace pm
{
// layout, 1920 x 1080
constexpr int PX = 440, PY = 40, PW = 1040, PH = 1000; // the panel
constexpr int HB = PY + 112; // the header's bottom line
constexpr int FY = PY + PH - 92; // the footer's top line
constexpr int RX = PX + 44, RR = PX + PW - 54, RY0 = PY + 186, RH = 46; // the rows
constexpr int kVisible = (FY - 14 - RY0) / RH;

const uint32_t kPanelTop = Rgb(27, 22, 80), kPanelBottom = Rgb(15, 13, 46);
const uint32_t kBorder = Rgb(122, 98, 255), kGlow = Rgb(90, 63, 240);
const uint32_t kTitle = Rgb(217, 210, 255), kSub = Rgb(143, 128, 232), kLine = Rgb(75, 63, 181);
const uint32_t kButton = Rgb(27, 24, 72), kButtonEdge = Rgb(58, 52, 144);
const uint32_t kSelA = Rgb(79, 51, 230), kSelB = Rgb(124, 93, 255), kSelEdge = Rgb(180, 163, 255);
const uint32_t kLabel = Rgb(228, 223, 255), kIcon = Rgb(179, 166, 245), kDimmed = Rgb(112, 106, 153);
const uint32_t kBox = Rgb(26, 23, 69), kBoxEdge = Rgb(60, 53, 145), kValue = Rgb(236, 232, 255);
const uint32_t kOn = Rgb(110, 76, 255), kOff = Rgb(52, 50, 79), kKnobOff = Rgb(189, 188, 210);
const uint32_t kRowSel = Rgb(36, 31, 99), kRowEdge = Rgb(93, 72, 230), kHint = Rgb(196, 188, 245);

void SpacedText(int x, int y, const char* s, int px, int spacing, uint32_t color)
{
	for (const char* p = s; *p; p++)
	{
		const char c[2] = {*p, 0};
		DrawTextPx(x, y, c, px, color);
		x += TextWidthPx(c, px) + spacing;
	}
}

int SpacedWidth(const char* s, int px, int spacing)
{
	int w = 0;
	for (const char* p = s; *p; p++)
	{
		const char c[2] = {*p, 0};
		w += TextWidthPx(c, px) + spacing;
	}
	return w - spacing;
}

// an icon in a size x size box: a drawn one, or a PromptFont glyph
struct RowIcon
{
	bool drawn;
	gfx::Icon icon;
	const char* glyph;
};

void DrawRowIcon(const RowIcon& ic, int x, int y, int size, uint32_t color)
{
	if (ic.drawn)
		gfx::DrawIcon(ic.icon, x, y, size, color);
	else
	{
		const int px = size * 9 / 10;
		const int w = TextWidthPx(ic.glyph, px);
		DrawTextPx(x + (size - w) / 2, y - size / 10, ic.glyph, px, color);
	}
}

RowIcon Drawn(gfx::Icon i)
{
	return {true, i, nullptr};
}

RowIcon Glyph(const char* g)
{
	return {false, gfx::Icon::Play, g};
}

// PromptFont glyphs used as icons
constexpr const char* kFloppy = "\xF0\x9F\x92\xBE"; // U+1F4BE
constexpr const char* kTrayIn = "\xE2\x8F\xB4"; // U+23F4
constexpr const char* kHome = "\xE2\x8F\x8F"; // U+23CF
constexpr const char* kDoor = "\xF0\x9F\x9A\xAA"; // U+1F6AA
constexpr const char* kStar = "\xE2\x98\x85"; // U+2605
constexpr const char* kLaptop = "\xF0\x9F\x92\xBB"; // U+1F4BB
constexpr const char* kSpeaker = "\xF0\x9F\x94\x88"; // U+1F508
constexpr const char* kNotes = "\xF0\x9F\x8E\xB6"; // U+1F3B6
constexpr const char* kCube = "\xE2\xAC\x9B"; // U+2B1B
constexpr const char* kResize = "\xF0\x9F\x96\xB5"; // U+1F5B5
constexpr const char* kChip = "\xF0\x9F\x96\xA5"; // U+1F5A5
constexpr const char* kHeadphones = "\xF0\x9F\x8E\xA7"; // U+1F3A7
constexpr const char* kGears = "\xE2\x9A\x99"; // U+2699
constexpr const char* kRam = "\xF0\x9F\x96\xAA"; // U+1F5AA
constexpr const char* kJoystick = "\xF0\x9F\x95\xB9"; // U+1F579
constexpr const char* kSwap = "\xE2\x86\x94"; // U+2194
constexpr const char* kInfo = "\xF0\x9F\x9B\x88"; // U+1F6C8

RowIcon SettingIcon(int s)
{
	switch (s)
	{
		case S_ASPECT: return Drawn(gfx::Icon::Screen);
		case S_SMOOTH: return Drawn(gfx::Icon::Grid);
		case S_VI_FILTER: return Glyph(kLaptop);
		case S_OVERSCAN: return Drawn(gfx::Icon::Frame);
		case S_FPS: return Drawn(gfx::Icon::Bars);
		case S_AUDIO: return Glyph(kSpeaker);
		case S_VOLUME: return Glyph(kNotes);
#ifdef N64PS5_VULKAN
		case S_RENDERER: return Glyph(kCube);
		case S_UPSCALE: return Glyph(kResize);
		case S_GPU_SYNC: return Drawn(gfx::Icon::Restart);
#endif
		case S_CPU: return Glyph(kChip);
		case S_OVERCLOCK: return Drawn(gfx::Icon::Gauge);
		case S_RSP_AUDIO: return Glyph(kHeadphones);
		case S_THREADS: return Glyph(kGears);
		case S_PAK: return Glyph(kRam);
		case S_DEADZONE: return Glyph(kJoystick);
		case S_INVERT:
		case S_INVERT_C: return Glyph(kSwap);
		case S_LAYOUT: return Glyph(icon::Dpad);
		case S_RESUME: return Drawn(gfx::Icon::Play);
		case S_VIEW: return Glyph(kHome);
		case S_COVERS: return Drawn(gfx::Icon::Download);
		case S_UPDATES: return Glyph(kInfo);
		default: return Glyph(kGears);
	}
}

// "Aspect ratio" -> "Aspect Ratio"; short words stay small
std::string TitleCase(const std::string& s)
{
	static const char* const kSmall[] = {"a", "an", "and", "for", "of", "the", "to", "in"};
	std::string out;
	size_t at = 0;
	while (at < s.size())
	{
		size_t end = s.find(' ', at);
		if (end == std::string::npos)
			end = s.size();
		std::string word = s.substr(at, end - at);
		bool small = false;
		for (const char* k : kSmall)
			small |= (at > 0 && word == k);
		if (!small && !word.empty() && word[0] >= 'a' && word[0] <= 'z')
			word[0] = char(word[0] - 'a' + 'A');
		out += (out.empty() ? "" : " ") + word;
		at = end + 1;
	}
	return out;
}

enum class Kind
{
	Choice, // < value >
	Toggle, // On / Off and a switch
	Slider, // the volume
	Page, // opens a page of its own: value >
	Action, // happens on Cross: value, then the Cross symbol
};

Kind SettingKind(int s)
{
	switch (s)
	{
		case S_SMOOTH:
		case S_VI_FILTER:
		case S_OVERSCAN:
		case S_FPS:
		case S_AUDIO:
		case S_COVERS:
		case S_UPDATES: return Kind::Toggle;
		case S_VOLUME: return Kind::Slider;
		case S_LAYOUT: return Kind::Page;
		default: return Kind::Choice;
	}
}

struct SettingRow
{
	RowIcon icon;
	std::string label;
	bool star; // applies when the game starts again
	Kind kind;
	std::string value;
	bool on; // a toggle's state
	int percent; // a slider's
	bool enabled;
};

void DrawChevron(int x, int cy, bool right, uint32_t color)
{
	// a small filled arrowhead, 9 x 14
	if (right)
		gfx::FillTriangle(x, cy - 7.0f, x, cy + 7.0f, x + 9.0f, float(cy), color);
	else
		gfx::FillTriangle(x + 9.0f, cy - 7.0f, x + 9.0f, cy + 7.0f, float(x), float(cy), color);
}

void DrawSettingRow(const SettingRow& r, int y, bool selected)
{
	const int cy = y + RH / 2;
	if (selected)
	{
		gfx::GlowRoundRect(RX - 10, y + 1, RR - RX + 20, RH - 2, 12, 10, kGlow, 90);
		gfx::FillRoundRect(RX - 10, y + 1, RR - RX + 20, RH - 2, 12, kRowSel, 235);
		gfx::StrokeRoundRect(RX - 10, y + 1, RR - RX + 20, RH - 2, 12, 1.5f, kRowEdge);
	}
	const uint32_t label = r.enabled ? kLabel : kDimmed;
	DrawRowIcon(r.icon, RX + 4, cy - 14, 28, r.enabled ? kIcon : kDimmed);
	const std::string text = FitTextPx(r.label, 23, 330);
	DrawTextPx(RX + 50, cy - 13, text.c_str(), 23, label);
	if (r.star)
		DrawTextPx(RX + 50 + TextWidthPx(text.c_str(), 23) + 6, cy - 13, "*", 20, kSub);

	const uint32_t value = r.enabled ? kValue : kDimmed;
	switch (r.kind)
	{
		case Kind::Choice:
		case Kind::Page:
		{
			const int bw = 260, bh = 36, bx = RR - 10 - bw, by = cy - bh / 2;
			gfx::FillRoundRect(bx, by, bw, bh, 10, kBox);
			gfx::StrokeRoundRect(bx, by, bw, bh, 10, 1.5f, selected ? kRowEdge : kBoxEdge);
			if (r.kind == Kind::Choice)
				DrawChevron(bx + 14, cy, false, r.enabled ? kIcon : kDimmed);
			DrawChevron(bx + bw - 23, cy, true, r.enabled ? kIcon : kDimmed);
			const std::string v = FitTextPx(r.value, 21, bw - 70);
			DrawTextPx(bx + (bw - TextWidthPx(v.c_str(), 21)) / 2, cy - 12, v.c_str(), 21, value);
			break;
		}
		case Kind::Action:
		{
			const int bx = RR - 22 - TextWidthPx(icon::Cross, 30);
			DrawTextPx(bx, cy - 16, icon::Cross, 30, r.enabled ? kBorder : kDimmed);
			DrawTextPx(bx - 14 - TextWidthPx(r.value.c_str(), 21), cy - 12, r.value.c_str(), 21, r.enabled ? kHint : kDimmed);
			break;
		}
		case Kind::Toggle:
		{
			const int sw = 54, sh = 30, sx = RR - 10 - sw, sy = cy - sh / 2;
			if (r.on)
				gfx::GradientRoundRect(sx, sy, sw, sh, sh / 2, kOn, kSelB, true);
			else
				gfx::FillRoundRect(sx, sy, sw, sh, sh / 2, kOff);
			gfx::FillCircle(r.on ? sx + sw - sh / 2.0f : sx + sh / 2.0f, cy, sh / 2.0f - 4.0f, r.on ? Rgb(255, 255, 255) : kKnobOff);
			const char* t = r.on ? "On" : "Off";
			DrawTextPx(sx - 16 - TextWidthPx(t, 21), cy - 12, t, 21, kHint);
			break;
		}
		case Kind::Slider:
		{
			char pct[16];
			snprintf(pct, sizeof(pct), "%d%%", r.percent);
			DrawTextPx(RR - 10 - TextWidthPx(pct, 21), cy - 12, pct, 21, kHint);
			const int tx = RR - 10 - 76 - 170, tw = 170;
			gfx::FillRoundRect(tx, cy - 4, tw, 8, 4, Rgb(44, 40, 96));
			const int fw = tw * r.percent / 100;
			if (fw > 0)
				gfx::GradientRoundRect(tx, cy - 4, fw, 8, 4, kSelA, kSelB, true);
			gfx::FillCircle(float(tx + fw), float(cy), 12.0f, kSelEdge);
			gfx::FillCircle(float(tx + fw), float(cy), 9.0f, Rgb(255, 255, 255));
			break;
		}
	}
}

// the buttons a panel's footer lists
struct Hint
{
	const char* button;
	const char* label;
};

// what a panel shows besides its rows
struct Panel
{
	RowIcon icon; // in the header's box
	std::string title;
	std::string right; // spaced capitals at the header's right, "PAUSED"
	std::string section; // spaced capitals over the rows (may be empty)
	std::string section_right; // the same over the values
	std::vector<Hint> hints;
	const std::vector<uint32_t>* behind; // the picture under the panel; nullptr: the paused game
};

// Settings on the game list keeps the game list's picture here for the pages it opens (Controller Layout)
const std::vector<uint32_t>* g_behind = nullptr;

// everything that stays while a panel is open: the picture under it, the panel, its header and its footer
void DrawPanel(const Panel& p)
{
	if (p.behind && p.behind->size() == size_t(W) * H)
		memcpy(ps5video::Surface(), p.behind->data(), p.behind->size() * sizeof(uint32_t));
	else
	{
		ps5video::FillRect(0, 0, W, H, Rgb(0, 0, 0));
		emu::RedrawLastFrame();
	}
	ps5video::DarkenRect(0, 0, W, H);
	gfx::GlowRoundRect(PX, PY, PW, PH, 26, 26, kGlow, 150);
	gfx::GradientRoundRect(PX, PY, PW, PH, 26, kPanelTop, kPanelBottom, false, 246);
	gfx::StrokeRoundRect(PX, PY, PW, PH, 26, 2.5f, kBorder);

	// header: the sign, the title, the label at the right
	gfx::StrokeRoundRect(PX + 38, PY + 24, 66, 66, 14, 3.0f, kBorder);
	DrawRowIcon(p.icon, PX + 46, PY + 32, 50, Rgb(183, 168, 255));
	const int right_w = SpacedWidth(p.right.c_str(), 24, 7);
	const int right_x = PX + PW - 48 - right_w;
	const int title_x = PX + 130;
	const std::string title = FitTextPx(p.title, 54, right_x - 60 - title_x);
	DrawTextPx(title_x, PY + 26, title.c_str(), 54, kTitle, false, 2);
	const int line_x = title_x + TextWidthPx(title.c_str(), 54) + 30;
	if (right_x - 26 - line_x > 40)
		gfx::HLine(line_x, PY + 58, right_x - 26 - line_x, kLine);
	SpacedText(right_x, PY + 44, p.right.c_str(), 24, 7, kSub);
	gfx::GlowRoundRect(PX + 2, HB - 1, PW - 4, 2, 1, 10, kGlow, 80);
	gfx::HLine(PX + 2, HB - 1, PW - 4, Rgb(102, 80, 240));
	gfx::HLine(PX + 2, HB, PW - 4, Rgb(102, 80, 240));

	// the rows' headings
	if (!p.section.empty())
		SpacedText(RX, PY + 138, FitTextPx(p.section, 20, (RR - RX) * 2 / 3).c_str(), 20, 5, kSub);
	if (!p.section_right.empty())
		SpacedText(RR - 10 - SpacedWidth(p.section_right.c_str(), 20, 5), PY + 138, p.section_right.c_str(), 20, 5, kSub);

	// footer: the buttons
	gfx::HLine(PX + 2, FY, PW - 4, Rgb(59, 53, 144));
	const int fpx = 24, bpx = 34, gap = 10;
	int total = 0;
	for (const Hint& h : p.hints)
		total += TextWidthPx(h.button, bpx) + gap + TextWidthPx(h.label, fpx);
	const int n = int(p.hints.size());
	const int sep = n > 1 ? std::min(56, (PW - 80 - total) / (n - 1)) : 0;
	int x = PX + (PW - total - sep * (n - 1)) / 2;
	const int ty = FY + 46 - 14;
	for (int i = 0; i < n; i++)
	{
		if (i)
			gfx::VLine(x - sep / 2, FY + 28, 36, Rgb(59, 53, 144));
		DrawTextPx(x, ty - 6, p.hints[size_t(i)].button, bpx, kBorder);
		x += TextWidthPx(p.hints[size_t(i)].button, bpx) + gap;
		DrawTextPx(x, ty, p.hints[size_t(i)].label, fpx, kHint);
		x += TextWidthPx(p.hints[size_t(i)].label, fpx) + sep;
	}
}

// the rows from `top` (kept so `sel` shows), a scrollbar when they don't all fit, and the note for a "*" row
void DrawRows(const std::vector<SettingRow>& rows, int sel, int& top, int visible)
{
	const int n = int(rows.size());
	if (sel < top)
		top = sel;
	if (sel >= top + visible)
		top = sel - visible + 1;
	for (int i = top; i < std::min(n, top + visible); i++)
		DrawSettingRow(rows[size_t(i)], RY0 + (i - top) * RH, i == sel);
	if (n > visible)
	{
		const int track = visible * RH, thumb = std::max(40, track * visible / n);
		const int ty = RY0 + (track - thumb) * top / (n - visible);
		gfx::FillRoundRect(RR + 20, RY0, 5, track, 2, Rgb(42, 38, 104));
		gfx::FillRoundRect(RR + 20, ty, 5, thumb, 2, kBorder);
	}
	if (sel >= 0 && sel < n && rows[size_t(sel)].star)
	{
		const char* note = "* applies when the game starts again";
		DrawTextPx(RR - TextWidthPx(note, 19), PY + 140, note, 19, kSub);
	}
}

void DrawToast(const std::string& text)
{
	const int tw = TextWidthPx(text.c_str(), 24);
	const int bx = PX + (PW - tw - 60) / 2, by = FY - 74;
	gfx::FillRoundRect(bx, by, tw + 60, 52, 26, Rgb(58, 47, 176), 240);
	gfx::StrokeRoundRect(bx, by, tw + 60, 52, 26, 1.5f, kBorder);
	DrawTextPx(bx + 30, by + 13, text.c_str(), 24, Rgb(255, 255, 255));
}

// Up / Down (wrapping), L1 / R1 five rows
void MoveSelection(const Nav& n, int& sel, int count)
{
	if (count <= 0)
		return;
	if (n.up)
		sel = (sel + count - 1) % count;
	if (n.down)
		sel = (sel + 1) % count;
	if (n.pgup)
		sel = std::max(0, sel - 5);
	if (n.pgdn)
		sel = std::min(count - 1, sel + 5);
}

// a setting (S_*) as a row
SettingRow SettingRowFor(int s)
{
	SettingRow row{SettingIcon(s), "", false, SettingKind(s), SettingValue(s), false, Config().volume, true};
	std::string label = SettingLabel(s);
	if (label.size() > 2 && label.compare(label.size() - 2, 2, " *") == 0)
	{
		label.resize(label.size() - 2);
		row.star = true;
	}
	row.label = TitleCase(label);
	row.on = row.value == "On";
	row.enabled = row.value != "GPU only";
	return row;
}

// words into lines of at most max_px
std::vector<std::string> WrapPx(const std::string& text, int px, int max_px)
{
	std::vector<std::string> lines;
	std::string line;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find(' ', at);
		if (end == std::string::npos)
			end = text.size();
		const std::string word = text.substr(at, end - at);
		at = end + 1;
		const std::string next = line.empty() ? word : line + " " + word;
		if (!line.empty() && TextWidthPx(next.c_str(), px) > max_px)
		{
			lines.push_back(line);
			line = word;
		}
		else
			line = next;
	}
	if (!line.empty())
		lines.push_back(line);
	return lines;
}

// a box under the rows, from y to the footer, with a heading and wrapped text
void DrawNote(int y, const char* heading, const std::string& text)
{
	const int h = FY - 14 - y;
	gfx::FillRoundRect(RX - 10, y, RR - RX + 20, h, 14, kBox, 230);
	gfx::StrokeRoundRect(RX - 10, y, RR - RX + 20, h, 14, 1.5f, kBoxEdge);
	SpacedText(RX + 10, y + 12, heading, 18, 4, kSub);
	const std::vector<std::string> lines = WrapPx(text, 21, RR - RX - 30);
	for (size_t i = 0; i < lines.size() && int(i) * 28 + 70 < h; i++)
		DrawTextPx(RX + 10, y + 42 + int(i) * 28, lines[i].c_str(), 21, kLabel);
}

// the DualSense controls in the Controller Layout: their symbol and name
const char* PadGlyph(int pad)
{
	switch (pad)
	{
		case N64PS5_PAD_CROSS: return icon::Cross;
		case N64PS5_PAD_CIRCLE: return icon::Circle;
		case N64PS5_PAD_SQUARE: return icon::Square;
		case N64PS5_PAD_TRIANGLE: return icon::Triangle;
		case N64PS5_PAD_L1: return icon::L1;
		case N64PS5_PAD_R1: return icon::R1;
		case N64PS5_PAD_L2: return icon::L2;
		case N64PS5_PAD_R2: return icon::R2;
		case N64PS5_PAD_L3: return icon::L3;
		case N64PS5_PAD_R3: return icon::R3;
		case N64PS5_PAD_OPTIONS: return icon::Options;
		case N64PS5_PAD_UP:
		case N64PS5_PAD_DOWN: return icon::DpadUpDown;
		case N64PS5_PAD_LEFT:
		case N64PS5_PAD_RIGHT: return icon::DpadLeftRight;
		default: return icon::R3; // the right stick
	}
}

const char* PadName(int pad)
{
	static const char* const kNames[N64PS5_PAD_COUNT] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2",
		"L3", "R3", "OPTIONS", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Right Stick Up",
		"Right Stick Down", "Right Stick Left", "Right Stick Right"};
	return pad >= 0 && pad < N64PS5_PAD_COUNT ? kNames[pad] : "";
}

// "Reset the game?" over the menu: Square asked for it
void DrawConfirmReset()
{
	gfx::FillRoundRect(PX + 3, PY + 3, PW - 6, PH - 6, 24, Rgb(5, 4, 18), 150);
	const int bw = 620, bh = 250, bx = PX + (PW - bw) / 2, by = PY + (PH - bh) / 2;
	gfx::GlowRoundRect(bx, by, bw, bh, 20, 22, kGlow, 150);
	gfx::GradientRoundRect(bx, by, bw, bh, 20, kPanelTop, kPanelBottom, false);
	gfx::StrokeRoundRect(bx, by, bw, bh, 20, 2.5f, kBorder);
	gfx::DrawIcon(gfx::Icon::Restart, bx + 40, by + 38, 50, kIcon);
	DrawTextPx(bx + 112, by + 40, "Reset the game?", 40, kTitle, false, 1);
	DrawTextPx(bx + 112, by + 98, "Progress you haven't saved is lost.", 23, kHint);
	const std::string yes = std::string(icon::Cross), no = std::string(icon::Circle);
	int x = bx + 112;
	const int y = by + 172;
	DrawTextPx(x, y - 6, yes.c_str(), 34, kBorder);
	x += TextWidthPx(yes.c_str(), 34) + 10;
	DrawTextPx(x, y, "Reset", 24, kLabel);
	x += TextWidthPx("Reset", 24) + 48;
	DrawTextPx(x, y - 6, no.c_str(), 34, kBorder);
	x += TextWidthPx(no.c_str(), 34) + 10;
	DrawTextPx(x, y, "Cancel", 24, kLabel);
}
} // namespace pm

// ---- the controller layout ------------------------------------------------------------------------------
const char* N64Label(int n64)
{
	static const char* const kNames[N64PS5_N64_COUNT] = {"Nothing", "A", "B", "Z", "L", "R", "Start", "C-up", "C-down",
		"C-left", "C-right", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
	return n64 >= 0 && n64 < N64PS5_N64_COUNT ? kNames[n64] : "";
}

// Until Cross, Circle and OPTIONS are up: the menu this one returns to must not take them as its own.
void WaitForRelease()
{
	const uint32_t keys = SCE_PAD_BUTTON_CROSS | SCE_PAD_BUTTON_CIRCLE | SCE_PAD_BUTTON_OPTIONS;
	for (int i = 0; i < 120; i++)
	{
		ps5input::Poll();
		if (!(ps5input::Pad(0).buttons & keys))
			return;
		Present();
	}
}

// What each DualSense control presses on the N64 pad. From Settings (the game list) it is every game's layout;
// from the pause menu, the running game's own (kept with the game; the same as every game's until changed).
// L3 and R3 aren't in it: in a game they load and save states.
void LayoutMenu()
{
	Settings& cfg = Config();
	const bool in_game = emu::GameLoaded();
	Layout layout = in_game ? emu::GameLayout() : cfg.layout;
	std::vector<int> pads;
	for (int i = 0; i < N64PS5_PAD_COUNT; i++)
		if (i != N64PS5_PAD_L3 && i != N64PS5_PAD_R3)
			pads.push_back(i);
	const int kPads = int(pads.size());
	std::vector<uint32_t> screen;
	if (!in_game && !pm::g_behind)
		screen.assign(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
	const pm::Panel panel{pm::Glyph(icon::Dpad), "Controller Layout", in_game ? "THIS GAME" : "ALL GAMES", "DUALSENSE",
		"N64", {{icon::Cross, "Change"}, {icon::Circle, "Back"}},
		in_game ? nullptr : pm::g_behind ? pm::g_behind : &screen};
	NavReader nav;
	int sel = 0, top = 0;
	std::vector<uint32_t> base;
	bool dirty = true;
	for (;;)
	{
		if (dirty)
		{
			pm::DrawPanel(panel);
			base.assign(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
			dirty = false;
		}
		else
			memcpy(ps5video::Surface(), base.data(), base.size() * sizeof(uint32_t));
		std::vector<pm::SettingRow> rows;
		for (int pad : pads)
			rows.push_back({pm::Glyph(pm::PadGlyph(pad)), pm::PadName(pad), false, pm::Kind::Choice,
				N64Label(layout.to[pad]), false, 0, true});
		rows.push_back({pm::Drawn(gfx::Icon::Restart),
			in_game ? "Use the Layout of All Games" : "Back to the N64 Pad's Layout", false, pm::Kind::Action, "", false,
			0, true});
		pm::DrawRows(rows, sel, top, pm::kVisible);
		Present();

		const Nav n = nav.Read();
		pm::MoveSelection(n, sel, int(rows.size()));
		if (sel < kPads && (n.left || n.right || n.ok))
		{
			unsigned char& to = layout.to[pads[size_t(sel)]];
			to = (unsigned char)((to + (n.left ? N64PS5_N64_COUNT - 1 : 1)) % N64PS5_N64_COUNT);
		}
		else if (sel == kPads && n.ok)
			layout = in_game ? cfg.layout : Layout();
		if (n.back || n.options)
		{
			if (in_game)
				emu::SetGameLayout(layout);
			else
			{
				cfg.layout = layout;
				cfg.Save();
			}
			WaitForRelease();
			return;
		}
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
		case S_VOLUME:
		{
			static const int steps[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
			c.volume = Step(c.volume, dir, steps, 11);
			break;
		}
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
		case S_OVERCLOCK: c.overclock = (std::clamp(c.overclock, 0, 2) + dir + 3) % 3; break;
		case S_RSP_AUDIO: c.hle_audio = !c.hle_audio; break;
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
		case S_INVERT: c.invert_stick = (c.invert_stick + dir + 4) & 3; break;
		case S_INVERT_C: c.invert_c_stick = (c.invert_c_stick + dir + 4) & 3; break;
		case S_LAYOUT:
			if (dir > 0)
				LayoutMenu();
			break;
		case S_RESUME: c.resume = !c.resume; break;
		case S_VIEW: c.game_view = (std::clamp(c.game_view, 0, 2) + dir + 3) % 3; break;
		case S_COVERS: c.covers_download = !c.covers_download; break;
		case S_UPDATES: c.updates = !c.updates; break;
	}
	emu::ApplySettings();
}

} // namespace

void SettingsMenu()
{
	// the game list stays under the panel (and under the pages it opens)
	const std::vector<uint32_t> behind(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
	pm::g_behind = &behind;
	const pm::Panel panel{pm::Glyph(pm::kGears), "Settings", "MUPEN64PLUS PS5", "", "",
		{{icon::Cross, "Confirm"}, {icon::Circle, "Back"}}, &behind};
	NavReader nav;
	int sel = 0, top = 0;
	std::vector<uint32_t> base;
	bool dirty = true;
	for (;;)
	{
		if (dirty)
		{
			pm::DrawPanel(panel);
			base.assign(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
			dirty = false;
		}
		else
			memcpy(ps5video::Surface(), base.data(), base.size() * sizeof(uint32_t));
		std::vector<pm::SettingRow> rows;
		for (int s = 0; s < S_COUNT; s++)
			rows.push_back(pm::SettingRowFor(s));
		pm::DrawRows(rows, sel, top, pm::kVisible);
		Present();

		const Nav n = nav.Read();
		pm::MoveSelection(n, sel, S_COUNT);
		if (n.back || n.options)
		{
			Config().Save();
			pm::g_behind = nullptr;
			return;
		}
		if (n.left || n.right || n.ok)
		{
			ChangeSetting(sel, n.left ? -1 : 1);
			if (sel == S_LAYOUT)
			{
				nav = NavReader();
				dirty = true;
			}
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

namespace
{
std::vector<std::string> WrapText(const std::string& text, int scale, int max_px); // below

std::string CheatState(const cheats::Cheat& c)
{
	if (c.state == 0)
		return "Off";
	return c.options.empty() ? "On" : c.options[size_t(c.state - 1)].first;
}

// The running game's cheats: each one off / on, or off / one of its choices, with the database's note under the
// list.
void CheatsMenu()
{
	std::vector<cheats::Cheat>& list = emu::Cheats();
	const pm::Panel panel{pm::Glyph(pm::kStar), "Cheats", "PAUSED", emu::CheatsGameName(), "",
		{{icon::Cross, "Change"}, {icon::Circle, "Back"}}, nullptr};
	const int visible = pm::kVisible - 3; // room for the note
	NavReader nav;
	int sel = 0, top = 0;
	std::vector<uint32_t> base;
	bool dirty = true;
	for (;;)
	{
		if (dirty)
		{
			pm::DrawPanel(panel);
			base.assign(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
			dirty = false;
		}
		else
			memcpy(ps5video::Surface(), base.data(), base.size() * sizeof(uint32_t));
		std::vector<pm::SettingRow> rows;
		for (const cheats::Cheat& c : list)
			rows.push_back({pm::Glyph(pm::kStar), cheats::DisplayName(c.name), false, pm::Kind::Choice, CheatState(c),
				false, 0, true});
		pm::DrawRows(rows, sel, top, visible);
		const bool has_note = sel < int(list.size()) && !list[size_t(sel)].note.empty();
		pm::DrawNote(pm::RY0 + visible * pm::RH + 10, has_note ? "ABOUT THIS CHEAT" : "NOTE",
			has_note ? list[size_t(sel)].note
					 : "Cheats can make a game misbehave or crash. If one does, turn it off (or reset the game).");
		Present();

		const Nav n = nav.Read();
		pm::MoveSelection(n, sel, int(list.size()));
		if (sel < int(list.size()) && (n.left || n.right || n.ok))
		{
			const cheats::Cheat& c = list[size_t(sel)];
			emu::SetCheat(sel, (c.state + (n.left ? c.States() - 1 : 1)) % c.States());
		}
		if (n.back || n.options)
		{
			WaitForRelease();
			return;
		}
	}
}


} // namespace

PauseAction PauseMenu()
{
	Settings& cfg = Config();
	NavReader nav;
	// the rows: an S_* setting by its number, or one of these
	enum
	{
		R_SAVE = 1000,
		R_LOAD,
		R_SLOT,
		R_SPEED,
		R_CHEATS,
		R_QUIT,
	};
	// the settings of the game list and the app's start stay in Settings there
	std::vector<int> ids = {R_SAVE, R_LOAD, R_SLOT, R_SPEED, R_CHEATS};
	for (int s = 0; s < S_COUNT; s++)
		if (s != S_RESUME && s != S_VIEW && s != S_COVERS && s != S_UPDATES)
			ids.push_back(s);
	ids.push_back(R_QUIT);
	const int count = int(ids.size());
	const pm::Panel panel{pm::Drawn(gfx::Icon::Pause), emu::GameName(), "PAUSED", "SETTINGS", "",
		{{icon::Touchpad, "Resume"}, {icon::Square, "Reset Game"}, {icon::Triangle, "Exit Game"},
			{icon::Cross, "Confirm"}, {icon::Circle, "Back"}},
		nullptr};
	int sel = 0, top = 0;
	bool confirm_reset = false;
	std::string toast;
	double toast_until = 0;
	std::vector<uint32_t> base; // the paused game, the panel, its header and footer
	bool dirty = true;

	auto row_for = [&](int id) -> pm::SettingRow {
		const bool used = emu::StateExists(cfg.state_slot);
		char text[48];
		switch (id)
		{
			case R_SAVE:
				snprintf(text, sizeof(text), "Slot %d", cfg.state_slot);
				return {pm::Glyph(pm::kFloppy), "Save State", false, pm::Kind::Action, text, false, 0, true};
			case R_LOAD:
				snprintf(text, sizeof(text), used ? "Slot %d" : "Slot %d is empty", cfg.state_slot);
				return {pm::Glyph(pm::kTrayIn), "Load State", false, pm::Kind::Action, text, false, 0, used};
			case R_SLOT:
				snprintf(text, sizeof(text), "%d (%s)", cfg.state_slot, used ? "used" : "empty");
				return {pm::Drawn(gfx::Icon::Tiles), "State Slot", false, pm::Kind::Choice, text, false, 0, true};
			case R_SPEED:
				return {pm::Drawn(gfx::Icon::FastForward), "Speed", false, pm::Kind::Choice,
					emu::FastForward() ? "Fast forward" : "Normal", false, 0, true};
			case R_CHEATS:
			{
				int on = 0;
				for (const cheats::Cheat& c : emu::Cheats())
					on += c.state ? 1 : 0;
				snprintf(text, sizeof(text), "%d on", on);
				return {pm::Glyph(pm::kStar), "Cheats", false, pm::Kind::Page,
					emu::Cheats().empty() ? "None for this game" : on ? text : "Off", false, 0, !emu::Cheats().empty()};
			}
			case R_QUIT:
				return {pm::Glyph(pm::kDoor), "Quit Mupen64Plus", false, pm::Kind::Action, "Close the app", false, 0, true};
			default: return pm::SettingRowFor(id);
		}
	};

	for (;;)
	{
		if (dirty)
		{
			pm::DrawPanel(panel);
			base.assign(ps5video::Surface(), ps5video::Surface() + size_t(W) * H);
			dirty = false;
		}
		else
			memcpy(ps5video::Surface(), base.data(), base.size() * sizeof(uint32_t));
		std::vector<pm::SettingRow> rows;
		for (int id : ids)
			rows.push_back(row_for(id));
		pm::DrawRows(rows, sel, top, pm::kVisible);
		if (!toast.empty() && Now() < toast_until)
			pm::DrawToast(toast);
		if (confirm_reset)
			pm::DrawConfirmReset();
		Present();

		const Nav n = nav.Read();
		if (confirm_reset)
		{
			if (n.ok)
			{
				emu::Reset();
				return PauseAction::Resume;
			}
			if (n.back || n.square || n.touchpad || n.options)
				confirm_reset = false;
			continue;
		}
		// the footer's buttons
		if (n.touchpad || n.back || n.options)
			return PauseAction::Resume;
		if (n.square)
		{
			confirm_reset = true;
			continue;
		}
		if (n.triangle)
			return PauseAction::BackToList;

		// the list
		pm::MoveSelection(n, sel, count);
		if (!(n.left || n.right || n.ok))
			continue;
		const int dir = n.left ? -1 : 1;
		switch (ids[size_t(sel)])
		{
			case R_SAVE:
				if (n.ok)
				{
					toast = emu::SaveState(cfg.state_slot) ? "Saved in slot " + std::to_string(cfg.state_slot)
														   : "Could not save the state";
					toast_until = Now() + 2.0;
					cfg.Save();
				}
				break;
			case R_LOAD:
				if (n.ok)
				{
					if (emu::LoadState(cfg.state_slot))
					{
						cfg.Save();
						emu::Osd("State loaded from slot %d", cfg.state_slot);
						return PauseAction::Resume;
					}
					toast = "Slot " + std::to_string(cfg.state_slot) + " is empty";
					toast_until = Now() + 2.0;
				}
				break;
			case R_SLOT: cfg.state_slot = (cfg.state_slot + (dir < 0 ? 9 : 1)) % 10; break;
			case R_SPEED: emu::SetFastForward(!emu::FastForward()); break;
			case R_CHEATS:
				if (dir > 0 && !emu::Cheats().empty())
				{
					CheatsMenu();
					nav = NavReader();
					dirty = true;
				}
				break;
			case R_QUIT:
				if (n.ok)
					return PauseAction::Quit;
				break;
			default:
			{
				const int s = ids[size_t(sel)];
				ChangeSetting(s, dir);
				cfg.Save();
				if (s == S_LAYOUT)
					nav = NavReader();
				dirty = true; // the picture (aspect, filters) or a page drawn over it
				break;
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
