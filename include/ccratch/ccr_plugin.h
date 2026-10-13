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
    CCR_PLUGIN_PERSONALITY = 1u << 4   /* rewrites diagnostics (Reimu)  */
} ccr_plugin_kind;

/* What the host gives to a plugin. Grows only at the end. */
typedef struct ccr_host_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void (*log)(ccr_log_level level, const char *plugin, const char *message);
} ccr_host_api;

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
