/*
 * AArch64 cooperative context switch
 *
 * Saves/restores callee-saved registers (x19-x30), SP, and return address.
 * Used by the sandbox mini-scheduler to switch between processes.
 */
#ifndef AIOS_ARCH_AARCH64_CONTEXT_H
#define AIOS_ARCH_AARCH64_CONTEXT_H

#include <stdint.h>

/* Process context — callee-saved registers per AAPCS64 */
typedef struct {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;    /* frame pointer */
    uint64_t x30;    /* link register (return address) */
    uint64_t sp;
} arch_context_t;

/*
 * arch_context_switch(old, new)
 *
 * Save callee-saved regs into *old, restore from *new.
 * Returns 0 when saving, 1 when resumed.
 *
 * Similar to setjmp/longjmp but explicit two-pointer form.
 */
static __attribute__((noinline)) int arch_save_context(arch_context_t *ctx) {
    register int ret __asm__("x0");
    __asm__ volatile(
        "add x2, sp, #0x10\n"  /* Adjust for arch_save_context own frame */
        "stp x19, x20, [%1, #0]\n"
        "stp x21, x22, [%1, #16]\n"
        "stp x23, x24, [%1, #32]\n"
        "stp x25, x26, [%1, #48]\n"
        "stp x27, x28, [%1, #64]\n"
        "stp x29, x30, [%1, #80]\n"
        "str x2, [%1, #96]\n"
        "mov %0, #0\n"
        : "=r"(ret)
        : "r"(ctx)
        : "x2", "memory"
    );
    return ret;
}

static __attribute__((noinline)) void arch_restore_context(arch_context_t *ctx) {
    __asm__ volatile(
        "ldp x19, x20, [%0, #0]\n"
        "ldp x21, x22, [%0, #16]\n"
        "ldp x23, x24, [%0, #32]\n"
        "ldp x25, x26, [%0, #48]\n"
        "ldp x27, x28, [%0, #64]\n"
        "ldp x29, x30, [%0, #80]\n"
        "ldr x2, [%0, #96]\n"
        "mov sp, x2\n"
        "mov x0, #1\n"
        "ret\n"
        :
        : "r"(ctx)
        : "x2", "memory"
    );
    __builtin_unreachable();
}

/*
 * arch_init_context — set up a context to start a new process
 *
 * When restored, execution begins at entry_point with sp set to stack_top.
 * The function receives arg0 in x0 (passed via x19 trampoline).
 */
static inline void arch_init_context(arch_context_t *ctx,
                                      uintptr_t entry_point,
                                      uintptr_t stack_top,
                                      uintptr_t arg0) {
    /* Zero everything */
    for (int i = 0; i < (int)(sizeof(arch_context_t) / 8); i++)
        ((uint64_t *)ctx)[i] = 0;

    ctx->sp = stack_top & ~15UL;   /* 16-byte aligned */
    ctx->x30 = entry_point;        /* lr = entry point, so 'ret' jumps there */
    ctx->x19 = arg0;               /* stash arg0, trampoline moves to x0 */
}

#endif /* AIOS_ARCH_AARCH64_CONTEXT_H */
