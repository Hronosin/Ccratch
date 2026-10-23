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

/*
 * Folders an add-on of kind `sub` ("lang", "themes") may live in, best
 * first: $CCRATCH_<SUB>_DIR, ~/.local/share/ccratch/<sub>, and
 * share/ccratch/<sub> next to the program (the build tree too).
 */
int  ccr_config_data_dirs(const char *sub, const char *env_var, char dirs[][1024], int cap);

/* Where the canvas layout of `file` is kept (~/.local/share/ccratch/
 * layout/<a hash of its full path>.layout); `make`: the folder made too. */
bool ccr_config_layout_path(const char *file, char *out, size_t cap, bool make);

/* The value of `key`, if the file has it. */
bool ccr_config_get(const char *key, char *out, size_t cap);

/* Sets `key` (one line, no newlines in the value), creating the file;
 * a NULL value takes the key out. */
bool ccr_config_set(const char *key, const char *value);

/* Where the file is (whether or not it is there yet). */
bool ccr_config_file(char *out, size_t cap);

#endif /* CCR_CONFIG_H */
