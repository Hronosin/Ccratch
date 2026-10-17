/*
 * ccr_paste - code from the clipboard, as blocks.
 *
 * Ctrl+C on a block puts its C (or CMake) on the clipboard; Ctrl+V reads
 * whatever C the clipboard holds (from another file, a browser, a chat)
 * and hands it to the editor as a stack of blocks to drop somewhere.
 *
 * The code is read inside the file it goes into, as the body of a made-up
 * function at its end: the file's own types, functions and includes are
 * known, so `draw_dot(x, y);` reads as a call to draw_dot. What clang
 * still cannot read (a variable from the function it was copied out of)
 * is not lost: it becomes code lines, the text exactly as it was.
 */
#ifndef CCR_PASTE_H
#define CCR_PASTE_H

#include "ccr_ast.h"

#include <stdbool.h>

#define CCR_SNIPPET_MAX 512

typedef struct ccr_snippet {
    ccr_ast     ast;                         /* the file with the code in it */
    ccr_node_id items[CCR_SNIPPET_MAX];      /* the statements, in order */
    int         count;
    bool        reporter;                    /* one expression, for a slot */
    bool        toplevel;                    /* functions, types, #includes: for the file */
    int         as_text;                     /* pieces kept as code lines */
} ccr_snippet;

/* Reads `code` as it would read at the end of the file `path` whose text
 * is `context` (may be NULL). `cmake`: it is CMake. False if nothing came
 * of it (empty, or clang could not start). */
bool ccr_snippet_parse(const char *code, const char *context, size_t context_len, const char *path, bool cmake,
                       const char *const *clang_args, int nargs, ccr_snippet *out);
void ccr_snippet_free(ccr_snippet *s);

/* What Ctrl+C puts on the clipboard: the code of `nodes` as saving would
 * write it, in file order, a line apart, their own indentation taken off
 * (malloc'd; NULL if none of them is in the file: a loose block made in
 * the editor that was never saved has no text yet). */
char *ccr_copy_text(const ccr_ast *ast, const ccr_node_id *nodes, int count);

#endif /* CCR_PASTE_H */
