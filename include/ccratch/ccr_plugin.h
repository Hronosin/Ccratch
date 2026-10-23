/*
 * Ccratch plugin ABI.
 *
 * A plugin is a shared library that exports one function:
 *
 *     const ccr_plugin *ccr_plugin_entry(void);
 *
 * The host checks abi_version before touching anything else, so an old
 * plugin fails politely instead of corrupting the shrine.
 *
 * Anything that can speak the C ABI can be a plugin. Yes, even Rust.
 */
#ifndef CCR_PLUGIN_H
#define CCR_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on every breaking change to the structs below. */
#define CCR_PLUGIN_ABI_VERSION 1u

#define CCR_PLUGIN_ENTRY_SYMBOL "ccr_plugin_entry"

typedef enum ccr_log_level {
    CCR_LOG_DEBUG,
    CCR_LOG_INFO,
    CCR_LOG_WARN,
    CCR_LOG_ERROR
} ccr_log_level;

typedef enum ccr_plugin_kind {
    CCR_PLUGIN_GENERIC     = 0,
    CCR_PLUGIN_DIALECT     = 1u << 0,  /* adds or removes blocks        */
    CCR_PLUGIN_PREPROCESS  = 1u << 1,  /* #define, #ifdef and friends   */
    CCR_PLUGIN_BUILD       = 1u << 2,  /* CMake, compilers, flags       */
    CCR_PLUGIN_PACKAGES    = 1u << 3,  /* git fetch/pull of libraries   */
    CCR_PLUGIN_PERSONALITY = 1u << 4,  /* rewrites diagnostics (Reimu)  */
    CCR_PLUGIN_CHECKER     = 1u << 5   /* reads code, says what is wrong */
} ccr_plugin_kind;

/* What the host gives to a plugin. Grows only at the end. */
typedef struct ccr_host_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void (*log)(ccr_log_level level, const char *plugin, const char *message);
} ccr_host_api;

/* ------------------------------------------------------- reading code
 *
 * A plugin that looks at code (CCR_PLUGIN_CHECKER) gets the file as a flat
 * array of nodes: the tree Ccratch draws blocks from, the same one
 * `ccratch --dump-ast FILE` prints. Links are indices into that array,
 * CCR_PLUGIN_NONE where there is nothing. Everything in it, strings too,
 * lives until check() returns: copy what you want to keep.
 */
#define CCR_PLUGIN_NONE 0xFFFFFFFFu

/* ccr_plugin_node.flags */
#define CCR_PNODE_DEFINITION (1u << 0)   /* a function or struct with a body */
#define CCR_PNODE_OUTSIDE    (1u << 1)   /* on the canvas but not in the program
                                            (a loose block, a commented-out one):
                                            nothing to say about it */
#define CCR_PNODE_POSTFIX    (1u << 2)   /* x++, not ++x */
#define CCR_PNODE_ARROW      (1u << 3)   /* p->x, not p.x */
#define CCR_PNODE_INIT       (1u << 4)   /* the value a Var starts with */
#define CCR_PNODE_ANGLED     (1u << 5)   /* #include <...> */

typedef struct ccr_plugin_node {
    const char *kind;     /* as --dump-ast says it: "Function", "Var", "If",
                             "Call", "BinaryOp", "Ref"... */
    const char *name;     /* the identifier, the literal, the called function */
    const char *type;     /* a declaration's C type: "char *", "int[64]" */
    const char *op;       /* "=", "+", "&", "*"... for BinaryOp / UnaryOp */
    uint32_t    parent, first_child, next_sibling;
    uint32_t    line, column;   /* 1-based; 0 for a block made in the editor */
    uint32_t    flags;          /* CCR_PNODE_* */
    uint32_t    src_begin, src_end;   /* its text in ccr_plugin_file.source;
                                         equal when it has none yet */
} ccr_plugin_node;

/* How bad. The host draws each its own way. */
typedef enum ccr_finding_level {
    CCR_FINDING_PRAISE,    /* something done right: it happens */
    CCR_FINDING_NIT,       /* style, taste */
    CCR_FINDING_WARNING,   /* will bite, sooner or later */
    CCR_FINDING_ERROR      /* wrong. NAK */
} ccr_finding_level;

typedef struct ccr_plugin_file {
    uint32_t               struct_size;
    const char            *path;        /* the file, absolute */
    const char            *language;    /* "c" or "cmake" */
    const char            *ui_language; /* the interface's: "en", "ru", "ja"... */
    bool                   plain;       /* ...in human words (no jargon) */
    const ccr_plugin_node *nodes;
    uint32_t               node_count;
    uint32_t               root;        /* the TranslationUnit */
    const char            *source;      /* the file as it was read */
    uint32_t               source_len;
    /* Says something about node `node`: a short title ("use after free")
     * and the whole of it. Copied by the host: pass anything. */
    void                 (*report)(const struct ccr_plugin_file *file, uint32_t node, ccr_finding_level level,
                                   const char *title, const char *message);
    void                  *host_data;   /* the host's; leave it alone */
} ccr_plugin_file;

/* What a plugin gives to the host. Grows only at the end. */
typedef struct ccr_plugin {
    uint32_t    abi_version;   /* must be CCR_PLUGIN_ABI_VERSION */
    uint32_t    struct_size;   /* sizeof(ccr_plugin) at build time */
    uint32_t    kinds;         /* bitmask of ccr_plugin_kind */
    const char *name;
    const char *version;
    const char *description;

    bool (*init)(const ccr_host_api *host);
    void (*shutdown)(void);

    /* -- since the checkers (still ABI 1: the host reads these only when
     *    struct_size says they are there) -- */
    const char *title;          /* to show: "Code Hater"; NULL: name */
    const char *author;
    /* The description in the interface's language ("ru"), or NULL for
     * `description`. */
    const char *(*describe)(const char *ui_language);
    /* CCR_PLUGIN_CHECKER: looks at a file after every change and reports
     * through file->report. Fast, please: it runs on the UI's thread. */
    void (*check)(const ccr_plugin_file *file);
    uint32_t    color;          /* its badges' colour, 0xRRGGBBAA; 0: the host's */
} ccr_plugin;

typedef const ccr_plugin *(*ccr_plugin_entry_fn)(void);

#if defined(_WIN32)
#  define CCR_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define CCR_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif /* CCR_PLUGIN_H */
