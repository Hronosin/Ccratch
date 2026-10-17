/*
 * ccr_choice - what a slot can be picked from.
 *
 * A slot that takes an enum (a library's KeyboardKey, a file's own
 * `enum dir`) or a bool gets a list to pick from when it is clicked:
 * the slot of a library block (its pack's `choices` and `enums`), an
 * argument of the file's own function, the other side of `==`, `!=` or
 * `=`, a variable's first value, a `case` in a switch over an enum, the
 * value a function returns.
 */
#ifndef CCR_CHOICE_H
#define CCR_CHOICE_H

#include "ccr_ast.h"

#define CCR_CHOICES_MAX 256

typedef struct ccr_choices {
    const char *v[CCR_CHOICES_MAX];   /* into store */
    int         n;
    char        title[64];            /* the list's name: KeyboardKey, enum dir */
    char        store[CCR_CHOICES_MAX * 48];
    int         used;
} ccr_choices;

/* The values slot `slot` of `ast` can take; false (n == 0) if it is not
 * a slot with a list. */
bool ccr_slot_choices(const ccr_ast *ast, ccr_node_id slot, ccr_choices *out);

/* The values of C type `type` ("enum dir", a typedef of one, bool): the
 * file's own enums first, then the block packs'. */
bool ccr_type_choices(const ccr_ast *ast, const char *type, ccr_choices *out);

#endif /* CCR_CHOICE_H */
