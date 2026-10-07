// Mupen64Plus PS5: Mupen64PS5-helper.elf built into the app (ProsperoJailbreak.h). Only the native eboot links
// this file; the Makefile passes HELPER_ELF, the helper payload's path.
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

__asm__(".section .rodata\n"
		".balign 16\n"
		".global n64ps5_helper_elf_begin\n"
		"n64ps5_helper_elf_begin:\n"
		".incbin \"" HELPER_ELF "\"\n"
		".global n64ps5_helper_elf_end\n"
		"n64ps5_helper_elf_end:\n"
		".previous\n");
extern "C" const unsigned char n64ps5_helper_elf_begin[];
extern "C" const unsigned char n64ps5_helper_elf_end[];

namespace jailbreak
{
Blob EmbeddedHelper()
{
	return {n64ps5_helper_elf_begin, size_t(n64ps5_helper_elf_end - n64ps5_helper_elf_begin)};
}
} // namespace jailbreak
