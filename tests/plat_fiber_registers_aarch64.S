/* Check actual AAPCS64-preserved registers across SwitchToFiber. */
#if defined(__aarch64__)
.text
.p2align 2
.global wt_test_switch_preserved
.type wt_test_switch_preserved, %function
wt_test_switch_preserved:
    hint #34
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #144
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8, d9, [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]
    mov x19, #119
    mov x20, #120
    mov x21, #121
    mov x22, #122
    mov x23, #123
    mov x24, #124
    mov x25, #125
    mov x26, #126
    mov x27, #127
    mov x28, #128
    mov x29, #129
    mov x9, #208
    fmov d8, x9
    mov x9, #209
    fmov d9, x9
    mov x9, #210
    fmov d10, x9
    mov x9, #211
    fmov d11, x9
    mov x9, #212
    fmov d12, x9
    mov x9, #213
    fmov d13, x9
    mov x9, #214
    fmov d14, x9
    mov x9, #215
    fmov d15, x9
    bl SwitchToFiber
    cmp x19, #119
    b.ne .Lfailed
    cmp x20, #120
    b.ne .Lfailed
    cmp x21, #121
    b.ne .Lfailed
    cmp x22, #122
    b.ne .Lfailed
    cmp x23, #123
    b.ne .Lfailed
    cmp x24, #124
    b.ne .Lfailed
    cmp x25, #125
    b.ne .Lfailed
    cmp x26, #126
    b.ne .Lfailed
    cmp x27, #127
    b.ne .Lfailed
    cmp x28, #128
    b.ne .Lfailed
    cmp x29, #129
    b.ne .Lfailed
    fmov x9, d8
    cmp x9, #208
    b.ne .Lfailed
    fmov x9, d9
    cmp x9, #209
    b.ne .Lfailed
    fmov x9, d10
    cmp x9, #210
    b.ne .Lfailed
    fmov x9, d11
    cmp x9, #211
    b.ne .Lfailed
    fmov x9, d12
    cmp x9, #212
    b.ne .Lfailed
    fmov x9, d13
    cmp x9, #213
    b.ne .Lfailed
    fmov x9, d14
    cmp x9, #214
    b.ne .Lfailed
    fmov x9, d15
    cmp x9, #215
    b.ne .Lfailed
    mov w0, #1
    b .Lrestore
.Lfailed:
    mov w0, #0
.Lrestore:
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp d8, d9, [sp, #80]
    ldp d10, d11, [sp, #96]
    ldp d12, d13, [sp, #112]
    ldp d14, d15, [sp, #128]
    add sp, sp, #144
    ldp x29, x30, [sp], #16
    ret
.size wt_test_switch_preserved, .-wt_test_switch_preserved
#endif
.section .note.GNU-stack,"",%progbits
