/* Several names in one declaration, outside functions and in structs:
 * every body rebuilt, the file must still read the same, and nothing is
 * refused as "overlapping code" (tests/CMakeLists.txt). */
#include <stddef.h>

struct p { int v; } one, two;
static int a, b = 2;
struct s { int x, y; char *name, *nick; };
typedef int A, *B;
enum { RED, GREEN } colour;
typedef struct point { int x; } point;

int f(void)
{
    struct s it = { 1, 2, NULL, NULL };
    A k = 1;
    B pk = &k;
    if (colour == RED)
        return one.v + two.v;
    return a + b + it.x + it.y + *pk;
}
