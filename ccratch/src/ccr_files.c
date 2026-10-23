#define _DEFAULT_SOURCE   /* realpath */
#include "ccr_files.h"
#include "ccr_asm.h"
#include "ccr_cmake.h"
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
    return (dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)) || ccr_cmake_is_cmake(name) ||
           ccr_asm_is_asm(name);
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

/* Adds what is under `dir` that holds sources; false if nothing did.
 * `*empty`: there is nothing in it at all (a folder just made: shown). */
static bool scan_dir(ccr_files *f, const char *dir, int depth, bool *empty)
{
    *empty = depth < CCR_FILES_DEPTH;   /* too deep to look: not "empty" */
    if (depth >= CCR_FILES_DEPTH)
        return false;
    DIR *d = opendir(dir);
    if (!d)
        return false;
    child *kids = NULL;
    int n = 0, cap = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        *empty = false;
        if (strlen(e->d_name) >= sizeof kids[0].name)
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
        bool hollow;
        if (e.dir && !scan_dir(f, e.path, depth + 1, &hollow) && !hollow) {
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
    bool hollow;
    scan_dir(f, f->root, 0, &hollow);

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

/* ------------------------------------------------------------ creating */

static bool bad_name(const char *name)
{
    return !*name || strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
           strlen(name) > 120;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

ccr_create_result ccr_files_create_file(const char *dir, const char *name, char *out, int cap)
{
    char clean[128];
    snprintf(clean, sizeof clean, "%s", name);
    /* no spaces around it */
    char *s = clean;
    while (*s == ' ')
        ++s;
    for (char *e = s + strlen(s); e > s && e[-1] == ' ';)
        *--e = '\0';
    if (bad_name(s))
        return CCR_CREATE_BAD_NAME;
    char file[136];
    snprintf(file, sizeof file, "%s%s", s, strchr(s, '.') ? "" : ".c");

    if (snprintf(out, (size_t)cap, "%s/%s", dir, file) >= cap)
        return CCR_CREATE_BAD_NAME;
    if (exists(out))
        return CCR_CREATE_EXISTS;

    const size_t n = strlen(file);
    const bool header = n > 2 && strcmp(file + n - 2, ".h") == 0;
    char text[512];
    if (ccr_asm_is_asm(file)) {
        /* assembly: a program that does nothing yet, in the file's syntax */
        if (ccr_asm_syntax_of(file, NULL, 0) == CCR_ASM_NASM)
            snprintf(text, sizeof text, "; %s\n        global  main\n\n        section .text\nmain:\n"
                                        "        xor     eax, eax\n        ret\n", file);
        else
            snprintf(text, sizeof text, "# %s\n        .text\n        .globl  main\nmain:\n"
                                        "        xorl    %%eax, %%eax\n        ret\n\n"
                                        "        .section .note.GNU-stack,\"\",@progbits\n", file);
    } else if (header) {
        /* util.h -> UTIL_H */
        char guard[136];
        size_t g = 0;
        for (const char *c = file; *c && g + 1 < sizeof guard; ++c)
            guard[g++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 'a' + 'A')
                       : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_';
        guard[g] = '\0';
        snprintf(text, sizeof text, "#ifndef %s\n#define %s\n\n#endif\n", guard, guard);
    } else {
        /* util.c next to util.h: it includes it */
        char stem[136], sibling[CCR_FILES_PATH + 140];
        snprintf(stem, sizeof stem, "%s", file);
        char *dot = strrchr(stem, '.');
        if (dot)
            *dot = '\0';
        snprintf(sibling, sizeof sibling, "%s/%s.h", dir, stem);
        if (exists(sibling))
            snprintf(text, sizeof text, "/* %s */\n#include \"%s.h\"\n", file, stem);
        else
            snprintf(text, sizeof text, "/* %s */\n", file);
    }
    FILE *f = fopen(out, "wx");   /* never over a file that appeared meanwhile */
    if (!f)
        return exists(out) ? CCR_CREATE_EXISTS : CCR_CREATE_FAILED;
    bool ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
    return ok ? CCR_CREATED : CCR_CREATE_FAILED;
}

ccr_create_result ccr_files_create_dir(const char *dir, const char *name, char *out, int cap)
{
    char clean[128];
    snprintf(clean, sizeof clean, "%s", name);
    char *s = clean;
    while (*s == ' ')
        ++s;
    for (char *e = s + strlen(s); e > s && e[-1] == ' ';)
        *--e = '\0';
    if (bad_name(s))
        return CCR_CREATE_BAD_NAME;
    if (snprintf(out, (size_t)cap, "%s/%s", dir, s) >= cap)
        return CCR_CREATE_BAD_NAME;
    if (exists(out))
        return CCR_CREATE_EXISTS;
    return mkdir(out, 0755) == 0 ? CCR_CREATED : CCR_CREATE_FAILED;
}

void ccr_files_project_of(const char *path, char *out, int cap)
{
    static const char *const marks[] = { ".git", "CMakeLists.txt", "Makefile", "makefile",
                                         "meson.build", "compile_commands.json" };
    char dir[FULL_PATH];
    ccr_files_dir_of(path, dir, (int)sizeof dir);
    char whole[FULL_PATH];   /* gui/src/x.c from the shell: walk up from where it really is */
    if (realpath(dir, whole) && strlen(whole) < sizeof dir)
        memcpy(dir, whole, strlen(whole) + 1);
    char d[FULL_PATH];
    snprintf(d, sizeof d, "%s", dir);
    for (int up = 0; up < 6 && d[0]; ++up) {
        for (size_t i = 0; i < sizeof marks / sizeof marks[0]; ++i) {
            char probe[FULL_PATH + 32];
            snprintf(probe, sizeof probe, "%s/%s", d, marks[i]);
            if (!exists(probe))
                continue;
            /* a CMakeLists.txt with one above it is a subdirectory of that
             * project (add_subdirectory): the project is further up */
            if (i == 1) {
                snprintf(probe, sizeof probe, "%s/.git", d);
                while (!exists(probe)) {
                    char parent[FULL_PATH];
                    snprintf(parent, sizeof parent, "%s", d);
                    char *ps = strrchr(parent, '/');
                    if (!ps || ps == parent)
                        break;
                    *ps = '\0';
                    snprintf(probe, sizeof probe, "%s/CMakeLists.txt", parent);
                    if (!exists(probe))
                        break;
                    snprintf(d, sizeof d, "%s", parent);
                    snprintf(probe, sizeof probe, "%s/.git", d);
                }
            }
            snprintf(out, (size_t)cap, "%s", d);
            return;
        }
        char *slash = strrchr(d, '/');
        if (!slash || slash == d)
            break;
        *slash = '\0';
    }
    /* nothing says where it starts: src/main.c is in a project above src */
    static const char *const inner[] = { "src", "source", "sources", "include", "inc" };
    const char *base = strrchr(dir, '/');
    for (size_t i = 0; base && base != dir && i < sizeof inner / sizeof inner[0]; ++i)
        if (strcmp(base + 1, inner[i]) == 0) {
            snprintf(out, (size_t)cap, "%.*s", (int)(base - dir), dir);
            return;
        }
    snprintf(out, (size_t)cap, "%s", dir);
}

bool ccr_files_find_include(const ccr_files *f, const char *from, const char *name,
                            char *out, int cap)
{
    if (!*name)
        return false;
    char dir[CCR_FILES_PATH];
    ccr_files_dir_of(from, dir, (int)sizeof dir);
    if (snprintf(out, (size_t)cap, "%s/%s", dir, name) < cap && exists(out))
        return true;
    /* src/x.c including "x.h" from ../include: the usual layout */
    char d[CCR_FILES_PATH];
    snprintf(d, sizeof d, "%s", dir);
    for (int up = 0; up < 4; ++up) {
        char *slash = strrchr(d, '/');
        if (!slash || slash == d)
            break;
        *slash = '\0';
        if (snprintf(out, (size_t)cap, "%s/include/%s", d, name) < cap && exists(out))
            return true;
        if (snprintf(out, (size_t)cap, "%s/%s", d, name) < cap && exists(out))
            return true;
    }
    const size_t n = strlen(name);
    for (int i = 0; i < f->count; ++i) {
        const char *p = f->v[i].path;
        const size_t len = strlen(p);
        if (!f->v[i].dir && len > n && strcmp(p + len - n, name) == 0 && p[len - n - 1] == '/') {
            snprintf(out, (size_t)cap, "%s", p);
            return true;
        }
    }
    return false;
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
#define TOOLS_W  136.0f   /* "+ file" "+ folder" in the header */
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
            snprintf(p->target, sizeof p->target, "%s", f->v[i].path);   /* new files go here */
            return CCR_FILES_NOTHING;
        }
        ccr_files_dir_of(f->v[i].path, p->target, (int)sizeof p->target);
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
    ccr_gui_clip_begin((ccr_rect){ area.x, area.y, area.w - TOOLS_W - 8.0f, HEADER_H });
    ccr_gui_text(root, area.x + 22.0f + tw, area.y + 12.0f, 14.0f, ccr_th(CCR_C_HINT));
    ccr_gui_clip_end();

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

        if (e->dir && p->target[0] && strcmp(p->target, e->path) == 0)   /* new files go here */
            ccr_gui_rect_rounded((ccr_rect){ r.x + 4, r.y + r.h - 4, r.w - 8, 2 }, 1.0f,
                                 with_alpha(ccr_th(CCR_C_GLOW), 0xC0u));
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
        const ccr_theme_cat cat = ccr_asm_is_asm(e->name) ? CCR_CAT_OPERATOR : header ? CCR_CAT_TYPE : CCR_CAT_CALL;
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

/* -------------------------------------------------- making new files */

static void note(ccr_files_panel *p, bool bad, ccr_text what, const char *name)
{
    ccr_trf(p->note, sizeof p->note, what, name);
    p->note_bad = bad;
    p->note_at = ccr_gui_time();
}

ccr_files_event ccr_files_panel_tools(ccr_files_panel *p, ccr_files *f, ccr_rect area,
                                      const ccr_button_style *style, char *path_out, int cap)
{
    ccr_files_event ev = CCR_FILES_NOTHING;
    /* the target must still be in this project */
    if (!p->target[0] || !ccr_files_inside(f->root, p->target))
        snprintf(p->target, sizeof p->target, "%s", f->root);

    const char *lf = ccr_tr(CCR_T_FILES_NEW_FILE), *ld = ccr_tr(CCR_T_FILES_NEW_DIR);
    const float wf = ccr_gui_text_width(lf, 14.0f) + 16.0f, wd = ccr_gui_text_width(ld, 14.0f) + 16.0f;
    const float x = area.x + area.w - 8.0f - wd - 6.0f - wf;
    if (ccr_gui_button((ccr_rect){ x, area.y + 8.0f, wf, 24.0f }, lf, style)) {
        p->naming = CCR_NAMING_FILE;
        p->name[0] = '\0';
        ccr_gui_textbox_reset();
    }
    if (ccr_gui_button((ccr_rect){ x + wf + 6.0f, area.y + 8.0f, wd, 24.0f }, ld, style)) {
        p->naming = CCR_NAMING_DIR;
        p->name[0] = '\0';
        ccr_gui_textbox_reset();
    }

    if (p->naming != CCR_NAMING_NONE) {
        /* the name field, at the bottom: where it goes, then what to type */
        const ccr_rect box = { area.x + 8.0f, area.y + area.h - 92.0f, area.w - 16.0f, 84.0f };
        ccr_gui_rect_rounded(box, 0.2f, ccr_th(CCR_C_PANEL));
        const char *where = strrchr(p->target, '/');
        char line[CCR_FILES_PATH + 8];
        snprintf(line, sizeof line, "%s/",
                 strcmp(p->target, f->root) == 0 ? "." : (where ? where + 1 : p->target));
        ccr_gui_text(line, box.x + 10.0f, box.y + 8.0f, 14.0f, ccr_th(CCR_C_PANEL_HINT));
        ccr_gui_text(ccr_tr(p->naming == CCR_NAMING_FILE ? CCR_T_FILES_NAME_FILE : CCR_T_FILES_NAME_DIR),
                     box.x + 10.0f + ccr_gui_text_width(line, 14.0f) + 6.0f, box.y + 8.0f, 13.0f,
                     ccr_th(CCR_C_PANEL_HINT));
        if (ccr_gui_key_pressed(CCR_KEY_ESCAPE)) {
            p->naming = CCR_NAMING_NONE;
        } else if (ccr_gui_textbox((ccr_rect){ box.x + 8.0f, box.y + 34.0f, box.w - 16.0f, 34.0f },
                                   p->name, (int)sizeof p->name, 19.0f)) {
            const bool file = p->naming == CCR_NAMING_FILE;
            p->naming = CCR_NAMING_NONE;
            if (p->name[0]) {
                char made[CCR_FILES_PATH];
                ccr_create_result r = file ? ccr_files_create_file(p->target, p->name, made, (int)sizeof made)
                                           : ccr_files_create_dir(p->target, p->name, made, (int)sizeof made);
                const char *base = strrchr(made, '/') ? strrchr(made, '/') + 1 : made;
                if (r == CCR_CREATED) {
                    note(p, false, CCR_T_FILES_CREATED, base);
                    ccr_files_scan(f, f->root, made);
                    if (file) {
                        snprintf(path_out, (size_t)cap, "%s", made);
                        ev = CCR_FILES_OPEN;
                    } else {
                        for (int i = 0; i < f->count; ++i)   /* open it: it is where you are now */
                            if (strcmp(f->v[i].path, made) == 0)
                                f->v[i].expanded = true;
                        snprintf(p->target, sizeof p->target, "%s", made);
                    }
                } else {
                    note(p, true, r == CCR_CREATE_EXISTS ? CCR_T_FILES_EXISTS : CCR_T_FILES_CANNOT,
                         r == CCR_CREATE_BAD_NAME ? p->name : base);
                }
            }
        }
    } else if (p->note[0] && ccr_gui_time() - p->note_at < 3.0) {
        const ccr_rect box = { area.x + 8.0f, area.y + area.h - 44.0f, area.w - 16.0f, 36.0f };
        ccr_gui_rect_rounded(box, 0.25f, p->note_bad ? ccr_th(CCR_C_ERROR) : ccr_th(CCR_C_OK));
        float size = 15.0f;
        while (size > 10.0f && ccr_gui_text_width(p->note, size) > box.w - 16.0f)
            size -= 0.5f;
        ccr_gui_text(p->note, box.x + 8.0f, box.y + (box.h - size) * 0.5f, size, 0xFFFFFFFFu);
    }
    return ev;
}

bool ccr_files_real(const char *path, char *out, int cap)
{
    char real[FULL_PATH];
    if (!realpath(path, real) || (int)strlen(real) >= cap)
        return false;
    memcpy(out, real, strlen(real) + 1);
    return true;
}
