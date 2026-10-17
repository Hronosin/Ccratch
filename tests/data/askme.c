/* Asks, then answers: the debugger's input, tested. */
#include <stdio.h>

int main(void)
{
    char name[64];
    printf("name? ");
    fflush(stdout);
    if (scanf("%63s", name) != 1)
        return 1;
    printf("hi %s\n", name);
    int n = 0;
    while (scanf("%d", &n) == 1)
        printf("got %d\n", n);
    printf("the end\n");
    return 0;
}
