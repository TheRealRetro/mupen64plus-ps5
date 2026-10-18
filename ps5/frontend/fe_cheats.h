// Mupen64Plus PS5 frontend: cheats -- the GameShark codes of mupen64plus-core's data/mupencheat.txt (built into
// eboot.bin, compressed) and the player's own (found with the cheat search or typed in), switched on and off in
// the pause menu and remembered per game (fe_emu.cpp).
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
	bool mine = false; // the player's own (found with the cheat search or typed in), not the database's
	int uid = 0; // its name in the core while the game runs (fe_emu.cpp)
	int States() const { return options.empty() ? 2 : int(options.size()) + 1; }
};

// The cheats of the game whose ROM header has these CRCs and country code (the database's "crc" lines), in
// the database's order; `game` gets its name there. Cheats that need the GameShark's button are left out:
// the DualSense has none.
std::vector<Cheat> ForGame(uint32_t crc1, uint32_t crc2, uint8_t country, std::string* game);

// The player's own cheats: a file in the database's format (" cn name" lines, each followed by its codes,
// "8033B21D 0064"), read leniently so one written on a PC works too; and back.
std::vector<Cheat> Parse(const std::string& text);
std::string Format(const std::vector<Cheat>& list);

// Codes typed in: pairs of an address (8 hex digits) and a value (up to 4), separated by spaces, commas or new
// lines. False with `error` set when they aren't (or need the GameShark's button).
bool ParseCodes(const std::string& text, Cheat& c, std::string* error);
// "8033B21D 0064, 8033B21E 0001" (the chosen option's value, or the first, where the code has "????")
std::string CodesText(const Cheat& c);

// "Play As\Player 1" -> "Play As: Player 1"
std::string DisplayName(const std::string& name);
} // namespace cheats
} // namespace fe
