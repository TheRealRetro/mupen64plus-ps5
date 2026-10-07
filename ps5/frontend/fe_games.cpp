// Mupen64Plus PS5 frontend: the game library (fe_games.h).
// SPDX-License-Identifier: MIT

#include "fe_games.h"

#include "OrbisPaths.h"

#include "unzip.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define FE_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const char sym##_begin[];                                                                       \
	extern "C" const char sym##_end[];

FE_INCBIN(n64ps5_gamedb, GAMEDB_TSV)

namespace fe
{
namespace
{
std::string Lower(std::string s)
{
	for (char& c : s)
		c = char(tolower(uint8_t(c)));
	return s;
}

// "Super Mario World (USA) (Rev 1)" -> "super mario world"; also drops [tags] and trims.
std::string Key(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (char c : name)
	{
		if (c == '(' || c == '[')
			depth++;
		else if ((c == ')' || c == ']') && depth > 0)
			depth--;
		else if (depth == 0)
			out += char(tolower(uint8_t(c)));
	}
	// collapse spaces, drop punctuation that file names often lose
	std::string k;
	bool space = false;
	for (char c : out)
	{
		if (c == '_' || c == ' ' || c == '.')
			space = !k.empty();
		else if (isalnum(uint8_t(c)) || c == '&' || c == '\'' || c == '-' || c == ',' || c == '!')
		{
			if (space)
				k += ' ';
			space = false;
			k += c;
		}
	}
	return k;
}

int RegionRank(const std::string& name)
{
	// prefer clean dumps: no Beta/Proto/Pirate/Virtual Console/etc.
	int penalty = 0;
	static const char* const bad[] = {"(Beta", "(Proto", "(Pirate", "(Virtual Console", "(Sample", "(Demo", "(Alt",
		"(Unl", "(Aftermarket", "(Arcade", "(Kiosk", "(Program", "(Hack"};
	for (const char* b : bad)
		if (name.find(b) != std::string::npos)
			penalty += 100;
	if (name.find("(Rev") != std::string::npos)
		penalty += 1;
	if (name.find("USA") != std::string::npos)
		return penalty + 0;
	if (name.find("World") != std::string::npos)
		return penalty + 2;
	if (name.find("Europe") != std::string::npos)
		return penalty + 4;
	if (name.find("Japan") != std::string::npos)
		return penalty + 6;
	return penalty + 8;
}

struct Db
{
	std::unordered_map<uint32_t, std::string> by_crc;
	std::unordered_map<std::string, std::string> exact; // lower-case name -> name
	std::unordered_map<std::string, std::string> loose; // Key -> best name
	Db()
	{
		const char* p = n64ps5_gamedb_begin;
		const char* end = n64ps5_gamedb_end;
		while (p < end)
		{
			const char* nl = static_cast<const char*>(memchr(p, '\n', size_t(end - p)));
			if (!nl)
				nl = end;
			if (*p != '#' && nl - p > 9 && p[8] == '\t')
			{
				const uint32_t crc = uint32_t(strtoul(std::string(p, 8).c_str(), nullptr, 16));
				std::string name(p + 9, size_t(nl - p - 9));
				by_crc.emplace(crc, name);
				exact.emplace(Lower(name), name);
				const std::string k = Key(name);
				auto it = loose.find(k);
				if (it == loose.end() || RegionRank(name) < RegionRank(it->second))
					loose[k] = name;
			}
			p = nl + 1;
		}
	}
};

const Db& D()
{
	static const Db db;
	return db;
}

bool HasRomExt(const std::string& ext)
{
	static const char* const exts[] = {".z64", ".n64", ".v64", ".rom", ".zip"};
	for (const char* e : exts)
		if (ext == e)
			return true;
	return false;
}

// A No-Intro file name ends with its region in brackets: "Super Mario 64 (USA)".
bool LooksNoIntro(const std::string& base)
{
	static const char* const regions[] = {"(USA", "(Europe", "(Japan", "(World", "(Australia", "(France",
		"(Germany", "(Italy", "(Spain", "(Korea", "(China", "(Brazil", "(Canada", "(Netherlands", "(Sweden"};
	for (const char* r : regions)
		if (base.find(r) != std::string::npos)
			return true;
	return false;
}

// Puts the first 64 bytes of a ROM in the N64's own (big-endian) order: .z64 is already in it, .v64 swaps
// every 16-bit word, .n64 every 32-bit word. False when the bytes aren't an N64 header.
bool NormalizeHeader(uint8_t* h)
{
	const uint32_t magic = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
	if (magic == 0x80371240u)
		return true;
	if (magic == 0x37804012u)
	{
		for (int i = 0; i < 64; i += 2)
			std::swap(h[i], h[i + 1]);
		return true;
	}
	if (magic == 0x40123780u)
	{
		for (int i = 0; i < 64; i += 4)
		{
			std::swap(h[i], h[i + 3]);
			std::swap(h[i + 1], h[i + 2]);
		}
		return true;
	}
	return false;
}

// ---- CRC cache: path \t size \t mtime \t crc ----
struct CrcCache
{
	std::mutex lock;
	std::unordered_map<std::string, std::string> lines; // path -> "size\tmtime\tcrc"
	bool dirty = false;
	std::string file;
	void Load()
	{
		file = OrbisDir("covers") + "/crc-cache.txt";
		FILE* f = fopen(file.c_str(), "r");
		if (!f)
			return;
		char line[2048];
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			char* tab = strchr(line, '\t');
			if (tab)
			{
				*tab = 0;
				lines[line] = tab + 1;
			}
		}
		fclose(f);
	}
	void Save()
	{
		if (!dirty)
			return;
		const std::string tmp = file + ".part";
		FILE* f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		for (const auto& kv : lines)
			fprintf(f, "%s\t%s\n", kv.first.c_str(), kv.second.c_str());
		fclose(f);
		rename(tmp.c_str(), file.c_str());
		dirty = false;
	}
};

bool CachedCrc(CrcCache& cache, const std::string& path, uint32_t* crc)
{
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return false;
	char key[64];
	snprintf(key, sizeof(key), "%lld\t%lld\t", (long long)st.st_size, (long long)st.st_mtime);
	auto it = cache.lines.find(path);
	if (it != cache.lines.end() && it->second.compare(0, strlen(key), key) == 0)
	{
		*crc = uint32_t(strtoul(it->second.c_str() + strlen(key), nullptr, 16));
		return true;
	}
	if (!RomHeaderCrc(path, crc))
		return false;
	char val[96];
	snprintf(val, sizeof(val), "%s%08X", key, *crc);
	cache.lines[path] = val;
	cache.dirty = true;
	return true;
}

void Walk(const std::string& dir, int depth, bool usb, std::vector<GameInfo>& out)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.')
			continue;
		const std::string path = dir + "/" + e->d_name;
		if (OrbisIsDir(path))
		{
			if (depth < 4)
				Walk(path, depth + 1, usb, out);
			continue;
		}
		const std::string name = e->d_name;
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos || dot == 0)
			continue;
		const std::string ext = Lower(name.substr(dot));
		if (!HasRomExt(ext))
			continue;
		GameInfo g;
		g.path = path;
		g.file_base = name.substr(0, dot);
		g.ext = ext;
		g.on_usb = usb;
		out.push_back(std::move(g));
	}
	closedir(d);
}
} // namespace

namespace gamedb
{
size_t Count()
{
	return D().by_crc.size();
}

std::string ByCrc(uint32_t crc)
{
	auto it = D().by_crc.find(crc);
	return it == D().by_crc.end() ? std::string() : it->second;
}

std::string Exact(const std::string& name)
{
	auto it = D().exact.find(Lower(name));
	return it == D().exact.end() ? std::string() : it->second;
}

std::string Loose(const std::string& file_base)
{
	auto it = D().loose.find(Key(file_base));
	return it == D().loose.end() ? std::string() : it->second;
}

std::string Title(const std::string& nointro)
{
	std::string t = nointro.substr(0, nointro.find(" ("));
	// "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
	static const char* const arts[] = {", The", ", A", ", An"};
	for (const char* a : arts)
	{
		const size_t pos = t.find(a);
		if (pos != std::string::npos)
		{
			const size_t end = pos + strlen(a);
			if (end == t.size() || t.compare(end, 3, " - ") == 0 || t[end] == ':')
			{
				const std::string art = std::string(a + 2);
				t = art + " " + t.substr(0, pos) + t.substr(end);
				break;
			}
		}
	}
	return t;
}

std::string Region(const std::string& nointro)
{
	const size_t a = nointro.find(" (");
	if (a == std::string::npos)
		return "";
	const size_t b = nointro.find(')', a);
	return b == std::string::npos ? "" : nointro.substr(a + 2, b - a - 2);
}
} // namespace gamedb

bool RomHeaderCrc(const std::string& path, uint32_t* crc)
{
	uint8_t h[64];
	bool got = false;
	const std::string ext = Lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
	if (ext == ".zip")
	{
		unzFile z = unzOpen(path.c_str());
		if (!z)
			return false;
		uLong best_size = 0;
		for (int r = unzGoToFirstFile(z); r == UNZ_OK; r = unzGoToNextFile(z))
		{
			unz_file_info info;
			char name[512];
			if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
				continue;
			if (info.uncompressed_size < best_size || info.uncompressed_size < 0x1000)
				continue; // the ROM is the biggest file in the archive
			if (unzOpenCurrentFile(z) != UNZ_OK)
				continue;
			uint8_t tmp[64];
			if (unzReadCurrentFile(z, tmp, sizeof(tmp)) == int(sizeof(tmp)) && NormalizeHeader(tmp))
			{
				memcpy(h, tmp, sizeof(h));
				best_size = info.uncompressed_size;
				got = true;
			}
			unzCloseCurrentFile(z);
		}
		unzClose(z);
	}
	else
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		got = fread(h, 1, sizeof(h), f) == sizeof(h) && NormalizeHeader(h);
		fclose(f);
	}
	if (!got)
		return false;
	*crc = (uint32_t(h[0x10]) << 24) | (uint32_t(h[0x11]) << 16) | (uint32_t(h[0x12]) << 8) | h[0x13];
	return true;
}

std::vector<GameInfo> ScanGames()
{
	std::vector<GameInfo> games;
	for (const std::string& root : OrbisRomRoots())
		Walk(root, 0, root.rfind("/mnt/", 0) == 0, games);

	CrcCache cache;
	cache.Load();
	for (GameInfo& g : games)
	{
		g.nointro = LooksNoIntro(g.file_base) ? g.file_base : gamedb::Exact(g.file_base);
		if (g.nointro.empty())
		{
			uint32_t crc = 0;
			if (CachedCrc(cache, g.path, &crc))
			{
				g.nointro = gamedb::ByCrc(crc);
				g.name_by_crc = !g.nointro.empty();
			}
		}
		if (g.nointro.empty())
			g.nointro = gamedb::Loose(g.file_base);
		g.title = g.nointro.empty() ? g.file_base : gamedb::Title(g.nointro);
		g.region = gamedb::Region(g.nointro);
	}
	cache.Save();

	std::sort(games.begin(), games.end(), [](const GameInfo& a, const GameInfo& b) {
		const int c = strcasecmp(a.title.c_str(), b.title.c_str());
		return c != 0 ? c < 0 : a.path < b.path;
	});
	OrbisLog("[games] %zu ROM(s); table of %zu names", games.size(), gamedb::Count());
	for (const GameInfo& g : games)
		OrbisLog("[games]   %s -> \"%s\"%s", g.path.c_str(), g.nointro.c_str(), g.name_by_crc ? " (by CRC)" : "");
	return games;
}
} // namespace fe
