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

typedef struct ccr_rename_place {
    int      file;      /* in ccr_rename.files */
    uint32_t offset;    /* the name's first byte */
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

#endif /* CCR_RENAME_H */
