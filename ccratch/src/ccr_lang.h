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
    X(FILES_TITLE,      "files.title",           0, "Files")                             \
    X(FILES_EMPTY,      "files.empty",           0, "no .c or .h files here")            \
    X(FILES_UNSAVED,    "files.unsaved",         0, "Not saved! Click x again to close anyway") \
    X(FILES_TOO_MANY,   "files.too_many",        0, "Too many files open: close one first") \
    X(FILES_NEW_FILE,   "files.new_file",        0, "+ file")                            \
    X(FILES_NEW_DIR,    "files.new_dir",         0, "+ folder")                          \
    X(FILES_NAME_FILE,  "files.name_file",       0, "File name (.c or .h), Enter to create") \
    X(FILES_NAME_DIR,   "files.name_dir",        0, "Folder name, Enter to create")      \
    X(FILES_EXISTS,     "files.exists",          0, "%s is already there")               \
    X(FILES_CREATED,    "files.created",         0, "%s created")                        \
    X(FILES_CANNOT,     "files.cannot",          0, "Could not create %s")               \
    X(INCLUDE_NOT_FOUND, "include.not_found",    0, "%s is neither next to this file nor in the project") \
    X(INCLUDE_SYSTEM,   "include.system",        0, "%s is a system header, not opening it") \
    X(LIBS_TITLE,       "libs.title",            0, "Libraries")                         \
    X(LIBS_HEADING,     "libs.heading",          0, "A library, as blocks")              \
    X(LIBS_EXPLAIN,     "libs.explain",          0,                                      \
      "Its header is read for the public functions (and its .so or .a, if found, for what it really exports). They become a block pack, the file includes the header, the blocks show in the palette.") \
    X(LIBS_IN_FILE,     "libs.in_file",          0, "This file includes them, no blocks yet") \
    X(LIBS_HAVE,        "libs.have",             0, "Blocks made, the file does not include them") \
    X(LIBS_NOTHING,     "libs.nothing",          0, "nothing here")                      \
    X(LIBS_MAKE,        "libs.make",             0, "make blocks")                       \
    X(LIBS_INCLUDE,     "libs.include",          0, "include")                           \
    X(LIBS_ASK,         "libs.ask",              0, "Any header: SDL2/SDL.h, zlib.h, a path... Enter") \
    X(LIBS_CLOSE,       "libs.close",            0, "Close")                             \
    X(LIBS_NOT_FOUND,   "libs.not_found",        0, "No %s next to the file, in the project or in the system") \
    X(LIBS_FAILED,      "libs.failed",           0, "Blocks for %s: %s")                 \
    X(LIBS_MADE,        "libs.made",             1,                                      \
      "%s: %u block, in %s|%s: %u blocks, in %s")                                        \
    X(LIBS_INCLUDED,    "libs.included",         0, "#include %s added")                 \
    X(CMAKE_MADE,       "cmake.made",            1,                                      \
      "CMakeLists.txt made: %u .c file, the libraries it includes linked"               \
      "|CMakeLists.txt made: %u .c files, the libraries they include linked")           \
    X(CMAKE_NO_SOURCES, "cmake.no_sources",      0, "No .c files in this project: nothing for CMake to build yet") \
    X(CMAKE_NO_PROJECT, "cmake.no_project",      0, "Open a .c file first: its project gets the CMakeLists.txt") \
    X(BUILD_PANEL,      "build.panel",           0, "Build")                             \
    X(BUILD_RUN,        "build.run",             0, "Run")                               \
    X(BUILD_STOP,       "build.stop",            0, "Stop")                              \
    X(BUILD_IDLE,       "build.idle",            0, "Ctrl+B builds, Ctrl+R builds and runs") \
    X(BUILD_CONFIGURING, "build.configuring",    0, "CMake is setting up the build...")  \
    X(BUILD_BUILDING,   "build.building",        0, "Building...")                       \
    X(BUILD_RUNNING,    "build.running",         0, "Running %s")                        \
    X(BUILD_BUILT,      "build.built",           0, "Built in %s s")                     \
    X(BUILD_FAILED,     "build.failed",          0, "Did not build: %s")                 \
    X(BUILD_ERRORS,     "build.errors",          1, "%u error|%u errors")                \
    X(BUILD_WARNINGS,   "build.warnings",        1, "%u warning|%u warnings")            \
    X(BUILD_STOPPED,    "build.stopped",         0, "Stopped")                           \
    X(BUILD_NO_CMAKE,   "build.no_cmake",        0, "CMake is not installed: dnf install cmake (or apt install cmake)") \
    X(BUILD_NOT_SAVED,  "build.not_saved",       0, "Not built: %s could not be saved")  \
    X(BUILD_FINISHED,   "build.finished",        0, "The program finished (code %d)")    \
    X(BUILD_CLICK,      "build.click",           0, "click an error to go to its block") \
    X(QUIT_TITLE,       "quit.title",            0, "Not saved yet")                     \
    X(QUIT_SAVE,        "quit.save",             0, "Save and quit")                     \
    X(QUIT_DISCARD,     "quit.discard",          0, "Quit without saving")               \
    X(QUIT_CANCEL,      "quit.cancel",           0, "Stay")                              \
    X(FIND_HINT,        "find.hint",             0, "find a name, a number, a word")     \
    X(FIND_COUNT,       "find.count",            0, "%d of %d")                          \
    X(FIND_NONE,        "find.none",             0, "nothing")                           \
    X(PAL_SEARCH,       "palette.search",        0, "search blocks")                     \
    X(PAL_NONE,         "palette.none",          0, "no block says that")                \
    X(RECENT,           "start.recent",          0, "Recent files")                      \
    X(HELP_TITLE,       "help.title",            0, "Keys and mouse")                    \
    X(HELP_CLOSE,       "help.close",            0, "F1 or Esc to close")                \
    X(HELP_DRAG,        "help.drag",             0, "move blocks; out of the palette, into slots") \
    X(HELP_PIE,         "help.pie",              0, "the pie menu")                      \
    X(HELP_PAN,         "help.pan",              0, "move around the canvas")            \
    X(HELP_ZOOM,        "help.zoom",             0, "zoom (Ctrl + = / - / 0 too)")       \
    X(HELP_HOME,        "help.home",             0, "back to the start of the file")     \
    X(HELP_SAVE,        "help.save",             0, "save")                              \
    X(HELP_UNDO,        "help.undo",             0, "undo / redo")                       \
    X(HELP_EDIT,        "help.edit",             0, "delete, duplicate, switch off the block under the mouse") \
    X(HELP_FIND,        "help.find",             0, "find blocks; Enter: next, Shift+Enter: back") \
    X(HELP_FILES,       "help.files",            0, "the files panel; the next open file") \
    X(HELP_BUILD,       "help.build",            0, "build; build and run")              \
    X(HELP_FOLD,        "help.fold",             0, "fold or unfold a block")            \
    X(HELP_RELOAD,      "help.reload",           0, "read the file again from disk")     \
    X(HELP_HELP,        "help.help",             0, "this list")                         \
    X(HELP_K_DRAG,      "help.k_drag",           0, "Drag")                              \
    X(HELP_K_RIGHT,     "help.k_right",          0, "Right click")                       \
    X(HELP_K_PAN,       "help.k_pan",            0, "Right / middle drag")               \
    X(HELP_K_WHEEL,     "help.k_wheel",          0, "Wheel")                             \
    X(HELP_K_DOUBLE,    "help.k_double",         0, "Double click")                      \
    X(HELP_LICENSE,     "help.license",          0, "The agreement")                     \
    X(TIP_RUN,          "tip.run",               0, "Build and run (Ctrl+R)")            \
    X(TIP_STOP,         "tip.stop",              0, "Stop")                              \
    X(TIP_FIT,          "tip.fit",               0, "Whole file in view (Home)")         \
    X(CAT_CONTROL,      "palette.control",       0, "Control")                           \
    X(CAT_OPERATORS,    "palette.operators",     0, "Operators")                         \
    X(CAT_VARIABLES,    "palette.variables",     0, "Variables")                         \
    X(CAT_POINTERS,     "palette.pointers",      0, "Pointers")                          \
    X(CAT_FUNCTIONS,    "palette.functions",     0, "Functions")                         \
    X(CAT_TYPES,        "palette.types",         0, "Types")                             \
    X(CAT_DECLS,        "palette.declarations",  0, "File")                              \
    X(CAT_CM_PROJECT,   "palette.cm_project",    0, "Project")                           \
    X(CAT_CM_TARGETS,   "palette.cm_targets",    0, "Targets")                           \
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
      "F1: keys and mouse") \
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
    X(PIE_FOLD,         "pie.fold",              0, "Fold")                              \
    X(PIE_UNFOLD,       "pie.unfold",            0, "Unfold")                            \
    X(PIE_KEY_DOUBLE,   "pie.key.double_click",  0, "double click")                      \
    X(PIE_CLICK_FIELD,  "pie.key.click_field",   0, "click field")                       \
    X(PIE_KEY_DELETE,   "pie.key.delete",        0, "Del")                               \
    /* saving refused */                                                                 \
    X(GEN_UNSAFE_GAP,   "save.unsafe_gap",       0,                                      \
      "line %u: this block has #if/#define or macro-made code between its statements, " \
      "so it cannot be rewritten safely yet")                                            \
    X(GEN_LOST,         "save.lost",             0,                                      \
      "line %u: changed code in a place I cannot locate in the file")                    \
    X(GEN_EMPTY_SLOT,   "save.empty_slot",       0,                                      \
      "line %u: an empty slot; type into it or drop a block there") \
    X(THEME_SHRINE,     "theme.shrine",          0, "Shrine")                            \
    X(THEME_BUTTON,     "topbar.theme",          0, "Theme")                             \
    X(THEME_STATUS,     "status.theme",          0, "Theme: %s")                         \
    X(JUMP_MISSING,     "jump.missing",          0, "no label %s: in this function")     \
    X(JUMP_ELSEWHERE,   "jump.elsewhere",        0,                                      \
      "label %s: is not in this function (loose on the canvas, or in another one)")      \
    /* the agreement (a joke; LICENSE is the real thing) */ \
    X(EULA_TITLE, "eula.title", 0, \
      "IHATERUSTL") \
    X(EULA_FULL_NAME, "eula.full_name", 0, \
      "Insane Harmonic Amazing Trust Elementary Random United Standardized Temple License") \
    X(EULA_SUBTITLE, "eula.subtitle", 0, \
      "End User License Agreement. Has no legal force whatsoever.") \
    X(EULA_OPENING, "eula.opening", 0, \
      "This is Scratch. It grew up, met RMS and understood that freedom matters most. The blocks are still sacred, though.") \
    X(EULA_AGREE, "eula.agree", 0, \
      "By pressing \"I accept\", the User agrees:") \
    X(EULA_C01, "eula.c01", 0, \
      "To say \"GNU/Linux\" (or \"GNU plus Linux\") and \"the Linux kernel\", never just \"Linux\". I'd just like to interject for a moment.") \
    X(EULA_C02, "eula.c02", 0, \
      "If you write C++, you accept being one of the causes of the phantom pains of the esteemed Linus Torvalds, who called it a horrible language back in 2007.") \
    X(EULA_C03, "eula.c03", 0, \
      "Not to use the Software during a game of chaturanga against an ancient Indian chess player, for whom the queen (the mantri, one step diagonally) is the most useless piece on the board.") \
    X(EULA_C04, "eula.c04", 0, \
      "To use the Software for trolling only on Thursdays that fall on the 30th of February of a non-leap year.") \
    X(EULA_C05, "eula.c05", 0, \
      "Not to use the Software while playing Go during a volcanic eruption within 100 metres of yourself, dressed in the internet's classic Japanese maid outfit, which on closer inspection is the far more provocative French maid outfit of the 16th to 18th centuries.") \
    X(EULA_C06, "eula.c06", 0, \
      "Not to rewrite the Software in Rust. An attempt counts as a full confession.") \
    X(EULA_C07, "eula.c07", 0, \
      "Not to keep the Software in the donation box of the Hakurei Shrine: it is empty anyway, do not give anyone false hope.") \
    X(EULA_C08, "eula.c08", 0, \
      "To acknowledge that goto is considered harmful (Dijkstra, 1968). Ccratch draws an arrow to its label, so you can see exactly where the harm goes.") \
    X(EULA_C09, "eula.c09", 0, \
      "To acknowledge NULL as a billion-dollar mistake (Hoare, 2009) and to contribute to it as best you can.") \
    X(EULA_C10, "eula.c10", 0, \
      "To acknowledge that undefined behaviour may make demons fly out of your nose (comp.std.c, 1992). Ccratch takes no responsibility for the demons.") \
    X(EULA_C11, "eula.c11", 0, \
      "That this Agreement is valid until 03:14:07 UTC on 19 January 2038, when time_t overflows and the Agreement comes into force again from 13 December 1901.") \
    X(EULA_C12, "eula.c12", 0, \
      "To look at Duff's device (1983) at least once in your life and feel something.") \
    X(EULA_C13, "eula.c13", 0, \
      "If you use Emacs, that it is a great operating system lacking only a good text editor. If you use Vim, that you still do not know how to quit it.") \
    X(EULA_C14, "eula.c14", 0, \
      "That an opening brace on the same line follows the will of Kernighan and Ritchie, on a line of its own the will of Allman, and indented by two spaces (GNU) needs a written explanation.") \
    X(EULA_C15, "eula.c15", 0, \
      "Kernighan first printed \"hello, world\" in 1972, in B. The User undertakes to print it at least once in every new language, except Rust.") \
    X(EULA_MEME_ZONE, "eula.meme_zone", 0, \
      "MEME ZONE. Everything below is memes and references. If you are offended, read the last point again.") \
    X(EULA_M01, "eula.m01", 0, \
      "If you are a femboy, you may not write C or assembly: your place is in C++. The Rust community has appealed, citing exclusive rights to programming socks. The appeal is being reviewed by the borrow checker; no date has been set.") \
    X(EULA_M02, "eula.m02", 0, \
      "If you are a girl, welcome to the low-level programming course for the littlest ones. It is taught by Ada Lovelace (the first published program, 1843) and Grace Hopper (the first compilers, and the first actual bug: a moth in a relay, 1947). So, honestly, the littlest ones here are us.") \
    X(EULA_FINAL, "eula.final", 0, \
      "None of the above has any legal force. The real license is the Mozilla Public License 2.0 (see LICENSE). The shrine maiden enforces nothing; she only collects donations.") \
    X(EULA_ACCEPT, "eula.accept", 0, \
      "I accept") \
    X(EULA_RUST, "eula.rust", 0, \
      "I write Rust") \
    X(EULA_CLOSE, "eula.close", 0, \
      "Close") \
    X(EULA_ACCEPTED, "eula.accepted", 0, \
      "Agreement accepted. Welcome to the shrine.") \
    X(EULA_RUSTED, "eula.rusted", 0, \
      "Noted. The borrow checker has been informed. Ccratch will still let you free() twice.")

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

/* Lower case for matching what someone typed: ASCII, and Cyrillic
 * (А-Я, Ё) in UTF-8. */
void ccr_fold_case(const char *in, char *out, size_t cap);

#endif /* CCR_LANG_H */
