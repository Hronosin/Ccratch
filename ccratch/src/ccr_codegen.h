/*
 * ccr_codegen - turns the (edited) syntax tree back into C source.
 *
 * It splices rather than prints: whatever was not touched is copied from
 * the original file byte for byte, comments, #ifdefs, macros and your
 * formatting included. Only Blocks whose statements were moved (flagged
 * CCR_FLAG_DIRTY) are rebuilt, out of their statements' original text.
 * Loose stacks on the canvas are not part of the program, so they are not
 * written at all.
 *
 * When a changed Block cannot be rebuilt without risk (a #if between its
 * statements, code made by a macro), it refuses and says why, rather
 * than write something subtly different.
 */
#ifndef CCR_CODEGEN_H
#define CCR_CODEGEN_H

#include "ccr_ast.h"

#include <stddef.h>

/*
 * On success returns true and a malloc'ed, NUL-terminated buffer in *out
 * (length in *out_len). On failure returns false with a reason in `err`.
 */
bool ccr_codegen(const ccr_ast *ast, char **out, size_t *out_len, char *err, size_t err_cap);

/*
 * Can expression `id` be taken out of the slot it fills (and something
 * else put there) and the file still be saved safely? Its parent, and
 * everything up to its statement, must have children that sit in its text
 * one after another; code made by a macro does not, and stays whole.
 */
bool ccr_codegen_can_move(const ccr_ast *ast, ccr_node_id id);

/* Can the operator of `id` (a BinaryOp or UnaryOp) be changed? Then it
 * is printed anew around its operands' own text, so it must not come from
 * a macro, and everything up to its function must be safe to rewrite. */
bool ccr_codegen_can_reprint(const ccr_ast *ast, ccr_node_id id);

#endif /* CCR_CODEGEN_H */
