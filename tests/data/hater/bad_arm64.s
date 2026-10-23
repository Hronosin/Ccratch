// Everything Code Hater says about AArch64, once.
        .text
        .globl  f
        .type   f, %function
f:
        mov     x19, x0
        bl      g
        ret

        .globl  g
        .type   g, %function
g:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        ldp     x29, x30, [sp, #0]
        ret
        add     x0, x0, #1

        .globl  h
        .type   h, %function
h:
        add     x0, x0, #1
