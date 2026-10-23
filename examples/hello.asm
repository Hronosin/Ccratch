; hello.asm - "Hello, world!" three times, no C library at all (NASM, Linux x86-64).
; Build:  nasm -f elf64 hello.asm && ld hello.o -o hello && ./hello

        global  _start

        section .text
_start:
        mov     r12, 3              ; three times
.again:
        mov     rax, 1              ; write(1, msg, len)
        mov     rdi, 1
        lea     rsi, [rel msg]
        mov     rdx, len
        syscall
        dec     r12
        jnz     .again

        mov     rax, 60             ; exit(0)
        xor     edi, edi
        syscall

        section .data
msg     db      "Hello, world!", 10
len     equ     $ - msg
