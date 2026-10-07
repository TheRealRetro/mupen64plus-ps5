// Mupen64Plus PS5 frontend: text, with the fonts PS5SX2's frontend uses -- PCSX2's Roboto Regular for words and
// PromptFont for the controller's button symbols (frontend/assets/fonts) -- rasterized with stb_truetype
// (anti-aliased, kerned, glyphs cached per size) on the CPU.
//
// Sizes are given as 'scale', the unit the frontend was laid out in: a line of text is about 10*scale
// pixels tall (the em is 10*scale px), and (x, y) is the top-left of that line.
// A character Roboto doesn't have is taken from PromptFont, drawn a bit larger, so button symbols can be
// written inline: "Jogar " + icon::Cross ...
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace fe
{
constexpr int kGlyphH = 10; // a line is kGlyphH * scale pixels

int TextWidth(const char* utf8, int scale);
// outline: a soft dark shadow under the text, for text over pictures.
void DrawText(int x, int y, const char* utf8, int scale, uint32_t color, bool outline = true);
// The same into any 32-bit pixel buffer (cover placeholders, from the cover thread).
struct Canvas
{
	uint32_t* px;
	int w, h, pitch;
};
void DrawTextOn(const Canvas& canvas, int x, int y, const char* utf8, int scale, uint32_t color, bool outline = true);
// Cuts the string so it fits max_px (adds "...").
std::string FitText(const std::string& utf8, int scale, int max_px);

// PromptFont's controller symbols (the same code points as PS5SX2's fe_text.h).
namespace icon
{
constexpr const char* Cross = "\xE2\x87\xA3";
constexpr const char* Circle = "\xE2\x87\xA2";
constexpr const char* Triangle = "\xE2\x87\xA1";
constexpr const char* Square = "\xE2\x87\xA0";
constexpr const char* L1 = "\xE2\x86\xB0";
constexpr const char* R1 = "\xE2\x86\xB1";
constexpr const char* L2 = "\xE2\x86\xB2";
constexpr const char* R2 = "\xE2\x86\xB3";
constexpr const char* L3 = "\xE2\x86\xBA";
constexpr const char* R3 = "\xE2\x86\xBB";
constexpr const char* DpadLeftRight = "\xE2\x86\xA2";
constexpr const char* DpadUpDown = "\xE2\x86\xA3";
constexpr const char* Options = "\xE2\x88\x88";
constexpr const char* GitHub = "\xEF\x82\x9B"; // U+F09B, Font Awesome Brands
} // namespace icon
} // namespace fe
