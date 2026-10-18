// Mupen64Plus PS5 frontend: anti-aliased shapes (fe_draw.h). Rounded rectangles and circles use their signed
// distance (coverage = 0.5 - distance), the triangles 4 x 4 samples per pixel: menus only, a few per frame.
// SPDX-License-Identifier: MIT

#include "fe_draw.h"

#include "ProsperoVideo.h"

#include <algorithm>
#include <cmath>

namespace fe
{
namespace gfx
{
namespace
{
constexpr int W = ps5video::kWidth;
constexpr int H = ps5video::kHeight;

inline float Clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline void Put(int x, int y, uint32_t color, float coverage, int alpha)
{
	if (coverage <= 0.0f || x < 0 || y < 0 || x >= W || y >= H)
		return;
	uint32_t& p = ps5video::Surface()[size_t(y) * W + x];
	p = Blend(p, color, int(coverage * alpha + 0.5f));
}

struct RoundRect
{
	float cx, cy, hw, hh, r;
	RoundRect(int x, int y, int w, int h, int radius)
	{
		hw = w * 0.5f;
		hh = h * 0.5f;
		cx = x + hw;
		cy = y + hh;
		r = std::max(0.0f, std::min(float(radius), std::min(hw, hh)));
	}
	// signed distance of a pixel's centre to the outline (negative inside)
	float Distance(int x, int y) const
	{
		const float ax = std::fabs(x + 0.5f - cx), ay = std::fabs(y + 0.5f - cy);
		const float qx = ax - (hw - r), qy = ay - (hh - r);
		if (qx <= 0.0f || qy <= 0.0f)
			return std::max(ax - hw, ay - hh);
		return std::sqrt(qx * qx + qy * qy) - r;
	}
	// deep inside, away from the edges and corners by more than `band`
	bool Inside(int x, int y, float band) const
	{
		const float ax = std::fabs(x + 0.5f - cx), ay = std::fabs(y + 0.5f - cy);
		return (ax < hw - r && ay < hh - band) || (ay < hh - r && ax < hw - band);
	}
};
} // namespace

uint32_t Blend(uint32_t dst, uint32_t color, int alpha)
{
	if (alpha <= 0)
		return dst;
	const uint32_t a = uint32_t(std::min(alpha, 255));
	const uint32_t w = a + (a >> 7);
	const uint32_t iw = 256 - w;
	const uint32_t rb = (((dst & 0xff00ffu) * iw + (color & 0xff00ffu) * w) >> 8) & 0xff00ffu;
	const uint32_t g = (((dst & 0x00ff00u) * iw + (color & 0x00ff00u) * w) >> 8) & 0x00ff00u;
	return 0xff000000u | rb | g;
}

uint32_t Mix(uint32_t a, uint32_t b, float t)
{
	return Blend(a, b, int(Clamp01(t) * 255.0f + 0.5f));
}

void FillRoundRect(int x, int y, int w, int h, int r, uint32_t color, int alpha)
{
	const RoundRect rr(x, y, w, h, r);
	for (int yy = std::max(0, y); yy < std::min(H, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(W, x + w); xx++)
			Put(xx, yy, color, rr.Inside(xx, yy, 1.0f) ? 1.0f : Clamp01(0.5f - rr.Distance(xx, yy)), alpha);
}

void GradientRoundRect(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1, bool horizontal, int alpha)
{
	const RoundRect rr(x, y, w, h, r);
	for (int yy = std::max(0, y); yy < std::min(H, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(W, x + w); xx++)
		{
			const float t = horizontal ? float(xx - x) / std::max(1, w - 1) : float(yy - y) / std::max(1, h - 1);
			Put(xx, yy, Mix(c0, c1, t), rr.Inside(xx, yy, 1.0f) ? 1.0f : Clamp01(0.5f - rr.Distance(xx, yy)), alpha);
		}
}

void StrokeRoundRect(int x, int y, int w, int h, int r, float thick, uint32_t color, int alpha)
{
	const RoundRect rr(x, y, w, h, r);
	for (int yy = std::max(0, y); yy < std::min(H, y + h); yy++)
		for (int xx = std::max(0, x); xx < std::min(W, x + w); xx++)
		{
			if (rr.Inside(xx, yy, thick + 1.0f))
				continue;
			const float d = rr.Distance(xx, yy);
			Put(xx, yy, color, Clamp01(0.5f - d) - Clamp01(0.5f - d - thick), alpha);
		}
}

void GlowRoundRect(int x, int y, int w, int h, int r, int size, uint32_t color, int alpha)
{
	const RoundRect rr(x, y, w, h, r);
	for (int yy = std::max(0, y - size); yy < std::min(H, y + h + size); yy++)
		for (int xx = std::max(0, x - size); xx < std::min(W, x + w + size); xx++)
		{
			if (rr.Inside(xx, yy, 0.0f))
				continue;
			const float d = rr.Distance(xx, yy);
			if (d <= 0.0f || d >= size)
				continue;
			const float f = 1.0f - d / size;
			Put(xx, yy, color, f * f, alpha);
		}
}

void FillCircle(float cx, float cy, float r, uint32_t color, int alpha)
{
	for (int yy = std::max(0, int(cy - r - 1)); yy <= std::min(H - 1, int(cy + r + 1)); yy++)
		for (int xx = std::max(0, int(cx - r - 1)); xx <= std::min(W - 1, int(cx + r + 1)); xx++)
		{
			const float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
			Put(xx, yy, color, Clamp01(0.5f - (std::sqrt(dx * dx + dy * dy) - r)), alpha);
		}
}

void StrokeCircle(float cx, float cy, float r, float thick, uint32_t color, int alpha)
{
	StrokeArc(cx, cy, r - thick * 0.5f, thick, 0.0f, 360.0f, color, alpha);
}

void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color, int alpha)
{
	const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (std::fabs(area) < 1e-3f)
		return;
	const float s = area > 0 ? 1.0f : -1.0f;
	auto edge = [s](float ax, float ay, float bx, float by, float px, float py) {
		return s * ((bx - ax) * (py - ay) - (by - ay) * (px - ax));
	};
	const int minx = std::max(0, int(std::floor(std::min({x0, x1, x2})))), maxx = std::min(W - 1, int(std::ceil(std::max({x0, x1, x2}))));
	const int miny = std::max(0, int(std::floor(std::min({y0, y1, y2})))), maxy = std::min(H - 1, int(std::ceil(std::max({y0, y1, y2}))));
	for (int yy = miny; yy <= maxy; yy++)
		for (int xx = minx; xx <= maxx; xx++)
		{
			int in = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++)
				{
					const float px = xx + (sx + 0.5f) / 4.0f, py = yy + (sy + 0.5f) / 4.0f;
					if (edge(x0, y0, x1, y1, px, py) >= 0 && edge(x1, y1, x2, y2, px, py) >= 0 && edge(x2, y2, x0, y0, px, py) >= 0)
						in++;
				}
			Put(xx, yy, color, in / 16.0f, alpha);
		}
}

void StrokeArc(float cx, float cy, float r, float thick, float a0, float a1, uint32_t color, int alpha)
{
	const float outer = r + thick * 0.5f + 1.0f;
	const bool full = a1 - a0 >= 360.0f;
	for (int yy = std::max(0, int(cy - outer)); yy <= std::min(H - 1, int(cy + outer)); yy++)
		for (int xx = std::max(0, int(cx - outer)); xx <= std::min(W - 1, int(cx + outer)); xx++)
		{
			const float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
			const float cov = Clamp01(thick * 0.5f + 0.5f - std::fabs(std::sqrt(dx * dx + dy * dy) - r));
			if (cov <= 0.0f)
				continue;
			if (!full)
			{
				float a = std::atan2(dy, dx) * 57.29578f;
				if (a < 0)
					a += 360.0f;
				float lo = std::fmod(a0, 360.0f), span = a1 - a0;
				if (lo < 0)
					lo += 360.0f;
				float rel = a - lo;
				if (rel < 0)
					rel += 360.0f;
				if (rel > span)
					continue;
			}
			Put(xx, yy, color, cov, alpha);
		}
}

void HLine(int x, int y, int w, uint32_t color, int alpha)
{
	for (int xx = x; xx < x + w; xx++)
		Put(xx, y, color, 1.0f, alpha);
}

void VLine(int x, int y, int h, uint32_t color, int alpha)
{
	for (int yy = y; yy < y + h; yy++)
		Put(x, yy, color, 1.0f, alpha);
}

void DrawIcon(Icon icon, int x, int y, int size, uint32_t color)
{
	const float s = float(size);
	const float cx = x + s * 0.5f, cy = y + s * 0.5f;
	switch (icon)
	{
		case Icon::Pause:
		{
			const int bw = int(s * 0.2f), bh = int(s * 0.62f), gap = int(s * 0.16f);
			const int top = int(cy - bh * 0.5f), left = int(cx - gap * 0.5f) - bw;
			FillRoundRect(left, top, bw, bh, bw / 3, color);
			FillRoundRect(int(cx + gap * 0.5f), top, bw, bh, bw / 3, color);
			break;
		}
		case Icon::Play:
			FillTriangle(x + s * 0.22f, y + s * 0.14f, x + s * 0.22f, y + s * 0.86f, x + s * 0.86f, cy, color);
			break;
		case Icon::Restart:
		{
			// most of a circle, open at the top, with an arrowhead at its upper left end pointing on round
			const float r = s * 0.32f, t = s * 0.12f;
			StrokeArc(cx, cy, r, t, 300.0f, 600.0f, color);
			const float a = 240.0f / 57.29578f;
			const float px = cx + r * std::cos(a), py = cy + r * std::sin(a);
			const float tx = -std::sin(a), ty = std::cos(a); // the way round (clockwise)
			const float nx = std::cos(a), ny = std::sin(a);
			const float hl = s * 0.22f, hw = s * 0.17f;
			FillTriangle(px + tx * hl, py + ty * hl, px + nx * hw, py + ny * hw, px - nx * hw, py - ny * hw, color);
			break;
		}
		case Icon::FastForward:
			FillTriangle(x + s * 0.08f, y + s * 0.2f, x + s * 0.08f, y + s * 0.8f, x + s * 0.5f, cy, color);
			FillTriangle(x + s * 0.5f, y + s * 0.2f, x + s * 0.5f, y + s * 0.8f, x + s * 0.92f, cy, color);
			break;
		case Icon::Grid:
			for (int j = 0; j < 3; j++)
				for (int i = 0; i < 3; i++)
					FillRoundRect(x + int(s * (0.1f + 0.29f * i)), y + int(s * (0.1f + 0.29f * j)), int(s * 0.22f),
						int(s * 0.22f), 2, color);
			break;
		case Icon::Tiles:
			for (int j = 0; j < 2; j++)
				for (int i = 0; i < 2; i++)
					FillRoundRect(x + int(s * (0.1f + 0.43f * i)), y + int(s * (0.1f + 0.43f * j)), int(s * 0.36f),
						int(s * 0.36f), int(s * 0.08f), color);
			break;
		case Icon::Frame:
		{
			// a dashed square: four dashes a side
			const int a = int(s * 0.12f), b = int(s * 0.88f), t = std::max(2, int(s * 0.09f)), d = (b - a) / 7;
			for (int k = 0; k < 7; k += 2)
			{
				FillRoundRect(x + a + k * d, y + a, d, t, 1, color);
				FillRoundRect(x + a + k * d, y + b - t, d, t, 1, color);
				FillRoundRect(x + a, y + a + k * d, t, d, 1, color);
				FillRoundRect(x + b - t, y + a + k * d, t, d, 1, color);
			}
			break;
		}
		case Icon::Bars:
			for (int i = 0; i < 3; i++)
			{
				const float hgt = s * (0.3f + 0.25f * i);
				FillRoundRect(x + int(s * (0.12f + 0.28f * i)), y + int(s * 0.88f - hgt), int(s * 0.2f), int(hgt), 2, color);
			}
			break;
		case Icon::Screen:
			StrokeRoundRect(x + int(s * 0.06f), y + int(s * 0.2f), int(s * 0.88f), int(s * 0.6f), int(s * 0.08f),
				std::max(2.0f, s * 0.09f), color);
			break;
		case Icon::Gauge:
		{
			const float r = s * 0.38f;
			StrokeArc(cx, cy + s * 0.08f, r, s * 0.1f, 150.0f, 390.0f, color);
			const float a = 300.0f / 57.29578f;
			const float ex = cx + r * 0.8f * std::cos(a), ey = cy + s * 0.08f + r * 0.8f * std::sin(a);
			const float nx = -std::sin(a) * s * 0.05f, ny = std::cos(a) * s * 0.05f;
			FillTriangle(cx + nx, cy + s * 0.08f + ny, cx - nx, cy + s * 0.08f - ny, ex, ey, color);
			FillCircle(cx, cy + s * 0.08f, s * 0.08f, color);
			break;
		}
		case Icon::Download:
			FillRoundRect(int(cx - s * 0.07f), y + int(s * 0.1f), int(s * 0.14f), int(s * 0.4f), 1, color);
			FillTriangle(cx - s * 0.25f, y + s * 0.45f, cx + s * 0.25f, y + s * 0.45f, cx, y + s * 0.72f, color);
			FillRoundRect(x + int(s * 0.12f), y + int(s * 0.8f), int(s * 0.76f), std::max(2, int(s * 0.09f)), 1, color);
			break;
		case Icon::Search:
		{
			// a magnifying glass: a ring at the upper left, its handle down to the lower right
			const float r = s * 0.25f, rx = x + s * 0.42f, ry = y + s * 0.42f;
			StrokeArc(rx, ry, r, s * 0.1f, 0.0f, 360.0f, color);
			const float d = 0.70710678f, hw = s * 0.07f;
			const float ax = rx + (r + s * 0.03f) * d, ay = ry + (r + s * 0.03f) * d;
			const float bx = x + s * 0.9f, by = y + s * 0.9f;
			FillTriangle(ax - hw * d, ay + hw * d, ax + hw * d, ay - hw * d, bx + hw * d, by - hw * d, color);
			FillTriangle(ax - hw * d, ay + hw * d, bx + hw * d, by - hw * d, bx - hw * d, by + hw * d, color);
			break;
		}
	}
}
} // namespace gfx
} // namespace fe
