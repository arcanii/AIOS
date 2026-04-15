.section .text
.global _start
_start:
    b relocate
    .word 0
    .quad 0
    .quad 1361376
    .quad 0
    .quad 0
    .quad 0
    .word 0x644d5241
    .word 0
    .quad 0
relocate:
    mov x20, x0
    msr daifset, #0xf
    adr x5, _start
    mov x0, x5
    add x0, x0, #4096
    movz x1, #0xc000
    movk x1, #0x370, lsl #16
    movz x2, #0xb5e0
    movk x2, #0x14, lsl #16
    mov x10, x1
1:  ldp x3, x4, [x0], #16
    stp x3, x4, [x1], #16
    subs x2, x2, #16
    b.gt 1b
    dsb sy
    isb
    ic iallu
    dsb sy
    isb
    mov x0, x20
    br x10
.balign 4096
