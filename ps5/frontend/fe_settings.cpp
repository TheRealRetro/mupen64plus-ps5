// Mupen64Plus PS5 frontend: settings file.
// SPDX-License-Identifier: MIT

#include "fe_settings.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace fe
{
namespace
{
std::string IniPath()
{
	return OrbisRoot() + "/mupen64plus-ps5.ini";
}

int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// the layout's names in the settings file and the per-game files
const char* const kPadKeys[N64PS5_PAD_COUNT] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
	"l3", "r3", "options", "up", "down", "left", "right", "rs_up", "rs_down", "rs_left", "rs_right"};
const char* const kN64Keys[N64PS5_N64_COUNT] = {"none", "a", "b", "z", "l", "r", "start", "c_up", "c_down",
	"c_left", "c_right", "d_up", "d_down", "d_left", "d_right"};

std::string GameLayoutPath(const std::string& file_base)
{
	return OrbisDir("layouts") + "/" + file_base + ".txt";
}
} // namespace

Layout::Layout()
{
	n64ps5_input_default_layout(to);
}

bool Layout::operator==(const Layout& o) const
{
	return memcmp(to, o.to, sizeof(to)) == 0;
}

std::string LayoutToText(const Layout& layout)
{
	std::string s;
	for (int i = 0; i < N64PS5_PAD_COUNT; i++)
	{
		if (i)
			s += ',';
		s += std::string(kPadKeys[i]) + '=' + kN64Keys[layout.to[i] < N64PS5_N64_COUNT ? layout.to[i] : 0];
	}
	return s;
}

Layout LayoutFromText(const std::string& text)
{
	Layout layout;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find(',', at);
		if (end == std::string::npos)
			end = text.size();
		const std::string item = text.substr(at, end - at);
		at = end + 1;
		const size_t eq = item.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string pad = item.substr(0, eq), n64 = item.substr(eq + 1);
		for (int i = 0; i < N64PS5_PAD_COUNT; i++)
			if (pad == kPadKeys[i])
				for (int t = 0; t < N64PS5_N64_COUNT; t++)
					if (n64 == kN64Keys[t])
						layout.to[i] = (unsigned char)t;
	}
	return layout;
}

bool LoadGameLayout(const std::string& file_base, Layout& out)
{
	FILE* f = fopen(GameLayoutPath(file_base).c_str(), "r");
	if (!f)
		return false;
	char line[1024] = "";
	const bool ok = fgets(line, sizeof(line), f) != nullptr;
	fclose(f);
	if (!ok)
		return false;
	line[strcspn(line, "\r\n")] = 0;
	out = LayoutFromText(line);
	return true;
}

void SaveGameLayout(const std::string& file_base, const Layout* layout)
{
	const std::string path = GameLayoutPath(file_base);
	if (!layout)
	{
		unlink(path.c_str());
		return;
	}
	OrbisMkdirs(OrbisDir("layouts"));
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fprintf(f, "%s\n", LayoutToText(*layout).c_str());
		fclose(f);
		OrbisLog("[settings] %s: own controller layout", path.c_str());
	}
}

Settings& Config()
{
	static Settings s;
	return s;
}

void Settings::Load()
{
	FILE* f = fopen(IniPath().c_str(), "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		const std::string key = line;
		const char* val = eq + 1;
		if (key == "aspect")
			aspect = Clamp(atoi(val), 0, 2);
		else if (key == "smooth")
			smooth = atoi(val) != 0;
		else if (key == "vi_filter")
			vi_filter = atoi(val) != 0;
		else if (key == "hide_overscan")
			hide_overscan = atoi(val) != 0;
		else if (key == "show_fps")
			show_fps = atoi(val) != 0;
		else if (key == "render_threads")
			render_threads = Clamp(atoi(val), 1, 12);
		else if (key == "dynarec")
			dynarec = atoi(val) != 0;
		else if (key == "hle_audio")
			hle_audio = atoi(val) != 0;
		else if (key == "overclock")
			overclock = Clamp(atoi(val), 0, 2);
		else if (key == "gpu")
			gpu = atoi(val) != 0;
		else if (key == "upscale")
		{
			const int u = atoi(val);
			upscale = u >= 8 ? 8 : u >= 4 ? 4 : u >= 2 ? 2 : 1;
		}
		else if (key == "gpu_sync")
			gpu_sync = atoi(val) != 0;
		else if (key == "dp_compat")
			dp_compat = Clamp(atoi(val), 0, 2);
		else if (key == "audio")
			audio = atoi(val) != 0;
		else if (key == "volume")
			volume = Clamp(atoi(val), 0, 100);
		else if (key == "pak")
			pak = Clamp(atoi(val), 0, 2);
		else if (key == "deadzone")
			deadzone = Clamp(atoi(val), 0, 30);
		else if (key == "invert_stick")
			invert_stick = Clamp(atoi(val), 0, 3);
		else if (key == "invert_c_stick")
			invert_c_stick = Clamp(atoi(val), 0, 3);
		else if (key == "layout")
			layout = LayoutFromText(val);
		else if (key == "game_view")
			game_view = Clamp(atoi(val), 0, 2);
		else if (key == "state_slot")
			state_slot = Clamp(atoi(val), 0, 9);
		else if (key == "resume")
			resume = atoi(val) != 0;
		else if (key == "covers_download")
			covers_download = atoi(val) != 0;
		else if (key == "updates")
			updates = atoi(val) != 0;
		else if (key == "last_dir")
			last_dir = val;
		else if (key == "last_rom")
			last_rom = val;
	}
	fclose(f);
	OrbisLog("[settings] loaded %s", IniPath().c_str());
}

void Settings::Save() const
{
	const std::string tmp = IniPath() + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		OrbisLog("[settings] can't write %s", tmp.c_str());
		return;
	}
	fprintf(f, "# Mupen64Plus PS5\n");
	fprintf(f, "aspect=%d\n", aspect);
	fprintf(f, "smooth=%d\n", smooth ? 1 : 0);
	fprintf(f, "vi_filter=%d\n", vi_filter ? 1 : 0);
	fprintf(f, "hide_overscan=%d\n", hide_overscan ? 1 : 0);
	fprintf(f, "show_fps=%d\n", show_fps ? 1 : 0);
	fprintf(f, "render_threads=%d\n", render_threads);
	fprintf(f, "dynarec=%d\n", dynarec ? 1 : 0);
	fprintf(f, "hle_audio=%d\n", hle_audio ? 1 : 0);
	fprintf(f, "overclock=%d\n", overclock);
	fprintf(f, "gpu=%d\n", gpu ? 1 : 0);
	fprintf(f, "upscale=%d\n", upscale);
	fprintf(f, "gpu_sync=%d\n", gpu_sync ? 1 : 0);
	fprintf(f, "dp_compat=%d\n", dp_compat);
	fprintf(f, "audio=%d\n", audio ? 1 : 0);
	fprintf(f, "volume=%d\n", volume);
	fprintf(f, "pak=%d\n", pak);
	fprintf(f, "deadzone=%d\n", deadzone);
	fprintf(f, "invert_stick=%d\n", invert_stick);
	fprintf(f, "invert_c_stick=%d\n", invert_c_stick);
	fprintf(f, "layout=%s\n", LayoutToText(layout).c_str());
	fprintf(f, "game_view=%d\n", game_view);
	fprintf(f, "state_slot=%d\n", state_slot);
	fprintf(f, "resume=%d\n", resume ? 1 : 0);
	fprintf(f, "covers_download=%d\n", covers_download ? 1 : 0);
	fprintf(f, "updates=%d\n", updates ? 1 : 0);
	fprintf(f, "last_dir=%s\n", last_dir.c_str());
	fprintf(f, "last_rom=%s\n", last_rom.c_str());
	fclose(f);
	rename(tmp.c_str(), IniPath().c_str());
}
} // namespace fe
