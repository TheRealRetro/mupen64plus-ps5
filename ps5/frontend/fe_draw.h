// Mupen64Plus PS5 frontend: anti-aliased shapes on the CPU-drawn screen (ps5video::Surface()), for the pause menu's
// look: rounded panels and buttons, glows, gradients, switches, sliders and a few drawn icons.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace fe
{
namespace gfx
{
// color over the pixel at `alpha` (0..255)
uint32_t Blend(uint32_t dst, uint32_t color, int alpha);
uint32_t Mix(uint32_t a, uint32_t b, float t); // a -> b

void FillRoundRect(int x, int y, int w, int h, int r, uint32_t color, int alpha = 255);
// from c0 to c1, left to right (horizontal) or top to bottom
void GradientRoundRect(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1, bool horizontal, int alpha = 255);
// a border `thick` pixels wide, inside the rectangle
void StrokeRoundRect(int x, int y, int w, int h, int r, float thick, uint32_t color, int alpha = 255);
// light around the rectangle, fading out over `size` pixels
void GlowRoundRect(int x, int y, int w, int h, int r, int size, uint32_t color, int alpha);
void FillCircle(float cx, float cy, float r, uint32_t color, int alpha = 255);
void StrokeCircle(float cx, float cy, float r, float thick, uint32_t color, int alpha = 255);
void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color, int alpha = 255);
// part of a ring, angles in degrees, clockwise from 3 o'clock (screen y grows downwards)
void StrokeArc(float cx, float cy, float r, float thick, float a0, float a1, uint32_t color, int alpha = 255);
void HLine(int x, int y, int w, uint32_t color, int alpha = 255);
void VLine(int x, int y, int h, uint32_t color, int alpha = 255);

// small icons drawn with the shapes above, in a `size` x `size` box
enum class Icon
{
	Pause,
	Play,
	Restart, // a circular arrow
	FastForward,
	Grid, // 3 x 3 squares
	Tiles, // 2 x 2 squares
	Frame, // a dashed square
	Bars, // a bar chart
	Screen, // a rectangle
	Gauge,
	Download,
	Search, // a magnifying glass
};
void DrawIcon(Icon icon, int x, int y, int size, uint32_t color);
} // namespace gfx
} // namespace fe
