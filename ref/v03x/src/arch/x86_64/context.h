/*
 * x86_64 cooperative context switch
 *
 * Saves/restores callee-saved registers per System V ABI:
 * rbx, rbp, r12-r15, rsp
 */
#ifndef AIOS_ARCH_X86_64_CONTEXT_H
#define AIOS_ARCH_X86_64_CONTEXT_H

#include <stdint.h>

typedef struct {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;     /* return address */
} arch_context_t;

static inline int arch_save_context(arch_context_t *ctx) {
    register int ret __asm__("eax");
    __asm__ volatile(
        "movq %%rbx, 0(%1)\n"
        "movq %%rbp, 8(%1)\n"
        "movq %%r12, 16(%1)\n"
        "movq %%r13, 24(%1)\n"
        "movq %%r14, 32(%1)\n"
        "movq %%r15, 40(%1)\n"
        "movq %%rsp, 48(%1)\n"
        "leaq 1f(%%rip), %%rdx\n"
        "movq %%rdx, 56(%1)\n"
        "movl $0, %0\n"
        "jmp 2f\n"
        "1:\n"
        "movl $1, %0\n"
        "2:\n"
        : "=a"(ret)
        : "r"(ctx)
        : "rdx", "memory"
    );
    return ret;
}

static inline void arch_restore_context(arch_context_t *ctx) {
    __asm__ volatile(
        "movq 0(%0), %%rbx\n"
        "movq 8(%0), %%rbp\n"
        "movq 16(%0), %%r12\n"
        "movq 24(%0), %%r13\n"
        "movq 32(%0), %%r14\n"
        "movq 40(%0), %%r15\n"
        "movq 48(%0), %%rsp\n"
        "movq 56(%0), %%rdx\n"
        "jmpq *%%rdx\n"
        :
        : "r"(ctx)
        : "rdx", "memory"
    );
    __builtin_unreachable();
}

static inline void arch_init_context(arch_context_t *ctx,
                                      uintptr_t entry_point,
                                      uintptr_t stack_top,
                                      uintptr_t arg0) {
    for (int i = 0; i < (int)(sizeof(arch_context_t) / 8); i++)
        ((uint64_t *)ctx)[i] = 0;

    ctx->rsp = stack_top & ~15UL;
    ctx->rip = entry_point;
    ctx->r12 = arg0;    /* stash arg0, trampoline moves to rdi */
}

#endif /* AIOS_ARCH_X86_64_CONTEXT_H */
