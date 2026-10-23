/* Code Hater must have nothing to say here (tests/CMakeLists.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *copy(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    return d;
}

int pick(int x)
{
    int y;
    if ((y = x * 2))
        return y;
    if (x == 1) {
        return 1;
    } else if (x == 2) {
        return 2;
    } else if (x == 3) {
        return 3;
    } else if (x == 4) {
        if (x)
            return 4;
    }
    return 0;
}

void list_free(int *p)
{
    free(p);
    p = NULL;
    (void)p;
}

int main(void)
{
    char *s = copy("trust the crust");
    if (!s)
        return 1;
    printf("%s\n", s);
    free(s);
    return pick(2) == 4 ? 0 : 1;
}
