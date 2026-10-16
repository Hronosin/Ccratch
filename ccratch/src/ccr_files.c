#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_files.h"
#include "ccr_lang.h"
#include "ccr_theme.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FULL_PATH 4096

/* ------------------------------------------------------------ scanning */

static bool is_source(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0);
}

/* Folders nobody wants to see code in. */
static bool skip_dir(const char *name)
{
    static const char *const skip[] = { "build", "_deps", "node_modules", "CMakeFiles", "out", "dist" };
    if (name[0] == '.')
        return true;
    if (strncmp(name, "cmake-build", 11) == 0)
        return true;
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; ++i)
        if (strcmp(name, skip[i]) == 0)
            return true;
    return false;
}

static bool push(ccr_files *f, const ccr_file_entry *e)
{
    if (f->count >= CCR_FILES_MAX) {
        f->truncated = true;
        return false;
    }
    if (f->count == f->cap) {
        int cap = f->cap ? f->cap * 2 : 64;
        ccr_file_entry *v = realloc(f->v, (size_t)cap * sizeof *v);
        if (!v)
            return false;
        f->v = v;
        f->cap = cap;
    }
    f->v[f->count++] = *e;
    return true;
}

typedef struct child { char name[128]; bool dir; } child;

static int compare_child(const void *a, const void *b)
{
    const child *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;   /* folders first */
    return strcmp(x->name, y->name);
}

/* Adds what is under `dir` that holds sources; false if nothing did. */
static bool scan_dir(ccr_files *f, const char *dir, int depth)
{
    if (depth >= CCR_FILES_DEPTH)
        return false;
    DIR *d = opendir(dir);
    if (!d)
        return false;
    child *kids = NULL;
    int n = 0, cap = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof kids[0].name)
            continue;
        char full[CCR_FILES_PATH];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        const bool is_dir = S_ISDIR(st.st_mode);
        if (is_dir ? skip_dir(e->d_name) : !(S_ISREG(st.st_mode) && is_source(e->d_name)))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            child *k = realloc(kids, (size_t)cap * sizeof *k);
            if (!k)
                break;
            kids = k;
        }
        memcpy(kids[n].name, e->d_name, strlen(e->d_name) + 1);   /* length checked above */
        kids[n].dir = is_dir;
        ++n;
    }
    closedir(d);
    if (n > 1)
        qsort(kids, (size_t)n, sizeof *kids, compare_child);

    bool any = false;
    for (int i = 0; i < n; ++i) {
        ccr_file_entry e = { .depth = depth, .dir = kids[i].dir };
        snprintf(e.path, sizeof e.path, "%s/%s", dir, kids[i].name);
        snprintf(e.name, sizeof e.name, "%s", kids[i].name);
        const int at = f->count;
        if (!push(f, &e))
            break;
        if (e.dir && !scan_dir(f, e.path, depth + 1)) {
            f->count = at;   /* a folder with no code in it: not worth a row */
            continue;
        }
        any = true;
    }
    free(kids);
    return any;
}

void ccr_files_dir_of(const char *path, char *out, int cap)
{
    char full[FULL_PATH];
    if (!realpath(path, full))
        snprintf(full, sizeof full, "%s", path);
    char *slash = strrchr(full, '/');
    if (slash == full)
        slash[1] = '\0';       /* a file right in / */
    else if (slash)
        *slash = '\0';
    else
        snprintf(full, sizeof full, ".");
    snprintf(out, (size_t)cap, "%s", full);
}

bool ccr_files_same(const char *a, const char *b)
{
    char x[FULL_PATH], y[FULL_PATH];
    if (realpath(a, x) && realpath(b, y))
        return strcmp(x, y) == 0;
    return strcmp(a, b) == 0;
}

bool ccr_files_inside(const char *root, const char *path)
{
    char full[FULL_PATH];
    if (!realpath(path, full))
        return false;
    const size_t n = strlen(root);
    return n > 0 && strncmp(full, root, n) == 0 && (full[n] == '/' || root[n - 1] == '/');
}

bool ccr_files_scan(ccr_files *f, const char *root, const char *focus)
{
    /* remember which folders were open */
    char (*open)[CCR_FILES_PATH] = NULL;
    int open_n = 0;
    if (f->count && strcmp(f->root, root) == 0) {
        open = malloc((size_t)f->count * sizeof *open);
        for (int i = 0; open && i < f->count; ++i)
            if (f->v[i].dir && f->v[i].expanded)
                snprintf(open[open_n++], CCR_FILES_PATH, "%s", f->v[i].path);
    }

    char rp[FULL_PATH];
    if (!realpath(root, rp))
        snprintf(rp, sizeof rp, "%s", root);
    const size_t rn = strlen(rp) < sizeof f->root ? strlen(rp) : sizeof f->root - 1;
    memcpy(f->root, rp, rn);
    f->root[rn] = '\0';
    f->count = 0;
    f->truncated = false;
    scan_dir(f, f->root, 0);

    char focus_full[FULL_PATH] = "";
    if (focus && !realpath(focus, focus_full))
        focus_full[0] = '\0';
    for (int i = 0; i < f->count; ++i) {
        ccr_file_entry *e = &f->v[i];
        if (!e->dir)
            continue;
        e->expanded = false;
        for (int k = 0; k < open_n && !e->expanded; ++k)
            e->expanded = strcmp(open[k], e->path) == 0;
        /* the folders down to the file being worked on */
        const size_t n = strlen(e->path);
        if (focus_full[0] && strncmp(focus_full, e->path, n) == 0 && focus_full[n] == '/')
            e->expanded = true;
    }
    free(open);
    return true;
}

void ccr_files_free(ccr_files *f)
{
    free(f->v);
    f->v = NULL;
    f->count = f->cap = 0;
}

bool ccr_files_visible(const ccr_files *f, int i)
{
    int need = f->v[i].depth;   /* the nearest folder above at each shallower depth */
    for (int k = i - 1; k >= 0 && need > 0; --k) {
        if (f->v[k].depth == need - 1) {
            if (!f->v[k].dir || !f->v[k].expanded)
                return false;
            --need;
        }
    }
    return true;
}

/* -------------------------------------------------------------- panel */

#define HEADER_H 38.0f
#define INDENT   16.0f
#define TEXT     17.0f

static bool inside(ccr_vec2 m, ccr_rect r)
{
    return m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}

static const ccr_open_file *find_open(const char *path, const ccr_open_file *open, int n)
{
    char a[FULL_PATH], b[FULL_PATH];
    if (!realpath(path, a))
        return NULL;
    for (int i = 0; i < n; ++i)
        if (open[i].path && realpath(open[i].path, b) && strcmp(a, b) == 0)
            return &open[i];
    return NULL;
}

static int visible_rows(const ccr_files *f)
{
    int rows = 0;
    for (int i = 0; i < f->count; ++i)
        rows += ccr_files_visible(f, i);
    return rows;
}

/* Calls back for each shown row: entry, its rectangle. */
#define FOR_ROWS(f, area, scroll, i, r)                                                  \
    for (int i = 0, row_ = 0; i < (f)->count; ++i)                                       \
        if (ccr_files_visible((f), i))                                                   \
            for (ccr_rect r = { (area).x, (area).y + HEADER_H + (float)row_++ * CCR_FILES_ROW_H \
                                              - (scroll), (area).w, CCR_FILES_ROW_H };    \
                 r.w > 0.0f; r.w = 0.0f)

static ccr_rect close_box(ccr_rect row)
{
    return (ccr_rect){ row.x + row.w - 30.0f, row.y + 4.0f, 22.0f, row.h - 8.0f };
}

ccr_files_event ccr_files_panel_update(ccr_files_panel *p, ccr_files *f, ccr_rect area,
                                       ccr_vec2 mouse, float wheel, bool click,
                                       const ccr_open_file *open, int open_count,
                                       char *path_out, int cap)
{
    p->hot = -1;
    p->hot_close = false;
    if (!inside(mouse, area))
        return CCR_FILES_NOTHING;
    p->scroll -= wheel * 60.0f;
    const float most = (float)visible_rows(f) * CCR_FILES_ROW_H - (area.h - HEADER_H) + 10.0f;
    if (p->scroll > most) p->scroll = most;
    if (p->scroll < 0.0f) p->scroll = 0.0f;

    FOR_ROWS(f, area, p->scroll, i, r) {
        if (r.y + r.h <= area.y + HEADER_H || !inside(mouse, r))
            continue;
        p->hot = i;
        const ccr_open_file *of = f->v[i].dir ? NULL : find_open(f->v[i].path, open, open_count);
        p->hot_close = of && inside(mouse, close_box(r));
        if (!click)
            return CCR_FILES_NOTHING;
        if (f->v[i].dir) {
            f->v[i].expanded = !f->v[i].expanded;
            return CCR_FILES_NOTHING;
        }
        snprintf(path_out, (size_t)cap, "%s", f->v[i].path);
        if (p->hot_close) {
            /* unsaved work: the first click only warns */
            if (of->modified && strcmp(p->armed, f->v[i].path) != 0) {
                snprintf(p->armed, sizeof p->armed, "%s", f->v[i].path);
                return CCR_FILES_NOTHING;
            }
            p->armed[0] = '\0';
            return CCR_FILES_CLOSE;
        }
        p->armed[0] = '\0';
        return CCR_FILES_OPEN;
    }
    if (click)
        p->armed[0] = '\0';
    return CCR_FILES_NOTHING;
}

static ccr_color with_alpha(ccr_color c, unsigned a) { return (c & 0xFFFFFF00u) | (a & 0xFFu); }

void ccr_files_panel_draw(const ccr_files_panel *p, const ccr_files *f, ccr_rect area,
                          const ccr_open_file *open, int open_count)
{
    ccr_gui_rect_rounded((ccr_rect){ area.x - 1.0f, area.y, area.w + 1.0f, area.h }, 0.0f,
                         ccr_th(CCR_C_PANEL_EDGE));
    ccr_gui_rect(area, ccr_th(CCR_C_PALETTE));

    /* header: the project folder */
    const char *root = strrchr(f->root, '/');
    root = (root && root[1]) ? root + 1 : f->root;
    ccr_gui_text(ccr_tr(CCR_T_FILES_TITLE), area.x + 12.0f, area.y + 10.0f, 17.0f,
                 ccr_th(CCR_C_PALETTE_TEXT));
    const float tw = ccr_gui_text_width(ccr_tr(CCR_T_FILES_TITLE), 17.0f);
    ccr_gui_text(root, area.x + 22.0f + tw, area.y + 12.0f, 14.0f, ccr_th(CCR_C_HINT));

    ccr_gui_clip_begin((ccr_rect){ area.x, area.y + HEADER_H, area.w, area.h - HEADER_H });
    if (f->count == 0)
        ccr_gui_text(ccr_tr(CCR_T_FILES_EMPTY), area.x + 14.0f, area.y + HEADER_H + 8.0f, 15.0f,
                     ccr_th(CCR_C_HINT));
    FOR_ROWS(f, area, p->scroll, i, r) {
        if (r.y + r.h < area.y + HEADER_H || r.y > area.y + area.h)
            continue;
        const ccr_file_entry *e = &f->v[i];
        const ccr_open_file *of = e->dir ? NULL : find_open(e->path, open, open_count);
        const float x = r.x + 10.0f + (float)e->depth * INDENT;
        const float cy = r.y + r.h * 0.5f;

        if (of && of->current)
            ccr_gui_rect_rounded((ccr_rect){ r.x + 4, r.y + 2, r.w - 8, r.h - 4 }, 0.3f,
                                 with_alpha(ccr_th(CCR_C_HOT), 0x55u));
        else if (p->hot == i)
            ccr_gui_rect_rounded((ccr_rect){ r.x + 4, r.y + 2, r.w - 8, r.h - 4 }, 0.3f,
                                 with_alpha(ccr_th(CCR_C_BUTTON_HOVER), 0xC0u));

        if (e->dir) {
            /* a little triangle: pointing right closed, down open */
            const ccr_color c = ccr_th(CCR_C_PALETTE_TEXT);
            if (e->expanded)
                ccr_gui_triangle((ccr_vec2){ x, cy - 3.0f }, (ccr_vec2){ x + 10.0f, cy - 3.0f },
                                 (ccr_vec2){ x + 5.0f, cy + 4.0f }, c);
            else
                ccr_gui_triangle((ccr_vec2){ x + 2.0f, cy - 5.0f }, (ccr_vec2){ x + 2.0f, cy + 5.0f },
                                 (ccr_vec2){ x + 9.0f, cy }, c);
            ccr_gui_text(e->name, x + 16.0f, cy - TEXT * 0.5f, TEXT, c);
            continue;
        }

        /* a file: a dot in the colour of what it holds, .c code, .h types */
        const bool header = e->name[strlen(e->name) - 1] == 'h';
        const ccr_theme_cat cat = header ? CCR_CAT_TYPE : CCR_CAT_CALL;
        ccr_gui_circle((ccr_vec2){ x + 5.0f, cy }, 6.0f, ccr_th_cat(cat, CCR_EDGE));
        ccr_gui_circle((ccr_vec2){ x + 5.0f, cy }, 4.5f, ccr_th_cat(cat, CCR_FILL));
        char label[160];
        snprintf(label, sizeof label, "%s%s", e->name, of && of->modified ? " *" : "");
        ccr_gui_text(label, x + 16.0f, cy - TEXT * 0.5f, TEXT,
                     of ? ccr_th(CCR_C_TITLE) : ccr_th(CCR_C_PALETTE_TEXT));
        if (of) {
            /* open: an x to close it; red once it has warned */
            const ccr_rect xb = close_box(r);
            const bool armed = strcmp(p->armed, e->path) == 0;
            if (armed || (p->hot == i && p->hot_close))
                ccr_gui_rect_rounded(xb, 0.4f, armed ? ccr_th(CCR_C_ERROR)
                                                     : ccr_th(CCR_C_BUTTON_HOVER));
            const ccr_color xc = armed ? 0xFFFFFFFFu : ccr_th(CCR_C_HINT);
            const ccr_vec2 c = { xb.x + xb.w * 0.5f, xb.y + xb.h * 0.5f };
            ccr_gui_line((ccr_vec2){ c.x - 4.5f, c.y - 4.5f }, (ccr_vec2){ c.x + 4.5f, c.y + 4.5f }, 2.0f, xc);
            ccr_gui_line((ccr_vec2){ c.x - 4.5f, c.y + 4.5f }, (ccr_vec2){ c.x + 4.5f, c.y - 4.5f }, 2.0f, xc);
        }
    }
    ccr_gui_clip_end();

    /* the warning for an unsaved file, at the bottom */
    if (p->armed[0]) {
        const char *msg = ccr_tr(CCR_T_FILES_UNSAVED);
        const ccr_rect box = { area.x + 8.0f, area.y + area.h - 52.0f, area.w - 16.0f, 44.0f };
        ccr_gui_rect_rounded(box, 0.25f, ccr_th(CCR_C_ERROR));
        float size = 15.0f;
        while (size > 10.0f && ccr_gui_text_width(msg, size) > box.w - 16.0f)
            size -= 0.5f;
        ccr_gui_text(msg, box.x + 8.0f, box.y + (box.h - size) * 0.5f, size, 0xFFFFFFFFu);
    }
}
