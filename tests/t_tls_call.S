// Call a TLS descriptor exactly the way guest code does, with the whole
// caller-saved register file poisoned first, and report what came back.
//
//   int64_t kl_test_tlsdesc_call(void *desc, uint64_t *out);
//
// out[0..14] = x1..x15 after the call, out[15] = NZCV, out[16..47] = d0..d31.
// The return value is x0 — the TP-relative offset the resolver produced.
.text
    .globl _kl_test_tlsdesc_call
    .p2align 2
_kl_test_tlsdesc_call:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    mov     x19, x1                     // out
    mov     x20, x0                     // desc

    // Poison x1-x15 and d0-d31 with values the resolver must not disturb.
    movz    x1, #0x1111, lsl #48
    movk    x1, #0x1111, lsl #32
    add     x2,  x1, #2
    add     x3,  x1, #3
    add     x4,  x1, #4
    add     x5,  x1, #5
    add     x6,  x1, #6
    add     x7,  x1, #7
    add     x8,  x1, #8
    add     x9,  x1, #9
    add     x10, x1, #10
    add     x11, x1, #11
    add     x12, x1, #12
    add     x13, x1, #13
    add     x14, x1, #14
    add     x15, x1, #15
    add     x1,  x1, #1                 // ...so xN == base + N for every one
    fmov    d0,  x1
    fmov    d1,  x2
    fmov    d2,  x3
    fmov    d3,  x4
    fmov    d4,  x5
    fmov    d5,  x6
    fmov    d6,  x7
    fmov    d7,  x8
    fmov    d8,  x9
    fmov    d9,  x10
    fmov    d10, x11
    fmov    d11, x12
    fmov    d12, x13
    fmov    d13, x14
    fmov    d14, x15
    fmov    d15, x1
    fmov    d16, x2
    fmov    d17, x3
    fmov    d18, x4
    fmov    d19, x5
    fmov    d20, x6
    fmov    d21, x7
    fmov    d22, x8
    fmov    d23, x9
    fmov    d24, x10
    fmov    d25, x11
    fmov    d26, x12
    fmov    d27, x13
    fmov    d28, x14
    fmov    d29, x15
    fmov    d30, x1
    fmov    d31, x2
    // A known flag state: 0x1 == V set, N/Z/C clear.
    cmp     x1, x1                      // Z=1, C=1
    // ...and the guest's own sequence.
    mov     x0, x20
    ldr     x16, [x20]                  // desc[0], the resolver
    blr     x16

    stp     x1,  x2,  [x19, #0]
    stp     x3,  x4,  [x19, #16]
    stp     x5,  x6,  [x19, #32]
    stp     x7,  x8,  [x19, #48]
    stp     x9,  x10, [x19, #64]
    stp     x11, x12, [x19, #80]
    stp     x13, x14, [x19, #96]
    str     x15,      [x19, #112]
    mrs     x1, nzcv
    str     x1,       [x19, #120]
    stp     d0,  d1,  [x19, #128]
    stp     d2,  d3,  [x19, #144]
    stp     d4,  d5,  [x19, #160]
    stp     d6,  d7,  [x19, #176]
    stp     d8,  d9,  [x19, #192]
    stp     d10, d11, [x19, #208]
    stp     d12, d13, [x19, #224]
    stp     d14, d15, [x19, #240]
    stp     d16, d17, [x19, #256]
    stp     d18, d19, [x19, #272]
    stp     d20, d21, [x19, #288]
    stp     d22, d23, [x19, #304]
    stp     d24, d25, [x19, #320]
    stp     d26, d27, [x19, #336]
    stp     d28, d29, [x19, #352]
    stp     d30, d31, [x19, #368]

    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
