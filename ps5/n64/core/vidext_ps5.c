// Mupen64Plus PS5: the core's video extension (replaces src/api/vidext.c).
//
// On the PS5 there is no OpenGL or Vulkan context to hand out: the video plugin (gfx_ps5.c, angrylion's
// software RDP) renders into memory and the frontend shows it through libSceVideoOut. So the video
// extension only reports a running fullscreen output and refuses every GL/VK request.
//
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "api/m64p_vidext.h"
#include "api/vidext.h"

static int l_VideoOutputActive = 0;

m64p_error OverrideVideoFunctions(m64p_video_extension_functions* VideoFunctionStruct)
{
	(void)VideoFunctionStruct;
	return M64ERR_UNSUPPORTED;
}

int VidExt_InFullscreenMode(void)
{
	return 1;
}

int VidExt_VideoRunning(void)
{
	return l_VideoOutputActive;
}

EXPORT m64p_error CALL VidExt_Init(void)
{
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_InitWithRenderMode(m64p_render_mode RenderMode)
{
	(void)RenderMode;
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_Quit(void)
{
	l_VideoOutputActive = 0;
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size* SizeArray, int* NumSizes)
{
	if (!SizeArray || !NumSizes || *NumSizes < 1)
		return M64ERR_INPUT_INVALID;
	SizeArray[0].uiWidth = 1920;
	SizeArray[0].uiHeight = 1080;
	*NumSizes = 1;
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size Size, int* NumRates, int* Rates)
{
	(void)Size;
	if (!NumRates || !Rates || *NumRates < 1)
		return M64ERR_INPUT_INVALID;
	Rates[0] = 60;
	*NumRates = 1;
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoMode(int Width, int Height, int BitsPerPixel, m64p_video_mode ScreenMode,
	m64p_video_flags Flags)
{
	(void)Width;
	(void)Height;
	(void)BitsPerPixel;
	(void)ScreenMode;
	(void)Flags;
	l_VideoOutputActive = 1;
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoModeWithRate(int Width, int Height, int RefreshRate, int BitsPerPixel,
	m64p_video_mode ScreenMode, m64p_video_flags Flags)
{
	(void)RefreshRate;
	return VidExt_SetVideoMode(Width, Height, BitsPerPixel, ScreenMode, Flags);
}

EXPORT m64p_error CALL VidExt_ResizeWindow(int Width, int Height)
{
	(void)Width;
	(void)Height;
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_SetCaption(const char* Title)
{
	(void)Title;
	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ToggleFullScreen(void)
{
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_function CALL VidExt_GL_GetProcAddress(const char* Proc)
{
	(void)Proc;
	return NULL;
}

EXPORT m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr Attr, int Value)
{
	(void)Attr;
	(void)Value;
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr Attr, int* pValue)
{
	(void)Attr;
	(void)pValue;
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_GL_SwapBuffers(void)
{
	// the core calls this while paused (pause_loop); the frontend owns the screen
	return M64ERR_SUCCESS;
}

EXPORT uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void)
{
	return 0;
}

EXPORT m64p_error CALL VidExt_VK_GetSurface(void** Surface, void* Instance)
{
	(void)Surface;
	(void)Instance;
	return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_VK_GetInstanceExtensions(const char** Extensions[], uint32_t* NumExtensions)
{
	(void)Extensions;
	(void)NumExtensions;
	return M64ERR_UNSUPPORTED;
}
