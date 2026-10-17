/* A program for the debugger tests: lines below are named in test_gdb.c */
#include <stdio.h>

struct pair { int a, b; };

static int sq(int v)
{
    int r = v * v;          /* line 8 */
    return r;
}

int main(void)
{
    int total = 0;
    struct pair p = { 1, 2 };
    for (int i = 0; i < 3; i++) {
        total += sq(i);     /* line 17 */
        printf("i=%d total=%d\n", i, total);
    }
    p.a = total;
    return total - 5 + p.b - 2;   /* 5 - 5 + 2 - 2 = 0 */
}
