/* For clang to turn into assembly for other machines (AArch64, ARM,
 * Thumb): no headers, so it compiles for any target without a sysroot. */
typedef unsigned long size_t;
int printf(const char *, ...);

struct node { int v; struct node *next; };
struct vec { float x, y, z; };

static int twice(int x) { return x * 2; }

int sum(const struct node *n)
{
    int s = 0;
    for (; n; n = n->next)
        s += twice(n->v);
    return s;
}

long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int collatz(unsigned long n)
{
    int k = 0;
    while (n != 1) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        ++k;
    }
    return k;
}

float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

const char *name_of(int day)
{
    switch (day) {
    case 0: return "mon";
    case 1: return "tue";
    case 2: return "wed";
    case 3: return "thu";
    case 4: return "fri";
    case 5: return "sat";
    default: return "sun";
    }
}

void fill(unsigned char *p, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; ++i)
        p[i] = (unsigned char)(v ^ (i & 0xFF));
}

int many(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    return a - b + c * d - e / (f | 1) + (g << 2) - (h >> 1) + i % (j | 1);
}

int main(void)
{
    struct vec u = { 1, 2, 3 }, w = { 4, 5, 6 };
    for (int i = 0; i < 5; ++i)
        printf("%d %ld %d %s\n", i, fib(i), collatz(i + 1), name_of(i));
    printf("%f %d\n", (double)dot(u, w), many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    return 0;
}
