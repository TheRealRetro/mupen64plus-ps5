// Mupen64Plus PS5: the PS5 system calls the port uses (libkernel, libSceVideoOut, libSceAudioOut, libScePad,
// libSceUserService, libSceSystemService). The ps5-payload-dev SDK ships the import stubs (target/lib/*.so)
// but no headers for them, so -- like PS5SX2's orbis-shims -- the prototypes are declared here by hand.
// The layouts come from the SDK's SDL2 port (ps5-payload-dev/SDL, src/{video,joystick,audio}/ps5).
//
// On the host build (ps5/host/) the same prototypes are implemented by host/sce_host.cpp, so the shims
// compile and run unchanged on Linux for the tests.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- libkernel -----------------------------------------------------------------------------------------
typedef void* SceKernelEqueue; // struct kevent* on the console; only passed around

int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int mem_type, intptr_t* phys_out);
int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags, intptr_t phys, size_t align);
int sceKernelReleaseDirectMemory(intptr_t phys, size_t len);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t align, int mem_type,
	intptr_t* phys_out);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* start_out, size_t* size_out);
int sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int prot, int flags, const char* name);
int sceKernelAvailableFlexibleMemorySize(size_t* size);

int sceKernelCreateEqueue(SceKernelEqueue* eq, const char* name);
int sceKernelWaitEqueue(SceKernelEqueue eq, void* events, int num, int* out, unsigned int* timeout_us);
int sceKernelDeleteEqueue(SceKernelEqueue eq);

int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
int sceKernelUsleep(unsigned int usec);

// ---- libSceSystemService ---------------------------------------------------------------------------------
int sceSystemServiceHideSplashScreen(void);
// "exit" ends the app the way the system wants (PS5SX2's OrbisExitApp: exit()/_exit() end in SIGSYS)
int sceSystemServiceLoadExec(const char* path, const char* argv[]);

// ---- libSceVideoOut ----------------------------------------------------------------------------------------
typedef struct SceVideoOutBuffers
{
	void* data;
	uint64_t junk0[3];
} SceVideoOutBuffers;

typedef struct SceVideoOutBufferAttribute2
{
	uint8_t junk0[80];
} SceVideoOutBufferAttribute2;

#define SCE_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB 0x8000000022000000ULL // what the SDK's SDL port uses
#define SCE_VIDEO_OUT_FLIP_MODE_VSYNC 1

int sceVideoOutOpen(int user, int bus_type, int index, const void* param);
int sceVideoOutClose(int handle);
int sceVideoOutAddFlipEvent(SceKernelEqueue eq, int handle, void* udata);
int sceVideoOutDeleteFlipEvent(SceKernelEqueue eq, int handle);
int sceVideoOutSetFlipRate(int handle, int rate); // 0: 60 Hz, 1: 30 Hz, 2: 20 Hz
int sceVideoOutSubmitFlip(int handle, int buffer_index, uint32_t flip_mode, int64_t flip_arg);
void sceVideoOutSetBufferAttribute2(SceVideoOutBufferAttribute2* attr, uint64_t pixel_format, uint32_t tiling,
	uint32_t width, uint32_t height, uint64_t option, uint32_t dcc_control, uint64_t dcc_cb_addr);
int sceVideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start, SceVideoOutBuffers* buffers,
	int count, SceVideoOutBufferAttribute2* attr, int category, void* option);

// ---- libSceAudioOut ----------------------------------------------------------------------------------------
#define SCE_AUDIO_OUT_PORT_TYPE_MAIN 0
#define SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1
#define SCE_USER_SERVICE_USER_ID_SYSTEM 0xff

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* p); // blocks until the previous grain is consumed
int32_t sceAudioOutClose(int32_t handle);

// ---- libScePad / libSceUserService -------------------------------------------------------------------------
#define SCE_PAD_PORT_TYPE_STANDARD 0
#define SCE_PAD_ERROR_ALREADY_OPENED ((int)0x80920004u)
#define SCE_USER_SERVICE_ERROR_ALREADY_INITIALIZED ((int)0x80960003u)

#define SCE_PAD_BUTTON_L3 0x0002
#define SCE_PAD_BUTTON_R3 0x0004
#define SCE_PAD_BUTTON_OPTIONS 0x0008
#define SCE_PAD_BUTTON_UP 0x0010
#define SCE_PAD_BUTTON_RIGHT 0x0020
#define SCE_PAD_BUTTON_DOWN 0x0040
#define SCE_PAD_BUTTON_LEFT 0x0080
#define SCE_PAD_BUTTON_L2 0x0100
#define SCE_PAD_BUTTON_R2 0x0200
#define SCE_PAD_BUTTON_L1 0x0400
#define SCE_PAD_BUTTON_R1 0x0800
#define SCE_PAD_BUTTON_TRIANGLE 0x1000
#define SCE_PAD_BUTTON_CIRCLE 0x2000
#define SCE_PAD_BUTTON_CROSS 0x4000
#define SCE_PAD_BUTTON_SQUARE 0x8000
#define SCE_PAD_BUTTON_TOUCH_PAD 0x100000

typedef struct ScePadTouch
{
	uint16_t x;
	uint16_t y;
	uint8_t finger;
	uint8_t pad[3];
} ScePadTouch;

typedef struct ScePadTouchData
{
	uint8_t fingers;
	uint8_t pad1[3];
	uint32_t pad2;
	ScePadTouch touch[2];
} ScePadTouchData;

typedef struct ScePadColor
{
	uint8_t r, g, b, a;
} ScePadColor;

typedef struct ScePadVibrationParam
{
	uint8_t largeMotor;
	uint8_t smallMotor;
} ScePadVibrationParam;

typedef struct ScePadData
{
	uint32_t buttons;
	struct { uint8_t x, y; } leftStick;
	struct { uint8_t x, y; } rightStick;
	struct { uint8_t l2, r2; } analogButtons;
	uint16_t padding;
	struct { float x, y, z, w; } quat;
	struct { float x, y, z; } vel;
	struct { float x, y, z; } acell;
	ScePadTouchData touch;
	uint8_t connected;
	uint64_t timestamp;
	uint8_t ext[16];
	uint8_t count;
	uint8_t unknown[15];
} ScePadData;

int scePadInit(void);
int scePadOpen(int user, int type, int index, void* param);
int scePadGetHandle(int user, int type, int index);
int scePadReadState(int handle, ScePadData* data);
int scePadSetLightBar(int handle, const ScePadColor* color);
int scePadSetVibration(int handle, const ScePadVibrationParam* param);
// The DualSense starts in its "advanced" haptics mode, where the PS4-style scePadSetVibration rumble does
// nothing; the compatible mode makes it emulate the two classic motors.
int scePadSetVibrationMode(int handle, int mode);
#define SCE_PAD_VIBRATION_MODE_ADVANCED 1
#define SCE_PAD_VIBRATION_MODE_COMPATIBLE 2
int scePadClose(int handle);

int sceUserServiceInitialize(void* params);
int sceUserServiceGetForegroundUser(int32_t* user);
int sceUserServiceGetLoginUserIdList(int32_t users[4]);

// ---- libSceNet / libSceNetCtl / libSceSsl / libSceHttp2 (as the SDK's http2_get sample and PS5SX2) ----
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlGetState(int* state); // 3: IP address obtained
int sceSslInit(size_t pool_size);
int sceSslTerm(int ctx);
int sceHttp2Init(int net_pool, int ssl_ctx, size_t pool_size, int max_requests);
int sceHttp2Term(int ctx);
int sceHttp2CreateTemplate(int ctx, const char* user_agent, int http_version, int auto_proxy);
int sceHttp2DeleteTemplate(int tmpl);
int sceHttp2CreateRequestWithURL(int tmpl, const char* method, const char* url, uint64_t content_length);
int sceHttp2DeleteRequest(int req);
int sceHttp2SendRequest(int req, const void* data, size_t size);
int sceHttp2GetStatusCode(int req, int* status);
int sceHttp2ReadData(int req, void* data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceHttp2AbortRequest(int req);

#ifdef __cplusplus
}
#endif
