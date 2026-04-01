/* aarch64 setjmp/longjmp for AIOS sandbox kernel
 *
 * Extended jmp_buf layout (96 x 8 = 768 bytes):
 *
 *   Integer callee-saved (offsets 0-96):
 *     [0]  x19   [1]  x20   [2]  x21   [3]  x22
 *     [4]  x23   [5]  x24   [6]  x25   [7]  x26
 *     [8]  x27   [9]  x28   [10] x29 (fp)
 *     [11] x30 (lr)         [12] sp
 *
 *   FP callee-saved (offsets 104-168) — kept for ABI compat:
 *     [13] d8    [14] d9    [15] d10   [16] d11
 *     [17] d12   [18] d13   [19] d14   [20] d15
 *     [21] reserved
 *
 *   FPCR/FPSR (offsets 176-184):
 *     [22] fpcr  [23] fpsr
 *
 *   Full SIMD q0-q31 (offsets 192-704, 512 bytes):
 *     [24..55] q0-q15   [56..87] q16-q31
 *     Each q-register is 128 bits = 2 x uint64_t
 *
 *   Total: 88 used slots, buffer sized to 96 for alignment
 */

.global setjmp
.type setjmp, %function
setjmp:
    /* Integer callee-saved */
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]

    /* FP callee-saved (d8-d15, backward compat) */
    stp d8,  d9,  [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]

    /* FPCR + FPSR */
    mrs x2, fpcr
    str x2, [x0, #176]
    mrs x2, fpsr
    str x2, [x0, #184]

    /* Full SIMD state q0-q31 (preemptive safety) */
    stp q0,  q1,  [x0, #192]
    stp q2,  q3,  [x0, #224]
    stp q4,  q5,  [x0, #256]
    stp q6,  q7,  [x0, #288]
    stp q8,  q9,  [x0, #320]
    stp q10, q11, [x0, #352]
    stp q12, q13, [x0, #384]
    stp q14, q15, [x0, #416]
    stp q16, q17, [x0, #448]
    stp q18, q19, [x0, #480]
    stp q20, q21, [x0, #512]
    stp q22, q23, [x0, #544]
    stp q24, q25, [x0, #576]
    stp q26, q27, [x0, #608]
    stp q28, q29, [x0, #640]
    stp q30, q31, [x0, #672]

    mov w0, #0
    ret

.global longjmp
.type longjmp, %function
longjmp:
    /* Integer callee-saved */
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2

    /* FP callee-saved */
    ldp d8,  d9,  [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]

    /* FPCR + FPSR */
    ldr x2, [x0, #176]
    msr fpcr, x2
    ldr x2, [x0, #184]
    msr fpsr, x2

    /* Full SIMD state q0-q31 */
    ldp q0,  q1,  [x0, #192]
    ldp q2,  q3,  [x0, #224]
    ldp q4,  q5,  [x0, #256]
    ldp q6,  q7,  [x0, #288]
    ldp q8,  q9,  [x0, #320]
    ldp q10, q11, [x0, #352]
    ldp q12, q13, [x0, #384]
    ldp q14, q15, [x0, #416]
    ldp q16, q17, [x0, #448]
    ldp q18, q19, [x0, #480]
    ldp q20, q21, [x0, #512]
    ldp q22, q23, [x0, #544]
    ldp q24, q25, [x0, #576]
    ldp q26, q27, [x0, #608]
    ldp q28, q29, [x0, #640]
    ldp q30, q31, [x0, #672]

    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret

.global sigsetjmp
.type sigsetjmp, %function
sigsetjmp:
    str w1, [x0, #752]
    b setjmp

.global siglongjmp
.type siglongjmp, %function
siglongjmp:
    b longjmp
