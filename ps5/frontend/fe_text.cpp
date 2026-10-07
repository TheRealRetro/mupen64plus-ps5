// Mupen64Plus PS5 frontend: text with Roboto and PromptFont (fe_text.h).
// SPDX-License-Identifier: MIT

#include "fe_text.h"

#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

#define FE_INCBIN(sym, path)                                                                                  \
	__asm__(".section .rodata\n"                                                                               \
			".balign 16\n"                                                                                     \
			".global " #sym "_begin\n" #sym "_begin:\n"                                                        \
			".incbin \"" path "\"\n"                                                                           \
			".global " #sym "_end\n" #sym "_end:\n"                                                            \
			".previous\n");                                                                                    \
	extern "C" const unsigned char sym##_begin[];                                                              \
	extern "C" const unsigned char sym##_end[];

FE_INCBIN(n64ps5_font_roboto, FE_FONT_DIR "/Roboto-Regular.ttf")
FE_INCBIN(n64ps5_font_prompt, FE_FONT_DIR "/promptfont.otf")
// Font Awesome Brands (as PS5SX2 ships it, for its author line): the GitHub mark on the shelf
FE_INCBIN(n64ps5_font_brands, FE_FONT_DIR "/fa-brands-400.otf")

namespace fe
{
namespace
{
constexpr float kIconScale = 1.45f; // button symbols next to words, as PS5SX2 draws them bigger

struct Font
{
	stbtt_fontinfo info;
	int ascent = 0, descent = 0, gap = 0;
};

struct Glyph
{
	int w = 0, h = 0, xoff = 0, yoff = 0; // bitmap placement relative to the pen on the baseline
	float advance = 0;
	std::vector<uint8_t> alpha;
	int font = 0;
};

struct Fonts
{
	Font f[3]; // 0 Roboto, 1 PromptFont, 2 Font Awesome Brands
	bool ok = false;
	std::mutex lock;
	std::unordered_map<uint64_t, std::unique_ptr<Glyph>> cache;

	Fonts()
	{
		const unsigned char* data[3] = {n64ps5_font_roboto_begin, n64ps5_font_prompt_begin, n64ps5_font_brands_begin};
		ok = true;
		for (int i = 0; i < 3; i++)
		{
			if (!stbtt_InitFont(&f[i].info, data[i], stbtt_GetFontOffsetForIndex(data[i], 0)))
				ok = false;
			else
				stbtt_GetFontVMetrics(&f[i].info, &f[i].ascent, &f[i].descent, &f[i].gap);
		}
	}

	// The glyph for code point cp at an em of px pixels (cached; the pointer stays valid).
	const Glyph* Get(uint32_t cp, int px)
	{
		const uint64_t key = (uint64_t(px) << 32) | cp;
		{
			std::lock_guard<std::mutex> l(lock);
			auto it = cache.find(key);
			if (it != cache.end())
				return it->second.get();
		}
		auto g = std::make_unique<Glyph>();
		int font = 0;
		int index = stbtt_FindGlyphIndex(&f[0].info, int(cp));
		if (index == 0)
		{
			const int alt = stbtt_FindGlyphIndex(&f[1].info, int(cp));
			if (alt != 0)
			{
				font = 1;
				index = alt;
			}
			else if (cp >= 0xE000 && cp <= 0xF8FF) // private use: the brand marks
			{
				const int brand = stbtt_FindGlyphIndex(&f[2].info, int(cp));
				if (brand != 0)
				{
					font = 2;
					index = brand;
				}
			}
		}
		g->font = font;
		const float em = font == 1 ? px * kIconScale : float(px);
		const float s = stbtt_ScaleForMappingEmToPixels(&f[font].info, em);
		int adv = 0, lsb = 0;
		stbtt_GetGlyphHMetrics(&f[font].info, index, &adv, &lsb);
		g->advance = adv * s;
		int x0, y0, x1, y1;
		stbtt_GetGlyphBitmapBox(&f[font].info, index, s, s, &x0, &y0, &x1, &y1);
		g->w = std::max(0, x1 - x0);
		g->h = std::max(0, y1 - y0);
		g->xoff = x0;
		g->yoff = y0;
		if (font == 1)
			g->yoff += int(px * 0.12f); // centre the bigger symbol on the words' x-height
		if (g->w > 0 && g->h > 0)
		{
			g->alpha.resize(size_t(g->w) * g->h);
			stbtt_MakeGlyphBitmap(&f[font].info, g->alpha.data(), g->w, g->h, g->w, s, s, index);
		}
		std::lock_guard<std::mutex> l(lock);
		auto& slot = cache[key];
		if (!slot)
			slot = std::move(g);
		return slot.get();
	}

	float Kern(uint32_t a, uint32_t b, int px)
	{
		const int ia = stbtt_FindGlyphIndex(&f[0].info, int(a)), ib = stbtt_FindGlyphIndex(&f[0].info, int(b));
		if (!ia || !ib)
			return 0;
		return stbtt_GetGlyphKernAdvance(&f[0].info, ia, ib) * stbtt_ScaleForMappingEmToPixels(&f[0].info, float(px));
	}
};

Fonts& F()
{
	static Fonts* fonts = new Fonts(); // never destroyed: the cover thread may draw at exit
	return *fonts;
}

uint32_t NextCodepoint(const char*& s)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
	uint32_t cp = *p++;
	int extra = 0;
	if (cp >= 0xF0)
	{
		cp &= 0x07;
		extra = 3;
	}
	else if (cp >= 0xE0)
	{
		cp &= 0x0F;
		extra = 2;
	}
	else if (cp >= 0xC0)
	{
		cp &= 0x1F;
		extra = 1;
	}
	for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++)
		cp = (cp << 6) | (*p++ & 0x3F);
	s = reinterpret_cast<const char*>(p);
	return cp;
}

int EmPx(int scale)
{
	return scale * kGlyphH;
}

inline uint32_t Blend(uint32_t dst, uint32_t color, uint32_t a) // a 0..255
{
	// weights out of 256, so red+blue fit one 32-bit multiply without carrying into each other
	const uint32_t w = a + (a >> 7);
	const uint32_t iw = 256 - w;
	const uint32_t rb = (((dst & 0xff00ffu) * iw + (color & 0xff00ffu) * w) >> 8) & 0xff00ffu;
	const uint32_t g = (((dst & 0x00ff00u) * iw + (color & 0x00ff00u) * w) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

struct Target
{
	uint32_t* px;
	int w, h, pitch;
};

void Draw(const Target& t, int x, int y, const char* utf8, int scale, uint32_t color, bool shadow)
{
	Fonts& fonts = F();
	if (!fonts.ok || !utf8)
		return;
	const int px = EmPx(scale);
	const int baseline = y + int(std::lround(px * 0.80f));
	const int sh = std::max(1, scale / 2); // shadow offset
	for (int pass = shadow ? 0 : 1; pass < 2; pass++)
	{
		const uint32_t col = pass == 0 ? 0xff000000u : color;
		const uint32_t strength = pass == 0 ? 150 : 255;
		const int ox = pass == 0 ? sh : 0, oy = pass == 0 ? sh : 0;
		float pen = float(x);
		uint32_t prev = 0;
		const char* s = utf8;
		while (*s)
		{
			const uint32_t cp = NextCodepoint(s);
			const Glyph* g = fonts.Get(cp, px);
			if (prev && g->font == 0)
				pen += fonts.Kern(prev, cp, px);
			const int gx = int(std::lround(pen)) + g->xoff + ox;
			const int gy = baseline + g->yoff + oy;
			for (int j = 0; j < g->h; j++)
			{
				const int yy = gy + j;
				if (yy < 0 || yy >= t.h)
					continue;
				uint32_t* row = t.px + size_t(yy) * t.pitch;
				const uint8_t* a = g->alpha.data() + size_t(j) * g->w;
				for (int i = 0; i < g->w; i++)
				{
					const int xx = gx + i;
					if (xx < 0 || xx >= t.w || a[i] == 0)
						continue;
					row[xx] = Blend(row[xx], col, (a[i] * strength) / 255);
				}
			}
			pen += g->advance;
			prev = g->font == 0 ? cp : 0;
		}
	}
}
} // namespace

int TextWidth(const char* utf8, int scale)
{
	Fonts& fonts = F();
	if (!fonts.ok || !utf8)
		return 0;
	const int px = EmPx(scale);
	float pen = 0;
	uint32_t prev = 0;
	const char* s = utf8;
	while (*s)
	{
		const uint32_t cp = NextCodepoint(s);
		const Glyph* g = fonts.Get(cp, px);
		if (prev && g->font == 0)
			pen += fonts.Kern(prev, cp, px);
		pen += g->advance;
		prev = g->font == 0 ? cp : 0;
	}
	return int(std::ceil(pen));
}

void DrawText(int x, int y, const char* utf8, int scale, uint32_t color, bool outline)
{
	uint32_t* surf = ps5video::Surface();
	if (!surf)
		return;
	Draw(Target{surf, ps5video::kWidth, ps5video::kHeight, ps5video::kWidth}, x, y, utf8, scale, color, outline);
}

void DrawTextOn(const Canvas& canvas, int x, int y, const char* utf8, int scale, uint32_t color, bool outline)
{
	Draw(Target{canvas.px, canvas.w, canvas.h, canvas.pitch}, x, y, utf8, scale, color, outline);
}

std::string FitText(const std::string& utf8, int scale, int max_px)
{
	if (TextWidth(utf8.c_str(), scale) <= max_px)
		return utf8;
	std::string s = utf8;
	while (!s.empty())
	{
		size_t n = s.size() - 1;
		while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80)
			n--;
		s.resize(n);
		const std::string t = s + "\xE2\x80\xA6"; // the ellipsis character, as PS5SX2
		if (TextWidth(t.c_str(), scale) <= max_px)
			return t;
	}
	return "\xE2\x80\xA6";
}
} // namespace fe
