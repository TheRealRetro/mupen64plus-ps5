// Mupen64Plus PS5 frontend: the cheat database (fe_cheats.h).
//
// mupencheat.txt, as the core ships it (converted from Project64's database by Gent):
//
//   crc 635A2BFF-8B022326-C:45      the ROM header's CRC1, CRC2 and country code
//   gn Super Mario 64 (U)
//    cn Infinite Lives               a cheat (a '\' puts it in a group)
//     cd Some explanation            optional
//     8033B21D 0064                  its codes: address, value
//     8013CD0E ???? 0001:"1 Lap",... a value to choose from a list
//
// SPDX-License-Identifier: MIT

#include "fe_cheats.h"

#include "OrbisPaths.h"

#include "zlib.h"

#include <cstdlib>
#include <cstring>

#ifndef CHEATS_Z
#error "CHEATS_Z: the compressed mupencheat.txt (the Makefile builds it)"
#endif

#define FE_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

// 4 bytes: the text's size (little endian), then the text, zlib-compressed
FE_INCBIN(n64ps5_cheats_z, CHEATS_Z)

namespace fe
{
namespace cheats
{
namespace
{
std::string Inflate()
{
	const unsigned char* z = n64ps5_cheats_z_begin;
	const size_t zsize = size_t(n64ps5_cheats_z_end - n64ps5_cheats_z_begin);
	if (zsize < 4)
		return "";
	uLongf size = uLongf(z[0]) | uLongf(z[1]) << 8 | uLongf(z[2]) << 16 | uLongf(z[3]) << 24;
	std::string text(size, '\0');
	if (uncompress(reinterpret_cast<Bytef*>(&text[0]), &size, z + 4, uLong(zsize - 4)) != Z_OK)
	{
		OrbisLog("[cheats] the built-in cheat list is damaged");
		return "";
	}
	text.resize(size);
	return text;
}

// a code line: "8033B21D 0064" or "8013CD0E ???? 0001:"1 Lap",0002:"2 Laps""
bool ParseCode(const std::string& line, Cheat& c)
{
	char* end = nullptr;
	const unsigned long address = strtoul(line.c_str(), &end, 16);
	if (end == line.c_str() || *end != ' ')
		return false;
	const std::string rest = end + 1;
	if (rest.compare(0, 4, "????") == 0)
	{
		c.addresses.push_back(uint32_t(address));
		c.values.push_back(-1);
		// the choices: hex value, ':', quoted name, separated by commas. Every "????" of a cheat takes the same
		// choice; the list is on one of its lines (28 cheats leave the first one's empty).
		std::vector<std::pair<std::string, int>> options;
		size_t at = 4;
		while (at < rest.size())
		{
			while (at < rest.size() && (rest[at] == ' ' || rest[at] == ','))
				at++;
			const size_t colon = rest.find(':', at);
			const size_t q1 = colon == std::string::npos ? colon : rest.find('"', colon);
			const size_t q2 = q1 == std::string::npos ? q1 : rest.find('"', q1 + 1);
			if (q2 == std::string::npos)
				break;
			const int value = int(strtol(rest.substr(at, colon - at).c_str(), nullptr, 16));
			options.emplace_back(rest.substr(q1 + 1, q2 - q1 - 1), value);
			at = q2 + 1;
		}
		if (c.options.empty())
			c.options = std::move(options);
		return true;
	}
	c.addresses.push_back(uint32_t(address));
	c.values.push_back(int(strtol(rest.c_str(), nullptr, 16)));
	return true;
}

// codes the core applies only while the GameShark's button is held
bool NeedsGameSharkButton(const Cheat& c)
{
	for (uint32_t a : c.addresses)
	{
		const uint32_t type = a & 0xFF000000u;
		if (type == 0x88000000u || type == 0x89000000u || type == 0xA8000000u || type == 0xA9000000u ||
			(type >= 0xD8000000u && type <= 0xDB000000u))
			return true;
	}
	return false;
}

// a cheat the menu can offer: it has codes, needs no GameShark button, and a choice has its list
bool Usable(const Cheat& c)
{
	if (c.addresses.empty() || NeedsGameSharkButton(c))
		return false;
	for (int v : c.values)
		if (v < 0 && c.options.empty())
			return false;
	return true;
}
} // namespace

std::vector<Cheat> ForGame(uint32_t crc1, uint32_t crc2, uint8_t country, std::string* game)
{
	std::vector<Cheat> out;
	const std::string text = Inflate();
	char want[48];
	snprintf(want, sizeof(want), "crc %08X-%08X-C:%02X", unsigned(crc1), unsigned(crc2), unsigned(country));
	size_t at = text.find(want);
	if (at == std::string::npos)
		return out;
	at = text.find('\n', at);
	int skipped = 0;
	Cheat* cur = nullptr;
	while (at != std::string::npos && at + 1 < text.size())
	{
		const size_t start = at + 1;
		at = text.find('\n', start);
		std::string line = text.substr(start, at == std::string::npos ? std::string::npos : at - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.compare(0, 4, "crc ") == 0)
			break; // the next game
		if (line.compare(0, 3, "gn ") == 0)
		{
			if (game)
				*game = line.substr(3);
		}
		else if (line.compare(0, 4, " cn ") == 0)
		{
			if (cur && !Usable(*cur))
			{
				skipped++;
				out.pop_back();
			}
			out.emplace_back();
			cur = &out.back();
			cur->name = line.substr(4);
		}
		else if (cur && line.compare(0, 5, "  cd ") == 0)
			cur->note = line.substr(5);
		else if (cur && line.size() > 2 && line[0] == ' ' && line[1] == ' ')
			ParseCode(line.substr(2), *cur);
	}
	if (cur && !Usable(*cur))
	{
		skipped++;
		out.pop_back();
	}
	if (skipped)
		OrbisLog("[cheats] %d cheat(s) left out (the GameShark's button, or no codes)", skipped);
	return out;
}

std::vector<Cheat> Parse(const std::string& text)
{
	std::vector<Cheat> out;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		size_t lead = 0;
		while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t'))
			lead++;
		line.erase(0, lead);
		if (line.empty() || line[0] == '#' || line.compare(0, 4, "crc ") == 0 || line.compare(0, 3, "gn ") == 0)
			continue;
		if (line.compare(0, 3, "cn ") == 0)
		{
			out.emplace_back();
			out.back().name = line.substr(3);
			out.back().mine = true;
		}
		else if (!out.empty() && line.compare(0, 3, "cd ") == 0)
			out.back().note = line.substr(3);
		else if (!out.empty())
			ParseCode(line, out.back());
	}
	std::vector<Cheat> usable;
	for (Cheat& c : out)
		if (!c.name.empty() && Usable(c))
			usable.push_back(std::move(c));
	return usable;
}

std::string Format(const std::vector<Cheat>& list)
{
	std::string out = "# Mupen64Plus PS5: your own cheats for this game, in the format of mupen64plus's cheat list.\n"
					  "# A cheat is a \" cn name\" line, then its codes: an address and a value, \"8033B21D 0064\".\n";
	for (const Cheat& c : list)
	{
		out += " cn " + c.name + "\n";
		if (!c.note.empty())
			out += "  cd " + c.note + "\n";
		char code[24];
		bool listed = false; // a cheat with choices lists them on its first "????" line
		for (size_t i = 0; i < c.addresses.size(); i++)
		{
			if (c.values[i] >= 0)
				snprintf(code, sizeof(code), "  %08X %04X", unsigned(c.addresses[i]), unsigned(c.values[i] & 0xFFFF));
			else
				snprintf(code, sizeof(code), "  %08X ????", unsigned(c.addresses[i]));
			out += code;
			if (c.values[i] < 0 && !listed)
			{
				listed = true;
				for (size_t k = 0; k < c.options.size(); k++)
				{
					snprintf(code, sizeof(code), "%s%04X:", k ? "," : " ", unsigned(c.options[k].second & 0xFFFF));
					out += code + ("\"" + c.options[k].first + "\"");
				}
			}
			out += "\n";
		}
	}
	return out;
}

bool ParseCodes(const std::string& text, Cheat& c, std::string* error)
{
	std::vector<std::string> tokens;
	std::string cur;
	for (const char ch : text + " ")
	{
		if (ch == ' ' || ch == ',' || ch == '\n' || ch == '\r' || ch == '\t' || ch == ';' || ch == ':' || ch == '+')
		{
			if (!cur.empty())
				tokens.push_back(cur);
			cur.clear();
		}
		else
			cur += ch;
	}
	auto hex = [](const std::string& t, size_t max) {
		if (t.empty() || t.size() > max)
			return false;
		for (const char ch : t)
			if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
				return false;
		return true;
	};
	Cheat out;
	for (size_t i = 0, step = 2; i < tokens.size(); i += step)
	{
		// "8033B21D0064" as one token is a code too
		std::string address = tokens[i], value = i + 1 < tokens.size() ? tokens[i + 1] : "";
		step = 2;
		if (address.size() == 12 && hex(address, 12))
		{
			value = address.substr(8);
			address.resize(8);
			step = 1;
		}
		if (address.size() != 8 || !hex(address, 8) || !hex(value, 4))
		{
			if (error)
				*error = "\"" + tokens[i] + (value.empty() ? "" : " " + value) +
						 "\" is not a code: a code is 8 hex digits and 4 more, like 8033B21D 0064";
			return false;
		}
		out.addresses.push_back(uint32_t(strtoul(address.c_str(), nullptr, 16)));
		out.values.push_back(int(strtol(value.c_str(), nullptr, 16)));
	}
	if (out.addresses.empty())
	{
		if (error)
			*error = "No codes: type them like 8033B21D 0064";
		return false;
	}
	if (NeedsGameSharkButton(out))
	{
		if (error)
			*error = "Codes starting 88, 89, A8, A9 or D8 to DB need the GameShark's button, which a DualSense hasn't got";
		return false;
	}
	c.addresses = std::move(out.addresses);
	c.values = std::move(out.values);
	c.options.clear();
	return true;
}

std::string CodesText(const Cheat& c)
{
	std::string out;
	char code[24];
	for (size_t i = 0; i < c.addresses.size(); i++)
	{
		int v = c.values[i];
		if (v < 0)
			v = c.options.empty() ? 0 : c.options[size_t(c.state > 0 ? c.state - 1 : 0)].second;
		snprintf(code, sizeof(code), "%s%08X %04X", out.empty() ? "" : ", ", unsigned(c.addresses[i]), unsigned(v & 0xFFFF));
		out += code;
	}
	return out;
}

std::string DisplayName(const std::string& name)
{
	std::string s = name;
	for (size_t i = s.find('\\'); i != std::string::npos; i = s.find('\\', i + 2))
		s.replace(i, 1, ": ");
	return s;
}
} // namespace cheats
} // namespace fe
