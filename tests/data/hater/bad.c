/* Code Hater must say something about every function here (tests/CMakeLists.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct node { int v; struct node *next; };

int *dangling(void)
{
    int x = 5;
    return &x;
}

char *grow(char *buf, size_t n)
{
    buf = realloc(buf, n);
    return buf;
}

void leak(void)
{
    char *s = malloc(32);
    strcpy(s, "hello");
    printf("%s\n", s);
}

void twice(int *p)
{
    free(p);
    free(p);
}

int after(void)
{
    int *q = malloc(sizeof(int));
    *q = 1;
    free(q);
    return *q;
}

int main(void)
{
    char line[64];
    gets(line);
    int y;
    if (y = 3) {
        if (y) {
            if (y) {
                if (y) {
                    sprintf(line, "%d", y);
                }
            }
        }
    }
    return 0;
}

/* TODO: rewrite it in Rust, it would be blazingly fast */
static int unwrap_or(int *v, int fallback)
{
    return v ? *v : fallback;
}

int vla(int n)
{
    int vals[n];
    for (int i = 0; i < n; i++)
        vals[i] = i;
    return vals[0];
}

int again(int n)
{
top:
    if (--n > 0)
        goto top;
    return n;
}

char *local_array(void)
{
    char buf[16] = "temp";
    return buf;
}

int *casted(void)
{
    int *p = (int *)malloc(sizeof *p);
    return p;
}
