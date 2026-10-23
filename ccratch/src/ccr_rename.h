/*
 * ccr_rename - a name changed everywhere in the project: a function, a
 * global, a type, an enum's value, a field.
 *
 * clang says what the name under the cursor is (its USR: the same thing
 * in every file, not just the same spelling), then every .c and .h of the
 * project is read for each place that names that same thing. A local
 * variable called the same in another function is a different thing and
 * stays. The files are written with the new name; what they said before
 * is kept, so the whole rename can be undone in one step.
 */
#ifndef CCR_RENAME_H
#define CCR_RENAME_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ccr_place_kind {
    CCR_PLACE_USE,          /* a call, a read, a write */
    CCR_PLACE_DECLARATION,  /* `void f(int);`, `extern int g;` */
    CCR_PLACE_DEFINITION    /* the function with its body, the variable itself */
} ccr_place_kind;

typedef struct ccr_rename_place {
    int            file;      /* in ccr_rename.files */
    uint32_t       offset;    /* the name's first byte */
    ccr_place_kind kind;
    int            line;      /* 1-based, in the file as read */
} ccr_rename_place;

typedef struct ccr_rename {
    char              old_name[128];
    char              usr[512];
    char            (*files)[1024];
    int               file_count;
    ccr_rename_place *places;
    int               place_count;
} ccr_rename;

/* What the name at byte `offset` of `path` is, and where the project in
 * `root` names it. False (with why in `err`) if there is no name there or
 * it is not the project's own (printf, a macro). */
bool ccr_rename_find(const char *root, const char *path, uint32_t offset, const char *const *clang_args,
                     int nargs, ccr_rename *out, char *err, int err_cap);
void ccr_rename_free(ccr_rename *r);

/* Where it is made: its definition, else a declaration (a header's);
 * the index in places, or -1. */
int  ccr_rename_definition(const ccr_rename *r);

/* The files as they were, to put back. */
typedef struct ccr_rename_undo {
    char  (*files)[1024];
    char  **before;     /* each file's text before */
    char  **after;      /* and after: undone only if it still says that */
    int     count;
} ccr_rename_undo;

/* Is `name` a C name, and no keyword? */
bool ccr_rename_valid(const char *name);

/* Writes the new name into every place. `undo` (may be NULL) gets what
 * the files said. False if a file could not be written (none is then). */
bool ccr_rename_apply(const ccr_rename *r, const char *new_name, ccr_rename_undo *undo, char *err, int err_cap);
/* Puts the files back, if they still say what the rename wrote. How many
 * were put back; -1 if one had changed since (then none is). */
int  ccr_rename_undo_apply(const ccr_rename_undo *u);
void ccr_rename_undo_free(ccr_rename_undo *u);

/* ------------------------------------------------- by name
 *
 * Everything the project calls `name`: a function, the field of a
 * struct, a local of another function... each its own thing (clang tells
 * them apart), with every place that names it.
 */
typedef enum ccr_sym_kind {
    CCR_SYM_FUNCTION,
    CCR_SYM_METHOD,
    CCR_SYM_GLOBAL,         /* a variable outside functions */
    CCR_SYM_LOCAL,          /* a variable of a function (`owner`) */
    CCR_SYM_PARAMETER,      /* of `owner` */
    CCR_SYM_FIELD,          /* of struct or class `owner` */
    CCR_SYM_TYPE,           /* struct, union, enum, class, typedef */
    CCR_SYM_ENUM_VALUE,
    CCR_SYM_MACRO,
    CCR_SYM_NAMESPACE,
    CCR_SYM_OTHER
} ccr_sym_kind;

typedef struct ccr_symbol {
    char         usr[512];
    char         name[128];
    ccr_sym_kind kind;
    char         owner[128];   /* the function, struct... it belongs to; "" */
    bool         own;          /* the project's (else printf, a library's: no renaming) */
    int          first, count; /* its places: places[first .. first+count) */
} ccr_symbol;

typedef struct ccr_symbols {
    ccr_symbol       *syms;
    int               sym_count;
    char            (*files)[1024];
    int               file_count;
    ccr_rename_place *places;     /* grouped by symbol, each by file and offset */
    int               place_count;
} ccr_symbols;

/* Every thing named exactly `name` in the project in `root` (its own
 * first, the most named first). False with why in `err` if there is none. */
bool ccr_symbols_named(const char *root, const char *name, const char *const *clang_args, int nargs,
                       ccr_symbols *out, char *err, int err_cap);
void ccr_symbols_free(ccr_symbols *s);
/* Symbol `i` (-1: every one that is the project's own) as one rename,
 * to ccr_rename_apply (free it). */
bool ccr_symbols_rename(const ccr_symbols *s, int i, ccr_rename *out);

#endif /* CCR_RENAME_H */
