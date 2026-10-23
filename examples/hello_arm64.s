// hello_arm64.s - counting to five on 64-bit ARM (AArch64: phones, Apple
// M chips, Raspberry Pi 4 and 5), GNU as.
// Build on one:     cc hello_arm64.s -o hello && ./hello
// From an x86 PC:   aarch64-linux-gnu-gcc hello_arm64.s -o hello &&
//                   qemu-aarch64 -L /usr/aarch64-linux-gnu ./hello
// ARM says the destination first: mov w19, #1 puts 1 in w19.

        .section .rodata
fmt:
        .string "%d\n"                  // what printf gets: a number and a new line

        .text
        .globl  main
        .type   main, %function
main:
        stp     x29, x30, [sp, #-32]!   // save the frame and the way back (x30)
        mov     x29, sp
        str     x19, [sp, #16]          // x19 is the caller's: save it too
        mov     w19, #1                 // i = 1

.Lagain:
        adrp    x0, fmt                 // printf(fmt, i)
        add     x0, x0, :lo12:fmt
        mov     w1, w19
        bl      printf
        add     w19, w19, #1            // i++
        cmp     w19, #5
        b.le    .Lagain                 // again while i <= 5

        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        mov     w0, #0                  // return 0
        ret
        .size   main, .-main

        .section .note.GNU-stack,"",%progbits
