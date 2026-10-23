/*
 * ccr_plugins - the plugins: found, loaded, switched on and off, and asked
 * about the file on screen.
 *
 * A plugin is a shared library (include/ccratch/ccr_plugin.h) in
 * $CCRATCH_PLUGIN_DIR, ~/.local/share/ccratch/plugins or
 * share/ccratch/plugins next to the program. One with the same file name
 * found earlier wins. "plugin.NAME = off" in the config switches it off.
 */
#ifndef CCR_PLUGINS_H
#define CCR_PLUGINS_H

#include "ccr_ast.h"

#include <stdbool.h>
#include <stdio.h>

/* How bad: ccr_finding_level's values (the plugin header is not included
 * here: its log levels would clash with the build log's). */
typedef enum ccr_find_level {
    CCR_FIND_PRAISE,
    CCR_FIND_NIT,
    CCR_FIND_WARNING,
    CCR_FIND_ERROR
} ccr_find_level;

/* Something a checker said about a node. */
typedef struct ccr_finding {
    int               plugin;    /* which one (ccr_plugins_*(i)) */
    ccr_node_id       node;
    ccr_find_level    level;
    char              title[96];
    char              message[1400];
    bool              poke;      /* the plugin wants the user's nose rubbed in it */
    char              say[160];  /* ...shouting this */
} ccr_finding;

/* Loads every plugin found (again: unloads the old ones first). Problems
 * go to `log`. The number loaded. */
int  ccr_plugins_load(FILE *log);
void ccr_plugins_unload(void);

int         ccr_plugins_count(void);
const char *ccr_plugins_name(int i);      /* "code_hater" */
const char *ccr_plugins_title(int i);     /* "Code Hater" */
const char *ccr_plugins_version(int i);
const char *ccr_plugins_author(int i);    /* "" if it does not say */
const char *ccr_plugins_path(int i);
/* Its description, in `ui_language` if it has one. */
const char *ccr_plugins_describe(int i, const char *ui_language);
uint32_t    ccr_plugins_color(int i);     /* 0: none of its own */
bool        ccr_plugins_is_checker(int i);
bool        ccr_plugins_enabled(int i);
/* On or off; `save`: remembered in the config. */
void        ccr_plugins_set_enabled(int i, bool on, bool save);

/* Its settings (Settings > Plugins), remembered as plugin.NAME.KEY. */
int         ccr_plugins_setting_count(int i);
const char *ccr_plugins_setting_key(int i, int k);
bool        ccr_plugins_setting_is_toggle(int i, int k);   /* else a choice */
const char *ccr_plugins_setting_title(int i, int k, const char *ui_language);
const char *ccr_plugins_setting_hint(int i, int k, const char *ui_language);
int         ccr_plugins_setting_choice_count(int i, int k);
const char *ccr_plugins_setting_choice(int i, int k, int c);
const char *ccr_plugins_setting_choice_title(int i, int k, int c, const char *ui_language);
const char *ccr_plugins_setting_value(int i, int k);   /* "on", "mother_cat"... */
void        ccr_plugins_set_setting(int i, int k, const char *value, bool save);

/*
 * Every checker that is on, on this tree. *out: a malloc'd array (free
 * it), sorted by line. The number of findings; 0 with *out NULL when
 * there are none (or no checkers).
 */
int ccr_plugins_check(const ccr_ast *ast, const char *path, const char *ui_language, bool plain, ccr_finding **out);

/* --check-code FILE: parses it and prints what the checkers say, one
 * "file:line:col: level: [plugin] title" and the message under it. The
 * number of findings worse than a nit, or -1 when it could not read it. */
int ccr_plugins_check_file(const char *path, const char *ui_language, FILE *out);

#endif /* CCR_PLUGINS_H */
