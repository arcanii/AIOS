.section .text.entry, "ax"
.globl _start
_start:
    /* Save firmware-provided registers (DTB addr etc) */
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3

    /* Park secondary cores */
    mrs x1, mpidr_el1
    and x1, x1, #3
    cbnz x1, park

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

park:
    wfe
    b park

.section .bss
.align 4
_stack_bottom:
    .space 8192
_stack_top:
