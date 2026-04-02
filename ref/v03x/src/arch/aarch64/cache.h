/*
 * AArch64 cache maintenance operations
 */
#ifndef AIOS_ARCH_AARCH64_CACHE_H
#define AIOS_ARCH_AARCH64_CACHE_H

#include <stdint.h>

/* Flush data cache and invalidate instruction cache for a memory region */
static inline void arch_flush_code_region(uintptr_t start, uintptr_t size) {
    uintptr_t addr = start;
    uintptr_t end = start + size;
    /* Clean data cache to point of unification */
    for (; addr < end; addr += 64)
        __asm__ volatile("dc cvau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    /* Invalidate instruction cache */
    addr = start;
    for (; addr < end; addr += 64)
        __asm__ volatile("ic ivau, %0" :: "r"(addr));
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/* Memory barrier */
static inline void arch_memory_barrier(void) {
    __asm__ volatile("dsb ish" ::: "memory");
}

/* Instruction barrier */
static inline void arch_instruction_barrier(void) {
    __asm__ volatile("isb" ::: "memory");
}

#endif /* AIOS_ARCH_AARCH64_CACHE_H */
