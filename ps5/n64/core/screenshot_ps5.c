// Mupen64Plus PS5: the core's screenshot module (replaces src/main/screenshot.c, which needs libpng).
// Screenshots are not offered on the PS5 yet (the console's own Create button takes them).
// SPDX-License-Identifier: MIT

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "main/screenshot.h"

void ScreenshotRomOpen(void)
{
}

void TakeScreenshot(int iFrameNumber)
{
	(void)iFrameNumber;
	DebugMessage(M64MSG_WARNING, "screenshots are not supported in this build");
}
