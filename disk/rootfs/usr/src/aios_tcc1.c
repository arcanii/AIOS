/*
 * aios_tcc1.c -- AIOS-side stubs for symbols missing from libtcc1.a
 *
 * v0.4.108: bridges TCC self-host on AIOS by providing:
 *   - __arm64_clear_cache: real cache flush for AArch64 (TCC inlines
 *     this when self-compiling, but our libtcc1.a was cross-compiled
 *     by host gcc which left it as an external reference)
 *   - __executable_start: weak fallback at start-of-text. TCC's ELF
 *     linker is supposed to define this via set_global_sym, but the
 *     host-built tcc2 doesn't always run that path. Weak so it gives
 *     way if tcc2 does provide it.
 *
 * Build:
 *   tcc -c /usr/src/aios_tcc1.c -o /usr/lib/aios_tcc1.o
 * Use:
 *   tcc2 ... /usr/lib/aios_tcc1.o ...
 * Or include in libtcc1.a (better long-term).
 */

#include <stddef.h>

/* AArch64 cache flush for [beg, end).
 * Walk by cache-line stride: data cache clean to PoU, then invalidate
 * instruction cache, then dsb + isb. */
void __arm64_clear_cache(void *beg, void *end)
{
    if (!beg || !end || beg >= end) return;
    /* Read CTR_EL0 to get D-cache and I-cache line sizes. */
    unsigned long ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    unsigned long dminline = 4UL << ((ctr >> 16) & 0xF);
    unsigned long iminline = 4UL << (ctr & 0xF);

    /* Data cache clean to PoU */
    unsigned long p = (unsigned long)beg & ~(dminline - 1);
    unsigned long e = (unsigned long)end;
    while (p < e) {
        __asm__ __volatile__("dc cvau, %0" :: "r"(p) : "memory");
        p += dminline;
    }
    __asm__ __volatile__("dsb ish" ::: "memory");

    /* Instruction cache invalidate */
    p = (unsigned long)beg & ~(iminline - 1);
    while (p < e) {
        __asm__ __volatile__("ic ivau, %0" :: "r"(p) : "memory");
        p += iminline;
    }
    __asm__ __volatile__("dsb ish" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

/* Weak fallback for __executable_start.
 * Real linkers define this at the start of the .text segment.
 * TCC's linker runs set_global_sym for it, but on AIOS the host-built
 * tcc2 binary may not always follow that path. Mark weak so the proper
 * definition wins when present. */
__attribute__((weak)) char __executable_start[1];
