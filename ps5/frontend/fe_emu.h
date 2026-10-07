// Mupen64Plus PS5 frontend: mupen64plus-core and its built-in plugins behind a small API (fe_emu.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace emu
{
// CoreStartup + the plugins' PluginStartup. Needs /data (the core's config and ROM catalog live there).
bool InitCore();
void DeinitCore();

enum class ExitReason
{
	BackToList, // the pause menu's "back to the game list"
	Quit, // "quit Mupen64Plus"
	Error, // the game could not be started (see `error`)
};
// Opens the ROM and runs it until the player leaves it (blocks; the pause menu runs inside).
ExitReason RunGame(const std::string& path, std::string* error);

bool GameLoaded();
std::string GameName(); // the title shown on the shelf (No-Intro name, ROM header name or file name)

// Settings from fe::Config() that apply while a game runs (aspect, smoothing, FPS, sound, dead zone).
void ApplySettings();

// Called from the pause menu (on the emulation thread): state saves and loads run at the next video
// interrupt, so right after the menu closes.
bool SaveState(int slot);
bool LoadState(int slot);
bool StateExists(int slot);
void Reset();
bool FastForward();
void SetFastForward(bool on);
void Osd(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // message drawn over the picture

// Redraws the last frame into the surface (the pause menu draws over it).
void RedrawLastFrame();
} // namespace emu
