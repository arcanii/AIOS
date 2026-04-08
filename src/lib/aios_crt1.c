/*
 * aios_crt1.c -- Custom CRT for tcc-compiled AIOS programs
 *
 * Provides _start + __sel4_start_c that routes through __aios_entry
 * instead of calling main directly. Bypasses --wrap=main which the
 * tcc built-in linker does not support.
 *
 * tcc link chain:
 *   crt1.o + crti.o + user.o + libc.a + libtcc1.a + crtn.o
 *
 * Call chain:
 *   _start -> __sel4_start_c -> __sel4runtime_start_main
 *     -> __aios_entry -> aios_init -> user main(clean argc/argv)
 */
#include <sel4runtime.h>
#include <sel4runtime/start.h>

int __aios_entry(int, char const *const *, char const *const *);

/* _start -- seL4 user process entry point (replaces crt0.S) */
__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    mov fp, #0\n"
    "    mov lr, #0\n"
    "    mov x0, sp\n"
    "    bl __sel4_start_c\n"
    "1:  b 1b\n"
);

/*
 * Parse argc/argv/envp/auxv from initial stack.
 * Same as sel4runtime crt1.c but passes __aios_entry
 * as the entry function instead of main.
 */
void __sel4_start_c(void const *stack)
{
    unsigned long argc = *((unsigned long const *)stack);
    char const *const *argv = &((char const *const *)stack)[1];
    char const *const *envp = &argv[argc + 1];
    int envc = 0;
    while (envp[envc] != SEL4RUNTIME_NULL) {
        envc++;
    }
    auxv_t const *auxv = (void const *)(&envp[envc + 1]);
    __sel4runtime_start_main(
        (int (*)())__aios_entry, argc, argv, envp, auxv);
}
