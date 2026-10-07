// Mupen64Plus PS5 frontend: the game library -- every ROM under the ROM folders, with its official name.
//
// A game is identified the way the shelf needs it for its title and its cover (libretro-thumbnails uses
// No-Intro names):
//   1. its file name, when it is already a No-Intro name ("Super Mario 64 (USA).z64");
//   2. otherwise the CRC1 checksum in its cartridge header, looked up in the table built into the ELF
//      (data/n64-gamedb.tsv, made from mupen64plus-core's ROM catalog by tools/make_n64db.py);
//   3. otherwise the file name, loosely: "super mario 64.z64" -> the best entry with that title.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
struct GameInfo
{
	std::string path; // full path
	std::string file_base; // file name without extension
	std::string ext; // ".z64", ".zip"... lower case
	std::string nointro; // official name, "" until known (or unknown)
	std::string title; // what the shelf shows
	std::string region; // "USA", "Europe"... from the name's tags
	bool on_usb = false;
	bool name_by_crc = false; // the name came from the header CRC (not the file name)
};

// The table built into the ELF.
namespace gamedb
{
size_t Count();
// "" when unknown.
std::string ByCrc(uint32_t crc);
// Exact No-Intro name ("Super Mario World (USA)") -> itself, or "".
std::string Exact(const std::string& name);
// Loose: title without tags, any case ("super mario world") -> best regional entry, or "".
std::string Loose(const std::string& file_base);
// "Super Mario World (USA) (Rev 1)" -> "Super Mario World"; "Legend of Zelda, The - ..." -> "The Legend of Zelda - ..."
std::string Title(const std::string& nointro);
std::string Region(const std::string& nointro);
} // namespace gamedb

// The CRC1 field of a ROM's cartridge header (.z64/.v64/.n64, or the biggest file in a .zip); false if the
// file isn't an N64 ROM.
bool RomHeaderCrc(const std::string& path, uint32_t* crc);

// Every ROM under the ROM folders (sub-folders included, 4 levels), sorted by title. Names that need a
// CRC are resolved here too, with a cache (/data/mupen64plus/covers/crc-cache.txt) so each ROM is read once.
std::vector<GameInfo> ScanGames();
} // namespace fe
