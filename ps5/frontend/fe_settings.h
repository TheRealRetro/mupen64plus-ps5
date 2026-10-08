// Mupen64Plus PS5 frontend: settings kept in /data/mupen64plus/mupen64plus-ps5.ini (key=value lines).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace fe
{
struct Settings
{
	// picture
	int aspect = 0; // ps5video::Aspect
	bool smooth = true; // bilinear scaling (else nearest neighbour)
	bool vi_filter = true; // the N64's own VI filter (anti-aliasing, divot, dither): angrylion "filtered"
	bool hide_overscan = false; // crop the black borders the N64 leaves around the picture
	bool show_fps = false;
	// emulation
	bool dynarec = true; // the x86-64 dynarec when the console gives us executable memory, else the interpreter
	bool gpu = true; // the GPU renderer, paraLLEl-RDP (builds with Vulkan only), else angrylion on the CPU
	int upscale = 4; // the GPU renderer's internal resolution: 1, 2, 4 or 8 times the N64's
	bool gpu_sync = true; // wait for the GPU wherever the game waits for the RDP (else faster, may glitch)
	int render_threads = 6; // angrylion workers (1 = single-threaded)
	int dp_compat = 1; // angrylion sync points between its workers: 0 fewest (fastest), 1, 2 most (safest)
	bool audio = true;
	// controller
	int pak = 1; // 0 none, 1 Controller Pak, 2 Rumble Pak
	int deadzone = 12; // % of the stick's travel
	// misc
	int state_slot = 0; // 0..9
	bool covers_download = true; // fetch box art from libretro-thumbnails
	bool updates = true; // offer new GitHub releases at start-up (fe_update.h)
	std::string last_dir; // the browser reopens here
	std::string last_rom; // and puts the cursor on this file

	void Load();
	void Save() const;
};

Settings& Config();
} // namespace fe
