/* Everything Code Hater says about asm() in C, once. */
#include <stdint.h>

int sneaky(int x)
{
    __asm__ volatile("movl $1, %%ecx\n\t"
                     "addl %%ecx, %0"
                     : "+r"(x));
    return x;
}

void store(int *p)
{
    __asm__("movl $0, (%0)" : : "r"(p));
}

uint64_t ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int honest(int x)
{
    __asm__("movl $1, %%ecx\n\taddl %%ecx, %0" : "+r"(x) : : "ecx", "cc");
    return x;
}
