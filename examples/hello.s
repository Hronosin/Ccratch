# hello.s - counting to five, in x86-64 assembly (GNU as, AT&T syntax).
# Build:  cc hello.s -o hello && ./hello
# AT&T says it source first, destination last: movl $0, %ebx puts 0 in ebx.

        .section .rodata
fmt:
        .string "%d\n"          # what printf gets: a number and a new line

        .text
        .globl  main
        .type   main, @function
main:
        pushq   %rbx            # rbx is ours to keep: save it
        movl    $1, %ebx        # i = 1

.Lagain:
        leaq    fmt(%rip), %rdi # printf(fmt, i)
        movl    %ebx, %esi
        xorl    %eax, %eax      # no vector registers used
        call    printf@PLT
        addl    $1, %ebx        # i++
        cmpl    $5, %ebx
        jle     .Lagain         # again while i <= 5

        popq    %rbx
        xorl    %eax, %eax      # return 0
        ret
        .size   main, .-main

        .section .note.GNU-stack,"",@progbits
