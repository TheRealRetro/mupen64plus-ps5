// Mupen64Plus PS5: the N64's RAM for the cheat search (n64_bridge.h).
// SPDX-License-Identifier: MIT

#include "n64_bridge.h"

#include "device/device.h"
#include "main/main.h"
#include "osal/preproc.h"

const uint32_t* n64ps5_ram(uint32_t* size)
{
	*size = (uint32_t)g_dev.rdram.dram_size;
	return g_dev.rdram.dram;
}

void n64ps5_ram_write(uint32_t address, uint32_t value, int bytes)
{
	address &= 0x00ffffff;
	if (address + (uint32_t)bytes > g_dev.rdram.dram_size)
		return;
	uint8_t* dram = (uint8_t*)g_dev.rdram.dram;
	switch (bytes)
	{
		case 1: dram[address ^ S8] = (uint8_t)value; break;
		case 2: *(uint16_t*)(dram + (address ^ S16)) = (uint16_t)value; break;
		default: *(uint32_t*)(dram + address) = value; break;
	}
	invalidate_r4300_cached_code(&g_dev.r4300, 0x80000000u | address, (size_t)bytes);
}
