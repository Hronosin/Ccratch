/*
 * ccr_ccdb - the flags a project's own build compiles a file with, from
 * its compile_commands.json (CMake writes one with
 * CMAKE_EXPORT_COMPILE_COMMANDS, Ccratch's own builds always do).
 *
 * Without them clang reads a file blind: `#include "raylib.h"` from a
 * library CMake fetched into build/_deps is not found, and every use of
 * it is an error. With them, the file reads the way it compiles.
 *
 * Only what changes how code reads is kept: -I and friends, -D/-U,
 * -std, -include. Paths are made absolute.
 */
#ifndef CCR_CCDB_H
#define CCR_CCDB_H

#include <stdbool.h>

#define CCR_CCDB_MAX_ARGS 160

typedef struct ccr_ccdb_args {
    const char *v[CCR_CCDB_MAX_ARGS];
    int         n;
    char        db[1024];     /* the compile_commands.json used */
    char        from[1024];   /* the file whose flags they are */
    bool        exact;        /* `from` is the file itself (not a neighbour's) */
    char       *store;        /* the strings v points into */
} ccr_ccdb_args;

/* The newest compile_commands.json of `project`: in it, in build/,
 * build/<preset>/, .ccratch/build, cmake-build-<x>/, out/build/<x>/. */
bool ccr_ccdb_find(const char *project, char *out, int cap);

/* The flags for `path` from `db`: its own entry, or for a header (or a
 * file the build does not know), the entry nearest to it. */
bool ccr_ccdb_args_for(const char *db, const char *path, ccr_ccdb_args *out);
void ccr_ccdb_args_free(ccr_ccdb_args *a);

#endif /* CCR_CCDB_H */
