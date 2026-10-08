// Mupen64Plus PS5: "dynamic libraries" linked statically into eboot.bin (osal_dynlib_static.c).
//
// mupen64plus-core and its plugins find each other's functions by name through osal_dynlib_getproc()
// (dlsym() on the desktop). A native PS5 title is one ELF, so every library is a table of (name, function)
// pairs and a m64p_dynlib_handle is a pointer to one of those tables.
//
// SPDX-License-Identifier: MIT
#pragma once

#include "api/m64p_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	const char* name;
	m64p_function fn;
} m64ps5_symbol;

typedef struct
{
	const char* lib_name;
	const m64ps5_symbol* syms; // ends with {NULL, NULL}
} m64ps5_library;

// the core, and the four plugins built into the app
extern const m64ps5_library m64ps5_core_lib; // osal_dynlib_static.c
extern const m64ps5_library m64ps5_rsp_lib; // ps5/n64/plugins/rsp_cxd4_ps5.c
extern const m64ps5_library m64ps5_gfx_lib; // ps5/n64/plugins/gfx_ps5.c
extern const m64ps5_library m64ps5_gfx_parallel_lib; // ps5/n64/plugins/gfx_parallel_ps5.cpp (VULKAN=1)
extern const m64ps5_library m64ps5_audio_lib; // ps5/n64/plugins/audio_ps5.cpp
extern const m64ps5_library m64ps5_input_lib; // ps5/n64/plugins/input_ps5.cpp

#define M64PS5_HANDLE(lib) ((m64p_dynlib_handle)(void*)&(lib))

// A function of one of the libraries above by name (NULL when it has none).
m64p_function m64ps5_getproc(const m64ps5_library* lib, const char* name);

#ifdef __cplusplus
}
#endif
