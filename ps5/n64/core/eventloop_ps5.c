// Mupen64Plus PS5: the core's event loop (replaces src/main/eventloop.c).
//
// The desktop event loop turns SDL keyboard/joystick events into core commands (save state, pause,
// fullscreen...). On the PS5 the frontend reads the DualSense itself and sends those commands with
// CoreDoCommand, so only the GameShark button state is kept here.
//
// SPDX-License-Identifier: MIT

#include "main/eventloop.h"

static int l_GamesharkActive = 0;

int event_set_core_defaults(void)
{
	return 1;
}

void event_initialize(void)
{
}

void event_sdl_keydown(int keysym, int keymod)
{
	(void)keysym;
	(void)keymod;
}

void event_sdl_keyup(int keysym, int keymod)
{
	(void)keysym;
	(void)keymod;
}

int event_gameshark_active(void)
{
	return l_GamesharkActive;
}

void event_set_gameshark(int active)
{
	l_GamesharkActive = active ? 1 : 0;
}
