/* Mupen64Plus PS5: the x86-64 new dynarec's entry and exit stubs, in GNU assembler syntax.
 *
 * A line-for-line translation of mupen64plus-core's src/device/r4300/new_dynarec/x64/linkage_x64.asm (NASM,
 * Copyright (C) 2009-2011 Ari64, GPL-2.0-or-later), System V ABI only, so the port needs no NASM. The
 * offsets come from asm_defines_gas.h, generated from the core's asm_defines.c like the core's Makefile does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "asm_defines_gas.h"

#define HOT(field) (g_dev + offsetof_struct_device_r4300 + offsetof_struct_r4300_core_new_dynarec_hot_state + \
	offsetof_struct_new_dynarec_hot_state_##field)

	.intel_syntax noprefix
	.section .note.GNU-stack,"",@progbits
	.text

	.globl jump_vaddr_eax
	.globl jump_vaddr_ecx
	.globl jump_vaddr_edx
	.globl jump_vaddr_ebx
	.globl jump_vaddr_ebp
	.globl jump_vaddr_esi
	.globl jump_vaddr_edi
	.globl verify_code
	.globl cc_interrupt
	.globl do_interrupt
	.globl fp_exception
	.globl jump_syscall
	.globl jump_eret
	.globl new_dyna_start
	.globl invalidate_block_eax
	.globl invalidate_block_ecx
	.globl invalidate_block_edx
	.globl invalidate_block_ebx
	.globl invalidate_block_ebp
	.globl invalidate_block_esi
	.globl invalidate_block_edi
	.globl breakpoint
	.globl dyna_linker
	.globl dyna_linker_ds

/* ARG1 = edi, CCREG = ebx (System V) */

jump_vaddr_eax:
	mov edi, eax
	jmp jump_vaddr
jump_vaddr_edx:
	mov edi, edx
	jmp jump_vaddr
jump_vaddr_ebx:
	int 3
jump_vaddr_edi:
	mov edi, edi
	jmp jump_vaddr
jump_vaddr_ebp:
	mov edi, ebp
	jmp jump_vaddr
jump_vaddr_esi:
	mov edi, esi
	jmp jump_vaddr
jump_vaddr_ecx:
	mov edi, ecx
jump_vaddr:
	call get_addr_ht
	jmp rax

verify_code:
	/* edi = head */
	add rsp, -8
	call verify_dirty
	test eax, eax
	jne .Lverify_dirty
	add rsp, 8
	ret
.Lverify_dirty:
	mov edi, eax
	call get_addr
	add rsp, 16
	jmp rax

cc_interrupt:
	mov DWORD PTR [rip + HOT(cycle_count)], ebx
	add rsp, -56 /* align the stack */
	mov DWORD PTR [rip + HOT(pending_exception)], 0
	call dynarec_gen_interrupt
	mov ebx, DWORD PTR [rip + HOT(cycle_count)]
	mov ecx, DWORD PTR [rip + HOT(pending_exception)]
	mov edx, DWORD PTR [rip + HOT(stop)]
	add rsp, 56
	test edx, edx
	jne .Lcc_stop
	test ecx, ecx
	jne .Lcc_exception
	ret
.Lcc_exception:
	add rsp, -8
	mov edi, DWORD PTR [rip + HOT(pcaddr)]
	call get_addr_ht
	add rsp, 16
	jmp rax
.Lcc_stop:
	add rsp, 8 /* pop the return address */
new_dyna_stop:
	add rsp, 56
	/* restore the callee-saved registers */
	pop rbp
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbx
	ret /* leave the dynarec */

do_interrupt:
	mov edx, DWORD PTR [rip + HOT(stop)]
	test edx, edx
	jne new_dyna_stop
	mov edi, DWORD PTR [rip + HOT(pcaddr)]
	call get_addr_ht
	mov ebx, DWORD PTR [rip + HOT(cycle_count)]
	jmp rax

fp_exception:
	mov DWORD PTR [rip + HOT(pcaddr)], eax
	call cop1_unusable
	jmp rax

jump_syscall:
	mov DWORD PTR [rip + HOT(pcaddr)], eax
	call SYSCALL_new
	jmp rax

jump_eret:
	mov DWORD PTR [rip + HOT(cycle_count)], ebx
	call ERET_new
	mov ebx, DWORD PTR [rip + HOT(cycle_count)]
	test rax, rax
	je new_dyna_stop
	jmp rax

dyna_linker:
	call dynamic_linker
	jmp rax

dyna_linker_ds:
	call dynamic_linker_ds
	jmp rax

new_dyna_start:
	/* an even number of pushes keeps the stack 16-byte aligned */
	push rbx
	push r12
	push r13
	push r14
	push r15
	push rbp
	add rsp, -56
	mov edi, 0xa4000040
	call new_recompile_block
	mov ebx, DWORD PTR [rip + HOT(cycle_count)]
	mov rax, QWORD PTR [rip + base_addr]
	jmp rax

invalidate_block_eax:
	mov edi, eax
	jmp invalidate_block_call
invalidate_block_edi:
	mov edi, edi
	jmp invalidate_block_call
invalidate_block_edx:
	mov edi, edx
	jmp invalidate_block_call
invalidate_block_ebx:
	mov edi, ebx
	jmp invalidate_block_call
invalidate_block_ebp:
	mov edi, ebp
	jmp invalidate_block_call
invalidate_block_esi:
	mov edi, esi
	jmp invalidate_block_call
invalidate_block_ecx:
	mov edi, ecx
invalidate_block_call:
	add rsp, -56
	call invalidate_block
	add rsp, 56
	ret

breakpoint:
	int 3
	ret
