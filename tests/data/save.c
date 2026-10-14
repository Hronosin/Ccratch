/* save.c: everything saving must keep intact */
#include <stdio.h>

#define TWICE(x) ((x) * 2)

static int counter = 0;   // file-level comment

int work(int n)
{
    // first: say hello
    printf("hello\n");
    int a = 1, b = 2;  /* group stays together */
    struct pair { int x; int y; } pr = { a, b };

    /* bump it */
    counter += TWICE(n);

    if (n > 3)
        n--;          // braceless then
    else if (n < 0)
        n = 0;
    else
        n++;

    for (int i = 0; i < n; i++)
        a += i;

    while (b < 10) {
        b++;
        // nothing else here
    }

    do b--; while (b > 5);

    return a + b + pr.x + pr.y;
}

int guarded(int v)
{
    v++;
#ifdef EXTRA
    v *= 2;
#endif
    v--;
    return v;
}

int main(void)
{
    printf("%d %d\n", work(5), guarded(1));
    return 0;
}
