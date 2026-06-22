/* E1 bare-metal stall reproducer -- MULTI-CORE variant (RPi4 / BCM2711 / A72).
 *
 * Single-core EL2 did NOT reproduce the freeze (trial 1). This variant brings
 * cores 1-3 into the coherency domain (SMPEN + shared MMU) so the cluster runs
 * like AIOS's 4-core state, then core 0 runs the same quiesce+timed-op test while
 * the secondaries sit idle (WFE) IN the coherency domain. Tests whether the
 * multi-core coherency/DVM domain is what makes the post-idle fabric op stall.
 *
 * Boot: firmware accepts the "ARMd" magic (at 0x30 AND 0x38), loads + jumps at EL2.
 * Cores 1-3 are held by the firmware spin-table; core 0 releases them.
 */
.section .text.boot, "ax"
.global _start
_start:
    b   _entry              /* 0x00: branch over the 64-byte image header     */
    .word 0                 /* 0x04                                           */
    .quad 0                 /* 0x08                                           */
    .quad 0                 /* 0x10: image size (firmware ignores for us)     */
    .quad 0                 /* 0x18                                           */
    .quad 0                 /* 0x20                                           */
    .quad 0                 /* 0x28                                           */
    .word 0x644d5241        /* 0x30: magic "ARMd" (AIOS/RPi firmware sniff)    */
    .word 0                 /* 0x34                                           */
    .word 0x644d5241        /* 0x38: magic "ARMd" (standard arm64 Linux hdr)  */
    .word 0                 /* 0x3C                                           */
_entry:                     /* 0x40 -- core 0 (firmware starts only core 0 here) */
    msr   daifset, #0xf

    /* clean slate: MMU + caches off (we install our own mapping in mmu_on) */
    mrs   x0, sctlr_el2
    bic   x0, x0, #(1 << 0)
    bic   x0, x0, #(1 << 2)
    bic   x0, x0, #(1 << 12)
    msr   sctlr_el2, x0
    isb

    /* zero .bss */
    adrp  x0, __bss_start
    add   x0, x0, #:lo12:__bss_start
    adrp  x1, __bss_end
    add   x1, x1, #:lo12:__bss_end
1:  cmp   x0, x1
    b.ge  2f
    str   xzr, [x0], #8
    b     1b
2:
    /* Release cores 1-3 from the firmware spin-table. Write secondary_entry to
     * all plausible release slots (0xd8/e0/e8/f0 covers both the 0xd0+N*8 and
     * 0xd8+N*8 armstub conventions), then SEV. MMU is off, so these are direct
     * physical writes coherent with the secondaries' (also MMU-off) reads. */
    adrp  x2, secondary_entry
    add   x2, x2, #:lo12:secondary_entry
    mov   x3, #0xd8
    str   x2, [x3]
    mov   x3, #0xe0
    str   x2, [x3]
    mov   x3, #0xe8
    str   x2, [x3]
    mov   x3, #0xf0
    str   x2, [x3]
    dsb   sy
    sev

    /* core 0 stack + run */
    adrp  x0, _stack_top
    add   x0, x0, #:lo12:_stack_top
    mov   sp, x0
    bl    repro_main
3:  wfe
    b     3b

/* Cores 1-3 land here from the spin-table (MMU off, EL2). */
.global secondary_entry
secondary_entry:
    msr   daifset, #0xf
    mrs   x0, sctlr_el2
    bic   x0, x0, #(1 << 0)
    bic   x0, x0, #(1 << 2)
    bic   x0, x0, #(1 << 12)
    msr   sctlr_el2, x0
    isb
    /* per-core stack: _sec_stacks_base + core*0x4000 + 0x4000 (top of slot) */
    mrs   x1, mpidr_el1
    and   x1, x1, #3
    adrp  x0, _sec_stacks_base
    add   x0, x0, #:lo12:_sec_stacks_base
    mov   x2, #0x4000
    madd  x0, x1, x2, x0
    add   x0, x0, x2
    mov   sp, x0
    mov   x0, x1                /* arg0 = core id */
    bl    secondary_main
4:  wfe                         /* idle IN the coherency domain (SMPEN stays on) */
    b     4b
