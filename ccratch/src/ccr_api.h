/*
 * ccr_api - what a library offers, read from its headers (the API) and,
 * when it can be found, its binary (the ABI); written out as a block pack.
 *
 * Like Qt's moc reading headers for signals and slots: the header and the
 * headers next to it are read with clang, and every public function is
 * kept with its parameters and types, with the enums that go with them.
 * "Public" means: declared in the library's own headers, not starting
 * with an underscore, and, when the library's .so or .a is found,
 * actually exported by it (static inline helpers in the header stay).
 *
 * A library can also pick its blocks itself: once any function has
 * `@block` in its doc comment, only those become blocks, with that text:
 *
 *     /// @block open a window {width} x {height} called {title}
 *     /// @block.ru открыть окно {width} x {height} с названием {title}
 *     void InitWindow(int width, int height, const char *title);
 */
#ifndef CCR_API_H
#define CCR_API_H

#include <stdbool.h>
#include <stdio.h>

#define CCR_API_MAX_PARAMS 16

typedef struct ccr_api_param {
    char name[48];
    char type[96];
} ccr_api_param;

typedef struct ccr_api_func {
    char          name[64];
    char          ret[96];
    int           nparams;
    ccr_api_param params[CCR_API_MAX_PARAMS];
    bool          variadic;
    bool          inline_def;    /* defined in the header: needs no binary */
    bool          exported;      /* found in the binary */
    char          doc[512];      /* its doc comment, raw */
} ccr_api_func;

typedef struct ccr_api_enum {
    char   name[64];
    int    count;
    char **values;
} ccr_api_enum;

typedef struct ccr_api {
    char          header[4096];   /* the file read */
    char          dir[4096];      /* its folder: the library's headers */
    ccr_api_func *funcs;
    int           func_count, func_cap;
    ccr_api_enum *enums;
    int           enum_count, enum_cap;
    char          binary[4096];   /* the .so/.a checked against, if any */
} ccr_api;

/* Reads the public functions and enums of the library behind `header`
 * (that file and every header in its folder that it pulls in). */
bool ccr_api_scan(const char *header, const char *const *clang_args, int nargs, ccr_api *out,
                  char *err, int err_cap);
void ccr_api_free(ccr_api *api);

/* libNAME.so / libNAME.a for a library NAME, in the project and the
 * usual system folders. */
bool ccr_api_find_binary(const char *name, const char *project, char *out, int cap);

/* Keeps the functions `binary` (an ELF .so or .a) exports, and those
 * defined in the header. Returns how many symbols the binary exports, -1
 * if it could not be read (then nothing is dropped). */
int  ccr_api_check_binary(ccr_api *api, const char *binary);

/* The pack: `include` is how files include the header ("raylib.h",
 * <SDL2/SDL.h>), `link` the library to link (may be empty). */
void ccr_api_write_pack(const ccr_api *api, const char *name, const char *include,
                        const char *link, FILE *out);

/* For the parser: called with what clang found. */
ccr_api_func *ccr_api_add_func(ccr_api *api, const char *name);
ccr_api_enum *ccr_api_add_enum(ccr_api *api, const char *name);
void          ccr_api_enum_value(ccr_api_enum *e, const char *value);

#endif /* CCR_API_H */
