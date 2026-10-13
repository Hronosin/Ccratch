/* A little tour of C, used to check that every kind of node turns
 * into something sensible. If a construct shows up as "Other" in
 * `ccratch --dump-ast examples/tour.c`, it has no block yet. */

#include <stdio.h>
#include <stdlib.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_LEN 4

typedef struct point {
    int x;
    int y;
} point;

enum mood { CALM, ANNOYED, INCIDENT };

static int sum(const int *values, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
        total += values[i];
    return total;
}

static const char *describe(enum mood m)
{
    switch (m) {
    case CALM:
        return "drinking tea";
    case ANNOYED:
        return "donation box is empty";
    default:
        break;
    }
    return "incident!";
}

int main(void)
{
    int numbers[ARRAY_LEN] = { 3, 1, 4, 1 };
    point *p = malloc(sizeof *p);
    if (!p)
        return 1;

    p->x = MAX(numbers[0], numbers[2]);
    p->y = -sum(numbers, ARRAY_LEN);

    int countdown = 3;
    while (countdown > 0) {
        --countdown;
        if (countdown == 1)
            continue;
    }

    do {
        countdown++;
    } while (countdown < 2);

    printf("%s: (%d, %d) %.1f %c\n", describe(INCIDENT), p->x, p->y, (double)p->x / 2.0, 'R');

    free(p);
    goto done;
done:
    return 0;
}
