/*
 * inline.c - assembly inside C. Each asm() is a block with its
 * instructions in its mouth: drag more in, take one out (on its own among
 * the C blocks it becomes an asm() of its own).
 */
#include <stdint.h>
#include <stdio.h>

/* The CPU's own clock: rdtsc puts it in edx:eax. */
static uint64_t cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* a + b, the long way round. */
static int add(int a, int b)
{
    int sum;
    __asm__("movl %1, %0\n\t"
            "addl %2, %0"
            : "=r"(sum)
            : "r"(a), "r"(b)
            : "cc");
    return sum;
}

int main(void)
{
    const uint64_t t0 = cycles();
    printf("2 + 3 = %d\n", add(2, 3));
    __asm__ volatile("pause");
    printf("that took %llu cycles\n", (unsigned long long)(cycles() - t0));
    return 0;
}
