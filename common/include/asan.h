/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2021 Intel Corporation
 *                    Paweł Marczewski <pawel@invisiblethingslab.com>
 */

/*
 * This file defines functions for address sanitization (ASan).
 *
 * Normally, code compiled with ASan is linked against a special library (libasan), but that library
 * is hard to adapt to a no-stdlib setting as well as all the custom memory handling that we
 * perform.
 *
 * See also `ubsan.c` for a similar (but much simpler) integration with UBSan.
 *
 * For more information, see:
 *
 * - ASan documentation: https://clang.llvm.org/docs/AddressSanitizer.html
 *
 * - libasan source code in LLVM repository: https://github.com/llvm/llvm-project/
 *   (compiler-rt/lib/asan/)
 *
 * - AddressSanitizer compiler code, also in LLVM repository, for flags that we use to configure
 *   (llvm/lib/Transforms/Instrumentation)
 */

/*
 * How to use ASan:
 *
 * - Make sure the program maps the shadow memory area at startup. This will be something like:
 *
 *       mmap((void*)ASAN_SHADOW_START, ASAN_SHADOW_LENGTH, PROT_READ | PROT_WRITE,
 *            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
 *            -1, 0);
 *
 * - Annotate all functions that shouldn't perform sanitized memory access with
 *   `__attribute_no_sanitize_address`.
 *
 * - Instrument your implementation of `malloc`/`free`:
 *
 *   * Make sure there is some padding before each object, to help detect overflows.
 *
 *   * In `malloc`, unpoison exactly the region requested (without rounding up the size).
 *
 *   * When freeing the allocated memory (in `free`), poison the memory region with
 *     ASAN_POISON_HEAP_AFTER_FREE.
 *
 *   * Make sure to manage mapped/unmapped pages (`system_malloc`/`system_free`). Newly mapped
 *     memory should be poisoned with ASAN_POISON_HEAP_LEFT_REDZONE, and unmapped memory should be
 *     unpoisoned before unmapping (in case ASan-unaware code uses this part of address space
 *     later).
 *
 * - You should compile the program with:
 *
 *       -fsanitize=address
 *       -fno-sanitize-link-runtime
 *       -mllvm -asan-mapping-offset=0x18000000000
 *       -mllvm -asan-use-after-return=0
 *       -mllvm -asan-stack=0
 *       -mllvm -asan-globals=0
 *       -DASAN
 */

#ifndef ASAN_H_
#define ASAN_H_

/* All ASan code should be guarded by `#ifdef ASAN`. */
#ifdef ASAN

#ifndef __x86_64__
/* Other systems will probably require different ASAN_SHADOW_* parameters */
#error ASan is currently supported only for x86_64
#endif

#ifndef __clang__
#error ASan is currently supported only for Clang
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * Parameters of the shadow memory area. Each byte of shadow memory corresponds to ASAN_SHADOW_ALIGN
 * (by default 8) bytes of user memory.
 *
 * Note that we override the address of shadow memory area (ASAN_SHADOW_START). We want the shadow
 * memory to begin at a high address, because the default for x86_64 (0x7fff8000, just before 2 GB)
 * doesn't work well with SGX: an enclave of size 2 GB or higher will be mapped over the shadow
 * memory. The same address has to be provided to the compiler using `-mllvm
 * -asan-mapping-offset=0x...`.
 *
 * (BEWARE when changing ASAN_SHADOW_START: the value should not be a power of two. For powers of
 * two, LLVM tries to optimize the generated code by emitting bitwise OR instead of addition in the
 * mem-to-shadow conversion. As a result, low values (such as 1 TB) will not work correctly. A value
 * at least as high as the shadow map length (1 << 44) should work, but it's probably better to stay
 * closer to the default configuration and not use a power of two.)
 *
 * The shadow memory bytes have the following meaning:
 *
 * - A value of 0 means all bytes are accessible.
 *
 * - A low value (01..07) means only the first N bytes are accessible.
 *
 * - A value with highest bit set (80..FF) means the memory is forbidden to use, and the exact value
 *   is used to diagnose the problem.
 */
#define ASAN_SHADOW_START 0x18000000000ULL /* 1.5 TB */
#define ASAN_SHADOW_SHIFT 3
#define ASAN_SHADOW_LENGTH (1ULL << 44)
#define ASAN_SHADOW_ALIGN (1 << ASAN_SHADOW_SHIFT)
#define ASAN_SHADOW_MASK ((1 << ASAN_SHADOW_SHIFT) - 1)

/* Conversion between user and shadow addresses */
#define ASAN_MEM_TO_SHADOW(addr) (((addr) >> ASAN_SHADOW_SHIFT) + ASAN_SHADOW_START)
#define ASAN_SHADOW_TO_MEM(addr) (((addr) - ASAN_SHADOW_START) << ASAN_SHADOW_SHIFT)

/* Magic values to mark different kinds of inaccessible memory. */
#define ASAN_POISON_HEAP_LEFT_REDZONE     0xfa
#define ASAN_POISON_HEAP_AFTER_FREE       0xfd

/* Poison a memory region. `addr` must be aligned to ASAN_SHADOW_ALIGN, and `size` is rounded up to
 * ASAN_SHADOW_ALIGN. */
void asan_poison_region(uintptr_t addr, size_t size, uint8_t value);

/* Unpoison a memory region. `addr` must be aligned to ASAN_SHADOW_ALIGN, but `size` is treated
 * exactly. */
void asan_unpoison_region(uintptr_t addr, size_t size);

/* Initialization callbacks. Generated in object .init sections. Graphene doesn't call these anyway,
 * so this needs to be a no-op. */
void __asan_init(void);
void __asan_version_mismatch_check_v8(void);

/*
 * Load/store callbacks:
 *
 * - `load` / `store`: check if memory under given address is accessible; if not, report the error
 *   and abort
 *
 * - `report_load` / `report_store`: directly report an illegal access and abort
 *
 * For small areas, instead of generating `load` and `store` callbacks, LLVM can generate inline
 * checks for the shadow memory (and calls to `report_load` / `report_store`). This is controlled by
 * `-mllvm -asan-instrumentation-with-call-threshold=N`.
 */

#define DECLARE_ASAN_LOAD_STORE_CALLBACKS(n)                \
    void __asan_load##n(uintptr_t p);                       \
    void __asan_store##n(uintptr_t p);                      \
    void __asan_report_load##n(uintptr_t p);                \
    void __asan_report_store##n(uintptr_t p);

DECLARE_ASAN_LOAD_STORE_CALLBACKS(1)
DECLARE_ASAN_LOAD_STORE_CALLBACKS(2)
DECLARE_ASAN_LOAD_STORE_CALLBACKS(4)
DECLARE_ASAN_LOAD_STORE_CALLBACKS(8)
DECLARE_ASAN_LOAD_STORE_CALLBACKS(16)

/* Variable-size version of load/store callbacks, used for large accesses. */
void __asan_loadN(uintptr_t p, size_t size);
void __asan_storeN(uintptr_t p, size_t size);
void __asan_report_load_n(uintptr_t p, size_t size);
void __asan_report_store_n(uintptr_t p, size_t size);

/* Called when entering a function marked as no-return. Used for stack sanitization. */
void __asan_handle_no_return(void);

/* Callbacks for setting the shadow memory to specific values. As with load/store callbacks, LLVM
 * normally generates inline stores and calls these functions only for bigger areas. This is
 * controlled by `-mllvm -asan-max-inline-poisoning-size=N`. */
void __asan_set_shadow_00(uintptr_t addr, size_t size);
void __asan_set_shadow_f1(uintptr_t addr, size_t size);
void __asan_set_shadow_f2(uintptr_t addr, size_t size);
void __asan_set_shadow_f3(uintptr_t addr, size_t size);
void __asan_set_shadow_f5(uintptr_t addr, size_t size);
void __asan_set_shadow_f8(uintptr_t addr, size_t size);

/* Sanitized versions of builtin functions. Note that ASan also overrides the normal versions
 * (`memcpy` etc.) */
void* __asan_memcpy(void *dst, const void *src, size_t size);
void* __asan_memset(void *s, int c, size_t n);
void* __asan_memmove(void* dest, const void* src, size_t n);

#endif /* ASAN */

#endif /* ASAN_H */
