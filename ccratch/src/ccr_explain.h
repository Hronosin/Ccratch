/*
 * ccr_explain - what each block in the palette is for and how to use it,
 * for the card that slides out when the mouse rests on one.
 *
 * The words come from lang/CODE.explain (found where language packs are):
 *
 *     if.what = Does the blocks inside only when the condition is true.
 *     if.how  = Put a green condition into its slot, blocks into its mouth.
 *
 * English (en.explain) first, the language over it, then its plain words
 * (CODE-plain.explain) over that: whatever a file leaves out, the one
 * under it says. The example under the words is C (or CMake), the same in
 * every language, and built in.
 */
#ifndef CCR_EXPLAIN_H
#define CCR_EXPLAIN_H

#include "ccr_template.h"

#include <stdbool.h>
#include <stdio.h>

/* Reads the explanations for language `code` ("ru"), with its plain words
 * if `plain`. Again whenever the language changes. */
void ccr_explain_load(const char *code, bool plain);

/* What the block does, how to use it in Ccratch ("" if no file says), and
 * a few lines of code that use it ("" for a pack's block). */
const char *ccr_explain_what(ccr_template t);
const char *ccr_explain_how(ccr_template t);
const char *ccr_explain_example(ccr_template t);

/* The key of a built-in block in .explain files: "if", "if_else"... */
const char *ccr_explain_key(ccr_template t);

/* Every code point above ASCII in what is loaded (for the font). */
int ccr_explain_codepoints(int *out, int cap);

/* --check-explain FILE: unknown keys, bad lines; with `complete`, the
 * blocks it says nothing of too. The number of problems. */
int ccr_explain_check(const char *path, bool complete, FILE *out);

void ccr_explain_free(void);

#endif /* CCR_EXPLAIN_H */
