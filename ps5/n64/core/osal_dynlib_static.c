// Mupen64Plus PS5: osal_dynlib_getproc() over statically linked symbol tables (replaces
// src/osal/dynamiclib_unix.c; see static_dynlib.h), and the core's own table.
// SPDX-License-Identifier: MIT

#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/m64p_common.h"
#include "api/m64p_config.h"
#include "api/m64p_debugger.h"
#include "api/m64p_frontend.h"
#include "api/m64p_types.h"
#include "api/m64p_vidext.h"
#include "osal/dynamiclib.h"

#include "static_dynlib.h"

m64p_function m64ps5_getproc(const m64ps5_library* lib, const char* name)
{
	if (!lib || !name)
		return NULL;
	for (const m64ps5_symbol* s = lib->syms; s->name; s++)
		if (strcmp(s->name, name) == 0)
			return s->fn;
	return NULL;
}

m64p_function osal_dynlib_getproc(m64p_dynlib_handle LibHandle, const char* pccProcedureName)
{
	return m64ps5_getproc((const m64ps5_library*)LibHandle, pccProcedureName);
}

#define SYM(f) {#f, (m64p_function)(f)}

static const m64ps5_symbol l_CoreSymbols[] = {
	// m64p_common.h / m64p_frontend.h
	SYM(PluginGetVersion),
	SYM(CoreGetAPIVersions),
	SYM(CoreErrorMessage),
	SYM(CoreStartup),
	SYM(CoreShutdown),
	SYM(CoreAttachPlugin),
	SYM(CoreDetachPlugin),
	SYM(CoreDoCommand),
	SYM(CoreOverrideVidExt),
	SYM(CoreAddCheat),
	SYM(CoreCheatEnabled),
	SYM(CoreGetRomSettings),
	// m64p_config.h
	SYM(ConfigListSections),
	SYM(ConfigOpenSection),
	SYM(ConfigListParameters),
	SYM(ConfigSaveFile),
	SYM(ConfigSaveSection),
	SYM(ConfigHasUnsavedChanges),
	SYM(ConfigDeleteSection),
	SYM(ConfigRevertChanges),
	SYM(ConfigSetParameter),
	SYM(ConfigSetParameterHelp),
	SYM(ConfigGetParameter),
	SYM(ConfigGetParameterType),
	SYM(ConfigGetParameterHelp),
	SYM(ConfigSetDefaultInt),
	SYM(ConfigSetDefaultFloat),
	SYM(ConfigSetDefaultBool),
	SYM(ConfigSetDefaultString),
	SYM(ConfigGetParamInt),
	SYM(ConfigGetParamFloat),
	SYM(ConfigGetParamBool),
	SYM(ConfigGetParamString),
	SYM(ConfigGetSharedDataFilepath),
	SYM(ConfigGetUserConfigPath),
	SYM(ConfigGetUserDataPath),
	SYM(ConfigGetUserCachePath),
	SYM(ConfigOverrideUserPaths),
	SYM(ConfigExternalOpen),
	SYM(ConfigExternalClose),
	SYM(ConfigExternalGetParameter),
	SYM(ConfigSendNetplayConfig),
	SYM(ConfigReceiveNetplayConfig),
	// m64p_debugger.h
	SYM(DebugSetCallbacks),
	SYM(DebugSetCoreCompare),
	SYM(DebugSetRunState),
	SYM(DebugGetState),
	SYM(DebugStep),
	SYM(DebugDecodeOp),
	SYM(DebugMemGetRecompInfo),
	SYM(DebugMemGetMemInfo),
	SYM(DebugMemGetPointer),
	SYM(DebugMemRead64),
	SYM(DebugMemRead32),
	SYM(DebugMemRead16),
	SYM(DebugMemRead8),
	SYM(DebugMemWrite64),
	SYM(DebugMemWrite32),
	SYM(DebugMemWrite16),
	SYM(DebugMemWrite8),
	SYM(DebugGetCPUDataPtr),
	SYM(DebugBreakpointLookup),
	SYM(DebugBreakpointCommand),
	SYM(DebugBreakpointTriggeredBy),
	SYM(DebugVirtualToPhysical),
	// m64p_vidext.h
	SYM(VidExt_Init),
	SYM(VidExt_InitWithRenderMode),
	SYM(VidExt_Quit),
	SYM(VidExt_ListFullscreenModes),
	SYM(VidExt_ListFullscreenRates),
	SYM(VidExt_SetVideoMode),
	SYM(VidExt_SetVideoModeWithRate),
	SYM(VidExt_ResizeWindow),
	SYM(VidExt_SetCaption),
	SYM(VidExt_ToggleFullScreen),
	SYM(VidExt_GL_GetProcAddress),
	SYM(VidExt_GL_SetAttribute),
	SYM(VidExt_GL_GetAttribute),
	SYM(VidExt_GL_SwapBuffers),
	SYM(VidExt_GL_GetDefaultFramebuffer),
	SYM(VidExt_VK_GetSurface),
	SYM(VidExt_VK_GetInstanceExtensions),
	{NULL, NULL},
};

const m64ps5_library m64ps5_core_lib = {"mupen64plus-core", l_CoreSymbols};
