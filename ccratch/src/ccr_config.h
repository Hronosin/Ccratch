/*
 * ccr_config - the settings file, ~/.config/ccratch/config.
 *
 * Plain "key = value" lines; '#' starts a comment. Setting one key keeps
 * every other line as it was, comments included, and writes the file by
 * renaming a temporary one over it.
 */
#ifndef CCR_CONFIG_H
#define CCR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* $XDG_<var> if set and absolute, else $HOME/<fallback>. */
bool ccr_config_home(const char *xdg_var, const char *fallback, char *out, size_t cap);

/* The value of `key`, if the file has it. */
bool ccr_config_get(const char *key, char *out, size_t cap);

/* Sets `key` (one line, no newlines in the value), creating the file. */
bool ccr_config_set(const char *key, const char *value);

#endif /* CCR_CONFIG_H */
