/* Code Hater once said something here, and was wrong every time
 * (tests/CMakeLists.txt): it must have nothing to say. */
#include "no_such_header.h"   /* the tree loses whatever uses what it declares */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct header { int len; };

/* What memcpy gives back is d: that is returning d. */
char *dup_n(const char *s, size_t l)
{
    char *d = malloc(l + 1);
    if (!d)
        return NULL;
    return memcpy(d, s, l + 1);
}

/* The memory goes out as map + 1. */
void *with_header(size_t len)
{
    struct header *map = malloc(sizeof *map + len);
    void *addr;
    if (!map)
        return NULL;
    map->len = (int)len;
    addr = map + 1;
    return addr;
}

/* A header and then the bytes: not "len things of that type". */
char *packet(size_t len)
{
    char *buf = malloc(sizeof(struct header) + len);
    if (!buf)
        return NULL;
    return buf;
}

/* Out of a ?:, still returned. */
char *maybe(int ok)
{
    char *p = malloc(16);
    if (!p)
        return NULL;
    return ok ? p : NULL;
}

/* free(h->buf) of a struct the missing header has: not free(h). */
void close_it(struct hidden *h)
{
    free(h->buf);
    free(h);
}

/* t = ... that clang could not read is still t = ...: alive again. */
void reuse(struct hidden *h)
{
    char *t = malloc(4);
    if (!t)
        return;
    free(t);
    t = hidden_text(h->name);
    puts(t);
    free(t);
}

/* Freed on the way out, freed again under a label nobody falls into. */
int cleanup(size_t n)
{
    char *hats = malloc(n);
    if (!hats)
        return -1;
    if (n > 4)
        goto oom;
    free(hats);
    return 0;
oom:
    free(hats);
    return -1;
}

/* Written again in a block further down, before it is read. */
void again(int x)
{
    char *o = malloc(8);
    if (!o)
        return;
    free(o);
    if (x) {
        o = malloc(8);
        if (o)
            puts(o);
        free(o);
    }
}

/* A macro's if: its condition is in the macro. */
#define FIX(x) do { if ((x) >= HIDDEN_LIMIT) (x) = HIDDEN_LIMIT; } while (0)
void limits(long *v)
{
    FIX(*v);
}

/* Borrowed references (Python), a bignum's borrow, AES key unwrap, a
 * game's loot: all just C. And my C is a bit rusty. */
void *get_item_borrow(void *list, long index);
int aes_unwrap_key(const unsigned char *in, unsigned char *out);
unsigned long sub_with_borrow(unsigned long a, unsigned long b, unsigned long *borrow);
int rusty_sword_damage(int level);

/* asm() for AArch64: %w0 is an operand, not x86's eax */
int add3(int a, int b)
{
    int r;
    __asm__("add %w0, %w1, %w2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* clobbers after a comment with a colon in it */
long fence(long x)
{
    __asm__ volatile(
        /* say: nothing */
        // XXX: a comment with a colon
        "movq $1, %%r11\n"
        : "+r"(x)
        :
        : "r11", "memory");
    return x;
}
