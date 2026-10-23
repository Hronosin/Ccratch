/*
 * ccr_arrange - an arrangement of your own: how a file's top-level blocks
 * are laid out on the canvas, as rules, not positions, so it fits any file.
 *
 * Columns, left to right. Each takes some kinds of things (the #includes,
 * the types, main, the other functions...), in an order (as in the file,
 * by name, the biggest first, or callers before what they call), and may
 * wrap into a next column of its own when it gets taller than `wrap`
 * (like a newspaper). What no column takes goes where "rest" is, or into
 * the last one.
 *
 * Kept as text, a file per arrangement (~/.local/share/ccratch/
 * arrangements/NAME.arrange):
 *
 *     @name    = Kernel
 *     column   = includes macros
 *     column   = types globals protos | order=name
 *     column   = main funcs | order=calls wrap=1600
 *
 * No window here: the editor lays out (ccr_editor_arrange_custom), the app
 * draws the builder.
 */
#ifndef CCR_ARRANGE_H
#define CCR_ARRANGE_H

#include <stdbool.h>
#include <stddef.h>

/* What a top-level block is. */
typedef enum ccr_akind {
    CCR_AK_INCLUDES = 1u << 0,   /* #include, using */
    CCR_AK_MACROS   = 1u << 1,   /* #define */
    CCR_AK_TYPES    = 1u << 2,   /* struct, union, enum, typedef */
    CCR_AK_GLOBALS  = 1u << 3,   /* variables outside functions */
    CCR_AK_PROTOS   = 1u << 4,   /* functions declared, not made here */
    CCR_AK_MAIN     = 1u << 5,
    CCR_AK_FUNCS    = 1u << 6,   /* the other functions */
    CCR_AK_CLASSES  = 1u << 7,   /* namespaces, classes with methods */
    CCR_AK_LOOSE    = 1u << 8,   /* stacks lying free on the canvas */
    CCR_AK_REST     = 1u << 9    /* whatever no other column takes */
} ccr_akind;

#define CCR_AK_COUNT 10

typedef enum ccr_aorder {
    CCR_AO_FILE,      /* as in the file */
    CCR_AO_NAME,      /* by name, a to z */
    CCR_AO_SIZE,      /* the biggest first */
    CCR_AO_CALLS,     /* callers before what they call */
    CCR_AO_COUNT
} ccr_aorder;

#define CCR_ARRANGE_MAX_COLS 8

typedef struct ccr_arrange_col {
    unsigned   kinds;   /* CCR_AK_* */
    ccr_aorder order;
    float      wrap;    /* 0: one column however tall; else a new one past this height */
} ccr_arrange_col;

typedef struct ccr_arrangement {
    char            name[64];
    char            names[4][2][64];   /* @name.ru = ...: language, name */
    char            code[64];    /* its file's name, without .arrange ("" before it is saved) */
    ccr_arrange_col cols[CCR_ARRANGE_MAX_COLS];
    int             ncols;
} ccr_arrangement;

/* The words of the file: "includes", "main"...; "file", "calls"... */
const char *ccr_akind_key(unsigned kind_bit);
const char *ccr_aorder_key(ccr_aorder o);

/* Reads the text into `out`. False (with the line and why in `err`) if
 * it says something it should not, or has no columns. */
bool ccr_arrangement_parse(const char *text, ccr_arrangement *out, char *err, size_t err_cap);
/* Its name in `ui_language` if it has one ("ru"), else @name. */
const char *ccr_arrangement_title(const ccr_arrangement *a, const char *ui_language);
/* And back to text. */
void ccr_arrangement_write(const ccr_arrangement *a, char *out, size_t cap);

/* A file name for `name`: small letters, digits and _ ("Моя" -> "arrangement"). */
void ccr_arrangement_code(const char *name, char *out, size_t cap);

#endif /* CCR_ARRANGE_H */
