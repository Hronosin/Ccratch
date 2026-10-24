/* A finding inside asm() lands on the line of its own instruction, with
 * comments between the strings (tests/CMakeLists.txt). */
long child(long x)
{
    __asm__ volatile(
        /* first, the syscall
         * and what it takes
         */
        "syscall\n"

        /* then this: line 12 */
        "xorq %%rbp, %%rbp\n"
        : "+a"(x)
        :
        : "rcx", "r11", "memory");
    return x;
}
