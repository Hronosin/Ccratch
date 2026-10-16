/*
 * ccr_lib - block packs: libraries turned into blocks.
 *
 * A pack is a YAML file, blocks/NAME.yaml, found at run time like themes
 * and language packs. It says how a library's functions read as blocks:
 *
 *     name: raylib
 *     header: raylib.h          # its blocks show when a file includes it
 *     color: library            # a theme category: library, call, control...
 *     blocks:
 *       - call: InitWindow
 *         text: open window {width} x {height} titled {title}
 *         text.ru: открыть окно {width} x {height} с заголовком {title}
 *         params: [width, height, title]
 *         defaults: {width: 800, height: 450, title: '"Hello"'}
 *       - call: WindowShouldClose
 *         returns: bool          # not void: a reporter; bool: a pointy one
 *
 * {name} in the text is that parameter's slot. A call in the code, old or
 * new, with the name and the number of arguments of a block in a pack is
 * drawn as that block; saving prints it as the plain C call it is.
 *
 * `ccratch --make-blocks lib.h` writes a first draft of a pack from a
 * header; `ccratch --check-blocks pack.yaml` says what is wrong with one.
 */
#ifndef CCR_LIB_H
#define CCR_LIB_H

#include <stdbool.h>
#include <stdio.h>

#define CCR_LIB_MAX_PARAMS 16
#define CCR_LIB_MAX_PACKS  16

typedef struct ccr_lib_block {
    char call[64];
    char text[256];       /* in the interface language, if the pack has it */
    char returns[64];     /* "void": a statement */
    int  nparams;
    char params[CCR_LIB_MAX_PARAMS][32];
    char defaults[CCR_LIB_MAX_PARAMS][128];
    int  pack;
} ccr_lib_block;

typedef struct ccr_lib_pack {
    char code[32];        /* file name without .yaml */
    char name[64];
    char header[128];     /* "raylib.h"; empty: always shown */
    int  category;        /* a ccr_theme_cat */
    int  first, count;    /* its blocks */
    char path[1024];
} ccr_lib_pack;

/* Reads every pack found ($CCRATCH_BLOCKS_DIR, ~/.local/share/ccratch/blocks,
 * share/ccratch/blocks next to the program). Again after a language switch:
 * texts come in the interface language. Problems go to `log`. */
void ccr_lib_load(FILE *log);
void ccr_lib_unload(void);

int                  ccr_lib_pack_count(void);
const ccr_lib_pack  *ccr_lib_pack_at(int i);
int                  ccr_lib_block_count(void);
const ccr_lib_block *ccr_lib_block_at(int i);

/* The block for a call to `name` with `nargs` arguments, or NULL. */
const ccr_lib_block *ccr_lib_find(const char *name, int nargs);
/* Its index in ccr_lib_block_at, or -1. */
int ccr_lib_index(const ccr_lib_block *b);

/* Does a file that includes `include` (as written: "raylib.h",
 * <GL/gl.h>...) get pack `p`'s blocks? */
bool ccr_lib_pack_matches(const ccr_lib_pack *p, const char *include);

/* --check-blocks: the problems found, 0 if none. */
int  ccr_lib_check(const char *path, FILE *out);

/* --make-blocks: a draft pack for the functions declared in `header`. */
bool ccr_lib_make(const char *header, const char *const *clang_args, int nargs, FILE *out);

#endif /* CCR_LIB_H */
