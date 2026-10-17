/*
 * ccr_files - the project: every .c and .h around the open file, as a
 * folder tree, and the panel that shows it.
 *
 * The project is the folder of the first file opened (a file from outside
 * it starts a new one). Hidden folders, build trees and the like are left
 * out. Scanning knows nothing about the window; the panel does.
 */
#ifndef CCR_FILES_H
#define CCR_FILES_H

#include "ccr_gui.h"

#include <stdbool.h>

#define CCR_FILES_PATH 1024
#define CCR_FILES_MAX  2048   /* entries; a bigger project is cut off */
#define CCR_FILES_DEPTH 6

typedef struct ccr_file_entry {
    char path[CCR_FILES_PATH];
    char name[128];
    int  depth;          /* 0: right in the root */
    bool dir;
    bool expanded;       /* folders */
} ccr_file_entry;

typedef struct ccr_files {
    char            root[CCR_FILES_PATH];
    ccr_file_entry *v;
    int             count, cap;
    bool            truncated;   /* more than CCR_FILES_MAX */
} ccr_files;

/* Reads the tree under `root` (folders first, then files, by name). Kept
 * folders stay expanded across scans; new ones start closed, except the
 * one holding `focus` (a file path, may be NULL), which opens. */
bool ccr_files_scan(ccr_files *f, const char *root, const char *focus);
void ccr_files_free(ccr_files *f);

/* The folder a file is in. */
void ccr_files_dir_of(const char *path, char *out, int cap);
/* The project a file belongs to: the nearest folder above it with .git,
 * CMakeLists.txt, a Makefile, meson.build or compile_commands.json in it;
 * else the file's own folder. */
void ccr_files_project_of(const char *path, char *out, int cap);
/* The whole path of a file (links and ../ resolved); false if it is not there. */
bool ccr_files_real(const char *path, char *out, int cap);
/* The same file, however the two paths spell it. */
bool ccr_files_same(const char *a, const char *b);
/* Is `path` inside `root`? */
bool ccr_files_inside(const char *root, const char *path);

typedef enum ccr_create_result {
    CCR_CREATED,
    CCR_CREATE_EXISTS,
    CCR_CREATE_BAD_NAME,    /* empty, a slash, "." or ".." */
    CCR_CREATE_FAILED       /* the system said no */
} ccr_create_result;

/* A new source file in folder `dir`. No extension: ".c". A header gets an
 * include guard (util.h: UTIL_H); a .c file includes its header if one
 * of the same name is next to it. `out` gets the full path. */
ccr_create_result ccr_files_create_file(const char *dir, const char *name, char *out, int cap);
/* A new folder in `dir`. */
ccr_create_result ccr_files_create_dir(const char *dir, const char *name, char *out, int cap);

/* Where `#include "name"` in file `from` points: next to `from`, else a
 * file of the project ending in /name (headers in include/, say). */
bool ccr_files_find_include(const ccr_files *f, const char *from, const char *name,
                            char *out, int cap);

/* Entry `i` is shown: all the folders above it are expanded. */
bool ccr_files_visible(const ccr_files *f, int i);

/* ------------------------------------------------------------ panel */

/* What the panel needs to know about a file that is open. */
typedef struct ccr_open_file {
    const char *path;
    bool        modified;
    bool        current;
} ccr_open_file;

typedef enum ccr_files_event {
    CCR_FILES_NOTHING,
    CCR_FILES_OPEN,      /* a file was clicked: open it, or go to it */
    CCR_FILES_CLOSE      /* the x of an open file */
} ccr_files_event;

typedef enum ccr_files_naming { CCR_NAMING_NONE, CCR_NAMING_FILE, CCR_NAMING_DIR } ccr_files_naming;

typedef struct ccr_files_panel {
    float scroll;
    char  target[CCR_FILES_PATH];  /* where new files go: the folder last clicked */
    ccr_files_naming naming;       /* the name field is open */
    char  name[128];
    char  note[256];               /* what just happened, under the list */
    bool  note_bad;
    double note_at;
    int   hot;            /* row under the mouse, or -1 */
    bool  hot_close;      /* on its x */
    char  armed[CCR_FILES_PATH];   /* closing an unsaved file: click x again */
} ccr_files_panel;

#define CCR_FILES_ROW_H 30.0f

ccr_files_event ccr_files_panel_update(ccr_files_panel *p, ccr_files *f, ccr_rect area,
                                       ccr_vec2 mouse, float wheel, bool click,
                                       const ccr_open_file *open, int open_count,
                                       char *path_out, int cap);
void ccr_files_panel_draw(const ccr_files_panel *p, const ccr_files *f, ccr_rect area,
                          const ccr_open_file *open, int open_count);

/* The "+ file" and "+ folder" buttons and the name field; call after the
 * draw, in the same frame. A file made here comes back as CCR_FILES_OPEN
 * with its path, to be opened. */
ccr_files_event ccr_files_panel_tools(ccr_files_panel *p, ccr_files *f, ccr_rect area,
                                      const ccr_button_style *style, char *path_out, int cap);

#endif /* CCR_FILES_H */
