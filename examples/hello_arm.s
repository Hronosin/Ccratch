@ hello_arm.s - counting to five on 32-bit ARM (Raspberry Pi OS 32-bit,
@ older phones), GNU as, unified syntax.
@ Build on one:     cc hello_arm.s -o hello && ./hello
@ From an x86 PC:   arm-linux-gnueabihf-gcc hello_arm.s -o hello &&
@                   qemu-arm -L /usr/arm-linux-gnueabihf ./hello

        .syntax unified
        .arch   armv7-a

        .section .rodata
fmt:
        .asciz  "%d\n"                  @ what printf gets: a number and a new line

        .text
        .globl  main
        .type   main, %function
main:
        push    {r4, lr}                @ r4 is the caller's, lr is the way back
        mov     r4, #1                  @ i = 1

.Lagain:
        ldr     r0, =fmt                @ printf(fmt, i)
        mov     r1, r4
        bl      printf
        add     r4, r4, #1              @ i++
        cmp     r4, #5
        ble     .Lagain                 @ again while i <= 5

        mov     r0, #0                  @ return 0
        pop     {r4, pc}                @ and back, through the saved lr
        .size   main, .-main

        .section .note.GNU-stack,"",%progbits
