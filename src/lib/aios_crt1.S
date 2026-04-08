/*
 * aios_crt1.S -- Custom CRT for tcc-compiled AIOS programs
 *
 * Pure assembly to avoid GCC relocations (ADR_PREL_PG_HI21,
 * ADD_ABS_LO12_NC) that tcc linker cannot resolve. Uses only
 * R_AARCH64_CALL26 relocations from bl instructions.
 *
 * Inlines the __sel4runtime_start_main logic: calls load_env
 * for TLS/constructor init, then __aios_entry, then exit.
 *
 * Call chain:
 *   _start -> __sel4_start_c (stack parse)
 *     -> __sel4runtime_load_env (TLS, auxv, constructors)
 *     -> __aios_entry (cap parse, aios_init, user main)
 *     -> sel4runtime_exit (cleanup)
 */

.section .text

.global _start
_start:
    mov     fp, #0
    mov     lr, #0
    mov     x0, sp
    bl      __sel4_start_c
1:  b       1b

.global __sel4_start_c
__sel4_start_c:
    /* x0 = initial stack pointer from _start */
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]

    /* argc = *(uint64_t *)stack */
    ldr     x19, [x0]

    /* argv = stack + 8 */
    add     x20, x0, #8

    /* envp = &argv[argc + 1] */
    add     x9, x19, #1
    add     x21, x20, x9, lsl #3

    /* Scan envp for NULL to locate auxv */
    mov     x9, x21
.Lscan_envp:
    ldr     x10, [x9], #8
    cbnz    x10, .Lscan_envp
    mov     x22, x9

    /* __sel4runtime_load_env(argc, argv, envp, auxv) */
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    mov     x3, x22
    bl      __sel4runtime_load_env

    /* __aios_entry(argc, argv, envp) */
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    bl      __aios_entry

    /* sel4runtime_exit(retval) -- x0 has return value */
    bl      sel4runtime_exit

    /* unreachable */
1:  b       1b
