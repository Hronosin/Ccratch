/*
 * ccr_cmake - CMake files as blocks.
 *
 * Reads CMakeLists.txt (and *.cmake) into the same ccr_ast C is read into,
 * with ast.lang = CCR_LANG_CMAKE, so the canvas, drag and drop, undo,
 * folding and saving work on it unchanged:
 *
 *     TranslationUnit                the file
 *       Function "CMakeLists.txt"    a hat: what CMake runs, top to bottom
 *         Block                      its commands
 *           Call "project"           a command; each argument a Text slot
 *           If                       if() ... elseif() ... else() ... endif()
 *             Text                   the condition, as written
 *             Block                  its commands
 *             Block                  else(): commands; elseif(): one If,
 *                                    flagged CCR_FLAG_ELSEIF
 *           Scope "foreach"          foreach/while/function/macro/block
 *             Text                   what is in its parentheses
 *             Block
 *
 * Saving copies text that was not touched, like C: comments, line breaks
 * inside a long add_executable(...), everything.
 */
#ifndef CCR_CMAKE_H
#define CCR_CMAKE_H

#include "ccr_ast.h"

#include <stdbool.h>
#include <stddef.h>

/* CMakeLists.txt, or anything ending in .cmake. */
bool ccr_cmake_is_cmake(const char *path);

/* Reads a CMake file into `out` (initialized). False with a diagnostic in
 * out->diags if it is not CMake Ccratch can read (unbalanced endif()...). */
bool ccr_cmake_parse_file(const char *path, ccr_ast *out);
/* The same from text; `name` is what the hat says. */
bool ccr_cmake_parse_text(const char *text, size_t len, const char *name, ccr_ast *out);

/* PRIVATE, REQUIRED, VERSION...: words a block shows as words, not slots. */
bool ccr_cmake_is_keyword(const char *word);
/* The colour of a command's block: a ccr_theme_cat. */
int  ccr_cmake_category(const char *command);

/* New blocks: `spec` is one command as CMake text ("project(game
 * LANGUAGES C)"); its words become Text slots (keywords too, shown as
 * words). Appended to `parent`. */
ccr_node_id ccr_cmake_build_command(ccr_ast *ast, ccr_node_id parent, const char *spec);
/* if() / foreach() ...: `head` the command, `args` its slot; with an
 * else() if `with_else`. */
ccr_node_id ccr_cmake_build_scope(ccr_ast *ast, ccr_node_id parent, const char *head,
                                  const char *args, bool with_else);

#endif /* CCR_CMAKE_H */
