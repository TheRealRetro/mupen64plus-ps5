// Mupen64Plus PS5 frontend: settings file.
// SPDX-License-Identifier: MIT

#include "fe_settings.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
} // namespace

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
		else if (key == "pak")
			pak = Clamp(atoi(val), 0, 2);
		else if (key == "deadzone")
			deadzone = Clamp(atoi(val), 0, 30);
		else if (key == "state_slot")
			state_slot = Clamp(atoi(val), 0, 9);
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
	fprintf(f, "gpu=%d\n", gpu ? 1 : 0);
	fprintf(f, "upscale=%d\n", upscale);
	fprintf(f, "gpu_sync=%d\n", gpu_sync ? 1 : 0);
	fprintf(f, "dp_compat=%d\n", dp_compat);
	fprintf(f, "audio=%d\n", audio ? 1 : 0);
	fprintf(f, "pak=%d\n", pak);
	fprintf(f, "deadzone=%d\n", deadzone);
	fprintf(f, "state_slot=%d\n", state_slot);
	fprintf(f, "covers_download=%d\n", covers_download ? 1 : 0);
	fprintf(f, "updates=%d\n", updates ? 1 : 0);
	fprintf(f, "last_dir=%s\n", last_dir.c_str());
	fprintf(f, "last_rom=%s\n", last_rom.c_str());
	fclose(f);
	rename(tmp.c_str(), IniPath().c_str());
}
} // namespace fe
