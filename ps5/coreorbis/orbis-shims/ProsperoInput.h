// Mupen64Plus PS5: DualSense / DualShock input through libScePad (up to 4 players).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ps5input
{
constexpr int kMaxPads = 4;

struct PadState
{
	bool connected = false;
	uint32_t buttons = 0; // SCE_PAD_BUTTON_*; the left stick also sets the D-pad bits
	uint32_t raw_buttons = 0; // without the stick
	uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
};

bool Init();
void Shutdown();

// Reads every pad. Player 1 is the foreground user; the other logged-in users follow in the system's
// order. Who is logged in is checked again every couple of seconds.
void Poll();
const PadState& Pad(int player);
int ConnectedCount();

// Player 1's buttons that went down in the last Poll (menus).
uint32_t Pressed();

// The controller's motors (0 = off, 255 = full), for the N64 Rumble Pak. Repeated values are not resent.
void SetRumble(int player, uint8_t large, uint8_t small);
} // namespace ps5input
