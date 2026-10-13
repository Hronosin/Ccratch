/*
 * Ccratch public entry point.
 *
 * The whole IDE lives in libccratch. Whoever links it only needs to
 * call ccr_run() - the launcher in app/ does literally nothing else.
 */
#ifndef CCRATCH_H
#define CCRATCH_H

#if defined(_WIN32)
#  ifdef CCR_BUILDING_LIB
#    define CCR_API __declspec(dllexport)
#  else
#    define CCR_API __declspec(dllimport)
#  endif
#else
#  define CCR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CCR_VERSION_MAJOR 0
#define CCR_VERSION_MINOR 0
#define CCR_VERSION_PATCH 1

/* Runs the IDE. Returns a process exit code. */
CCR_API int ccr_run(int argc, char **argv);

/* "0.0.1" */
CCR_API const char *ccr_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CCRATCH_H */
