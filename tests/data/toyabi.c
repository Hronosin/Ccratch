/* The toy library's binary: what it really exports. toy_printf is in the
 * header, but not here; toy_secret is here, but static. */
#include "toy.h"

static int toy_secret(void) { return 7; }

void draw_dot(int x, int y) { (void)x; (void)y; (void)toy_secret(); }
bool is_ready(void) { return true; }
