/*
 * aios_stubs.c -- Stub symbols for tcc-linked AIOS programs
 *
 * Provides symbols that are normally defined by the GNU linker
 * script or GCC built-ins, which tcc linker does not provide.
 */

/* Linker-defined symbol: start of .text section.
 * Referenced by some seL4 library code. */
#include "arch.h"
char __executable_start[1] __attribute__((section(".text"), weak)) = {0};

/* Platform hardware init -- not needed for user programs.
 * Pulled in by libplatsupport dependency chains. */
void uart_init(void) {}

/* IRQ cap copy -- root task only, stub for user programs. */
int sel4platsupport_arch_copy_irq_cap(void *a, void *b, void *c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    return -1;
}

/* GCC built-in for cache coherence. AArch64 DSB+ISB sequence. */
void __arm64_clear_cache(void *beg, void *end) {
    (void)beg; (void)end;
    arch_dsb_isb();
}
