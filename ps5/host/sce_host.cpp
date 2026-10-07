// Mupen64Plus PS5 host build: the PS5 system calls of include-orbis/ProsperoSce.h, implemented on Linux so the
// whole port (main-boot, shims, frontend, core) runs unchanged in the tests.
//
//   N64PS5_HOST_PAD="frame:hexbuttons[;frame:hexbuttons...]"  pad 1 script (by flip count)
//   N64PS5_HOST_DUMP="frame[,frame...]"                       flips to save as PPM (de-tiled)
//   N64PS5_HOST_DUMP_DIR=dir                                  where (default .)
//   N64PS5_HOST_MAX_FLIPS=n                                   abort after n flips (a hang guard)
//   N64PS5_HOST_REALTIME=1                                    flips wait 1/60 s
//
// SPDX-License-Identifier: MIT

#include "ProsperoSce.h"

#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
SceVideoOutBuffers g_bufs[2];
int g_w = 0, g_h = 0;
std::atomic<uint64_t> g_flips{0};
std::atomic<uint64_t> g_audio_frames{0};
std::map<uint64_t, uint32_t> g_script;
std::set<uint64_t> g_dumps;
bool g_inited = false;

void InitEnv()
{
	if (g_inited)
		return;
	g_inited = true;
	if (const char* s = getenv("N64PS5_HOST_PAD"))
	{
		std::string str = s;
		size_t pos = 0;
		while (pos < str.size())
		{
			size_t end = str.find(';', pos);
			if (end == std::string::npos)
				end = str.size();
			const std::string item = str.substr(pos, end - pos);
			const size_t colon = item.find(':');
			if (colon != std::string::npos)
				g_script[strtoull(item.substr(0, colon).c_str(), nullptr, 10)] =
					uint32_t(strtoul(item.substr(colon + 1).c_str(), nullptr, 16));
			pos = end + 1;
		}
	}
	if (const char* s = getenv("N64PS5_HOST_DUMP"))
	{
		std::string str = s;
		size_t pos = 0;
		while (pos < str.size())
		{
			size_t end = str.find(',', pos);
			if (end == std::string::npos)
				end = str.size();
			g_dumps.insert(strtoull(str.substr(pos, end - pos).c_str(), nullptr, 10));
			pos = end + 1;
		}
	}
}

// The same swizzle as ProsperoVideo.cpp, written out again here as the reference.
uint32_t TileOffset(uint32_t x, uint32_t y)
{
	return (((x)&1u) << 0 | ((x >> 1) & 1u) << 1 | ((y >> 0) & 1u) << 2 | ((y >> 1) & 1u) << 3 |
			((y >> 2) & 1u) << 4 | ((x >> 2) & 1u) << 5 | (((x >> 3) ^ (y >> 3)) & 1u) << 6 |
			(((x >> 4) ^ (y >> 4)) & 1u) << 7 | (((x >> 6) ^ (y >> 5)) & 1u) << 8 |
			(((x >> 5) ^ (y >> 6)) & 1u) << 9 | ((y >> 3) & 1u) << 10 | ((x >> 4) & 1u) << 11 |
			((y >> 6) & 1u) << 12 | ((x >> 6) & 1u) << 13 | ((x >> 7) & 1u) << 14 | ((x >> 8) & 1u) << 15);
}

void Dump(int idx, uint64_t n)
{
	const char* dir = getenv("N64PS5_HOST_DUMP_DIR");
	char path[512];
	snprintf(path, sizeof(path), "%s/flip%05llu.ppm", dir ? dir : ".", (unsigned long long)n);
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
	const uint32_t* t = static_cast<const uint32_t*>(g_bufs[idx].data);
	std::vector<uint8_t> row(size_t(g_w) * 3);
	for (int y = 0; y < g_h; y++)
	{
		for (int x = 0; x < g_w; x++)
		{
			const uint32_t off = (x / 512) * 65536 + (y / 128) * (128 * g_w) + (TileOffset(x % 512, 0) ^ TileOffset(0, y % 128));
			const uint32_t p = t[off];
			row[x * 3] = p & 0xff;
			row[x * 3 + 1] = (p >> 8) & 0xff;
			row[x * 3 + 2] = (p >> 16) & 0xff;
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
	fprintf(stderr, "[host] dumped %s\n", path);
}
} // namespace

extern "C" {
// N64PS5_HOST_DIRECT_MAX_MIB=n: bigger direct allocations fail with EAGAIN, like a payload with little memory
// N64PS5_HOST_DIRECT_FAIL_FIRST=n: the first n direct allocations fail (the launcher app still holds memory)
static int DirectFails(size_t len)
{
	static int calls = 0;
	calls++;
	if (const char* f = getenv("N64PS5_HOST_DIRECT_FAIL_FIRST"))
		if (calls <= atoi(f))
			return 1;
	if (const char* m = getenv("N64PS5_HOST_DIRECT_MAX_MIB"))
		if (len > size_t(atoi(m)) << 20)
			return 1;
	return 0;
}
int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t len, size_t align, int type, intptr_t* phys_out)
{
	return sceKernelAllocateMainDirectMemory(len, align, type, phys_out);
}
size_t sceKernelGetDirectMemorySize(void)
{
	return size_t(5) << 30;
}
int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t* start_out, size_t* size_out)
{
	*start_out = 0;
	*size_out = size_t(64) << 20;
	return 0;
}
int sceKernelMapNamedFlexibleMemory(void**, size_t, int, int, const char*)
{
	return (int)0x8002000c; // ENOMEM: the host never hands out flexible memory, so that path stays untested here
}
int sceKernelAvailableFlexibleMemorySize(size_t* size)
{
	*size = 0;
	return 0;
}
int sceKernelAllocateMainDirectMemory(size_t len, size_t, int, intptr_t* phys_out)
{
	if (DirectFails(len))
		return (int)0x80020023;
	void* p = aligned_alloc(0x20000, len);
	*phys_out = reinterpret_cast<intptr_t>(p);
	return p ? 0 : -1;
}
int sceKernelMapDirectMemory(void** addr, size_t, int, int, intptr_t phys, size_t)
{
	*addr = reinterpret_cast<void*>(phys);
	return 0;
}
int sceKernelReleaseDirectMemory(intptr_t phys, size_t)
{
	free(reinterpret_cast<void*>(phys));
	return 0;
}
int sceKernelCreateEqueue(SceKernelEqueue* eq, const char*)
{
	*eq = reinterpret_cast<SceKernelEqueue>(0x1);
	return 0;
}
int sceKernelWaitEqueue(SceKernelEqueue, void*, int, int* out, unsigned int*)
{
	if (getenv("N64PS5_HOST_REALTIME"))
	{
		// a flip completes at the next 60 Hz vblank, like on the console
		static const auto t0 = std::chrono::steady_clock::now();
		const auto period = std::chrono::microseconds(16667);
		const auto since = std::chrono::steady_clock::now() - t0;
		const auto next = t0 + (since / period + 1) * period;
		std::this_thread::sleep_until(next);
	}
	if (out)
		*out = 1;
	return 0;
}
int sceKernelDeleteEqueue(SceKernelEqueue)
{
	return 0;
}
int sceKernelSendNotificationRequest(int, void* request, size_t, int)
{
	fprintf(stderr, "[host] notification: %s\n", static_cast<const char*>(request) + 0x2d);
	return 0;
}
int sceKernelUsleep(unsigned int usec)
{
	return usleep(usec);
}
int sceSystemServiceHideSplashScreen(void)
{
	fprintf(stderr, "[host] splash screen hidden\n");
	return 0;
}
int sceSystemServiceLoadExec(const char* path, const char**)
{
	// the host has no system to close the app: the caller falls back to _exit()
	fprintf(stderr, "[host] sceSystemServiceLoadExec(%s)\n", path ? path : "(null)");
	return -1;
}

// N64PS5_HOST_VIDEO_BUSY=N: the first N opens find the screen held by another process (0x80290009)
int sceVideoOutOpen(int, int, int, const void*)
{
	InitEnv();
	static int busy = getenv("N64PS5_HOST_VIDEO_BUSY") ? atoi(getenv("N64PS5_HOST_VIDEO_BUSY")) : 0;
	if (busy > 0)
	{
		busy--;
		return (int)0x80290009;
	}
	return 1;
}
int sceVideoOutClose(int)
{
	fprintf(stderr, "[host] video closed after %llu flips\n", (unsigned long long)g_flips.load());
	return 0;
}
int sceVideoOutAddFlipEvent(SceKernelEqueue, int, void*)
{
	return 0;
}
int sceVideoOutDeleteFlipEvent(SceKernelEqueue, int)
{
	return 0;
}
int sceVideoOutSetFlipRate(int, int)
{
	return 0;
}
void sceVideoOutSetBufferAttribute2(SceVideoOutBufferAttribute2* attr, uint64_t, uint32_t, uint32_t w, uint32_t h,
	uint64_t, uint32_t, uint64_t)
{
	memcpy(attr->junk0, &w, 4);
	memcpy(attr->junk0 + 4, &h, 4);
}
int sceVideoOutRegisterBuffers2(int, int, int, SceVideoOutBuffers* buffers, int count, SceVideoOutBufferAttribute2* attr,
	int, void*)
{
	for (int i = 0; i < count && i < 2; i++)
		g_bufs[i] = buffers[i];
	memcpy(&g_w, attr->junk0, 4);
	memcpy(&g_h, attr->junk0 + 4, 4);
	return 0;
}
int sceVideoOutSubmitFlip(int, int idx, uint32_t, int64_t)
{
	const uint64_t n = g_flips.fetch_add(1);
	if (g_dumps.count(n))
		Dump(idx, n);
	if (const char* m = getenv("N64PS5_HOST_MAX_FLIPS"))
		if (n >= strtoull(m, nullptr, 10))
		{
			fprintf(stderr, "[host] N64PS5_HOST_MAX_FLIPS reached: abort\n");
			_exit(3);
		}
	return 0;
}

int32_t sceAudioOutInit(void)
{
	return 0;
}
int32_t sceAudioOutOpen(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t)
{
	return 7;
}
int32_t sceAudioOutOutput(int32_t, const void* p)
{
	// play at the real rate so the audio thread paces like on the console: against an absolute clock (a plain
	// sleep per grain drifts 1-2% slow, more than the audio plugin's rate control may correct)
	static auto next = std::chrono::steady_clock::now();
	next += std::chrono::microseconds(256 * 1000000 / 48000) + std::chrono::nanoseconds(256 * 1000000000LL / 48000 % 1000);
	const auto now = std::chrono::steady_clock::now();
	if (next < now - std::chrono::milliseconds(50))
		next = now; // fell far behind (the process was stopped): start again
	std::this_thread::sleep_until(next);
	const int16_t* s = static_cast<const int16_t*>(p);
	static uint64_t nonzero = 0;
	for (int i = 0; i < 512; i++)
		nonzero += s[i] != 0;
	g_audio_frames += 256;
	if (getenv("N64PS5_HOST_AUDIO_STATS") && (g_audio_frames % (48000 * 2)) < 256)
		fprintf(stderr, "[host] audio: %llu frames played, %llu non-zero samples\n",
			(unsigned long long)g_audio_frames.load(), (unsigned long long)nonzero);
	return 0;
}
int32_t sceAudioOutClose(int32_t)
{
	return 0;
}

int scePadInit(void)
{
	return 0;
}
// N64PS5_HOST_PAD_SHARED=1: what the first console log showed -- the system already has the pad open
// (scePadOpen: already opened) and scePadGetHandle hands out 0x809b0081, negative as an int.
static bool PadShared()
{
	const char* e = getenv("N64PS5_HOST_PAD_SHARED");
	return e && *e && strcmp(e, "0") != 0;
}
int scePadOpen(int, int, int, void*)
{
	return PadShared() ? SCE_PAD_ERROR_ALREADY_OPENED : 0x100;
}
int scePadGetHandle(int, int, int)
{
	return PadShared() ? (int)0x809b0081 : 0x100;
}
int scePadReadState(int handle, ScePadData* data)
{
	InitEnv();
	if (handle != (PadShared() ? (int)0x809b0081 : 0x100))
		return (int)0x80920003; // invalid handle
	memset(data, 0, sizeof(*data));
	data->connected = 1;
	data->leftStick.x = data->leftStick.y = 128;
	data->rightStick.x = data->rightStick.y = 128;
	const uint64_t now = g_flips.load();
	auto it = g_script.upper_bound(now);
	if (it != g_script.begin())
		data->buttons = std::prev(it)->second;
	return 0;
}
int scePadSetLightBar(int, const ScePadColor*)
{
	return 0;
}
int scePadSetVibration(int, const ScePadVibrationParam*)
{
	return 0;
}
int scePadSetVibrationMode(int, int)
{
	return 0;
}
int scePadClose(int)
{
	return 0;
}

int sceUserServiceInitialize(void*)
{
	return 0;
}
int sceUserServiceGetForegroundUser(int32_t* user)
{
	*user = 0x10000001;
	return 0;
}
int sceUserServiceGetLoginUserIdList(int32_t users[4])
{
	users[0] = 0x10000001;
	users[1] = users[2] = users[3] = -1;
	return 0;
}

// ---- network: libSceNetCtl says "connected" (N64PS5_HOST_OFFLINE=1: not), libSceHttp2 runs curl ----
int sceNetInit(void) { return 0; }
int sceNetPoolCreate(const char*, int, int) { return 1; }
int sceNetPoolDestroy(int) { return 0; }
int sceNetCtlInit(void) { return 0; }
void sceNetCtlTerm(void) {}
int sceNetCtlGetState(int* state)
{
	const char* o = getenv("N64PS5_HOST_OFFLINE");
	*state = (o && *o && strcmp(o, "0") != 0) ? 0 : 3;
	return 0;
}
int sceSslInit(size_t) { return 2; }
int sceSslTerm(int) { return 0; }
int sceHttp2Init(int, int, size_t, int) { return 3; }
int sceHttp2Term(int) { return 0; }
int sceHttp2CreateTemplate(int, const char*, int, int) { return 4; }
int sceHttp2DeleteTemplate(int) { return 0; }
}

namespace
{
struct HostReq
{
	std::string url;
	std::vector<uint8_t> body;
	size_t off = 0;
	int status = -1;
};
std::mutex g_req_lock;
std::map<int, HostReq> g_reqs;
int g_next_req = 100;
std::atomic<int> g_http_count{0};
}

extern "C" {
int sceHttp2CreateRequestWithURL(int, const char*, const char* url, uint64_t)
{
	std::lock_guard<std::mutex> lock(g_req_lock);
	const int id = g_next_req++;
	g_reqs[id].url = url;
	return id;
}
int sceHttp2DeleteRequest(int req)
{
	std::lock_guard<std::mutex> lock(g_req_lock);
	g_reqs.erase(req);
	return 0;
}
int sceHttp2SendRequest(int req, const void*, size_t)
{
	std::string url;
	{
		std::lock_guard<std::mutex> lock(g_req_lock);
		url = g_reqs[req].url;
	}
	g_http_count++;
	char tmp[] = "/tmp/n64ps5-http-XXXXXX";
	const int fd = mkstemp(tmp);
	if (fd < 0)
		return -1;
	close(fd);
	std::string q;
	for (char c : url)
		q += c == '\'' ? std::string("'\\''") : std::string(1, c);
	const std::string cmd = "curl -s -L --max-time 20 -o " + std::string(tmp) + " -w '%{http_code}' '" + q + "'";
	FILE* p = popen(cmd.c_str(), "r");
	int status = -1;
	if (p)
	{
		char buf[32] = {};
		if (fgets(buf, sizeof(buf), p))
			status = atoi(buf);
		pclose(p);
	}
	std::vector<uint8_t> body;
	if (FILE* f = fopen(tmp, "rb"))
	{
		uint8_t b[65536];
		size_t n;
		while ((n = fread(b, 1, sizeof(b), f)) > 0)
			body.insert(body.end(), b, b + n);
		fclose(f);
	}
	unlink(tmp);
	std::lock_guard<std::mutex> lock(g_req_lock);
	g_reqs[req].status = status <= 0 ? -1 : status;
	g_reqs[req].body.swap(body);
	return status <= 0 ? (int)0x80000000 : 0;
}
int sceHttp2GetStatusCode(int req, int* status)
{
	std::lock_guard<std::mutex> lock(g_req_lock);
	*status = g_reqs[req].status;
	return 0;
}
int sceHttp2ReadData(int req, void* data, size_t size)
{
	std::lock_guard<std::mutex> lock(g_req_lock);
	HostReq& r = g_reqs[req];
	const size_t n = std::min(size, r.body.size() - r.off);
	memcpy(data, r.body.data() + r.off, n);
	r.off += n;
	return int(n);
}
int sceHttp2SetResolveTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetConnectTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetSendTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetRecvTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetAutoRedirect(int, int) { return 0; }
int sceHttp2AbortRequest(int) { return 0; }
}
