// Mupen64Plus PS5: SDL2's threads, mutexes, condition variables and clock on pthreads (SDL.h).
// SPDX-License-Identifier: MIT

#include "SDL.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct SDL_mutex
{
	pthread_mutex_t m;
};

struct SDL_cond
{
	pthread_cond_t c;
};

struct SDL_Thread
{
	pthread_t t;
	SDL_ThreadFunction fn;
	void* data;
	int status;
};

SDL_mutex* SDL_CreateMutex(void)
{
	SDL_mutex* m = (SDL_mutex*)malloc(sizeof(*m));
	if (!m)
		return NULL;
	// SDL mutexes are recursive
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&m->m, &attr) != 0)
	{
		pthread_mutexattr_destroy(&attr);
		free(m);
		return NULL;
	}
	pthread_mutexattr_destroy(&attr);
	return m;
}

int SDL_LockMutex(SDL_mutex* m)
{
	return m ? pthread_mutex_lock(&m->m) : -1;
}

int SDL_UnlockMutex(SDL_mutex* m)
{
	return m ? pthread_mutex_unlock(&m->m) : -1;
}

void SDL_DestroyMutex(SDL_mutex* m)
{
	if (!m)
		return;
	pthread_mutex_destroy(&m->m);
	free(m);
}

SDL_cond* SDL_CreateCond(void)
{
	SDL_cond* c = (SDL_cond*)malloc(sizeof(*c));
	if (c && pthread_cond_init(&c->c, NULL) != 0)
	{
		free(c);
		return NULL;
	}
	return c;
}

int SDL_CondWait(SDL_cond* c, SDL_mutex* m)
{
	return (c && m) ? pthread_cond_wait(&c->c, &m->m) : -1;
}

int SDL_CondSignal(SDL_cond* c)
{
	return c ? pthread_cond_signal(&c->c) : -1;
}

int SDL_CondBroadcast(SDL_cond* c)
{
	return c ? pthread_cond_broadcast(&c->c) : -1;
}

void SDL_DestroyCond(SDL_cond* c)
{
	if (!c)
		return;
	pthread_cond_destroy(&c->c);
	free(c);
}

static void* ThreadTrampoline(void* arg)
{
	SDL_Thread* t = (SDL_Thread*)arg;
	t->status = t->fn(t->data);
	return NULL;
}

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data)
{
	(void)name;
	SDL_Thread* t = (SDL_Thread*)calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->fn = fn;
	t->data = data;
	// the SDK's default pthread stack is small; the core's worker (savestate compression) wants room
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	const int r = pthread_create(&t->t, &attr, ThreadTrampoline, t);
	pthread_attr_destroy(&attr);
	if (r != 0)
	{
		free(t);
		return NULL;
	}
	return t;
}

void SDL_WaitThread(SDL_Thread* t, int* status)
{
	if (!t)
		return;
	pthread_join(t->t, NULL);
	if (status)
		*status = t->status;
	free(t);
}

static uint64_t NowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

Uint32 SDL_GetTicks(void)
{
	static uint64_t start;
	if (!start)
		start = NowMs();
	return (Uint32)(NowMs() - start);
}

void SDL_Delay(Uint32 ms)
{
	usleep((useconds_t)ms * 1000u);
}

Uint32 SDL_WasInit(Uint32 flags)
{
	(void)flags;
	return 0;
}

void SDL_Quit(void)
{
}

void SDL_PumpEvents(void)
{
}

const char* SDL_GetError(void)
{
	return "";
}
