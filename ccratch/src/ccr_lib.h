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
 *         choices: {title: Titles}   # a list to pick from for that slot
 *       - call: WindowShouldClose
 *         returns: bool          # not void: a reporter; bool: a pointy one
 *     choices: {color: Color}    # every slot called color, in every block
 *     enums:                     # the lists; also picked by C type (`types:`)
 *       Titles: ['"Hello"', '"Game"']
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
    char types[CCR_LIB_MAX_PARAMS][64];     /* C types, if the pack says (`types:`) */
    char choices[CCR_LIB_MAX_PARAMS][64];   /* the enum a slot picks from (`choices:`) */
    int  pack;
} ccr_lib_block;

typedef struct ccr_lib_pack {
    char code[32];        /* file name without .yaml */
    char name[64];
    char header[128];     /* "raylib.h"; empty: always shown */
    char include[140];    /* how a file includes it: "raylib.h" or <SDL2/SDL.h> */
    char link[64];        /* the library to link, if the pack says */
    char find[96];        /* CMake's find_package() for it ("raylib REQUIRED") */
    int  category;        /* a ccr_theme_cat */
    int  first, count;    /* its blocks */
    char path[1024];
} ccr_lib_pack;

/* Reads every pack found ($CCRATCH_BLOCKS_DIR, ~/.local/share/ccratch/blocks,
 * share/ccratch/blocks next to the program). Again after a language switch:
 * texts come in the interface language. Problems go to `log`. */
void ccr_lib_load(FILE *log);
/* The same, with a project's own packs (PROJECT/.ccratch/blocks) first. */
void ccr_lib_load_project(const char *project, FILE *log);
/* Where packs for a project's own libraries go. */
void ccr_lib_project_dir(const char *project, char *out, int cap);
/* Where packs for the system's libraries go (~/.local/share/ccratch/blocks). */
bool ccr_lib_user_dir(char *out, int cap);
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

/* The values of enum `type` from the packs ("KeyboardKey", "enum Foo",
 * "const Color"): how many, up to `cap` of them in `out`. */
int ccr_lib_enum_values(const char *type, const char **out, int cap);
/* What slot `param` of `b` can be picked from: its `choices`, else the
 * enum its type names. 0 if nothing. `name` gets the list's name. */
int ccr_lib_param_choices(const ccr_lib_block *b, int param, const char **out, int cap,
                          char *name, int name_cap);

/* --check-blocks: the problems found, 0 if none. */
int  ccr_lib_check(const char *path, FILE *out);

/* --make-blocks: a draft pack for the public functions of the library
 * behind `header` (see ccr_api), checked against its binary if found in
 * `project` (may be NULL) or the system. */
bool ccr_lib_make(const char *header, const char *const *clang_args, int nargs,
                  const char *project, FILE *out);

/* How a file of `project` includes `header`: <SDL2/SDL.h> for system
 * headers, "path" from the project (or its include/), else "name.h". */
void ccr_lib_include_for(const char *header, const char *project, char *out, int cap);

/* Where the header a user typed is: "zlib.h", <SDL2/SDL.h>, "zlib",
 * a path. Looked for next to file `from`, in the project and its
 * include/, the -I folders in `clang_args`, then the system's. */
bool ccr_lib_find_header(const char *spelled, const char *from, const char *project,
                         const char *const *clang_args, int nargs, char *out, int cap);

/* Makes a pack for `header` into folder `dir` (made if missing), named
 * after it; an existing pack is left alone (it may have been edited) and
 * reused. `include` is how files include the header. `path` gets the
 * pack's file, `count` its blocks. False with `err` if it went wrong. */
bool ccr_lib_create_pack(const char *header, const char *include, const char *dir,
                         const char *const *clang_args, int nargs, const char *project,
                         char *path, int path_cap, int *count, char *err, int err_cap);

#endif /* CCR_LIB_H */
