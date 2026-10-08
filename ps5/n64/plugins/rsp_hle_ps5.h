// Mupen64Plus PS5: rsp-hle in front of cxd4 (rsp_hle_ps5.c), for the RSP plugin (rsp_cxd4_ps5.c).
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>

void n64ps5_hle_startup(void* context, void (*debug_callback)(void*, int, const char*));
void n64ps5_hle_init(const RSP_INFO* info); // m64p_plugin.h
void n64ps5_hle_rom_closed(void);
bool n64ps5_hle_enabled(void);
void n64ps5_hle_execute(void); // one task; tasks rsp-hle doesn't know go to cxd4_DoRspCycles
