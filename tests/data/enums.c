#include <stdbool.h>
enum dir { UP, DOWN, LEFT, RIGHT };
typedef enum { RED, GREEN } colour;
typedef enum shape { CIRCLE, SQUARE } shape;
struct thing { enum dir d; colour c; };
static void go(enum dir d, colour c, bool fast) { (void)d; (void)c; (void)fast; }
static colour pick(void) { return RED; }
int main(void)
{
    enum dir d = UP;
    struct thing t;
    shape s = CIRCLE;
    go(d, GREEN, true);
    if (d == DOWN) d = LEFT;
    t.c = pick();
    switch (s) { case CIRCLE: break; default: break; }
    return 0;
}
