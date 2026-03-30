/*
 * x86_64 cache operations (mostly no-ops — x86 has coherent I/D caches)
 */
#ifndef AIOS_ARCH_X86_64_CACHE_H
#define AIOS_ARCH_X86_64_CACHE_H

#include <stdint.h>

static inline void arch_flush_code_region(uintptr_t start, uintptr_t size) {
    (void)start; (void)size;
    /* x86_64 has coherent instruction and data caches */
    __asm__ volatile("mfence" ::: "memory");
}

static inline void arch_memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static inline void arch_instruction_barrier(void) {
    /* No equivalent needed on x86_64 */
    __asm__ volatile("" ::: "memory");
}

#endif /* AIOS_ARCH_X86_64_CACHE_H */
