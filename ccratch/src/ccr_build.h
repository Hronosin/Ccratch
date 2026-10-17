/*
 * ccr_build - the project, as something to build.
 *
 * For now: a first CMakeLists.txt for a project that has none, made from
 * what is there. Every .c file goes into one program (a second main() is
 * left out, and said so), include/ is searched for headers, and the
 * libraries the code includes are linked: the ones block packs know (their
 * `link:` and `find_package:`), math.h's libm, pthread.h's threads.
 */
#ifndef CCR_BUILD_H
#define CCR_BUILD_H

#include <stdbool.h>

/* The CMakeLists.txt text for `project` (malloc'd, NULL if it has no .c
 * file or memory ran out). `main_hint`, if it has a main(), is the
 * program's; may be NULL. `files_out`, if not NULL, gets how many .c
 * files went in. */
char *ccr_build_cmake_text(const char *project, const char *main_hint, int *files_out);

typedef enum ccr_cmake_made {
    CCR_CMAKE_MADE,
    CCR_CMAKE_THERE,      /* the project has one already: `path` is it */
    CCR_CMAKE_NO_SOURCES,
    CCR_CMAKE_CANNOT      /* could not write it */
} ccr_cmake_made;

/* PROJECT/CMakeLists.txt, made if missing. `path` gets where it is. */
ccr_cmake_made ccr_build_make_cmake(const char *project, const char *main_hint, char *path,
                                    int cap, int *files_out);

#endif /* CCR_BUILD_H */
