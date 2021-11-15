/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * This file contains code for calling Gramine from userspace. It can be used in patched
 * applications and libraries (e.g. glibc).
 *
 * To use, include this file in patched code and replace SYSCALL instructions with invocations of
 * SYSCALLDB assembly macro.
 */

#ifndef SHIM_ENTRY_API_H_
#define SHIM_ENTRY_API_H_

/* Offsets for GS register at which entry vectors can be found */
#define SHIM_SYSCALLDB_OFFSET 24
#define SHIM_CALL_OFFSET      32

#ifdef __ASSEMBLER__
/* clang-format off */

.macro SYSCALLDB
leaq .Lafter_syscalldb\@(%rip), %rcx
jmpq *%gs:SHIM_SYSCALLDB_OFFSET
.Lafter_syscalldb\@:
.endm

/* clang-format on */
#else /* !__ASSEMBLER__ */

#define SHIM_STR(x)  #x
#define SHIM_XSTR(x) SHIM_STR(x)

/* clang-format off */
__asm__(
    ".macro SYSCALLDB\n"
    "leaq .Lafter_syscalldb\\@(%rip), %rcx\n"
    "jmpq *%gs:" SHIM_XSTR(SHIM_SYSCALLDB_OFFSET) "\n"
    ".Lafter_syscalldb\\@:\n"
    ".endm\n"
);
/* clang-format on */

#undef SHIM_XSTR
#undef SHIM_STR

/* Custom call numbers */
enum {
    SHIM_CALL_REGISTER_LIBRARY = 1,
    SHIM_CALL_RUN_TEST,
};

static inline int shim_call(int number, unsigned long arg1, unsigned long arg2) {
    long (*handle_call)(int number, unsigned long arg1, unsigned long arg2);
    /* clang-format off */
    __asm__("movq %%gs:%c1, %0"
            : "=r"(handle_call)
            : "i"(SHIM_CALL_OFFSET)
            : "memory");
    /* clang-format on */
    return handle_call(number, arg1, arg2);
}

static inline int shim_register_library(const char* name, unsigned long load_address) {
    return shim_call(SHIM_CALL_REGISTER_LIBRARY, (unsigned long)name, load_address);
}

static inline int shim_run_test(const char* test_name) {
    return shim_call(SHIM_CALL_RUN_TEST, (unsigned long)test_name, 0);
}

#endif /* __ASSEMBLER__ */

#endif /* SHIM_ENTRY_API_H_ */
