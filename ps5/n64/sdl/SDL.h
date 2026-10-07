// Mupen64Plus PS5: the part of SDL2 that mupen64plus-core uses, on pthreads (sdl_shim.c).
//
// The core takes threads, mutexes, condition variables and a millisecond clock from SDL; its video
// extension, event loop and screenshot code (the parts that need a real SDL) are replaced by the port
// (ps5/n64/core/*.c), so nothing else is declared here.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t Sint8;
typedef int16_t Sint16;
typedef int32_t Sint32;
typedef int64_t Sint64;

#define SDL_VERSION_ATLEAST(x, y, z) ((x) <= 2)

#define SDL_INIT_TIMER 0x00000001u
#define SDL_INIT_VIDEO 0x00000020u

typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef int (*SDL_ThreadFunction)(void* data);

SDL_mutex* SDL_CreateMutex(void);
int SDL_LockMutex(SDL_mutex* m);
int SDL_UnlockMutex(SDL_mutex* m);
void SDL_DestroyMutex(SDL_mutex* m);

SDL_cond* SDL_CreateCond(void);
int SDL_CondWait(SDL_cond* c, SDL_mutex* m);
int SDL_CondSignal(SDL_cond* c);
int SDL_CondBroadcast(SDL_cond* c);
void SDL_DestroyCond(SDL_cond* c);

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data);
void SDL_WaitThread(SDL_Thread* t, int* status);

Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);

Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);
void SDL_PumpEvents(void);
const char* SDL_GetError(void);

#ifdef __cplusplus
}
#endif
