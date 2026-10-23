# Everything Code Hater says about assembly files, once.
        .text
        .globl  f
        .type   f, @function
f:
        pushq   %rbx
        movl    $0, %eax
        movq    %rdi, %rbx
        ret
        addl    $1, %eax

        .globl  g
        .type   g, @function
g:
        movq    $5, %r12
        movl    %edi, %eax
