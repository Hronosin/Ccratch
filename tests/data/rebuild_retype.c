/* Expressions typed back exactly as they were: no parentheses they did not
 * have (sizeof, L"", casts after casts, glued strings, .5) (tests/CMakeLists.txt). */
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

static char buf[16];

int f(int z, double x)
{
    size_t n;
    const wchar_t *w;
    void *q;
    const char *s;
    double d;
    int y;
    n = sizeof "rounds=" - 1;
    w = L"@";
    y = (int)sizeof buf - 2;
    q = (void *)(uintptr_t)z;
    s = "glued " "together";
    d = .5 - x;
    n = sizeof buf / sizeof *buf;
    return (int)n + (int)d + y + (q != NULL) + (w != NULL) + (s != NULL);
}
