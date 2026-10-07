// Mupen64Plus PS5: executable memory for the new dynarec (jit_ps5.c).
// SPDX-License-Identifier: MIT
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Finds out once whether this process can run generated code: 0 no, 1 mprotect works, 2 executable direct
// memory works. Call it before a game starts: it makes the code cache executable and tests it (faults caught).
int n64ps5_jit_probe(void);
const char* n64ps5_jit_probe_result(void); // a line for the log

// new_dynarec.c's mprotect (compiled with -Dmprotect=n64ps5_jit_mprotect).
int n64ps5_jit_mprotect(void* addr, size_t len, int prot);

#ifdef __cplusplus
}
#endif
