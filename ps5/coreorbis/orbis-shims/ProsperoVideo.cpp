// Mupen64Plus PS5: the picture, through libSceVideoOut.
//
// The scan-out setup and the tile swizzle are the ones of the ps5-payload-dev SDK's SDL2 port
// (src/video/ps5/SDL_ps5video.c and SDL_ps5tilemap.c, (C) 2026 John Tornblom, zlib licence), which a
// payload ELF can use without any GPU driver:
//   - sceVideoOutOpen(255, 0, 0, NULL) on the system user, a 64 MiB direct-memory block split in two
//     scan-out buffers, sceVideoOutSetBufferAttribute2(A8B8G8R8, 1920x1080), RegisterBuffers2;
//   - a flip event queue to know when a buffer is on screen;
//   - the buffers are tiled (64 KiB tiles of 512x128 pixels, with an XOR swizzle inside), so every frame
//     the changed rectangle of a linear surface is swizzled in with AVX2 streaming stores.
// Each scan-out buffer remembers what changed since it was last written ("pending"), so with two buffers
// only the dirty rectangle has to be copied, never the whole screen.
//
// SPDX-License-Identifier: MIT

#include "ProsperoVideo.h"

#include "OrbisPaths.h"
#include "ProsperoSce.h"

#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps5video
{
namespace
{
constexpr uint32_t kTileW = 512;
constexpr uint32_t kTileH = 128;
constexpr uint32_t kTileSize = kTileW * kTileH;
constexpr uint32_t kBand = 8;
constexpr size_t k2M = 0x200000;

struct R
{
	int x = 0, y = 0, w = 0, h = 0;
	bool Empty() const { return w <= 0 || h <= 0; }
};

void Union(R& acc, const R& r)
{
	if (r.Empty())
		return;
	if (acc.Empty())
	{
		acc = r;
		return;
	}
	const int x0 = std::min(acc.x, r.x), y0 = std::min(acc.y, r.y);
	const int x1 = std::max(acc.x + acc.w, r.x + r.w), y1 = std::max(acc.y + acc.h, r.y + r.h);
	acc = {x0, y0, x1 - x0, y1 - y0};
}

bool Intersect(R& r, const R& clip)
{
	const int x0 = std::max(r.x, clip.x), y0 = std::max(r.y, clip.y);
	const int x1 = std::min(r.x + r.w, clip.x + clip.w), y1 = std::min(r.y + r.h, clip.y + clip.h);
	r = {x0, y0, x1 - x0, y1 - y0};
	return !r.Empty();
}

uint32_t TileOffset(uint32_t x, uint32_t y)
{
	return (((x)&1u) << 0 | ((x >> 1) & 1u) << 1 | ((y >> 0) & 1u) << 2 | ((y >> 1) & 1u) << 3 |
			((y >> 2) & 1u) << 4 | ((x >> 2) & 1u) << 5 | (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
			(((x >> 4) ^ (y >> 4)) & 1u) << 7 | (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
			(((x >> 5) ^ (y >> 6)) & 1u) << 9 | ((y >> 3) & 1u) << 10 | ((x >> 4) & 1u) << 11 |
			((y >> 6) & 1u) << 12 | ((x >> 6) & 1u) << 13 | ((x >> 7) & 1u) << 14 | ((x >> 8) & 1u) << 15);
}

uint32_t TilePixel(uint32_t x, uint32_t y, uint32_t width)
{
	return (x / kTileW) * kTileSize + (y / kTileH) * (kTileH * width) +
		   (TileOffset(x % kTileW, 0) ^ TileOffset(0, y % kTileH));
}

size_t TiledBufferSize(uint32_t width, uint32_t height)
{
	const uint32_t last_band = ((height - 1) / kTileH) * (kTileH * width);
	const uint32_t last_tile = ((width - 1) / kTileW) * kTileSize;
	return size_t(last_band + last_tile + kTileSize) * sizeof(uint32_t);
}

struct State
{
	bool ok = false;
	int handle = -1;
	SceKernelEqueue flip_queue = nullptr;
	intptr_t phys = 0; // direct memory (0 when the buffers are flexible memory)
	void* vmem = nullptr;
	size_t vmem_len = 0;
	int out_w = kWidth, out_h = kHeight; // scan-out size: 1920x1080, or 1280x720 when memory is short
	std::vector<uint32_t> scaled; // the surface scaled to out_w x out_h (720p only)
	std::vector<uint32_t> xmap, ymap; // out pixel -> surface pixel
	SceVideoOutBuffers vbuf[2] = {};
	uint32_t* surface = nullptr;
	std::vector<uint32_t> colx; // tiled offset of every 4th column
	uint32_t yoff[kTileH] = {};
	R pending[2];
	bool full[2] = {true, true};
	uint32_t frame = 0;
	int flips_in_flight = 0;
};
State g;

__attribute__((target("avx2"))) void TileArea(const uint32_t* src, uint32_t pitch, uint32_t* dst, uint32_t x0,
	uint32_t x1, uint32_t y0, uint32_t y1)
{
	const uint32_t* colx = g.colx.data();
	const uint32_t width = g.out_w;
	const uint32_t quads = x1 / 4;

	for (uint32_t y = y0; y < y1; y += kBand)
	{
		const uint32_t base = (y / kTileH) * (kTileH * width);
		const uint32_t yo = g.yoff[y % kTileH];
		const uint32_t* s = src + size_t(y) * pitch;
		uint32_t* d = dst + base;
		const uint32_t rows = std::min(kBand, y1 - y);

		for (uint32_t q = x0 / 4; q < quads; q++)
		{
			const uint32_t* p = s + (q << 2);
			uint32_t* o = d + (colx[q] ^ yo);
			uint32_t k = 0;
			for (; k + 1 < rows; k += 2)
			{
				const __m256i v = _mm256_inserti128_si256(
					_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))),
					_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + pitch)), 1);
				_mm256_stream_si256(reinterpret_cast<__m256i*>(o), v);
				p += 2 * pitch;
				o += 8;
			}
			if (rows & 1)
				_mm_stream_si128(reinterpret_cast<__m128i*>(o), _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)));
		}
		for (uint32_t k = 0; k < rows; k++)
		{
			const uint32_t* p = s + size_t(k) * pitch;
			for (uint32_t x = quads * 4; x < x1; x++)
				dst[TilePixel(x, y + k, width)] = p[x];
		}
	}
	_mm_sfence();
}

void TileBlit(int buf_idx, R damage)
{
	const R screen = {0, 0, g.out_w, g.out_h};
	R area;
	if (g.full[buf_idx])
		area = screen;
	else
	{
		area = g.pending[buf_idx];
		Union(area, damage);
	}
	g.full[buf_idx] = false;
	g.pending[buf_idx] = R();
	Union(g.pending[buf_idx ^ 1], damage); // the other buffer still shows the old picture there

	if (area.Empty())
		return;

	area.w += area.x & 3;
	area.x &= ~3;
	area.w = std::min((area.w + 3) & ~3, g.out_w - area.x);
	area.h += area.y & (kBand - 1);
	area.y &= ~(kBand - 1);
	area.h = std::min(int((area.h + kBand - 1) & ~(kBand - 1)), g.out_h - area.y);

	if (g.out_w != kWidth)
	{
		// 720p scan-out: nearest-neighbour downscale of the (aligned) area first
		for (int y = area.y; y < area.y + area.h; y++)
		{
			const uint32_t* src = g.surface + size_t(g.ymap[y]) * kWidth;
			uint32_t* dst = g.scaled.data() + size_t(y) * g.out_w;
			for (int x = area.x; x < area.x + area.w; x++)
				dst[x] = src[g.xmap[x]];
		}
	}
	const uint32_t* src = g.out_w != kWidth ? g.scaled.data() : g.surface;
	TileArea(src, g.out_w, static_cast<uint32_t*>(g.vbuf[buf_idx].data), area.x, area.x + area.w, area.y,
		area.y + area.h);
}

void WaitOneFlip()
{
	alignas(16) uint8_t ev[128]; // one struct kevent (64 bytes on FreeBSD 12+)
	int out = 0;
	const int r = sceKernelWaitEqueue(g.flip_queue, ev, 1, &out, nullptr);
	if (r != 0)
		OrbisLog("[video] sceKernelWaitEqueue -> %x", r);
	if (g.flips_in_flight > 0)
		g.flips_in_flight--;
}

// ---- scan-out memory --------------------------------------------------------------------------------
// The SDK's SDL port asks for 64 MiB of main direct memory. A payload process can get much less than that
// (the first console logs: sceKernelAllocateMainDirectMemory -> 0x80020023, EAGAIN), so ask only for what
// two buffers need, then try other ways to get it, then 720p.
void FreeScanout()
{
	if (g.phys)
		sceKernelReleaseDirectMemory(g.phys, g.vmem_len); // also unmaps
	else if (g.vmem)
		munmap(g.vmem, g.vmem_len);
	g.phys = 0;
	g.vmem = nullptr;
	g.vmem_len = 0;
}

bool TryRegister(int w, int h)
{
	memset(g.vmem, 0, g.vmem_len);
	g.vbuf[0].data = g.vmem;
	g.vbuf[1].data = static_cast<uint8_t*>(g.vmem) + g.vmem_len / 2;
	SceVideoOutBufferAttribute2 attr;
	memset(&attr, 0, sizeof(attr));
	sceVideoOutSetBufferAttribute2(&attr, SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, 0, w, h, 0, 0, 0);
	const int r = sceVideoOutRegisterBuffers2(g.handle, 0, 0, g.vbuf, 2, &attr, 0, nullptr);
	OrbisLog("[video]   sceVideoOutRegisterBuffers2(%dx%d) -> %x", w, h, r);
	return r == 0;
}

bool TryDirect(const char* how, int r, size_t len, size_t align, int w, int h)
{
	OrbisLog("[video]   %s(%zu MiB) -> %x phys=%lx", how, len >> 20, r, long(g.phys));
	if (r != 0)
	{
		g.phys = 0;
		return false;
	}
	g.vmem_len = len;
	g.vmem = nullptr;
	r = sceKernelMapDirectMemory(&g.vmem, len, 0x33, 0, g.phys, align);
	OrbisLog("[video]   sceKernelMapDirectMemory -> %x at %p", r, g.vmem);
	if (r != 0)
	{
		sceKernelReleaseDirectMemory(g.phys, len);
		g.phys = 0;
		g.vmem = nullptr;
		return false;
	}
	if (TryRegister(w, h))
		return true;
	FreeScanout();
	return false;
}

bool AllocScanout(int w, int h)
{
	const size_t each = (TiledBufferSize(w, h) + k2M - 1) & ~(k2M - 1);
	const size_t len = each * 2;
	const int64_t dsize = int64_t(sceKernelGetDirectMemorySize());
	int64_t avail_start = 0;
	size_t avail = 0;
	const int ar = sceKernelAvailableDirectMemorySize(0, dsize, 0x10000, &avail_start, &avail);
	size_t flex = 0;
	sceKernelAvailableFlexibleMemorySize(&flex);
	OrbisLog("[video] %dx%d needs %zu MiB; direct memory %lld MiB, largest free %zu MiB (%x), flexible free %zu MiB",
		w, h, len >> 20, (long long)(dsize >> 20), avail >> 20, ar, flex >> 20);

	static const int types[] = {3, 0};
	for (int type : types)
	{
		int r = sceKernelAllocateMainDirectMemory(len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateMainDirectMemory type 3" : "AllocateMainDirectMemory type 0", r, len,
				k2M, w, h))
			return true;
		r = sceKernelAllocateDirectMemory(0, dsize, len, k2M, type, &g.phys);
		if (TryDirect(type == 3 ? "AllocateDirectMemory type 3" : "AllocateDirectMemory type 0", r, len, k2M, w,
				h))
			return true;
	}
	// last resort: flexible memory mapped for the GPU (not known to be accepted by the display yet)
	void* addr = nullptr;
	const int r = sceKernelMapNamedFlexibleMemory(&addr, len, 0x33, 0, "n64 scanout");
	OrbisLog("[video]   MapNamedFlexibleMemory(%zu MiB) -> %x at %p", len >> 20, r, addr);
	if (r == 0 && addr)
	{
		g.phys = 0;
		g.vmem = addr;
		g.vmem_len = len;
		if (TryRegister(w, h))
			return true;
		FreeScanout();
	}
	return false;
}

void SetOutputSize(int w, int h)
{
	g.out_w = w;
	g.out_h = h;
	g.colx.assign(w / 4 + 1, 0);
	for (uint32_t x = 0; x < uint32_t(w); x += 4)
		g.colx[x / 4] = (x / kTileW) * kTileSize + TileOffset(x % kTileW, 0);
	if (w != kWidth)
	{
		g.scaled.assign(size_t(w) * h, Rgb(0, 0, 0));
		g.xmap.resize(w);
		g.ymap.resize(h);
		for (int x = 0; x < w; x++)
			g.xmap[x] = uint32_t((int64_t(x) * kWidth + kWidth / 2) / w);
		for (int y = 0; y < h; y++)
			g.ymap[y] = uint32_t((int64_t(y) * kHeight + kHeight / 2) / h);
	}
	else
	{
		g.scaled.clear();
		g.xmap.clear();
		g.ymap.clear();
	}
	g.full[0] = g.full[1] = true;
}
} // namespace

bool Init()
{
	if (g.ok)
		return true;

	g.surface = static_cast<uint32_t*>(aligned_alloc(64, size_t(kWidth) * kHeight * 4));
	if (!g.surface)
	{
		OrbisLog("[video] out of memory for the surface");
		return false;
	}
	std::fill(g.surface, g.surface + size_t(kWidth) * kHeight, Rgb(0, 0, 0));
	for (uint32_t y = 0; y < kTileH; y++)
		g.yoff[y] = TileOffset(0, y);

	// 0x80290009: the screen is still held by another process (the 1.5 console log: a second copy started
	// while the first still ran). Give it a few seconds to let go.
	for (int attempt = 0;; attempt++)
	{
		g.handle = sceVideoOutOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, 0, 0, nullptr);
		OrbisLog("[video] sceVideoOutOpen -> %d (%x)", g.handle, unsigned(g.handle));
		if (g.handle >= 0 || unsigned(g.handle) != 0x80290009u || attempt >= 10)
			break;
		usleep(500 * 1000);
	}
	if (g.handle < 0)
		return false;

	int r = sceKernelCreateEqueue(&g.flip_queue, "n64 flip");
	if (r != 0)
	{
		OrbisLog("[video] sceKernelCreateEqueue -> %x", r);
		return false;
	}
	r = sceVideoOutAddFlipEvent(g.flip_queue, g.handle, nullptr);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutAddFlipEvent -> %x", r);
		return false;
	}
	sceVideoOutSetFlipRate(g.handle, 0); // 60 Hz

	// When started from the dashboard icon, the launcher app still holds the game memory for a moment
	// after it hands us over: keep trying for a few seconds before giving up.
	bool ok = false;
	for (int attempt = 0; attempt < 12 && !ok; attempt++)
	{
		if (attempt > 0)
		{
			OrbisLog("[video] no scan-out memory yet, retrying (%d)", attempt);
			usleep(500000);
		}
		static const int sizes[][2] = {{kWidth, kHeight}, {1280, 720}};
		for (const auto& sz : sizes)
		{
			if (AllocScanout(sz[0], sz[1]))
			{
				SetOutputSize(sz[0], sz[1]);
				ok = true;
				break;
			}
		}
	}
	if (!ok)
	{
		OrbisLog("[video] could not get memory for the scan-out buffers");
		return false;
	}
	OrbisLog("[video] scan-out %dx%d, %zu MiB of %s memory", g.out_w, g.out_h, g.vmem_len >> 20,
		g.phys ? "direct" : "flexible");

	g.ok = true;
	Present(0, 0, 0, 0, true); // black screen over the home screen
	return true;
}

void Shutdown()
{
	if (g.handle >= 0)
	{
		while (g.flips_in_flight > 0)
			WaitOneFlip();
		if (g.flip_queue)
			sceVideoOutDeleteFlipEvent(g.flip_queue, g.handle);
		sceVideoOutClose(g.handle);
		g.handle = -1;
	}
	if (g.flip_queue)
	{
		sceKernelDeleteEqueue(g.flip_queue);
		g.flip_queue = nullptr;
	}
	FreeScanout();
	free(g.surface);
	g.surface = nullptr;
	g.ok = false;
}

uint32_t* Surface()
{
	return g.surface;
}

void Present(int x, int y, int w, int h, bool wait_vsync)
{
	if (!g.ok)
		return;
	// The buffer about to be written may still be on screen until its last flip completes.
	while (g.flips_in_flight > 0)
		WaitOneFlip();

	R damage = (w <= 0 || h <= 0) ? R{0, 0, kWidth, kHeight} : R{x, y, w, h};
	Intersect(damage, R{0, 0, kWidth, kHeight});

	const int idx = g.frame & 1;
	TileBlit(idx, damage);

	const int r = sceVideoOutSubmitFlip(g.handle, idx, SCE_VIDEO_OUT_FLIP_MODE_VSYNC, g.frame);
	if (r != 0)
	{
		OrbisLog("[video] sceVideoOutSubmitFlip -> %x", r);
		return;
	}
	g.flips_in_flight++;
	g.frame++;
	if (wait_vsync)
		WaitOneFlip();
}

void FillRect(int x, int y, int w, int h, uint32_t color)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
		std::fill(g.surface + size_t(j) * kWidth + r.x, g.surface + size_t(j) * kWidth + r.x + r.w, color);
}

void DarkenRect(int x, int y, int w, int h)
{
	R r{x, y, w, h};
	if (!g.surface || !Intersect(r, R{0, 0, kWidth, kHeight}))
		return;
	for (int j = r.y; j < r.y + r.h; j++)
	{
		uint32_t* p = g.surface + size_t(j) * kWidth + r.x;
		for (int i = 0; i < r.w; i++)
			p[i] = ((p[i] >> 1) & 0x007f7f7fu) | 0xff000000u;
	}
}

// ---------------------------------------------------------------------------------------------------------
// The N64 picture
namespace
{
struct N64Geom
{
	int src_w = 0, src_h = 0;
	Aspect aspect = Aspect::Count;
	bool smooth = false;
	Rect dst = {0, 0, 0, 0};
	// nearest: dst column/row -> src column/row
	std::vector<uint16_t> xlut, ylut;
	// bilinear: dst column -> left src column and the weight of the right one (0..255); same for rows
	std::vector<uint16_t> bx0, by0;
	std::vector<uint16_t> bfx, bfy;
	// horizontally scaled source rows, in two slots by row parity (the rows above and below an output row)
	std::vector<uint32_t> hrow[2];
	int hrow_src[2] = {-1, -1};
};
N64Geom s_geom;

Rect ComputeRect(int src_w, int src_h, Aspect aspect)
{
	int dw = kWidth, dh = kHeight;
	switch (aspect)
	{
		case Aspect::Ratio4x3:
			dh = kHeight;
			dw = kHeight * 4 / 3;
			break;
		case Aspect::Integer:
		{
			const int k = std::max(1, std::min(kWidth / src_w, kHeight / src_h));
			dw = src_w * k;
			dh = src_h * k;
			break;
		}
		default:
			break;
	}
	dw = std::min(dw, int(kWidth)) & ~1;
	dh = std::min(dh, int(kHeight));
	return Rect{(kWidth - dw) / 2, (kHeight - dh) / 2, dw, dh};
}

// Source position of an output pixel's centre, in 1/256ths of a source pixel: (i + 0.5) * src / dst - 0.5.
void BilinearTap(int i, int src, int dst, uint16_t* first, uint16_t* frac)
{
	int64_t s = (int64_t(2 * i + 1) * src * 256) / (2 * dst) - 128;
	if (s < 0)
		s = 0;
	int p = int(s >> 8);
	int f = int(s & 255);
	if (p >= src - 1)
	{
		p = src - 1;
		f = 0;
	}
	*first = uint16_t(p);
	*frac = uint16_t(f);
}

void BuildLuts(int w, int h)
{
	const Rect& d = s_geom.dst;
	s_geom.xlut.resize(d.w);
	s_geom.bx0.resize(d.w);
	s_geom.bfx.resize(d.w);
	for (int x = 0; x < d.w; x++)
	{
		s_geom.xlut[x] = uint16_t((int64_t(x) * w) / d.w);
		BilinearTap(x, w, d.w, &s_geom.bx0[x], &s_geom.bfx[x]);
	}
	s_geom.ylut.resize(d.h);
	s_geom.by0.resize(d.h);
	s_geom.bfy.resize(d.h);
	for (int y = 0; y < d.h; y++)
	{
		s_geom.ylut[y] = uint16_t((int64_t(y) * h) / d.h);
		BilinearTap(y, h, d.h, &s_geom.by0[y], &s_geom.bfy[y]);
	}
	s_geom.hrow[0].assign(d.w, 0);
	s_geom.hrow[1].assign(d.w, 0);
	s_geom.hrow_src[0] = s_geom.hrow_src[1] = -1;
}

inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t f)
{
	const uint32_t inv = 256 - f;
	const uint32_t rb = (((a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * f) >> 8) & 0x00ff00ffu;
	const uint32_t g = (((a >> 8) & 0xffu) * inv + ((b >> 8) & 0xffu) * f) & 0x0000ff00u;
	return rb | g | 0xff000000u;
}

// One source row scaled horizontally into hrow[slot] (skipped when the slot already holds it).
void HScaleRow(const uint32_t* row, int slot, int src_y)
{
	if (s_geom.hrow_src[slot] == src_y)
		return;
	uint32_t* out = s_geom.hrow[slot].data();
	const uint16_t* x0 = s_geom.bx0.data();
	const uint16_t* fx = s_geom.bfx.data();
	const int n = s_geom.dst.w;
	for (int x = 0; x < n; x++)
	{
		const uint32_t a = row[x0[x]];
		const uint32_t f = fx[x];
		out[x] = f ? Lerp(a, row[x0[x] + 1], f) : (a | 0xff000000u);
	}
	s_geom.hrow_src[slot] = src_y;
}

// out = (top * (256 - f) + bottom * f) / 256 per channel, 8 pixels at a time
__attribute__((target("avx2"))) void VBlend(const uint32_t* top, const uint32_t* bot, uint32_t* out, int n, uint32_t f)
{
	const __m256i wf = _mm256_set1_epi16(int16_t(f));
	const __m256i wi = _mm256_set1_epi16(int16_t(256 - f));
	const __m256i zero = _mm256_setzero_si256();
	const __m256i alpha = _mm256_set1_epi32(int(0xff000000u));
	int x = 0;
	for (; x + 8 <= n; x += 8)
	{
		const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(top + x));
		const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bot + x));
		const __m256i alo = _mm256_unpacklo_epi8(a, zero), ahi = _mm256_unpackhi_epi8(a, zero);
		const __m256i blo = _mm256_unpacklo_epi8(b, zero), bhi = _mm256_unpackhi_epi8(b, zero);
		const __m256i lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(alo, wi), _mm256_mullo_epi16(blo, wf)), 8);
		const __m256i hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(ahi, wi), _mm256_mullo_epi16(bhi, wf)), 8);
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(out + x), _mm256_or_si256(_mm256_packus_epi16(lo, hi), alpha));
	}
	for (; x < n; x++)
		out[x] = Lerp(top[x], bot[x], f);
}
} // namespace

const char* AspectName(Aspect a)
{
	switch (a)
	{
		case Aspect::Ratio4x3: return "4:3";
		case Aspect::Integer: return "Integer scale";
		case Aspect::Stretch: return "Stretch 16:9";
		default: return "?";
	}
}

Rect LastN64Rect()
{
	return s_geom.dst;
}

void InvalidateN64()
{
	s_geom.src_w = 0;
}

Rect DrawN64(const uint32_t* src, int pitch, int w, int h, Aspect aspect, bool smooth)
{
	if (!g.surface || !src || w <= 0 || h <= 0)
		return Rect{0, 0, 0, 0};

	bool cleared = false;
	if (w != s_geom.src_w || h != s_geom.src_h || aspect != s_geom.aspect || smooth != s_geom.smooth)
	{
		s_geom.src_w = w;
		s_geom.src_h = h;
		s_geom.aspect = aspect;
		s_geom.smooth = smooth;
		s_geom.dst = ComputeRect(w, h, aspect);
		BuildLuts(w, h);
		FillRect(0, 0, kWidth, kHeight, Rgb(0, 0, 0));
		cleared = true;
	}
	// a new picture: the cached horizontal rows are stale
	s_geom.hrow_src[0] = s_geom.hrow_src[1] = -1;

	const Rect& d = s_geom.dst;
	if (!smooth)
	{
		const uint16_t* xl = s_geom.xlut.data();
		const uint32_t* prev_in = nullptr;
		uint32_t* prev_out = nullptr;
		for (int y = 0; y < d.h; y++)
		{
			uint32_t* out = g.surface + size_t(d.y + y) * kWidth + d.x;
			const uint32_t* in = src + size_t(s_geom.ylut[y]) * pitch;
			if (in == prev_in)
			{
				memcpy(out, prev_out, size_t(d.w) * 4);
				continue;
			}
			for (int x = 0; x < d.w; x++)
				out[x] = in[xl[x]] | 0xff000000u;
			prev_in = in;
			prev_out = out;
		}
	}
	else
	{
		for (int y = 0; y < d.h; y++)
		{
			uint32_t* out = g.surface + size_t(d.y + y) * kWidth + d.x;
			const int y0 = s_geom.by0[y];
			const uint32_t f = s_geom.bfy[y];
			const int s0 = y0 & 1;
			HScaleRow(src + size_t(y0) * pitch, s0, y0);
			if (f == 0)
			{
				memcpy(out, s_geom.hrow[s0].data(), size_t(d.w) * 4);
				continue;
			}
			const int y1 = std::min(y0 + 1, h - 1);
			const int s1 = y1 & 1;
			HScaleRow(src + size_t(y1) * pitch, s1, y1);
			VBlend(s_geom.hrow[s0].data(), s_geom.hrow[s1].data(), out, d.w, f);
		}
	}
	return cleared ? Rect{0, 0, kWidth, kHeight} : d;
}
} // namespace ps5video
