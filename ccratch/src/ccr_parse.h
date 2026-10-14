/*
 * ccr_parse - the only place in Ccratch that knows libclang exists.
 *
 * Turns a C file into a ccr_ast. Only nodes from the file itself are kept;
 * the thousands of declarations pulled in by headers are skipped.
 */
#ifndef CCR_PARSE_H
#define CCR_PARSE_H

#include "ccr_ast.h"

/*
 * Parses `path` into `out` (which must be initialized).
 * `args` are passed to clang as-is; when NULL/empty, a sensible default is
 * used (-std=c11 for C, -std=c++20 for C++ by file extension).
 *
 * Returns false only if clang could not produce a translation unit at all.
 * Syntax errors still return true; check out->diags.
 */
bool ccr_parse_file(const char *path, const char *const *args, int arg_count, ccr_ast *out);

/*
 * Where clang's builtin headers (stddef.h, stdarg.h...) are, or NULL to let
 * libclang guess. Checked in order:
 *   1. $CCRATCH_CLANG_RESOURCE_DIR
 *   2. searched at run time next to the loaded libclang, same version
 *   3. the directory found at build time
 *   4. the newest of any version on the system
 * Passed to clang as -resource-dir unless the user gave one.
 * `how`, if not NULL, receives a short human explanation.
 */
const char *ccr_parse_resource_dir(const char **how);

/* "clang version 22.1.8 ..." of the libclang actually loaded. */
void ccr_parse_clang_version(char *out, unsigned int cap);

#endif /* CCR_PARSE_H */
