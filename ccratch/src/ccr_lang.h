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
    X(CMAKE_BUTTON,     "topbar.cmake",          0, "CMake")                             \
    X(PALETTE_TITLE,    "palette.title",         0, "Blocks")                            \
    X(PALETTE_DROP,     "palette.drop",          0, "drop to delete")                    \
    X(TEMPLATE_CODE,    "palette.code_line",     0, "code line")                         \
    X(FILES_TITLE,      "files.title",           0, "Files")                             \
    X(FILES_EMPTY,      "files.empty",           0, "no .c, .h or .s files here")            \
    X(FILES_UNSAVED,    "files.unsaved",         0, "Not saved! Click x again to close anyway") \
    X(FILES_TOO_MANY,   "files.too_many",        0, "Too many files open: close one first") \
    X(FILES_NEW_FILE,   "files.new_file",        0, "+ file")                            \
    X(FILES_NEW_DIR,    "files.new_dir",         0, "+ folder")                          \
    X(FILES_NAME_FILE,  "files.name_file",       0, "File name (.c, .h, .s or .asm), Enter to create") \
    X(FILES_NAME_DIR,   "files.name_dir",        0, "Folder name, Enter to create")      \
    X(FILES_EXISTS,     "files.exists",          0, "%s is already there")               \
    X(FILES_CREATED,    "files.created",         0, "%s created")                        \
    X(FILES_CANNOT,     "files.cannot",          0, "Could not create %s")               \
    X(INCLUDE_NOT_FOUND, "include.not_found",    0, "%s is neither next to this file nor in the project") \
    X(INCLUDE_SYSTEM,   "include.system",        0, "%s is a system header, not opening it") \
    X(LIBS_TITLE,       "libs.title",            0, "Libraries")                         \
    X(PROJECT_BLOCKS,   "palette.project",       0, "This project")                      \
    X(NAV_NONE,         "nav.none",              0, "No name under the mouse to go to")  \
    X(NAV_UNSAVED,      "nav.unsaved",           0, "This block is not in the file yet: save first") \
    X(NAV_CANNOT,       "nav.cannot",            0, "Cannot tell: %s")                   \
    X(NAV_NO_DEF,       "nav.no_def",            0, "%s is made nowhere in the project") \
    X(NAV_WENT,         "nav.went",              0, "%s: %s, line %d (Alt+Left: back)")  \
    X(NAV_REFS,         "nav.refs",              0, "%s: %d places in %d files")         \
    X(NAV_KIND_DEF,     "nav.kind_def",          0, "made here")                         \
    X(NAV_KIND_DECL,    "nav.kind_decl",         0, "declared")                          \
    X(NAV_KIND_USE,     "nav.kind_use",          0, "used")                              \
    X(NAV_FROM,         "nav.from",              0, "from %s")                           \
    X(NAV_FROM_HERE,    "nav.from_here",         0, "in this file")                      \
    X(NAV_HINT,         "nav.hint",              0, "Ctrl+click or F12: go there, Shift+F12: where it is used") \
    X(HELP_NAV,         "help.nav",              0, "to where it is made (also Ctrl+click), where it is used, back and forward") \
    X(REN_TITLE,        "ren.title",             0, "Rename %s: %d places in %d files")  \
    X(REN_HINT,         "ren.hint",              0, "Enter renames it everywhere, Esc leaves it") \
    X(REN_BAD,          "ren.bad",               0, "That is no C name (letters, digits, _; no keyword)") \
    X(REN_NONE,         "ren.none",              0, "F2 renames a name: point at a function, a variable, a type") \
    X(REN_CANNOT,       "ren.cannot",            0, "Not renamed: %s")                   \
    X(REN_DONE,         "ren.done",              0, "Renamed %s to %s: %d places in %d files (Ctrl+Z takes it back)") \
    X(REN_UNDONE,       "ren.undone",            0, "%s is %s again, in every file")     \
    X(REN_UNDO_STALE,   "ren.undo_stale",        0, "The files changed since the rename: it stays") \
    X(HELP_RENAME,      "help.rename",           0, "rename the name under the mouse in every file of the project") \
    X(HELP_SEARCH,      "help.search",           0, "search the whole project, and replace (Ctrl+Shift+H)") \
    X(ARR_TIP,          "arrange.tip",           0, "Arrange the blocks: in columns, as a newspaper, by who calls whom") \
    X(ARR_TITLE,        "arrange.title",         0, "Arrange the blocks")                \
    X(ARR_COLUMNS,      "arrange.columns",       0, "Columns")                           \
    X(ARR_COLUMNS_HINT, "arrange.columns_hint",  0, "#include and types on the left, functions on the right") \
    X(ARR_TILES,        "arrange.tiles",         0, "Newspaper")                             \
    X(ARR_TILES_HINT,   "arrange.tiles_hint",    0, "in the file's order: down a column, then the next, as wide as the screen") \
    X(ARR_CALLS,        "arrange.calls",         0, "By calls")                          \
    X(ARR_CALLS_HINT,   "arrange.calls_hint",    0, "main on the left, what it calls to its right, and on") \
    X(ARR_FREE,         "arrange.free",          0, "Or drag any block anywhere: it stays where you put it, lines up with its neighbours on the way, and is there again when the file opens. Ctrl+Z puts an arrangement back.") \
    X(ARR_DONE,         "arrange.done",          0, "Arranged: %s (Ctrl+Z puts it back)") \
    X(ARR_NEW,          "arrange.new",           0, "+ An arrangement of your own...")  \
    X(ARR_MINE,         "arrange.mine",          0, "Mine")                              \
    X(ARR_EDIT_TIP,     "arrange.edit_tip",      0, "Change it")                         \
    X(ARR_DELETE_TIP,   "arrange.delete_tip",    0, "Delete it")                         \
    X(ARR_DELETE_ASK,   "arrange.delete_ask",    0, "Click again: it is deleted")        \
    X(ARR_DEFAULT_TIP,  "arrange.default_tip",   0, "The star: files that open for the first time are laid out like this") \
    X(ARR_DEFAULT_NOW,  "arrange.default_now",   0, "Files that open for the first time are laid out: %s") \
    X(ARR_BUILDER,      "arrange.builder",       0, "An arrangement of your own")        \
    X(ARR_NAME,         "arrange.name",          0, "Its name")                          \
    X(ARR_HINT,         "arrange.hint",          0, "Columns, left to right. Each takes what is ticked in it; what no column takes goes to \"everything else\". It fits any file: it is rules, not places.") \
    X(ARR_COLUMN,       "arrange.column",        0, "Column %d")                         \
    X(ARR_ADD_COLUMN,   "arrange.add_column",    0, "+ column")                          \
    X(ARR_K_INCLUDES,   "arrange.k_includes",    0, "#include, using")                   \
    X(ARR_K_MACROS,     "arrange.k_macros",      0, "#define")                           \
    X(ARR_K_TYPES,      "arrange.k_types",       0, "types")                             \
    X(ARR_K_GLOBALS,    "arrange.k_globals",     0, "global variables")                  \
    X(ARR_K_PROTOS,     "arrange.k_protos",      0, "function declarations")             \
    X(ARR_K_MAIN,       "arrange.k_main",        0, "main")                              \
    X(ARR_K_FUNCS,      "arrange.k_funcs",       0, "functions")                         \
    X(ARR_K_CLASSES,    "arrange.k_classes",     0, "classes, namespaces")               \
    X(ARR_K_LOOSE,      "arrange.k_loose",       0, "loose stacks")                      \
    X(ARR_K_REST,       "arrange.k_rest",        0, "everything else")                   \
    X(ARR_ORDER_FILE,   "arrange.order_file",    0, "as in the file")                    \
    X(ARR_ORDER_NAME,   "arrange.order_name",    0, "by name")                           \
    X(ARR_ORDER_SIZE,   "arrange.order_size",    0, "the biggest first")                 \
    X(ARR_ORDER_CALLS,  "arrange.order_calls",   0, "callers first")                     \
    X(ARR_ORDER_TIP,    "arrange.order_tip",     0, "The order down the column (a click: the next one)") \
    X(ARR_WRAP_NONE,    "arrange.wrap_none",     0, "one column")                        \
    X(ARR_WRAP_OFTEN,   "arrange.wrap_often",    0, "wraps often")                       \
    X(ARR_WRAP_SOME,    "arrange.wrap_some",     0, "wraps now and then")                \
    X(ARR_WRAP_RARE,    "arrange.wrap_rare",     0, "wraps rarely")                      \
    X(ARR_WRAP_TIP,     "arrange.wrap_tip",      0, "Too tall: goes on in a column beside it, like a newspaper (a click: the next)") \
    X(ARR_PREVIEW,      "arrange.preview",       0, "The file on screen, so arranged:")  \
    X(ARR_ON_OPEN,      "arrange.on_open",       0, "lay out files like this when they open for the first time") \
    X(ARR_SAVE,         "arrange.save",          0, "Save")                              \
    X(ARR_TRY,          "arrange.try",           0, "Try on this file")                  \
    X(ARR_CANCEL,       "arrange.cancel",        0, "Cancel")                            \
    X(ARR_SAVED,        "arrange.saved",         0, "Saved: %s (in the arrange menu now)") \
    X(ARR_NOT_SAVED,    "arrange.not_saved",     0, "Not saved: %s")                     \
    X(SRCH_TITLE,       "search.title",          0, "Search")                            \
    X(SRCH_TAB_TIP,     "search.tab_tip",        0, "Find in every file, and replace (Ctrl+Shift+F, Ctrl+Shift+H)") \
    X(SRCH_TEXT,        "search.text",           0, "Text")                              \
    X(SRCH_NAMES,       "search.names",          0, "Names")                             \
    X(SRCH_TEXT_TIP,    "search.text_tip",       0, "Wherever a file says it: the code, the strings, the comments") \
    X(SRCH_NAMES_TIP,   "search.names_tip",      0, "Everything the project calls that, as the compiler sees it: a function, a field, a local of another function... each on its own") \
    X(SRCH_PROJECT,     "search.project",        0, "whole project")                     \
    X(SRCH_THIS_FILE,   "search.this_file",      0, "this file")                         \
    X(SRCH_SCOPE_TIP,   "search.scope_tip",      0, "Where to look: every file of the project, or only the one on screen") \
    X(SRCH_FIND,        "search.find",           0, "Find (Enter: to the next one)")     \
    X(SRCH_FIND_NAME,   "search.find_name",      0, "A name, Enter")                     \
    X(SRCH_WITH,        "search.with",           0, "Replace with")                      \
    X(SRCH_WITH_NAME,   "search.with_name",      0, "Rename to")                         \
    X(SRCH_REPLACE_TIP, "search.replace_tip",    0, "Replace (Ctrl+Shift+H)")            \
    X(SRCH_CASE_TIP,    "search.case_tip",       0, "The case matters: Hello is not hello") \
    X(SRCH_WORD_TIP,    "search.word_tip",       0, "Whole words: not inside a longer name") \
    X(SRCH_KEEP_TIP,    "search.keep_tip",       0, "Keep the case: Hello becomes Hi, HELLO becomes HI") \
    X(SRCH_IN_ALL,      "search.in_all",         0, "everywhere")                        \
    X(SRCH_IN_CODE,     "search.in_code",        0, "code")                              \
    X(SRCH_IN_STRINGS,  "search.in_strings",     0, "strings")                           \
    X(SRCH_IN_COMMENTS, "search.in_comments",    0, "comments")                          \
    X(SRCH_COUNT,       "search.count",          0, "Found: %d, files: %d")              \
    X(SRCH_NONE,        "search.none",           0, "Nothing found")                     \
    X(SRCH_MANY,        "search.many",           0, "(the first %d shown)")             \
    X(SRCH_EMPTY,       "search.empty",          0, "Type what to find: every file of the project is looked through as you type. The > opens Replace: one place, a whole file or all of them, and one Ctrl+Z puts it all back.") \
    X(SRCH_REPLACE_ALL, "search.replace_all",    0, "Replace all (%d)")                  \
    X(SRCH_RENAME_ALL,  "search.rename_all",     0, "Rename all (%d)")                   \
    X(SRCH_REPLACE_ONE_TIP, "search.replace_one_tip", 0, "Replace this one")            \
    X(SRCH_REPLACE_FILE_TIP, "search.replace_file_tip", 0, "Replace all in this file")  \
    X(SRCH_DISMISS_TIP, "search.dismiss_tip",    0, "Leave this one out")                \
    X(SRCH_RENAME_TIP,  "search.rename_tip",     0, "Rename this one only")              \
    X(SRCH_REPLACED,    "search.replaced",       0, "Replaced: %d, files: %d (Ctrl+Z takes it back)") \
    X(SRCH_CANNOT,      "search.cannot",         0, "Cannot write %s")                   \
    X(SRCH_NAMES_COUNT, "search.names_count",    0, "Things: %d, places: %d, files: %d") \
    X(SRCH_NAMES_HINT,  "search.names_hint",     0, "Enter: everything called %s")       \
    X(SRCH_NOT_OURS,    "search.not_ours",       0, "not the project's")                 \
    X(SRCH_NOT_OURS_TIP, "search.not_ours_tip",  0, "It comes from outside the project (a system header, a library): it is shown, not renamed") \
    X(SRCH_SYM_FUNCTION, "search.sym_function",  0, "function")                          \
    X(SRCH_SYM_METHOD,  "search.sym_method",     0, "method of %s")                      \
    X(SRCH_SYM_GLOBAL,  "search.sym_global",     0, "global variable")                   \
    X(SRCH_SYM_LOCAL,   "search.sym_local",      0, "variable in %s()")                  \
    X(SRCH_SYM_PARAM,   "search.sym_param",      0, "parameter of %s()")                 \
    X(SRCH_SYM_FIELD,   "search.sym_field",      0, "field of %s")                       \
    X(SRCH_SYM_TYPE,    "search.sym_type",       0, "type")                              \
    X(SRCH_SYM_ENUM,    "search.sym_enum",       0, "enum value")                        \
    X(SRCH_SYM_MACRO,   "search.sym_macro",      0, "macro")                             \
    X(SRCH_SYM_NAMESPACE, "search.sym_namespace", 0, "namespace")                        \
    X(SRCH_SYM_OTHER,   "search.sym_other",      0, "name")                              \
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
    X(COPIED,           "clip.copied",           1, "Copied: %u block|Copied: %u blocks") \
    X(COPY_NONE,        "clip.none",             0, "Nothing to copy here")              \
    X(COPY_HOLE,        "clip.hole",             0, "Not copied: a slot is empty, put something in it first") \
    X(PASTED,           "clip.pasted",           1, "Pasted: %u block, click where it goes|Pasted: %u blocks, click where they go") \
    X(PASTED_DECLS,     "clip.pasted_decls",     1, "Pasted into the file: %u declaration|Pasted into the file: %u declarations") \
    X(PASTED_TEXT,      "clip.as_text",          1, "(%u as a code line: clang could not read it here)|(%u as code lines: clang could not read them here)") \
    X(PASTE_EMPTY,      "clip.empty",            0, "The clipboard has no code in it")   \
    X(SELECTED,         "sel.count",             1, "%u selected: Del deletes, Ctrl+C copies|%u selected: Del deletes, Ctrl+C copies") \
    X(DELETED,          "sel.deleted",           1, "%u block deleted (Ctrl+Z brings it back)|%u blocks deleted (Ctrl+Z brings them back)") \
    X(DBG_TITLE,        "dbg.title",             0, "Debugger")                          \
    X(DBG_INPUT,        "dbg.input",             0, "Type to the program (Enter sends)") \
    X(DBG_EOF,          "dbg.eof",               0, "The end of its input (Ctrl+D)")     \
    X(DBG_CONTINUE,     "dbg.continue",          0, "Continue")                          \
    X(DBG_NEXT,         "dbg.next",              0, "Step")                              \
    X(DBG_STEP,         "dbg.step",              0, "Into")                              \
    X(DBG_OUT,          "dbg.out",               0, "Out")                               \
    X(DBG_PAUSE,        "dbg.pause",             0, "Pause")                             \
    X(DBG_STOP,         "dbg.stop",              0, "Stop")                              \
    X(DBG_AGAIN,        "dbg.again",             0, "Again")                             \
    X(DBG_VARS,         "dbg.vars",              0, "Variables")                         \
    X(DBG_STACK,        "dbg.stack",             0, "Call stack")                        \
    X(DBG_OUTPUT,       "dbg.output",            0, "What the program printed")          \
    X(DBG_STARTING,     "dbg.starting",          0, "Building, then gdb starts the program...") \
    X(DBG_RUNNING,      "dbg.running",           0, "Running (F9 on a block: stop there; Pause: stop now)") \
    X(DBG_AT_BREAK,     "dbg.at_break",          0, "Stopped at a breakpoint: %s, line %d") \
    X(DBG_STOPPED,      "dbg.stopped",           0, "Stopped: %s, line %d")              \
    X(DBG_CRASHED,      "dbg.crashed",           0, "Crashed (%s) in %s, line %d")       \
    X(DBG_EXITED,       "dbg.exited",            0, "The program finished with code %d") \
    X(DBG_KILLED,       "dbg.killed",            0, "The program was killed by %s")      \
    X(DBG_ENDED,        "dbg.ended",             0, "Debugging is over")                 \
    X(DBG_NO_GDB,       "dbg.no_gdb",            0, "gdb is not installed (sudo apt install gdb), so no debugging") \
    X(DBG_NO_PROGRAM,   "dbg.no_program",        0, "Nothing to debug: the build made no program") \
    X(DBG_BP_ON,        "dbg.bp_on",             0, "Breakpoint: the program will stop on this block (F9 again takes it away)") \
    X(DBG_BP_OFF,       "dbg.bp_off",            0, "Breakpoint taken away")             \
    X(DBG_BP_NONE,      "dbg.bp_none",           0, "A breakpoint goes on a block inside a function: point at one and press F9") \
    X(DBG_BP_LATER,     "dbg.bp_later",          0, "Breakpoint set; this block is new, so it counts from the next build") \
    X(DBG_NONE_HERE,    "dbg.none_here",         0, "(none here)")                       \
    X(DBG_NO_SOURCE,    "dbg.no_source",         0, "no source")                         \
    X(DBG_TIP,          "dbg.tip",               0, "Debug: build, run under gdb, stop at breakpoints (F8)") \
    X(DBG_KEYS,         "dbg.keys",              0, "F8 continue, F10 step, F11 into, Shift+F11 out, F9 breakpoint") \
    X(HELP_DEBUG,       "help.debug",            0, "debugger: start / continue, breakpoint on the block under the mouse, step, into, out") \
    X(AGO_NOW,          "ago.now",               0, "just now")                          \
    X(AGO_MINUTES,      "ago.minutes",           1, "%u minute ago|%u minutes ago")      \
    X(AGO_HOURS,        "ago.hours",             1, "%u hour ago|%u hours ago")          \
    X(AGO_DAYS,         "ago.days",              1, "%u day ago|%u days ago")            \
    X(AGO_WEEKS,        "ago.weeks",             1, "%u week ago|%u weeks ago")          \
    X(AGO_MONTHS,       "ago.months",            1, "%u month ago|%u months ago")        \
    X(AGO_YEARS,        "ago.years",             1, "%u year ago|%u years ago")          \
    X(GIT_OURS,         "git.ours",              0, "ours")                              \
    X(GIT_THEIRS,       "git.theirs",            0, "theirs")                            \
    X(GIT_BOTH,         "git.both",              0, "both")                              \
    X(GIT_RESOLVED,     "git.resolved",          0, "%s resolved and staged")            \
    X(GIT_MERGE,        "git.merge",             0, "merge in")                          \
    X(GIT_MERGED_IN,    "git.merged_in",         0, "%s merged in")                      \
    X(GIT_CONFLICTED,   "git.conflicted",        0, "Merging %s: some files conflict, resolve them below") \
    X(GIT_MERGING,      "git.merging",           0, "%s: resolve the conflicts, then Commit") \
    X(GIT_MERGE_READY,  "git.merge_ready",       0, "%s: all resolved, Commit finishes it") \
    X(GIT_OP_MERGE,     "git.op_merge",          0, "Merging")                           \
    X(GIT_OP_REVERT,    "git.op_revert",         0, "Reverting")                         \
    X(GIT_OP_PICK,      "git.op_pick",           0, "Cherry-picking")                    \
    X(GIT_MERGE_ABORT,  "git.merge_abort",       0, "Give up")                           \
    X(GIT_MERGED,       "git.merged",            0, "Merged")                            \
    X(GIT_STASH,        "git.stash",             0, "Stash")                             \
    X(GIT_STASHED,      "git.stashed",           0, "Stashed")                           \
    X(GIT_STASHED_NOW,  "git.stashed_now",       0, "Changes put aside: the files are as committed (Stashed, below, brings them back)") \
    X(GIT_UNSTASH,      "git.unstash",           0, "bring back")                        \
    X(GIT_DROP_STASH,   "git.drop_stash",        0, "Throw it away")                     \
    X(TIP_STASH,        "tip.stash",             0, "Put every change aside for now, and get the files as committed") \
    X(GIT_BLAME,        "git.blame",             0, "who changed each block (point at one)") \
    X(GIT_BLAME_MINE,   "git.blame_mine",        0, "You, not committed yet")            \
    X(GIT_BLAME_MINE_HINT, "git.blame_mine_hint", 0, "This block changed since the last commit.") \
    X(GIT_BLAME_OPEN,   "git.blame_open",        0, "click the badge: the whole commit") \
    X(GIT_AND_MORE,     "git.and_more",          1, "%s and %u more|%s and %u more")     \
    X(GIT_TOGETHER,     "git.together",          0, "together with %s")                  \
    X(GIT_COAUTHOR_ADD, "git.coauthor_add",      0, "+ co-author")                       \
    X(GIT_COAUTHOR_NEW, "git.coauthor_new",      0, "someone else: Name <mail>, Enter")  \
    X(GIT_COAUTHOR_TIP, "git.coauthor_tip",      0, "Made it together? Their names go into the message as Co-authored-by: lines, and GitHub shows everyone") \
    X(GIT_COAUTHORS_FULL, "git.coauthors_full",  0, "%u co-authors at most")             \
    X(GIT_REVERT,       "git.revert",            0, "Revert")                            \
    X(GIT_CHERRY,       "git.cherry",            0, "Cherry-pick")                       \
    X(GIT_BRANCH_HERE,  "git.branch_here",       0, "Branch from here")                  \
    X(GIT_REVERTED,     "git.reverted",          0, "Undone by a new commit: %s")        \
    X(GIT_PICKED,       "git.picked",            0, "Brought onto this branch: %s")      \
    X(GIT_NAME_BRANCH,  "git.name_branch",       0, "Name the branch that starts at %.8s, Enter") \
    X(GIT_TITLE,        "git.title",             0, "Git")                               \
    X(GIT_NO_GIT,       "git.no_git",            0, "git is not installed (sudo apt install git)") \
    X(GIT_NO_REPO,      "git.no_repo",           0, "This project is not in a git repository") \
    X(GIT_INIT,         "git.init",              0, "Make it one (git init)")            \
    X(GIT_MESSAGE,      "git.message",           0, "What it does (Ctrl+Enter commits)") \
    X(GIT_COMMIT,       "git.commit",            0, "Commit")                            \
    X(GIT_AMEND,        "git.amend",             0, "into the last one")                 \
    X(GIT_STAGED,       "git.staged",            0, "Staged")                            \
    X(GIT_CHANGES,      "git.changes",           0, "Changes")                           \
    X(GIT_CONFLICTS,    "git.conflicts",         0, "Conflicts")                         \
    X(GIT_HISTORY,      "git.history",           0, "History")                           \
    X(GIT_CLEAN,        "git.clean",             0, "Nothing changed since the last commit") \
    X(GIT_COMMITTED,    "git.committed",         0, "Committed: %s")                     \
    X(GIT_COMMITTED_ALL,"git.committed_all",     0, "Nothing was staged, so every change went in: %s") \
    X(GIT_NOTHING,      "git.nothing",           0, "Nothing to commit")                 \
    X(GIT_NEED_MESSAGE, "git.need_message",      0, "First write what the commit does")  \
    X(GIT_PULL,         "git.pull",              0, "Pull")                              \
    X(GIT_PUSH,         "git.push",              0, "Push")                              \
    X(GIT_FETCH,        "git.fetch",             0, "Fetch")                             \
    X(GIT_PULLED,       "git.pulled",            0, "Pulled")                            \
    X(GIT_PUSHED,       "git.pushed",            0, "Pushed")                            \
    X(GIT_FETCHED,      "git.fetched",           0, "Fetched")                           \
    X(GIT_NET_FAILED,   "git.net_failed",        0, "Did not work: %s")                  \
    X(GIT_NO_REMOTE,    "git.no_remote",         0, "Nowhere to push: no remote yet (git remote add origin URL)") \
    X(GIT_NEW_BRANCH,   "git.new_branch",        0, "a new branch: its name, Enter")     \
    X(GIT_SWITCHED,     "git.switched",          0, "On branch %s now")                  \
    X(GIT_DISCARD_ASK,  "git.discard_ask",       0, "Throw away what changed in %s? Click again") \
    X(GIT_DISCARDED,    "git.discarded",         0, "What changed in %s is thrown away") \
    X(GIT_DIFF_STAGED,  "git.diff_staged",       0, "%s: staged")                        \
    X(GIT_DIFF_WORK,    "git.diff_work",         0, "%s: not staged")                    \
    X(GIT_DIFF_NEW,     "git.diff_new",          0, "%s: new file")                      \
    X(GIT_OPEN,         "git.open",              0, "Open")                              \
    X(GIT_HUNK_IN,      "git.hunk_in",           0, "+ stage this part")                 \
    X(GIT_HUNK_OUT,     "git.hunk_out",          0, "- unstage this part")               \
    X(GIT_RELOADED,     "git.reloaded",          0, "%s changed on disk: read again")    \
    X(GIT_KEPT,         "git.kept",              0, "%s changed on disk, but has unsaved changes here: kept as it is") \
    X(GIT_DETACHED,     "git.detached",          0, "(no branch)")                       \
    X(GIT_WORKING,      "git.working",           0, "git is talking to the server...")   \
    X(TIP_GIT,          "tip.git",               0, "Git: what changed, commit, branches, push and pull (Ctrl+G)") \
    X(TIP_STAGE,        "tip.stage",             0, "Stage")                             \
    X(TIP_UNSTAGE,      "tip.unstage",           0, "Unstage")                           \
    X(TIP_DISCARD,      "tip.discard",           0, "Throw the changes away")            \
    X(HELP_GIT,         "help.git",              0, "git: what changed (bars beside blocks), commit, branches, history, stash, merge, push, pull") \
    X(AUTOSAVE_ON,      "autosave.on",           0, "autosave: on")                      \
    X(AUTOSAVE_OFF,     "autosave.off",          0, "autosave: off")                     \
    X(TIP_AUTOSAVE,     "tip.autosave",          0, "Save by itself a moment after every change") \
    X(TIP_MINIMAP,      "tip.minimap",           0, "The whole file (Ctrl+M hides it)")  \
    X(HELP_COPY,        "help.copy",             0, "copy the block under the mouse (or the selected); paste code as blocks") \
    X(HELP_SELECT,      "help.select",           0, "select blocks to delete or copy together") \
    X(HELP_K_BAND,      "help.k_band",           0, "Drag on empty canvas")              \
    X(HELP_MINIMAP,     "help.minimap",          0, "the minimap on and off")            \
    X(HELP_PALETTE,     "help.palette",          0, "bigger or smaller blocks in the palette; drag a panel's edge to size it") \
    X(HELP_K_PALZOOM,   "help.k_palzoom",        0, "Ctrl+wheel on the palette")         \
    X(CAT_CONTROL,      "palette.control",       0, "Control")                           \
    X(CAT_OPERATORS,    "palette.operators",     0, "Operators")                         \
    X(CAT_VARIABLES,    "palette.variables",     0, "Variables")                         \
    X(CAT_POINTERS,     "palette.pointers",      0, "Pointers")                          \
    X(CAT_FUNCTIONS,    "palette.functions",     0, "Functions")                         \
    X(CAT_TYPES,        "palette.types",         0, "Types")                             \
    X(CAT_DECLS,        "palette.declarations",  0, "File")                              \
    X(CAT_CLASSES,      "palette.classes",       0, "Classes")                           \
    X(CAT_STD,          "palette.std",           0, "Standard library")                  \
    X(PAL_LANG_TIP,     "palette.lang_tip",      0, "Blocks for %s (a .cpp file picks C++ by itself)") \
    X(PAL_LANG_OTHER,   "palette.lang_other",    0, "C++ blocks, and %s is C: the C compiler will not take them") \
    X(PAL_DROP_OTHER,   "palette.drop_other",    0, "That is a C++ block, and %s is C: it will not compile there") \
    X(PAL_ASM_ATT,      "palette.asm_att",       0, "GNU as, AT&T syntax: the source first, the destination last; registers start with a percent sign, numbers with $") \
    X(PAL_ASM_INTEL,    "palette.asm_intel",     0, "GNU as, Intel syntax (.intel_syntax): destination first (mov rax, 1)") \
    X(PAL_ASM_NASM,     "palette.asm_nasm",      0, "NASM: destination first (mov rax, 1); ; starts a comment") \
    X(PAL_ASM_ARM64,    "palette.asm_arm64",     0, "64-bit ARM (AArch64: phones, Apple M chips, Raspberry Pi 4 and 5), GNU as: destination first (mov x0, #1); numbers with #, // starts a comment") \
    X(PAL_ASM_ARM32,    "palette.asm_arm32",     0, "32-bit ARM and Thumb (Raspberry Pi OS 32-bit, older phones), GNU as: destination first (mov r0, #1); numbers with #, @ starts a comment") \
    X(PAL_STD_ON,       "palette.std_on",        0, "The standard library's blocks (printf, malloc, std::cout...): shown. A click hides them: only the language and your own functions") \
    X(PAL_STD_OFF,      "palette.std_off",       0, "No standard library: only the language and your own functions. A click brings printf and the rest back") \
    X(SET_STD,          "set.std",               0, "Standard library blocks")           \
    X(SET_STD_HINT,     "set.std_hint",          0, "printf, malloc, std::cout... in a section of their own. Off: only the language itself and your own functions") \
    X(CAT_CM_PROJECT,   "palette.cm_project",    0, "Project")                           \
    X(CAT_CM_TARGETS,   "palette.cm_targets",    0, "Targets")                           \
    X(CAT_ASM,          "palette.asm",           0, "Assembly")                          \
    X(CAT_ASM_MOVE,     "palette.asm_move",      0, "Moving data")                       \
    X(CAT_ASM_MATH,     "palette.asm_math",      0, "Arithmetic")                        \
    X(CAT_ASM_JUMPS,    "palette.asm_jumps",     0, "Jumps")                             \
    X(CAT_ASM_DATA,     "palette.asm_data",      0, "Sections and data")                 \
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
    X(PICK_THEMES,      "pick.themes",           0, "Themes") \
    X(PICK_LANGS,       "pick.langs",            0, "Language")                          \
    X(PICK_BUILTIN,     "pick.builtin",          0, ", built in")                        \
    X(UI_SCALE,         "ui.scale",              0, "Interface size: %d%% (Ctrl+Alt+0: as the screen wants)") \
    X(UI_SCALE_AUTO,    "ui.scale_auto",         0, "Interface size: %d%%, as the screen wants") \
    X(HELP_UI_SCALE,    "help.ui_scale",         0, "the interface bigger / smaller / as the screen wants") \
    X(PICK_PLAIN,       "pick.plain",            0, "Human language")                    \
    X(PICK_PLAIN_HINT,  "pick.plain_hint",       0, "Everything said in plain words, for people who don't program") \
    X(PICK_THEME_HINT,  "pick.theme_hint",       0, "Your own: a .theme file in ~/.local/share/ccratch/themes (ccratch --check-theme checks it)") \
    X(PICK_LANG_HINT,   "pick.lang_hint",        0, "Your own: ccratch --lang-template > CODE.lang, translated, in ~/.local/share/ccratch/lang") \
    X(THEME_STATUS,     "status.theme",          0, "Theme: %s")                         \
    X(JUMP_MISSING,     "jump.missing",          0, "no label %s: in this function")     \
    X(JUMP_ELSEWHERE,   "jump.elsewhere",        0,                                      \
      "label %s: is not in this function (loose on the canvas, or in another one)")      \
    X(SET_TITLE,        "set.title", 0, "Settings") \
    X(SET_TIP,          "set.tip", 0, "Settings (Ctrl+,)") \
    X(SET_CLOSE,        "set.close", 0, "Ctrl+, or Esc to close") \
    X(SET_SEC_LOOK,     "set.sec_look", 0, "Look") \
    X(SET_SEC_EDITOR,   "set.sec_editor", 0, "Editor") \
    X(SET_SEC_BUILD,    "set.sec_build", 0, "Building") \
    X(SET_SEC_OTHER,    "set.sec_other", 0, "Other") \
    X(SET_THEME_HINT,   "set.theme_hint", 0, "The colours of everything. Click the name for the next one") \
    X(SET_LANG_HINT,    "set.lang_hint", 0, "What Ccratch speaks. Click the name for the next one") \
    X(SET_UI_SCALE,     "set.ui_scale", 0, "Interface size") \
    X(SET_UI_SCALE_HINT, "set.ui_scale_hint", 0, "Everything bigger or smaller; Ctrl+Alt+= and Ctrl+Alt+- do it too") \
    X(SET_AUTO,         "set.auto", 0, "auto, %d%%") \
    X(SET_ANIMATIONS,   "set.animations", 0, "Animations") \
    X(SET_ANIMATIONS_HINT, "set.animations_hint", 0, "Blocks glide, menus unfold, cards slide out") \
    X(SET_AUTOSAVE,     "set.autosave", 0, "Autosave") \
    X(SET_AUTOSAVE_DELAY, "set.autosave_delay", 0, "Autosave after") \
    X(SET_AUTOSAVE_DELAY_HINT, "set.autosave_delay_hint", 0, "How long after the last change the file saves itself") \
    X(SET_SECONDS,      "set.seconds", 0, "%s s") \
    X(SET_MINIMAP_HINT, "set.minimap_hint", 0, "A small map of the file in the corner, when it does not fit on screen (Ctrl+M)") \
    X(SET_MINIMAP,      "set.minimap", 0, "Minimap") \
    X(SET_GRID,         "set.grid", 0, "Dots on the canvas") \
    X(SET_GRID_HINT,    "set.grid_hint", 0, "The grid behind the blocks") \
    X(SET_EXPLAIN,      "set.explain", 0, "Block explanations") \
    X(SET_EXPLAIN_HINT, "set.explain_hint", 0, "A card about a block when the mouse rests on it in the palette") \
    X(SET_AUTHORS,      "set.authors", 0, "Git authors beside blocks") \
    X(SET_AUTHORS_HINT, "set.authors_hint", 0, "Who last changed each piece, from git blame") \
    X(SET_BUILD_MODE,   "set.build_mode", 0, "Build as") \
    X(SET_MODE_DEBUG,   "set.mode_debug", 0, "Debug") \
    X(SET_MODE_DEBUG_HINT, "set.mode_debug_hint", 0, "Quick to build, easy to debug. For every day") \
    X(SET_MODE_RELEASE, "set.mode_release", 0, "Release") \
    X(SET_MODE_RELEASE_HINT, "set.mode_release_hint", 0, "Optimised (-O2): a faster program, a slower build. The bug button still builds for debugging") \
    X(SET_MODE_SANITIZE, "set.mode_sanitize", 0, "Bug hunt") \
    X(SET_MODE_SANITIZE_HINT, "set.mode_sanitize_hint", 0, "AddressSanitizer and UBSan: a bad pointer, an overflow or a leak stops the program and says where. Needs libasan and libubsan") \
    X(SET_RESET_LAYOUT, "set.reset_layout", 0, "Reset panel sizes") \
    X(SET_RESET_LAYOUT_HINT, "set.reset_layout_hint", 0, "The palette, the file list and the build panel as they were at first") \
    X(SET_CLEAR_RECENT, "set.clear_recent", 0, "Forget recent files") \
    X(SET_CLEAR_RECENT_HINT, "set.clear_recent_hint", 0, "The list on the start screen") \
    X(SET_AGREEMENT_HINT, "set.agreement_hint", 0, "Read it again. It is still not legally binding") \
    X(SET_DO,           "set.do", 0, "Do it") \
    X(SET_OPEN,         "set.open", 0, "Open") \
    X(SET_CONFIG,       "set.config", 0, "The settings file:") \
    X(SET_LAYOUT_DONE,  "set.layout_done", 0, "Panel sizes as they were at first") \
    X(SET_RECENT_DONE,  "set.recent_done", 0, "Recent files forgotten") \
    X(HELP_SETTINGS,    "help.settings", 0, "settings") \
    X(SET_SEC_PLUGINS,  "set.sec_plugins", 0, "Plugins") \
    X(SET_PLUGINS_NONE, "set.plugins_none", 0, "No plugins found. A plugin is a .so file in the folder below") \
    X(SET_PLUGINS_DIR,  "set.plugins_dir", 0, "Your own plugins go in:") \
    X(SET_PLUGIN_BY,    "set.plugin_by", 0, "version %s, by %s") \
    X(PF_CHIP_TIP,      "pf.chip_tip", 0, "What the plugins say about this file. Click: the next one") \
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
    X(EULA_C16, "eula.c16", 0, \
      "To acknowledge that a transistor differs from a programmer in that it actually does the work. By the laws of thermodynamics, work comes with heat, and here the programmer is the transistor's equal: just as much heat, with an efficiency tending to zero. A deadline is the only method known to science of raising a programmer's efficiency above that of a steam engine.") \
    X(EULA_C17, "eula.c17", 0, \
      "That the heat released by the User while waiting for the build is not counted as work, even when the fans are spinning loudly enough to sound like it.") \
    X(EULA_MEME_ZONE, "eula.meme_zone", 0, \
      "MEME ZONE. Everything below is memes and references. If you are offended, read the last point again.") \
    X(EULA_M01, "eula.m01", 0, \
      "If you are a femboy, you may not write C or assembly: your place is in C++. The Rust community has appealed, citing exclusive rights to programming socks. The appeal is being reviewed by the borrow checker; no date has been set.") \
    X(EULA_M02, "eula.m02", 0, \
      "If you are a girl, welcome to the low-level programming course for the littlest ones. It is taught by Ada Lovelace (the first published program, 1843) and Grace Hopper (the first compilers, and the first actual bug: a moth in a relay, 1947). So, honestly, the littlest ones here are us.") \
    X(EULA_M03, "eula.m03", 0, \
      "AI will replace everyone. This clause was written by an AI. So were the others. So was most of Ccratch's code. Who it has replaced so far is unclear: a human still has to read the error, say \"hmm\" and press the button again. Ccratch undertakes to make way for AI as soon as AI learns to quit Vim.") \
    X(EULA_FINAL, "eula.final", 0, \
      "None of the above has any legal force. The real license is the Mozilla Public License 2.0 (see LICENSE). The shrine maiden enforces nothing; she only collects donations.") \
    X(EULA_ACCEPT, "eula.accept", 0, \
      "I accept") \
    X(EULA_RUST, "eula.rust", 0, \
      "I write Rust") \
    X(EULA_DECLINE, "eula.decline", 0, \
      "I don't want to") \
    X(EULA_HURT, "eula.hurt", 0, \
      "Fine... That hurts, you know.") \
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
    bool has_plain;         /* a CODE-plain pack says it in plain words */
    char font[64];          /* its @font, if it names a built-in one ("noto-jp") */
} ccr_lang_info;

/* Looks for packs: $CCRATCH_LANG_DIR, ~/.local/share/ccratch/lang, then
 * share/ccratch/lang next to the program. Built-in English is always
 * first. A pack found earlier wins over one with the same code later.
 * CODE-plain packs are not listed: they set `has_plain` on CODE. */
int  ccr_lang_scan(ccr_lang_info *out, int cap);

/* Switches to a language by code ("en" or a pack). Problems are written to
 * `log` if it is not NULL. False if no such pack: English stays. In plain
 * words (ccr_lang_set_plain), CODE-plain is used if there is one. */
bool ccr_lang_use(const char *code, FILE *log);

/*
 * Plain words: "human language" for people who do not program. A pack
 * CODE-plain.lang says `@base = CODE` and gives only the strings it puts
 * differently (commit -> save a version...); the rest is CODE's.
 */
void ccr_lang_set_plain(bool on);
bool ccr_lang_plain(void);
bool ccr_lang_save_plain(bool on);
/* Is the text now in plain words (the language has them)? */
bool ccr_lang_in_plain_words(void);

/* The language in use (its code without -plain). */
const char *ccr_lang_code(void);
const char *ccr_lang_name(void);

/* @font of the pack in use: the name of a built-in font for its letters
 * ("noto-jp"...), a font file, or "" (Ccratch's own font has them all). */
const char *ccr_lang_font(void);
/* Every code point above ASCII in the strings now in use, ascending (the
 * letters the font needs). How many, at most `cap`. */
int ccr_lang_codepoints(int *out, int cap);

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
