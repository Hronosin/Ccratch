/*
 * ccr_lang - language packs for the interface.
 *
 * Every piece of text the window shows is listed below, in English, with a
 * key. That English is built in, so Ccratch always has something to say.
 * A language pack is a plain text file, lang/CODE.lang, that gives other
 * text for those keys:
 *
 *     @name   = Русский
 *     @plural = slavic
 *     pie.save = Сохранить
 *
 * Packs are found at run time, so adding a language is dropping a file in
 * a folder, no rebuild. Anything a pack leaves out (or gets wrong) falls
 * back to English. `ccratch --lang-template` prints a pack to start from,
 * `ccratch --check-lang FILE` says what is missing or broken in one.
 *
 * C keywords on the blocks (if, while, struct...) are C, not English, and
 * are never translated. The command line stays in English too.
 */
#ifndef CCR_LANG_H
#define CCR_LANG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/*
 * X(ID, "key", plural?, "English")
 *
 * Plural strings hold every form, split by '|': English has two (one|other),
 * a pack has as many as its @plural rule says. printf-style %s/%u/%d in a
 * translation must match the English ones exactly, in order: a pack cannot
 * crash the program with a stray %s.
 */
#define CCR_STRINGS(X)                                                                   \
    /* window */                                                                         \
    X(BUILD,            "topbar.build",          0, "Build")                             \
    X(PALETTE_TITLE,    "palette.title",         0, "Blocks")                            \
    X(PALETTE_DROP,     "palette.drop",          0, "drop to delete")                    \
    X(TEMPLATE_CODE,    "palette.code_line",     0, "code line")                         \
    X(EMPTY_TITLE,      "empty.title",           0, "Drop a .c file here")               \
    X(EMPTY_HINT,       "empty.hint",            0, "(or: ccratch file.c)")              \
    /* status bar, left: what just happened */                                           \
    X(NO_FILE,          "status.no_file",        0, "No file. Drop a .c file onto the window.") \
    X(CANNOT_READ,      "status.cannot_read",    0, "Could not read %s. Opyat' rabotat'...") \
    X(OOM_LAYOUT,       "status.oom_layout",     0, "Out of memory while laying out blocks.") \
    X(LOADED_CLEAN,     "status.loaded_clean",   0, "%s: no incidents. Donation box: still empty.") \
    X(LOADED_ERRORS,    "status.loaded_errors",  1,                                      \
      "%s: %u error (details in the terminal). Opyat' rabotat'..."                       \
      "|%s: %u errors (details in the terminal). Opyat' rabotat'...")                    \
    X(NOT_SAVED,        "status.not_saved",      0, "Not saved: %s")                     \
    X(NO_BACKUP,        "status.no_backup",      0, "Not saved: could not write the backup %s~") \
    X(CANNOT_WRITE,     "status.cannot_write",   0, "Not saved: could not write %s")     \
    X(SAVED,            "status.saved",          0, "Saved %s (original in %s~).")       \
    X(SAVED_ERRORS,     "status.saved_errors",   1,                                      \
      "Saved %s, but clang now sees %u error (details in the terminal)."                 \
      "|Saved %s, but clang now sees %u errors (details in the terminal).")              \
    X(LEFT_OUT,         "status.left_out",       1,                                      \
      "%u loose stack left out.|%u loose stacks left out.")                              \
    X(LANGUAGE,         "status.language",       0, "Language: %s")                      \
    /* status bar, right: help */                                                        \
    X(BAR_LOOSE,        "bar.loose",             1,                                      \
      "%u loose stack (not compiled)|%u loose stacks (not compiled)")                    \
    X(BAR_ZOOM,         "bar.zoom",              0, "zoom %d%%")                         \
    X(BAR_HINTS,        "bar.hints",             0,                                      \
      "right click: menu  |  RMB drag pan  wheel zoom  |  Ctrl+S save  Z undo  Y redo  |  Home  F5") \
    /* pie menu */                                                                       \
    X(PIE_SAVE,         "pie.save",              0, "Save")                              \
    X(PIE_COMMENT,      "pie.comment",           0, "Comment out")                       \
    X(PIE_UNCOMMENT,    "pie.uncomment",         0, "Uncomment")                         \
    X(PIE_REDO,         "pie.redo",              0, "Redo")                              \
    X(PIE_EDIT_TEXT,    "pie.edit_text",         0, "Edit text")                         \
    X(PIE_DELETE,       "pie.delete",            0, "Delete")                            \
    X(PIE_FIT_VIEW,     "pie.fit_view",          0, "Fit view")                          \
    X(PIE_UNDO,         "pie.undo",              0, "Undo")                              \
    X(PIE_DUPLICATE,    "pie.duplicate",         0, "Duplicate")                         \
    X(PIE_CLICK_FIELD,  "pie.key.click_field",   0, "click field")                       \
    X(PIE_KEY_DELETE,   "pie.key.delete",        0, "Del")                               \
    /* saving refused */                                                                 \
    X(GEN_UNSAFE_GAP,   "save.unsafe_gap",       0,                                      \
      "line %u: this block has #if/#define or macro-made code between its statements, " \
      "so it cannot be rewritten safely yet")                                            \
    X(GEN_LOST,         "save.lost",             0,                                      \
      "line %u: changed code in a place I cannot locate in the file")

typedef enum ccr_text {
#define X(id, key, plural, en) CCR_T_##id,
    CCR_STRINGS(X)
#undef X
    CCR_T_COUNT
} ccr_text;

/* How a language picks a plural form; a pack names it with @plural. */
typedef enum ccr_plural_rule {
    CCR_PLURAL_ONE_OTHER,   /* "germanic": 1 | everything else (en, de, es, it...) */
    CCR_PLURAL_SLAVIC,      /* 1, 21, 31 | 2-4, 22-24 | rest (ru, uk, be, sr, hr) */
    CCR_PLURAL_POLISH,      /* 1 | 2-4, 22-24 (not 12-14) | rest */
    CCR_PLURAL_CZECH,       /* 1 | 2-4 | rest (cs, sk) */
    CCR_PLURAL_FRENCH,      /* 0, 1 | rest (fr, pt-BR) */
    CCR_PLURAL_NONE,        /* one form for every number (ja, zh, ko, tr...) */
    CCR_PLURAL_RULE_COUNT
} ccr_plural_rule;

/* The text for an id in the current language. */
const char *ccr_tr(ccr_text id);

/* A plural string's form for n (any string works; non-plural ones ignore n). */
const char *ccr_trn(ccr_text id, unsigned long n);

/* snprintf with a translated format; the arguments are those of the English. */
int  ccr_trf(char *out, size_t cap, ccr_text id, ...);
int  ccr_trnf(char *out, size_t cap, ccr_text id, unsigned long n, ...);

/* ---------------------------------------------------------- packs */

typedef struct ccr_lang_info {
    char code[16];          /* file name without .lang: "ru", "pt-BR" */
    char name[64];          /* @name, in the language itself */
    char path[1024];        /* "" for built-in English */
} ccr_lang_info;

/* Looks for packs: $CCRATCH_LANG_DIR, ~/.local/share/ccratch/lang, then
 * share/ccratch/lang next to the program. Built-in English is always
 * first. A pack found earlier wins over one with the same code later. */
int  ccr_lang_scan(ccr_lang_info *out, int cap);

/* Switches to a language by code ("en" or a pack). Problems are written to
 * `log` if it is not NULL. False if no such pack: English stays. */
bool ccr_lang_use(const char *code, FILE *log);

/* The language in use. */
const char *ccr_lang_code(void);
const char *ccr_lang_name(void);

/*
 * The language to start in: --lang, else $CCRATCH_LANG, else the one saved
 * in ~/.config/ccratch/config, else the system's ($LC_ALL, $LC_MESSAGES,
 * $LANG: "ru_UA.UTF-8" -> "ru"), else English. NULL for `cli`: no --lang.
 */
void ccr_lang_pick(const char *cli, FILE *log);

/* Remembers the language for next time, in ~/.config/ccratch/config. */
bool ccr_lang_save_choice(const char *code);

/* --check-lang: reads a pack, reports to `out`. Returns the number of
 * problems (bad lines, broken formats, missing or unknown keys). */
int  ccr_lang_check(const char *path, FILE *out);

/* --lang-template: the English as a pack file. */
void ccr_lang_write_template(FILE *out);

/* Back to built-in English; frees the pack. */
void ccr_lang_reset(void);

#endif /* CCR_LANG_H */
