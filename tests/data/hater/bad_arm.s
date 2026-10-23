@ Everything Code Hater says about 32-bit ARM, once.
        .syntax unified
        .text
        .globl  f
        .type   f, %function
f:
        push    {lr}
        mov     r4, r0
        bl      g
        pop     {pc}

        .globl  g
        .type   g, %function
g:
        push    {r4, r5}
        mov     r4, #1
        bx      lr
