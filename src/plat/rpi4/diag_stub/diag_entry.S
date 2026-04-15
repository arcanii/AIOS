.section .text.entry, "ax"
.globl _start
_start:
    /* Save firmware-provided registers (DTB addr etc) */
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3

    /* Park secondary cores -- restore firmware spin-table protocol.
     * Each secondary spins reading its cpu-release-addr until non-zero,
     * then jumps to the written address. This matches the DTS
     * enable-method "spin-table" that the seL4 elfloader expects.
     *
     * cpu-release-addr per core (from rpi4.dts):
     *   core 1: 0xd8    core 2: 0xe0    core 3: 0xe8
     * Core 0 (mpidr & 3 == 0) falls through to diag_main. */
    mrs x1, mpidr_el1
    and x1, x1, #3
    cbnz x1, spin_table_park

    /* Disable D-cache before mailbox (new EEPROM enables it) */
    mrs x0, sctlr_el2
    bic x0, x0, #4         /* bit 2: C = D-cache off */
    msr sctlr_el2, x0
    isb

    /* Set stack and call C entry */
    ldr x1, =_stack_top
    mov sp, x1
    bl diag_main

hang:
    b hang

spin_table_park:
    /* x1 = core number (1, 2, or 3).
     * cpu-release-addr = 0xd0 + core * 8
     * (core 1 = 0xd8, core 2 = 0xe0, core 3 = 0xe8) */
    lsl x2, x1, #3         /* x2 = core * 8 */
    add x2, x2, #0xd0      /* x2 = cpu-release-addr */

    /* Clear the release address (ensure we spin) */
    str xzr, [x2]
    dsb sy

1:  /* Spin until release address is non-zero */
    wfe
    ldr x3, [x2]
    cbz x3, 1b

    /* Jump to the address written by elfloader */
    dsb sy
    isb
    br x3

.section .bss
.align 4
_stack_bottom:
    .space 8192
_stack_top:
