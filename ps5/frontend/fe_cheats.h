// Mupen64Plus PS5 frontend: cheats -- the GameShark codes of mupen64plus-core's data/mupencheat.txt (built into
// eboot.bin, compressed), switched on and off in the pause menu and remembered per game (fe_emu.cpp).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fe
{
namespace cheats
{
struct Cheat
{
	std::string name; // as in the file: "Play As\Player 1" is "Player 1" in the group "Play As"
	std::string note; // what to know before using it (its "cd" line), often empty
	std::vector<uint32_t> addresses;
	std::vector<int> values; // -1: the chosen option's value
	std::vector<std::pair<std::string, int>> options; // a cheat with choices ("????"), else empty
	int state = 0; // 0 off; then 1 on, or with options 1 + the chosen option's index
	int States() const { return options.empty() ? 2 : int(options.size()) + 1; }
};

// The cheats of the game whose ROM header has these CRCs and country code (the database's "crc" lines), in
// the database's order; `game` gets its name there. Cheats that need the GameShark's button are left out:
// the DualSense has none.
std::vector<Cheat> ForGame(uint32_t crc1, uint32_t crc2, uint8_t country, std::string* game);

// "Play As\Player 1" -> "Play As: Player 1"
std::string DisplayName(const std::string& name);
} // namespace cheats
} // namespace fe
