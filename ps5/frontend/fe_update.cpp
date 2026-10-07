// Mupen64Plus PS5: updates from the project's GitHub releases (fe_update.h).
//
// SPDX-License-Identifier: MIT

#include "fe_update.h"

#include "fe_http.h"
#include "fe_menu.h"

#include "OrbisPaths.h"
#include "ProsperoNotify.h"
#include "ProsperoSce.h"

#include "unzip.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>

#ifndef N64PS5_VERSION
#define N64PS5_VERSION "dev"
#endif
#ifndef N64PS5_CONTENT_VERSION
#define N64PS5_CONTENT_VERSION "00.000.000"
#endif
#ifndef N64PS5_TITLE_ID
#define N64PS5_TITLE_ID "PPSA99064"
#endif
#ifndef N64PS5_GITHUB_REPO
#define N64PS5_GITHUB_REPO "TheRealRetro/mupen64plus-ps5"
#endif

namespace fe
{
namespace
{
constexpr size_t kMaxReleaseJson = 1u << 20;
constexpr size_t kMaxZip = 64u << 20;
constexpr size_t kMaxFile = 32u << 20;
const char* const kAsset = N64PS5_TITLE_ID ".zip";

// ---- a small JSON reader (the release object) --------------------------------------------------------------
struct Json
{
	enum Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	} type = Null;
	double number = 0;
	bool boolean = false;
	std::string text;
	std::vector<Json> items;                              // Array
	std::vector<std::pair<std::string, Json>> members; // Object

	const Json* Get(const char* key) const
	{
		for (const auto& m : members)
			if (m.first == key)
				return &m.second;
		return nullptr;
	}
	std::string Str(const char* key) const
	{
		const Json* v = Get(key);
		return v && v->type == String ? v->text : std::string();
	}
};

class JsonReader
{
public:
	explicit JsonReader(const std::string& s) : m_s(s) {}
	bool Parse(Json& out)
	{
		if (!Value(out, 0))
			return false;
		Space();
		return m_i == m_s.size();
	}

private:
	void Space()
	{
		while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r'))
			m_i++;
	}
	bool Literal(const char* word)
	{
		const size_t n = strlen(word);
		if (m_s.compare(m_i, n, word) != 0)
			return false;
		m_i += n;
		return true;
	}
	static void PutUtf8(std::string& o, uint32_t c)
	{
		if (c < 0x80)
			o += char(c);
		else if (c < 0x800)
		{
			o += char(0xC0 | (c >> 6));
			o += char(0x80 | (c & 0x3F));
		}
		else if (c < 0x10000)
		{
			o += char(0xE0 | (c >> 12));
			o += char(0x80 | ((c >> 6) & 0x3F));
			o += char(0x80 | (c & 0x3F));
		}
		else
		{
			o += char(0xF0 | (c >> 18));
			o += char(0x80 | ((c >> 12) & 0x3F));
			o += char(0x80 | ((c >> 6) & 0x3F));
			o += char(0x80 | (c & 0x3F));
		}
	}
	bool Hex4(uint32_t& v)
	{
		if (m_i + 4 > m_s.size())
			return false;
		v = 0;
		for (int k = 0; k < 4; k++)
		{
			const char c = m_s[m_i++];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= uint32_t(c - 'A' + 10);
			else
				return false;
		}
		return true;
	}
	bool Str(std::string& o)
	{
		if (m_i >= m_s.size() || m_s[m_i] != '"')
			return false;
		m_i++;
		while (m_i < m_s.size())
		{
			const char c = m_s[m_i++];
			if (c == '"')
				return true;
			if (c != '\\')
			{
				o += c;
				continue;
			}
			if (m_i >= m_s.size())
				return false;
			const char e = m_s[m_i++];
			switch (e)
			{
				case '"': o += '"'; break;
				case '\\': o += '\\'; break;
				case '/': o += '/'; break;
				case 'b': o += '\b'; break;
				case 'f': o += '\f'; break;
				case 'n': o += '\n'; break;
				case 'r': o += '\r'; break;
				case 't': o += '\t'; break;
				case 'u':
				{
					uint32_t c1;
					if (!Hex4(c1))
						return false;
					if (c1 >= 0xD800 && c1 < 0xDC00 && m_s.compare(m_i, 2, "\\u") == 0)
					{
						m_i += 2;
						uint32_t c2;
						if (!Hex4(c2))
							return false;
						c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
					}
					PutUtf8(o, c1);
					break;
				}
				default: return false;
			}
		}
		return false;
	}
	bool Value(Json& v, int depth)
	{
		if (depth > 32)
			return false;
		Space();
		if (m_i >= m_s.size())
			return false;
		const char c = m_s[m_i];
		if (c == '{')
		{
			v.type = Json::Object;
			m_i++;
			Space();
			if (m_i < m_s.size() && m_s[m_i] == '}')
			{
				m_i++;
				return true;
			}
			for (;;)
			{
				Space();
				std::string key;
				if (!Str(key))
					return false;
				Space();
				if (m_i >= m_s.size() || m_s[m_i++] != ':')
					return false;
				v.members.emplace_back(std::move(key), Json());
				if (!Value(v.members.back().second, depth + 1))
					return false;
				Space();
				if (m_i >= m_s.size())
					return false;
				if (m_s[m_i] == ',')
				{
					m_i++;
					continue;
				}
				if (m_s[m_i] == '}')
				{
					m_i++;
					return true;
				}
				return false;
			}
		}
		if (c == '[')
		{
			v.type = Json::Array;
			m_i++;
			Space();
			if (m_i < m_s.size() && m_s[m_i] == ']')
			{
				m_i++;
				return true;
			}
			for (;;)
			{
				v.items.emplace_back();
				if (!Value(v.items.back(), depth + 1))
					return false;
				Space();
				if (m_i >= m_s.size())
					return false;
				if (m_s[m_i] == ',')
				{
					m_i++;
					continue;
				}
				if (m_s[m_i] == ']')
				{
					m_i++;
					return true;
				}
				return false;
			}
		}
		if (c == '"')
		{
			v.type = Json::String;
			return Str(v.text);
		}
		if (Literal("true"))
		{
			v.type = Json::Bool;
			v.boolean = true;
			return true;
		}
		if (Literal("false"))
		{
			v.type = Json::Bool;
			return true;
		}
		if (Literal("null"))
			return true;
		const char* start = m_s.c_str() + m_i;
		char* end = nullptr;
		v.number = strtod(start, &end);
		if (end == start)
			return false;
		v.type = Json::Number;
		m_i += size_t(end - start);
		return true;
	}

	const std::string& m_s;
	size_t m_i = 0;
};

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------
std::string Sha256Hex(const uint8_t* data, size_t len)
{
	static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
		0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
		0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138,
		0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70,
		0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
		0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
	auto block = [&](const uint8_t* p) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	};
	size_t i = 0;
	for (; i + 64 <= len; i += 64)
		block(data + i);
	uint8_t tail[128] = {};
	const size_t rest = len - i;
	memcpy(tail, data + i, rest);
	tail[rest] = 0x80;
	const size_t tail_len = rest < 56 ? 64 : 128;
	const uint64_t bits = uint64_t(len) * 8;
	for (int b = 0; b < 8; b++)
		tail[tail_len - 1 - b] = uint8_t(bits >> (8 * b));
	block(tail);
	if (tail_len == 128)
		block(tail + 64);
	static const char hex[] = "0123456789abcdef";
	std::string out;
	for (uint32_t v : h)
		for (int s = 28; s >= 0; s -= 4)
			out += hex[(v >> s) & 15];
	return out;
}

int Compare(const int a[3], const int b[3])
{
	for (int i = 0; i < 3; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

std::string DisplayVersion(const int v[3])
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d.%d.%d", v[0], v[1], v[2]);
	return buf;
}

// The release, from GET /repos/<repo>/releases/latest. False (with a reason) when it isn't usable.
struct Release
{
	std::string tag, notes, url, sha256;
	int version[3] = {};
	size_t size = 0;
};

bool ParseRelease(const std::string& body, Release& r, std::string& why)
{
	Json root;
	if (!JsonReader(body).Parse(root) || root.type != Json::Object)
	{
		why = "the answer isn't a JSON object";
		return false;
	}
	r.tag = root.Str("tag_name");
	if (!ParseVersion(r.tag, r.version))
	{
		why = "tag '" + r.tag + "' isn't a version";
		return false;
	}
	r.notes = root.Str("body");
	const Json* assets = root.Get("assets");
	const Json* asset = nullptr;
	if (assets && assets->type == Json::Array)
		for (const Json& a : assets->items)
			if (a.type == Json::Object && a.Str("name") == kAsset)
				asset = &a;
	if (!asset)
	{
		why = std::string("release ") + r.tag + " has no " + kAsset;
		return false;
	}
	r.url = asset->Str("browser_download_url");
	const std::string https = "https://";
	const char* test = getenv("N64PS5_UPDATE_API"); // host tests serve the release over plain http
	if (r.url.compare(0, https.size(), https) != 0 && !(test && *test && r.url.compare(0, 7, "http://") == 0))
	{
		why = "unexpected download URL '" + r.url + "'";
		return false;
	}
	const std::string digest = asset->Str("digest");
	if (digest.size() != 7 + 64 || digest.compare(0, 7, "sha256:") != 0)
	{
		why = std::string(kAsset) + " has no SHA-256 digest";
		return false;
	}
	r.sha256 = digest.substr(7);
	for (char& c : r.sha256)
		c = char(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
	const Json* size = asset->Get("size");
	if (!size || size->type != Json::Number || size->number <= 0 || size->number > double(kMaxZip))
	{
		why = std::string(kAsset) + " has no usable size";
		return false;
	}
	r.size = size_t(size->number);
	return true;
}

// ---- installing ------------------------------------------------------------------------------------------
// Every copy of the app folder the console can see (ShadowMountPlus's scan paths).
std::vector<std::string> AppFolders()
{
	std::vector<std::string> out;
	std::vector<std::string> bases;
	const char* hb = getenv("N64PS5_PS5_HOMEBREW"); // host tests
	bases.push_back(hb && *hb ? hb : "/data/homebrew");
	if (!(hb && *hb))
	{
		char buf[64];
		for (int i = 0; i < 8; i++)
		{
			snprintf(buf, sizeof(buf), "/mnt/usb%d/homebrew", i);
			bases.push_back(buf);
			snprintf(buf, sizeof(buf), "/mnt/usb%d", i);
			bases.push_back(buf);
		}
		for (int i = 0; i < 2; i++)
		{
			snprintf(buf, sizeof(buf), "/mnt/ext%d/homebrew", i);
			bases.push_back(buf);
			snprintf(buf, sizeof(buf), "/mnt/ext%d", i);
			bases.push_back(buf);
		}
	}
	for (const std::string& b : bases)
	{
		const std::string dir = b + "/" N64PS5_TITLE_ID;
		if (OrbisIsFile(dir + "/sce_sys/param.json") && OrbisIsFile(dir + "/eboot.bin"))
			out.push_back(dir);
	}
	return out;
}

// A path inside the zip we accept: under PPSA99064/, no "..", no absolute path or backslash.
bool SafeEntry(const std::string& name, std::string& rel)
{
	const std::string prefix = N64PS5_TITLE_ID "/";
	if (name.compare(0, prefix.size(), prefix) != 0 || name.find("..") != std::string::npos ||
		name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
		return false;
	rel = name.substr(prefix.size());
	return !rel.empty() && rel[0] != '/';
}

bool ReadZip(const std::string& path, std::map<std::string, std::vector<uint8_t>>& files, std::string& why)
{
	unzFile z = unzOpen(path.c_str());
	if (!z)
	{
		why = "not a zip file";
		return false;
	}
	bool ok = true;
	size_t total = 0;
	for (int r = unzGoToFirstFile(z); r == UNZ_OK && ok; r = unzGoToNextFile(z))
	{
		char name[512];
		unz_file_info info;
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
		{
			why = "unreadable entry";
			ok = false;
			break;
		}
		const std::string n = name;
		if (!n.empty() && n.back() == '/')
			continue; // a folder
		std::string rel;
		if (!SafeEntry(n, rel))
		{
			why = "unexpected entry '" + n + "'";
			ok = false;
			break;
		}
		if (info.uncompressed_size > kMaxFile || (total += info.uncompressed_size) > 4 * kMaxZip)
		{
			why = "entry '" + n + "' is too big";
			ok = false;
			break;
		}
		std::vector<uint8_t>& data = files[rel];
		data.resize(info.uncompressed_size);
		if (unzOpenCurrentFile(z) != UNZ_OK)
		{
			why = "can't open '" + n + "'";
			ok = false;
			break;
		}
		const int got = data.empty() ? 0 : unzReadCurrentFile(z, data.data(), unsigned(data.size()));
		const int close = unzCloseCurrentFile(z); // checks the CRC
		if (got != int(data.size()) || close != UNZ_OK)
		{
			why = "'" + n + "' is damaged";
			ok = false;
		}
	}
	unzClose(z);
	if (ok && files.find("eboot.bin") == files.end())
	{
		why = "no eboot.bin in it";
		ok = false;
	}
	return ok;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
	return fclose(f) == 0 && ok;
}

// The new files over one app folder: all written as "<file>.new" first, then renamed into place (the
// running eboot.bin keeps its old copy open until the restart).
bool InstallInto(const std::string& dir, const std::map<std::string, std::vector<uint8_t>>& files, std::string& why)
{
	for (const auto& f : files)
	{
		const std::string path = dir + "/" + f.first;
		const size_t slash = path.rfind('/');
		OrbisMkdirs(path.substr(0, slash));
		if (!WriteFile(path + ".new", f.second))
		{
			why = "can't write " + path + ".new";
			for (const auto& g : files)
				unlink((dir + "/" + g.first + ".new").c_str());
			return false;
		}
	}
	for (const auto& f : files)
	{
		const std::string path = dir + "/" + f.first;
		if (rename((path + ".new").c_str(), path.c_str()) != 0)
		{
			why = "can't replace " + path;
			return false;
		}
	}
	return true;
}

std::string StampPath()
{
	return OrbisDir("update") + "/installed.txt";
}

void Restart(const UpdateOffer& offer)
{
	const char* path = "/data/homebrew/" N64PS5_TITLE_ID "/eboot.bin";
	if (access(path, F_OK) != 0)
		path = "/app0/eboot.bin";
	OrbisLog("[update] installed %s: restarting %s", offer.tag.c_str(), path);
	ProsperoNotifyFlush();
	OrbisLogClose();
	const int rc = sceSystemServiceLoadExec(path, nullptr);
	if (rc == 0)
		for (int i = 0; i < 100; i++)
			usleep(100 * 1000);
	OrbisLogOpen("boot-after-update");
	OrbisLog("[update] sceSystemServiceLoadExec(%s) -> %x", path, unsigned(rc));
	MessageBox("Mupen64Plus PS5 " + offer.version + " is installed", "Close the app and open it again to use it.");
}
} // namespace

bool ParseVersion(const std::string& text, int out[3])
{
	std::string t = text;
	if (!t.empty() && (t[0] == 'v' || t[0] == 'V'))
		t.erase(0, 1);
	int n = 0;
	const char* p = t.c_str();
	while (n < 3)
	{
		if (*p < '0' || *p > '9')
			return false;
		char* end = nullptr;
		const long v = strtol(p, &end, 10);
		if (v < 0 || v > 99999)
			return false;
		out[n++] = int(v);
		p = end;
		if (n < 3)
		{
			if (*p != '.')
				return false;
			p++;
		}
	}
	return *p == '\0';
}

const char* BuildContentVersion()
{
	return N64PS5_CONTENT_VERSION;
}

UpdateOffer CheckForUpdate()
{
	UpdateOffer offer;
	int mine[3];
	if (!ParseVersion(N64PS5_CONTENT_VERSION, mine))
		return offer;
	const char* api_env = getenv("N64PS5_UPDATE_API"); // host tests
	const std::string api = api_env && *api_env ? std::string(api_env)
												: std::string("https://api.github.com/repos/" N64PS5_GITHUB_REPO "/releases/latest");
	Http http;
	std::vector<uint8_t> body;
	const int status = http.Get(api, body, kMaxReleaseJson);
	if (status != 200)
	{
		OrbisLog("[update] latest release: HTTP %d%s", status, http.Offline() ? " (offline)" : "");
		return offer;
	}
	Release r;
	std::string why;
	if (!ParseRelease(std::string(body.begin(), body.end()), r, why))
	{
		OrbisLog("[update] latest release unusable: %s", why.c_str());
		return offer;
	}
	if (Compare(r.version, mine) <= 0)
	{
		OrbisLog("[update] up to date: %s (latest release %s)", N64PS5_CONTENT_VERSION, r.tag.c_str());
		return offer;
	}
	OrbisLog("[update] %s is newer than %s: downloading %s (%zu bytes)", r.tag.c_str(), N64PS5_CONTENT_VERSION,
		r.url.c_str(), r.size);
	ProsperoNotify("Mupen64Plus PS5: downloading version %s...", DisplayVersion(r.version).c_str());
	std::vector<uint8_t> zip;
	const int zs = http.Get(r.url, zip, kMaxZip);
	http.Term();
	if (zs != 200 || zip.size() != r.size)
	{
		OrbisLog("[update] download: HTTP %d, %zu of %zu bytes", zs, zip.size(), r.size);
		return offer;
	}
	const std::string sum = Sha256Hex(zip.data(), zip.size());
	if (sum != r.sha256)
	{
		OrbisLog("[update] SHA-256 mismatch: %s, release says %s", sum.c_str(), r.sha256.c_str());
		return offer;
	}
	OrbisLog("[update] %s downloaded and verified (sha256 %s)", kAsset, sum.c_str());
	offer.available = true;
	offer.tag = r.tag;
	offer.version = DisplayVersion(r.version);
	offer.notes = r.notes;
	offer.zip.swap(zip);
	return offer;
}

void OfferUpdate(UpdateOffer& offer, bool ask)
{
	if (!offer.available)
		return;
	// The last install of this same version didn't take (the restart ran an older eboot): don't loop.
	if (FILE* f = fopen(StampPath().c_str(), "r"))
	{
		char tag[64] = {};
		const bool got = fgets(tag, sizeof(tag), f) != nullptr;
		fclose(f);
		if (got)
		{
			tag[strcspn(tag, "\r\n")] = 0;
			if (offer.tag == tag)
			{
				OrbisLog("[update] %s was installed before but %s is running: not offering it again", tag,
					N64PS5_CONTENT_VERSION);
				return;
			}
		}
	}
	if (!ask)
	{
		OrbisLog("[update] %s available; update checks are off", offer.tag.c_str());
		return;
	}
	const std::string title = "Mupen64Plus PS5 " + offer.version + " is available";
	std::string text = std::string("You have ") + N64PS5_VERSION + ".";
	if (!offer.notes.empty())
		text += "\n\n" + offer.notes;
	if (!Confirm(title, text, "Update now", "Later"))
	{
		OrbisLog("[update] %s: later", offer.tag.c_str());
		return;
	}
	ShowBusy("Updating to " + offer.version + "...");

	OrbisMkdirs(OrbisDir("update"));
	const std::string zip_path = OrbisDir("update") + "/" + kAsset;
	std::map<std::string, std::vector<uint8_t>> files;
	std::string why;
	bool ok = WriteFile(zip_path, offer.zip);
	if (!ok)
		why = "can't write " + zip_path;
	ok = ok && ReadZip(zip_path, files, why);
	unlink(zip_path.c_str());
	std::vector<std::string> folders;
	if (ok)
	{
		folders = AppFolders();
		if (folders.empty())
		{
			ok = false;
			why = "no " N64PS5_TITLE_ID " folder found in /data/homebrew or on a USB drive (an image can't be updated: "
				  "copy the new folder by hand)";
		}
	}
	int done = 0;
	for (const std::string& dir : folders)
	{
		std::string err;
		if (InstallInto(dir, files, err))
		{
			done++;
			OrbisLog("[update] %s: %zu files written", dir.c_str(), files.size());
		}
		else
		{
			OrbisLog("[update] %s: %s", dir.c_str(), err.c_str());
			why = err;
		}
	}
	if (!ok || done == 0)
	{
		OrbisLog("[update] %s failed: %s", offer.tag.c_str(), why.c_str());
		MessageBox("The update failed", why);
		return;
	}
	if (FILE* f = fopen(StampPath().c_str(), "w"))
	{
		fprintf(f, "%s\n", offer.tag.c_str());
		fclose(f);
	}
	offer.zip.clear();
	Restart(offer);
}
} // namespace fe
