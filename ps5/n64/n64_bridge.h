// Mupen64Plus PS5: what the built-in plugins (C) and the frontend (C++, frontend/fe_emu.cpp) say to each
// other while a game runs.
//
// The emulation runs inside CoreDoCommand(M64CMD_EXECUTE) on the frontend's thread. Once per N64 video
// interrupt the video plugin hands the frontend the finished picture (n64ps5_on_vi); that is where the
// frontend shows it, reads the pads, opens the pause menu and paces the game to the TV.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- video (plugins/gfx_ps5.c) -------------------------------------------------------------------------
typedef struct
{
	const uint32_t* pixels; // A8B8G8R8 in memory order R,G,B,A (alpha not meaningful)
	int width, height; // what angrylion's VI produced (e.g. 640x480)
	int pitch; // in pixels
} n64ps5_frame;

typedef struct
{
	int vi_mode; // 0 filtered (the real VI: AA + divot + dither filter), 1 unfiltered
	bool hide_overscan; // crop the VI's blank borders
	int num_workers; // angrylion render threads (0 = one per hardware thread)
	int dp_compat; // 0 fast, 1 moderate, 2 slow (sync points between render threads)
} n64ps5_gfx_options;

// The frontend sets these before the game starts; RomOpen applies them.
void n64ps5_gfx_set_options(const n64ps5_gfx_options* opt);

// Implemented by the frontend: called on every video interrupt. `frame` is NULL when the VI shows nothing
// (blanked) this time. The pixels stay valid until the next call.
void n64ps5_on_vi(const n64ps5_frame* frame);

// ---- audio (plugins/audio_ps5.cpp) ---------------------------------------------------------------------
void n64ps5_audio_set_mute(bool mute);
// Fast forward: the audio plugin drops what doesn't fit instead of slowing the game down.
void n64ps5_audio_set_fast_forward(bool on);
int n64ps5_audio_game_rate(void); // the game's current output rate in Hz (0 before it sets one)
int n64ps5_audio_target(void); // the AudioOut ring level the rate control aims at (48 kHz frames)

// ---- input (plugins/input_ps5.cpp) ---------------------------------------------------------------------
typedef struct
{
	int deadzone; // percent of the stick's travel ignored around the centre (0..30)
	int range; // N64 stick range at full tilt (the real stick reaches about 80)
	int pak; // 0 none, 1 Controller Pak (memory), 2 Rumble Pak
} n64ps5_input_options;

void n64ps5_input_set_options(const n64ps5_input_options* opt);
// While the frontend uses the pad itself (a hotkey held, the pause menu just closed), the game sees no
// buttons.
void n64ps5_input_block(bool block);

#ifdef __cplusplus
}
#endif
