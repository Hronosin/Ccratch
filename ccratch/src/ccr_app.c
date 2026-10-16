#include "ccratch/ccratch.h"
#include "ccr_ast.h"
#include "ccr_canvas.h"
#include "ccr_codegen.h"
#include "ccr_config.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_lang.h"
#include "ccr_parse.h"
#include "ccr_files.h"
#include "ccr_lib.h"
#include "ccr_palette.h"
#include "ccr_pie.h"
#include "ccr_theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Colours: whatever the theme says */
#define COL_BG        ccr_th(CCR_C_BG)
#define COL_BAR       ccr_th(CCR_C_BAR)
#define COL_BAR_TEXT  ccr_th(CCR_C_BAR_TEXT)
#define COL_TITLE     ccr_th(CCR_C_TITLE)
#define COL_HINT      ccr_th(CCR_C_HINT)
#define COL_ACCENT    ccr_th(CCR_C_ACCENT)
#define COL_OK        ccr_th(CCR_C_OK)

/* Generated from assets/icon.png at build time */
extern const unsigned char ccr_icon_png[];
extern const unsigned int  ccr_icon_png_size;
extern const unsigned char ccr_font_ttf[];
extern const unsigned int  ccr_font_ttf_size;

/* Characters the font is rasterized for; tools/make_font.py keeps these. */
static const int g_font_ranges[][2] = {
    { 0x0020, 0x007E }, { 0x00A0, 0x017F }, { 0x0400, 0x04FF },
    { 0x2010, 0x2027 }, { 0x2030, 0x203A }, { 0x2116, 0x2116 }, { 0x2190, 0x2193 },
};

static bool load_font(void)
{
    static int cps[1024];
    int n = 0;
    for (size_t r = 0; r < sizeof g_font_ranges / sizeof g_font_ranges[0]; ++r)
        for (int c = g_font_ranges[r][0]; c <= g_font_ranges[r][1] && n < 1024; ++c)
            cps[n++] = c;
    return ccr_gui_load_font(ccr_font_ttf, ccr_font_ttf_size, 48.0f, cps, n);
}

#define TOPBAR_H     40.0f
#define PALETTE_W   CCR_PALETTE_W
#define COL_PALETTE  ccr_th(CCR_C_PALETTE)
#define STATUSBAR_H  26.0f
#define PATH_MAX_LEN 4096

typedef struct ccr_options {
    int         smoke_frames;   /* >0: quit after N frames (CI, headless) */
    const char *screenshot;     /* save the last frame here */
    const char *dump_ast;       /* parse this file, print the tree, exit */
    bool        ranges;         /* --ranges: with the source text of each node */
    const char *roundtrip;      /* parse, write back unchanged, compare */
    const char *open;           /* file to open in the editor */
    int         zoom_percent;   /* initial zoom, 0 = 100% */
    const char *lang;           /* --lang CODE, NULL: pick one */
    bool        eula;           /* --eula: print the agreement, exit */
    const char *theme;          /* --theme NAME, NULL: the saved one */
    bool        no_animations;  /* --no-animations */
    const char *make_blocks;    /* --make-blocks HEADER: a draft pack, exit */
    const char *const *clang_args;  /* everything after "--" */
    int         clang_arg_count;
} ccr_options;

/* What is currently open in the editor. */
typedef struct ccr_document {
    bool       loaded;
    char       path[PATH_MAX_LEN];
    ccr_editor ed;
    uint32_t   errors_at_load;
    bool       backed_up;      /* the original was copied to FILE~ */
    char       status[512];
    bool       has_errors;
} ccr_document;

/* Blocks glide, menus pop, deleted blocks go poof. Off with
 * --no-animations or "animations = off" in the config file. */
static bool g_animate = true;

/* The blocks to take from: one palette for the whole session. */
static ccr_palette g_palette;

/* Colours (or words) on the blocks changed: lay everything out again. */
static void relayout_everything(ccr_document *doc)
{
    if (doc->loaded)
        ccr_editor_relayout_all(&doc->ed);
    if (g_palette.ready && !ccr_palette_relayout(&g_palette))
        fprintf(stderr, "ccratch: out of memory laying out the palette\n");
}

const char *ccr_version(void)
{
    return "0.0.1";
}

static void print_usage(const char *argv0)
{
    printf("Ccratch %s - Scratch for raw C\n\n"
           "usage: %s [options] [FILE.c]\n\n"
           "  FILE.c                 open this file as blocks (or drop it on the window)\n"
           "  --help                 show this and leave\n"
           "  --version              print version\n"
           "  --about                show which libclang and headers are used\n"
           "  --dump-ast FILE.c      parse FILE and print its syntax tree, no window\n"
           "  --ranges               with --dump-ast: the source bytes of every node\n"
           "  --roundtrip FILE.c     check that saving an unchanged FILE gives it back\n"
           "                         byte for byte\n"
           "  -- ARGS...             pass ARGS to clang (e.g. -- -std=c99 -Iinclude)\n"
           "  --zoom PERCENT         start at this zoom (25..300)\n"
           "  --lang CODE            interface language (en, ru...); remembered when\n"
           "                         picked with the button in the window\n"
           "  --list-langs           show the language packs found, and where\n"
           "  --lang-template        print the English text as a pack to translate\n"
           "  --eula                 the IHATERUSTL agreement (a joke; LICENSE is real)\n"
           "  --theme NAME           look: shrine, serious, masochist, scratch...\n"
           "  --list-themes          show the themes found, and where\n"
           "  --check-theme FILE     check a theme file for unknown keys and bad colours\n"
           "  --check-lang FILE      check a language pack for missing or broken text\n"
           "  --make-blocks HEADER   print a draft block pack for a library's header\n"
           "                         (save it as blocks/NAME.yaml and make it read well)\n"
           "  --check-blocks FILE    check a block pack\n"
           "  --list-blocks          show the block packs found, and where\n"
           "  --no-animations        blocks just jump into place (for good: put\n"
           "                         'animations = off' in ~/.config/ccratch/config)\n"
           "  --smoke-frames N       run N frames and exit (for CI)\n"
           "  --screenshot FILE.png  save the last frame before exiting\n",
           ccr_version(), argv0);
}

static void print_about(void)
{
    char clang_version[256];
    ccr_parse_clang_version(clang_version, sizeof clang_version);
    const char *how = NULL;
    const char *resource = ccr_parse_resource_dir(&how);
    printf("Ccratch %s\n"
           "libclang:     %s\n"
           "resource dir: %s\n"
           "              %s\n"
           "              (override with CCRATCH_CLANG_RESOURCE_DIR)\n",
           ccr_version(), clang_version,
           resource ? resource : "(libclang's own guess)", how);
}

/* Missing builtin headers look like a broken file; say what it really is. */
static void hint_missing_headers(const ccr_ast *ast)
{
    for (uint32_t i = 0; i < ast->diag_count; ++i) {
        const char *msg = ccr_ast_str(ast, ast->diags[i].message);
        if (strstr(msg, "stddef.h' file not found") || strstr(msg, "stdarg.h' file not found")) {
            fflush(stdout);  /* keep the hint after the tree */
            fprintf(stderr,
                    "[ccratch] hint: clang's own headers were not found; your code is probably fine.\n"
                    "          Run 'ccratch --about' to see where Ccratch looked.\n");
            return;
        }
    }
}

/* Exit codes: 0 clean, 1 the code has errors, 2 could not parse at all. */
/* --dump-ast --ranges: each node with the bytes it covers, for debugging
 * what saving splices. */
static void dump_ranges(const ccr_ast *a, ccr_node_id id, int depth)
{
    const ccr_node *n = &a->nodes[id];
    int len = n->src_e > n->src_b ? (int)(n->src_e - n->src_b) : 0;
    char text[64];
    int k = 0;
    for (int i = 0; i < len && k < 48; ++i) {
        char c = a->source[n->src_b + i];
        text[k++] = (c == '\n' || c == '\t') ? ' ' : c;
    }
    text[k] = '\0';
    printf("%*s%s %s  [%u,%u) \"%s%s\"\n", depth * 2, "", ccr_node_kind_name(n->kind),
           ccr_ast_str(a, n->name), n->src_b, n->src_e, text, len > 48 ? "..." : "");
    if (depth < 200)
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = a->nodes[c].next_sibling)
            dump_ranges(a, c, depth + 1);
}

static int run_dump_ast(const ccr_options *opt)
{
    ccr_ast ast;
    if (!ccr_ast_init(&ast)) {
        fprintf(stderr, "ccratch: out of memory\n");
        return 2;
    }

    if (!ccr_parse_file(opt->dump_ast, opt->clang_args, opt->clang_arg_count, &ast)) {
        fprintf(stderr, "ccratch: clang could not read '%s'. Opyat' rabotat'...\n", opt->dump_ast);
        ccr_ast_free(&ast);
        return 2;
    }

    if (opt->ranges)
        dump_ranges(&ast, 0, 0);
    else
        ccr_ast_dump(&ast, stdout);
    hint_missing_headers(&ast);
    int code = ccr_ast_count_diags(&ast, CCR_DIAG_ERROR) ? 1 : 0;
    ccr_ast_free(&ast);
    return code;
}

/* Exit codes: 0 identical, 1 different, 2 could not parse or generate. */
static int run_roundtrip(const ccr_options *opt)
{
    ccr_ast ast;
    if (!ccr_ast_init(&ast))
        return 2;
    if (!ccr_parse_file(opt->roundtrip, opt->clang_args, opt->clang_arg_count, &ast)) {
        fprintf(stderr, "ccratch: clang could not read '%s'\n", opt->roundtrip);
        ccr_ast_free(&ast);
        return 2;
    }
    char *text = NULL, err[256];
    size_t len = 0;
    int code = 2;
    if (!ccr_codegen(&ast, &text, &len, err, sizeof err)) {
        fprintf(stderr, "ccratch: %s\n", err);
    } else if (len == ast.source_len && memcmp(text, ast.source, len) == 0) {
        printf("%s: identical (%zu bytes)\n", opt->roundtrip, len);
        code = 0;
    } else {
        size_t i = 0;
        while (i < len && i < ast.source_len && text[i] == ast.source[i])
            ++i;
        printf("%s: differs at byte %zu (%zu vs %u bytes)\n", opt->roundtrip, i, len, ast.source_len);
        code = 1;
    }
    free(text);
    ccr_ast_free(&ast);
    return code;
}

/* Returns -1 to keep running, otherwise an exit code. */
static int parse_args(int argc, char **argv, ccr_options *opt)
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("%s\n", ccr_version());
            return 0;
        } else if (strcmp(a, "--about") == 0) {
            print_about();
            return 0;
        } else if (strcmp(a, "--") == 0) {
            opt->clang_args = (const char *const *)&argv[i + 1];
            opt->clang_arg_count = argc - (i + 1);
            break;
        } else if (strcmp(a, "--dump-ast") == 0 && i + 1 < argc) {
            opt->dump_ast = argv[++i];
        } else if (strcmp(a, "--roundtrip") == 0 && i + 1 < argc) {
            opt->roundtrip = argv[++i];
        } else if (strcmp(a, "--smoke-frames") == 0 && i + 1 < argc) {
            opt->smoke_frames = atoi(argv[++i]);
        } else if (strcmp(a, "--screenshot") == 0 && i + 1 < argc) {
            opt->screenshot = argv[++i];
        } else if (strcmp(a, "--no-animations") == 0) {
            opt->no_animations = true;
        } else if (strcmp(a, "--theme") == 0 && i + 1 < argc) {
            opt->theme = argv[++i];
        } else if (strcmp(a, "--list-themes") == 0) {
            static ccr_theme_info themes[64];
            int n = ccr_theme_scan(themes, 64);
            for (int k = 0; k < n; ++k)
                printf("%-12s %s\n", themes[k].code, themes[k].path[0] ? themes[k].path : "(built in)");
            return 0;
        } else if (strcmp(a, "--check-theme") == 0 && i + 1 < argc) {
            return ccr_theme_check(argv[++i], true, stdout) == 0 ? 0 : 1;
        } else if (strcmp(a, "--eula") == 0) {
            opt->eula = true;
        } else if (strcmp(a, "--ranges") == 0) {
            opt->ranges = true;
        } else if (strcmp(a, "--lang") == 0 && i + 1 < argc) {
            opt->lang = argv[++i];
        } else if (strcmp(a, "--list-langs") == 0) {
            static ccr_lang_info langs[64];
            int n = ccr_lang_scan(langs, 64);
            for (int k = 0; k < n; ++k) {
                int chars = 0;   /* letters, not bytes: names are UTF-8 */
                for (const char *c = langs[k].name; *c; ++c)
                    chars += ((unsigned char)*c & 0xC0) != 0x80;
                printf("%-6s %s%*s %s\n", langs[k].code, langs[k].name,
                       chars < 20 ? 20 - chars : 0, "",
                       langs[k].path[0] ? langs[k].path : "(built in)");
            }
            return 0;
        } else if (strcmp(a, "--lang-template") == 0) {
            ccr_lang_write_template(stdout);
            return 0;
        } else if (strcmp(a, "--make-blocks") == 0 && i + 1 < argc) {
            opt->make_blocks = argv[++i];
        } else if (strcmp(a, "--check-blocks") == 0 && i + 1 < argc) {
            ccr_lang_pick(opt->lang, NULL);
            return ccr_lib_check(argv[++i], stdout) == 0 ? 0 : 1;
        } else if (strcmp(a, "--list-blocks") == 0) {
            ccr_lib_load(stderr);
            for (int k = 0; k < ccr_lib_pack_count(); ++k) {
                const ccr_lib_pack *pk = ccr_lib_pack_at(k);
                printf("%-12s %4d blocks  %-16s %s\n", pk->code, pk->count,
                       pk->header[0] ? pk->header : "(always)", pk->path);
            }
            ccr_lib_unload();
            return 0;
        } else if (strcmp(a, "--check-lang") == 0 && i + 1 < argc) {
            return ccr_lang_check(argv[++i], stdout) == 0 ? 0 : 1;
        } else if (strcmp(a, "--zoom") == 0 && i + 1 < argc) {
            opt->zoom_percent = atoi(argv[++i]);
        } else if (a[0] != '-' && !opt->open) {
            opt->open = a;
        } else {
            fprintf(stderr, "ccratch: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }
    return -1;
}

/* ------------------------------------------------------- document */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void document_close(ccr_document *doc)
{
    if (doc->loaded)
        ccr_editor_free(&doc->ed);
    doc->loaded = false;
}

/* "Home" glides the view back instead of jumping (when animating). */
static struct {
    bool       active;
    ccr_canvas from, to;
    float      t;
} g_glide;

static void fit_view(ccr_canvas *cv, const ccr_document *doc, ccr_rect viewport, float zoom)
{
    g_glide.active = false;   /* a jump ends any glide */
    if (zoom < 0.25f) zoom = 0.25f;
    if (zoom > 3.0f)  zoom = 3.0f;
    cv->zoom = zoom;
    ccr_rect b = doc->loaded ? ccr_editor_bounds(&doc->ed) : (ccr_rect){ 0, 0, 0, 0 };
    cv->offset = (ccr_vec2){ viewport.x + 48.0f - b.x * zoom, viewport.y + 32.0f - b.y * zoom };
}

#define GLIDE_TIME 0.32f

static void fit_view_smooth(ccr_canvas *cv, const ccr_document *doc, ccr_rect viewport)
{
    ccr_canvas to = *cv;
    fit_view(&to, doc, viewport, 1.0f);
    if (!g_animate) {
        *cv = to;
        return;
    }
    g_glide.active = true;
    g_glide.from = *cv;
    g_glide.to = to;
    g_glide.t = 0.0f;
}

/* Glides the view so `node` is in the middle of it, and rings it. */
static void glide_to_node(ccr_canvas *cv, ccr_document *doc, ccr_rect viewport, ccr_node_id node)
{
    ccr_rect r;
    if (doc->loaded)
        ccr_editor_reveal(&doc->ed, node);   /* a label in a folded block: unfold it */
    if (!doc->loaded || !ccr_editor_node_rect(&doc->ed, node, &r))
        return;
    ccr_canvas to = *cv;
    to.offset.x = viewport.x + viewport.w * 0.5f - (r.x + r.w * 0.5f) * cv->zoom;
    to.offset.y = viewport.y + viewport.h * 0.5f - (r.y + r.h * 0.5f) * cv->zoom;
    if (!g_animate) {
        *cv = to;
        return;
    }
    g_glide.active = true;
    g_glide.from = *cv;
    g_glide.to = to;
    g_glide.t = 0.0f;
    ccr_editor_flash(&doc->ed, node, GLIDE_TIME);
}

/* Moves the glide on; true while it is still going. */
static bool glide_step(ccr_canvas *cv, float dt)
{
    if (!g_glide.active)
        return false;
    g_glide.t += dt;
    float k = g_glide.t / GLIDE_TIME;
    if (k >= 1.0f) {
        *cv = g_glide.to;
        g_glide.active = false;
        return false;
    }
    /* ease in and out */
    const float e = k < 0.5f ? 4.0f * k * k * k : 1.0f - powf(-2.0f * k + 2.0f, 3.0f) * 0.5f;
    cv->zoom = g_glide.from.zoom + (g_glide.to.zoom - g_glide.from.zoom) * e;
    cv->offset.x = g_glide.from.offset.x + (g_glide.to.offset.x - g_glide.from.offset.x) * e;
    cv->offset.y = g_glide.from.offset.y + (g_glide.to.offset.y - g_glide.from.offset.y) * e;
    return true;
}

static bool document_open(ccr_document *doc, const char *path, const ccr_options *opt)
{
    /* static: big (4 KB path), and only one is ever being opened */
    static ccr_document next;
    memset(&next, 0, sizeof next);
    snprintf(next.path, sizeof next.path, "%s", path);

    ccr_ast ast;
    if (!ccr_ast_init(&ast))
        return false;

    /* the project's own headers are found without -I: its root and its
     * include/ are searched, after whatever was given on the command line */
    enum { MAX_ARGS = 64 };
    const char *args[MAX_ARGS + 2];
    int nargs = 0;
    for (int i = 0; i < opt->clang_arg_count && nargs < MAX_ARGS; ++i)
        args[nargs++] = opt->clang_args[i];
    static char inc_root[CCR_FILES_PATH + 3], inc_dir[CCR_FILES_PATH + 11];
    char project[CCR_FILES_PATH];
    ccr_files_project_of(path, project, (int)sizeof project);
    snprintf(inc_root, sizeof inc_root, "-I%s", project);
    snprintf(inc_dir, sizeof inc_dir, "-I%s/include", project);
    args[nargs++] = inc_root;
    args[nargs++] = inc_dir;

    if (!ccr_parse_file(path, args, nargs, &ast)) {
        ccr_ast_free(&ast);
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CANNOT_READ, base_name(path));
        doc->has_errors = true;
        return false;
    }

    if (ast.diag_count) {
        fprintf(stderr, "[ccratch] %s:\n", path);
        ccr_ast_dump_diags(&ast, stderr);
        hint_missing_headers(&ast);
    }
    uint32_t errors = ccr_ast_count_diags(&ast, CCR_DIAG_ERROR);

    next.errors_at_load = errors;
    if (!ccr_editor_init(&next.ed, &ast)) {   /* takes the ast either way */
        ccr_editor_free(&next.ed);
        ccr_trf(doc->status, sizeof doc->status, CCR_T_OOM_LAYOUT);
        doc->has_errors = true;
        return false;
    }
    next.loaded = true;
    next.ed.animate = g_animate;
    next.has_errors = errors > 0;
    if (errors)
        ccr_trnf(next.status, sizeof next.status, CCR_T_LOADED_ERRORS, errors,
                 base_name(path), errors);
    else
        ccr_trf(next.status, sizeof next.status, CCR_T_LOADED_CLEAN, base_name(path));

    document_close(doc);
    *doc = next;
    return true;
}

/* ------------------------------------------------- several open files */

/* The file on screen is `doc`; the others wait here, undo history,
 * unsaved changes and view included. */
#define MAX_OPEN 12

typedef struct stashed_doc {
    ccr_document *doc;
    ccr_canvas    view;
} stashed_doc;

static stashed_doc g_stash[MAX_OPEN];
static int         g_stash_n;

/* The project the files panel shows. */
static ccr_files       g_files;
static ccr_files_panel g_fpanel;
static bool            g_files_open;
static float           g_files_t;   /* 0 hidden .. 1 out */

static bool same_file(const char *a, const char *b) { return ccr_files_same(a, b); }

/* A file from outside the project starts a new project, in its folder. */
static void project_follow(const char *path)
{
    if (g_files.root[0] && ccr_files_inside(g_files.root, path))
        return;
    char dir[CCR_FILES_PATH];
    ccr_files_project_of(path, dir, (int)sizeof dir);
    ccr_files_scan(&g_files, dir, path);
}

/* Shows `path`: brought back if it is open already, opened if not. The
 * file on screen waits with the others. */
static bool switch_to(ccr_document *doc, ccr_canvas *cv, ccr_rect viewport, const char *path,
                      const ccr_options *opt)
{
    if (doc->loaded && same_file(doc->path, path))
        return true;
    for (int i = 0; i < g_stash_n; ++i) {
        if (!same_file(g_stash[i].doc->path, path))
            continue;
        stashed_doc back = g_stash[i];
        if (doc->loaded) {
            /* swap: the one on screen takes its place in the line */
            ccr_document *slot = back.doc;
            ccr_document tmp = *slot;
            *slot = *doc;
            g_stash[i].view = *cv;
            *doc = tmp;
        } else {
            *doc = *back.doc;
            free(back.doc);
            g_stash[i] = g_stash[--g_stash_n];
        }
        *cv = back.view;
        g_glide.active = false;
        return true;
    }

    ccr_document *waiting = NULL;
    if (doc->loaded) {
        if (g_stash_n == MAX_OPEN) {
            ccr_trf(doc->status, sizeof doc->status, CCR_T_FILES_TOO_MANY);
            doc->has_errors = true;
            return false;
        }
        waiting = malloc(sizeof *waiting);
        if (!waiting)
            return false;
        *waiting = *doc;
        g_stash[g_stash_n++] = (stashed_doc){ waiting, *cv };
        doc->loaded = false;   /* its editor belongs to the stash now */
    }
    if (!document_open(doc, path, opt)) {
        if (waiting) {   /* could not read it: the old one comes back */
            char status[sizeof doc->status];
            snprintf(status, sizeof status, "%s", doc->status);
            *doc = *waiting;
            *cv = g_stash[--g_stash_n].view;
            free(waiting);
            snprintf(doc->status, sizeof doc->status, "%s", status);
            doc->has_errors = true;
        }
        return false;
    }
    project_follow(path);
    fit_view(cv, doc, viewport, 1.0f);
    return true;
}

/* Closes `path`, saved or not (the panel asked twice if not). */
static void close_file(ccr_document *doc, ccr_canvas *cv, const char *path)
{
    if (doc->loaded && same_file(doc->path, path)) {
        document_close(doc);
        if (g_stash_n > 0) {   /* the one that waited longest... no: the last one left */
            stashed_doc back = g_stash[--g_stash_n];
            *doc = *back.doc;
            *cv = back.view;
            free(back.doc);
        }
        g_glide.active = false;
        return;
    }
    for (int i = 0; i < g_stash_n; ++i)
        if (same_file(g_stash[i].doc->path, path)) {
            document_close(g_stash[i].doc);
            free(g_stash[i].doc);
            for (int k = i + 1; k < g_stash_n; ++k)
                g_stash[k - 1] = g_stash[k];
            --g_stash_n;
            return;
        }
}

/* Ctrl+Tab: the file that has waited longest comes, this one waits. */
static void next_file(ccr_document *doc, ccr_canvas *cv)
{
    if (g_stash_n == 0 || !doc->loaded)
        return;
    stashed_doc first = g_stash[0];
    for (int k = 1; k < g_stash_n; ++k)
        g_stash[k - 1] = g_stash[k];
    ccr_document tmp = *first.doc;
    *first.doc = *doc;
    g_stash[g_stash_n - 1] = (stashed_doc){ first.doc, *cv };
    *doc = tmp;
    *cv = first.view;
    g_glide.active = false;
}

/* A click on an #include: the header it names, opened next to this file. */
static void open_include(ccr_document *doc, ccr_canvas *cv, ccr_rect viewport, ccr_node_id inc,
                         const ccr_options *opt)
{
    const ccr_node *n = &doc->ed.ast.nodes[inc];
    char name[512], path[CCR_FILES_PATH], from[sizeof doc->path];
    snprintf(name, sizeof name, "%s", ccr_ast_str(&doc->ed.ast, n->name));
    snprintf(from, sizeof from, "%s", doc->path);
    const bool angled = (n->flags & CCR_FLAG_ANGLED) != 0;
    if (!g_files.root[0])
        project_follow(from);
    if (ccr_files_find_include(&g_files, from, name, path, (int)sizeof path)) {
        switch_to(doc, cv, viewport, path, opt);
        return;
    }
    ccr_trf(doc->status, sizeof doc->status, angled ? CCR_T_INCLUDE_SYSTEM : CCR_T_INCLUDE_NOT_FOUND,
            name);
    doc->has_errors = !angled;
}

/* The packs in the palette follow what the file on screen includes. */
static void palette_follow(const ccr_document *doc)
{
    static char last_path[sizeof doc->path];
    static uint32_t last_nodes, last_history;
    const uint32_t nodes = doc->loaded ? doc->ed.ast.node_count : 0;
    const uint32_t hist = doc->loaded ? doc->ed.history_len : 0;
    if (strcmp(last_path, doc->loaded ? doc->path : "") == 0 && nodes == last_nodes &&
        hist == last_history)
        return;
    snprintf(last_path, sizeof last_path, "%s", doc->loaded ? doc->path : "");
    last_nodes = nodes;
    last_history = hist;

    const char *inc[CCR_PALETTE_MAX_INCLUDES];
    int n = 0;
    if (doc->loaded && doc->ed.ast.node_count)
        for (ccr_node_id c = doc->ed.ast.nodes[0].first_child; c != CCR_NONE && n < CCR_PALETTE_MAX_INCLUDES;
             c = doc->ed.ast.nodes[c].next_sibling) {
            const ccr_node *k = &doc->ed.ast.nodes[c];
            if (k->kind == CCR_NODE_INCLUDE && !(k->flags & CCR_FLAG_DEAD))
                inc[n++] = ccr_ast_str(&doc->ed.ast, k->name);
        }
    if (g_palette.ready && !ccr_palette_set_includes(&g_palette, inc, n))
        fprintf(stderr, "ccratch: out of memory laying out the palette\n");
}

/* What the panel shows as open. */
static int open_files(const ccr_document *doc, ccr_open_file *out)
{
    int n = 0;
    if (doc->loaded)
        out[n++] = (ccr_open_file){ doc->path, ccr_editor_modified(&doc->ed), true };
    for (int i = 0; i < g_stash_n; ++i)
        out[n++] = (ccr_open_file){ g_stash[i].doc->path, ccr_editor_modified(&g_stash[i].doc->ed),
                                    false };
    return n;
}

static void files_toggle(const ccr_document *doc)
{
    g_files_open = !g_files_open;
    if (g_files_open) {   /* fresh: files may have come and gone */
        const char *root = g_files.root[0] ? g_files.root : ".";
        char dir[CCR_FILES_PATH];
        if (!g_files.root[0] && doc->loaded) {
            ccr_files_project_of(doc->path, dir, (int)sizeof dir);
            root = dir;
        }
        char keep[CCR_FILES_PATH];
        snprintf(keep, sizeof keep, "%s", root);
        ccr_files_scan(&g_files, keep, doc->loaded ? doc->path : NULL);
    }
}

#define FILES_W 290.0f

static ccr_rect files_area(int w, int h)
{
    const float k = g_files_t;
    const float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
    return (ccr_rect){ (float)w - FILES_W * e, TOPBAR_H, FILES_W,
                       (float)h - TOPBAR_H - STATUSBAR_H };
}

static bool write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/*
 * Ctrl+S. The text is written to FILE.ccratch-tmp and renamed over FILE,
 * so a crash halfway never leaves half a file. The first save of a
 * session copies the file as it was loaded to FILE~.
 */
static void document_save(ccr_document *doc, const ccr_options *opt)
{
    if (!doc->loaded)
        return;
    ccr_editor_settle(&doc->ed);   /* a block still gliding in goes in first */
    const char *name = base_name(doc->path);

    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(&doc->ed.ast, &text, &len, err, sizeof err)) {
        ccr_trf(doc->status, sizeof doc->status, CCR_T_NOT_SAVED, err);
        doc->has_errors = true;
        fprintf(stderr, "[ccratch] %s not saved: %s\n", doc->path, err);
        return;
    }

    char tmp[PATH_MAX_LEN + 32], backup[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.ccratch-tmp", doc->path);
    snprintf(backup, sizeof backup, "%s~", doc->path);

    if (!doc->backed_up) {
        if (!write_file(backup, doc->ed.ast.source, doc->ed.ast.source_len)) {
            ccr_trf(doc->status, sizeof doc->status, CCR_T_NO_BACKUP, name);
            doc->has_errors = true;
            free(text);
            return;
        }
        doc->backed_up = true;
    }
    if (!write_file(tmp, text, len) || rename(tmp, doc->path) != 0) {
        remove(tmp);
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CANNOT_WRITE, name);
        doc->has_errors = true;
        free(text);
        return;
    }
    free(text);
    ccr_editor_mark_saved(&doc->ed);

    /* read it back: did the move make code clang no longer accepts? */
    uint32_t errors = 0;
    ccr_ast check;
    if (ccr_ast_init(&check)) {
        if (ccr_parse_file(doc->path, opt->clang_args, opt->clang_arg_count, &check))
            errors = ccr_ast_count_diags(&check, CCR_DIAG_ERROR);
        if (errors > doc->errors_at_load) {
            fprintf(stderr, "[ccratch] after saving %s:\n", doc->path);
            ccr_ast_dump_diags(&check, stderr);
        }
        ccr_ast_free(&check);
    }

    uint32_t loose = ccr_editor_loose_count(&doc->ed);
    char extra[160] = "", said[340];
    if (loose) {
        extra[0] = ' ';
        ccr_trnf(extra + 1, sizeof extra - 1, CCR_T_LEFT_OUT, loose, loose);
    }
    if (errors > doc->errors_at_load) {
        ccr_trnf(said, sizeof said, CCR_T_SAVED_ERRORS, errors, name, errors);
        doc->has_errors = true;
    } else {
        ccr_trf(said, sizeof said, CCR_T_SAVED, name, name);
        doc->has_errors = false;
    }
    snprintf(doc->status, sizeof doc->status, "%s%s", said, extra);
}

/* -------------------------------------------------------- drawing */

/* A button the colour of the theme; `main`: the one to press. */
static const ccr_button_style *button_style(bool main)
{
    static ccr_button_style st;
    st = (ccr_button_style){
        .fill  = main ? ccr_th(CCR_C_ACCENT) : ccr_th(CCR_C_BUTTON),
        .hover = main ? ccr_th(CCR_C_ACCENT) : ccr_th(CCR_C_BUTTON_HOVER),
        .press = ccr_th(CCR_C_BUTTON_PRESS),
        .edge  = main ? ccr_th(CCR_C_ACCENT) : ccr_th(CCR_C_BUTTON_EDGE),
        .text  = main ? 0xFFFFFFFFu : ccr_th(CCR_C_BUTTON_TEXT),
    };
    if (main)   /* a little lighter under the mouse */
        st.hover = (st.fill & 0xFFu) | (((((st.fill >> 24) & 0xFFu) * 7 + 0xFF) / 8) << 24) |
                   (((((st.fill >> 16) & 0xFFu) * 7 + 0xFF) / 8) << 16) |
                   (((((st.fill >> 8) & 0xFFu) * 7 + 0xFF) / 8) << 8);
    return &st;
}

/* ------------------------------------------------------- the agreement */

/* What the IHATERUSTL agreement says, in order. */
typedef enum eula_style { EULA_HEAD, EULA_SMALL, EULA_LEAD, EULA_TEXT, EULA_CLAUSE,
                          EULA_WARN, EULA_GAP } eula_style;

static const struct { ccr_text text; eula_style style; } g_eula[] = {
    { CCR_T_EULA_TITLE, EULA_HEAD },     { CCR_T_EULA_FULL_NAME, EULA_SMALL },
    { CCR_T_EULA_SUBTITLE, EULA_SMALL }, { CCR_T_COUNT, EULA_GAP },
    { CCR_T_EULA_OPENING, EULA_LEAD },   { CCR_T_COUNT, EULA_GAP },
    { CCR_T_EULA_AGREE, EULA_TEXT },
    { CCR_T_EULA_C01, EULA_CLAUSE }, { CCR_T_EULA_C02, EULA_CLAUSE }, { CCR_T_EULA_C03, EULA_CLAUSE },
    { CCR_T_EULA_C04, EULA_CLAUSE }, { CCR_T_EULA_C05, EULA_CLAUSE }, { CCR_T_EULA_C06, EULA_CLAUSE },
    { CCR_T_EULA_C07, EULA_CLAUSE }, { CCR_T_EULA_C08, EULA_CLAUSE }, { CCR_T_EULA_C09, EULA_CLAUSE },
    { CCR_T_EULA_C10, EULA_CLAUSE }, { CCR_T_EULA_C11, EULA_CLAUSE }, { CCR_T_EULA_C12, EULA_CLAUSE },
    { CCR_T_EULA_C13, EULA_CLAUSE }, { CCR_T_EULA_C14, EULA_CLAUSE }, { CCR_T_EULA_C15, EULA_CLAUSE },
    { CCR_T_COUNT, EULA_GAP },
    { CCR_T_EULA_MEME_ZONE, EULA_WARN },
    { CCR_T_EULA_M01, EULA_CLAUSE }, { CCR_T_EULA_M02, EULA_CLAUSE },
    { CCR_T_COUNT, EULA_GAP },
    { CCR_T_EULA_FINAL, EULA_SMALL },
};
#define EULA_ITEMS ((int)(sizeof g_eula / sizeof g_eula[0]))

/* --eula: the same, in the terminal, wrapped at 76 letters; lines after
 * the first start with `indent`. */
static void print_wrapped(const char *prefix, const char *text, const char *indent, int width)
{
    const int indent_n = (int)strlen(indent);
    int col = (int)strlen(prefix);
    bool first = true;
    fputs(prefix, stdout);
    for (const char *p = text; *p;) {
        while (*p == ' ')
            ++p;
        const char *w = p;
        int letters = 0;
        for (; *p && *p != ' '; ++p)
            letters += ((unsigned char)*p & 0xC0) != 0x80;   /* UTF-8: count letters */
        if (p == w)
            break;
        if (!first && col + 1 + letters > width) {
            printf("\n%s", indent);
            col = indent_n;
        } else if (!first) {
            fputc(' ', stdout);
            ++col;
        }
        fwrite(w, 1, (size_t)(p - w), stdout);
        col += letters;
        first = false;
    }
    fputc('\n', stdout);
}

static void print_eula(void)
{
    int n = 0;
    for (int i = 0; i < EULA_ITEMS; ++i) {
        const char *t = g_eula[i].text < CCR_T_COUNT ? ccr_tr(g_eula[i].text) : "";
        char num[16];
        switch (g_eula[i].style) {
        case EULA_GAP:
            fputc('\n', stdout);
            break;
        case EULA_CLAUSE:
            snprintf(num, sizeof num, "%2d. ", ++n);
            print_wrapped(num, t, "    ", 76);
            break;
        case EULA_HEAD:
            printf("%s\n", t);
            break;
        default:
            print_wrapped("", t, "", 76);
            break;
        }
    }
}

typedef struct eula_view {
    bool  open;
    bool  first_run;   /* must be answered: accept, or admit to Rust */
    float scroll;
} eula_view;

/*
 * Lays out `text` in lines at most `w` wide at (x, y); draws them if
 * `draw`. Returns the height used.
 */
static float wrap_text(const char *text, float x, float y, float w, float size, ccr_color c,
                       bool draw)
{
    const float lh = size * 1.3f;
    char line[1200] = "", trial[2400];
    float h = 0.0f;
    for (const char *p = text; *p;) {
        const char *word = p;
        while (*p && *p != ' ')
            ++p;
        snprintf(trial, sizeof trial, "%s%s%.*s", line, *line ? " " : "", (int)(p - word), word);
        if (*line && ccr_gui_text_width(trial, size) > w) {
            if (draw)
                ccr_gui_text(line, x, y + h, size, c);
            h += lh;
            snprintf(line, sizeof line, "%.*s", (int)(p - word), word);
        } else {
            size_t n = strlen(trial);   /* a word longer than a line just runs on */
            if (n >= sizeof line)
                n = sizeof line - 1;
            memcpy(line, trial, n);
            line[n] = '\0';
        }
        while (*p == ' ')
            ++p;
    }
    if (*line) {
        if (draw)
            ccr_gui_text(line, x, y + h, size, c);
        h += lh;
    }
    return h;
}

/* All of the agreement, from (x, y). Returns its height. */
static float eula_body(float x, float y, float w, bool draw)
{
    float h = 0.0f;
    int n = 0;
    for (int i = 0; i < EULA_ITEMS; ++i) {
        const char *t = g_eula[i].text < CCR_T_COUNT ? ccr_tr(g_eula[i].text) : "";
        char num[16];
        switch (g_eula[i].style) {
        case EULA_GAP:
            h += 14.0f;
            break;
        case EULA_HEAD:
            if (draw)
                ccr_gui_text(t, x, y + h, 40.0f, COL_ACCENT);
            h += 46.0f;
            break;
        case EULA_SMALL:
            h += wrap_text(t, x, y + h, w, 15.0f, ccr_th(CCR_C_PANEL_HINT), draw);
            break;
        case EULA_LEAD:
            h += wrap_text(t, x, y + h, w, 21.0f, ccr_th(CCR_C_PANEL_TEXT), draw);
            break;
        case EULA_TEXT:
            h += wrap_text(t, x, y + h, w, 17.0f, ccr_th(CCR_C_PANEL_TEXT), draw) + 6.0f;
            break;
        case EULA_WARN:
            h += wrap_text(t, x, y + h, w, 17.0f, ccr_th(CCR_C_GLOW), draw) + 6.0f;
            break;
        case EULA_CLAUSE:
            snprintf(num, sizeof num, "%d.", ++n);
            if (draw)
                ccr_gui_text(num, x, y + h, 17.0f, COL_ACCENT);
            h += wrap_text(t, x + 34.0f, y + h, w - 34.0f, 17.0f, ccr_th(CCR_C_PANEL_HINT), draw) + 6.0f;
            break;
        }
    }
    return h;
}

/* The agreement over everything. On first run it waits for an answer. */
static void eula_frame(eula_view *ev, int width, int height, ccr_document *doc)
{
    if (!ev->open)
        return;
    ccr_gui_rect((ccr_rect){ 0, 0, (float)width, (float)height }, ccr_th(CCR_C_BACKDROP));
    const float pw = width - 80.0f < 780.0f ? (float)width - 80.0f : 780.0f;
    const float ph = (float)height - 80.0f;
    const ccr_rect panel = { ((float)width - pw) * 0.5f, 40.0f, pw, ph };
    ccr_gui_rect_rounded((ccr_rect){ panel.x - 2, panel.y - 2, panel.w + 4, panel.h + 4 }, 0.03f,
                         COL_ACCENT);
    ccr_gui_rect_rounded(panel, 0.03f, ccr_th(CCR_C_PANEL) | 0xFFu);

    const float pad = 28.0f, buttons_h = 64.0f;
    const ccr_rect text = { panel.x + pad, panel.y + pad, panel.w - pad * 2.0f,
                            panel.h - pad - buttons_h };
    const float full = eula_body(text.x, 0.0f, text.w - 12.0f, false);
    const float max_scroll = full > text.h ? full - text.h : 0.0f;
    ev->scroll -= ccr_gui_wheel() * 60.0f;
    if (ev->scroll < 0.0f) ev->scroll = 0.0f;
    if (ev->scroll > max_scroll) ev->scroll = max_scroll;

    ccr_gui_clip_begin(text);
    eula_body(text.x, text.y - ev->scroll, text.w - 12.0f, true);
    ccr_gui_clip_end();
    if (max_scroll > 0.0f) {   /* a thin scroll bar */
        float bh = text.h * text.h / full;
        float by = text.y + (text.h - bh) * (ev->scroll / max_scroll);
        ccr_gui_rect_rounded((ccr_rect){ text.x + text.w - 4.0f, by, 4.0f, bh }, 1.0f, ccr_th(CCR_C_PANEL_EDGE));
    }

    const float by = panel.y + panel.h - buttons_h + 14.0f;
    if (ev->first_run) {
        const ccr_rect rust = { panel.x + panel.w - pad - 180.0f, by, 180.0f, 34.0f };
        const ccr_rect ok = { rust.x - 12.0f - 180.0f, by, 180.0f, 34.0f };
        bool accepted = ccr_gui_button(ok, ccr_tr(CCR_T_EULA_ACCEPT), button_style(true));
        bool rusted = !accepted && ccr_gui_button(rust, ccr_tr(CCR_T_EULA_RUST), button_style(false));
        if (accepted || rusted) {
            if (!ccr_config_set("eula", accepted ? "accepted" : "rust"))
                fprintf(stderr, "ccratch: could not remember the answer in ~/.config/ccratch\n");
            ccr_trf(doc->status, sizeof doc->status,
                    accepted ? CCR_T_EULA_ACCEPTED : CCR_T_EULA_RUSTED);
            doc->has_errors = rusted;
            ev->open = ev->first_run = false;
        }
    } else {
        const ccr_rect close = { panel.x + panel.w - pad - 180.0f, by, 180.0f, 34.0f };
        if (ccr_gui_button(close, ccr_tr(CCR_T_EULA_CLOSE), button_style(true)) ||
            ccr_gui_key_pressed(CCR_KEY_ESCAPE))
            ev->open = false;
    }
}

/* The language button: the next pack each click, remembered for next time. */
static void next_language(ccr_document *doc)
{
    static ccr_lang_info langs[64];
    int n = ccr_lang_scan(langs, 64);
    int cur = 0;
    for (int i = 0; i < n; ++i)
        if (strcmp(langs[i].code, ccr_lang_code()) == 0)
            cur = i;
    for (int step = 1; step <= n; ++step) {
        const ccr_lang_info *li = &langs[(cur + step) % n];
        if (ccr_lang_use(li->code, stderr)) {
            if (!ccr_lang_save_choice(li->code))
                fprintf(stderr, "ccratch: could not remember the language in ~/.config/ccratch\n");
            char theme[32];
            snprintf(theme, sizeof theme, "%s", ccr_theme_code());
            ccr_theme_use(theme, NULL);   /* theme names come in this language too */
            ccr_trf(doc->status, sizeof doc->status, CCR_T_LANGUAGE, ccr_lang_name());
            doc->has_errors = false;
            /* C keywords stay, but pack blocks speak this language now */
            ccr_lib_load(stderr);
            relayout_everything(doc);
            for (int k = 0; k < g_stash_n; ++k)
                ccr_editor_relayout_all(&g_stash[k].doc->ed);
            return;
        }
    }
}

/* The theme button: the next theme each click, remembered. Blocks keep
 * their colours from when they were laid out, so all are laid out again. */
static void next_theme(ccr_document *doc)
{
    static ccr_theme_info themes[64];
    int n = ccr_theme_scan(themes, 64);
    int cur = 0;
    for (int i = 0; i < n; ++i)
        if (strcmp(themes[i].code, ccr_theme_code()) == 0)
            cur = i;
    for (int step = 1; step <= n; ++step) {
        const ccr_theme_info *ti = &themes[(cur + step) % n];
        if (ccr_theme_use(ti->code, stderr)) {
            if (!ccr_theme_save_choice(ti->code))
                fprintf(stderr, "ccratch: could not remember the theme in ~/.config/ccratch\n");
            ccr_trf(doc->status, sizeof doc->status, CCR_T_THEME_STATUS, ccr_theme_name());
            doc->has_errors = false;
            relayout_everything(doc);
            return;
        }
    }
}

static void draw_topbar(int width, ccr_document *doc, eula_view *ev)
{
    const bool modal = ev->open;
    ccr_gui_rect((ccr_rect){ 0, 0, (float)width, TOPBAR_H }, COL_BAR);
    ccr_gui_circle((ccr_vec2){ 22.0f, TOPBAR_H * 0.5f }, 8.0f, COL_ACCENT);
    ccr_gui_text("Ccratch", 38.0f, 8.0f, 22.0f, COL_TITLE);
    if (doc->loaded) {
        char title[300];
        snprintf(title, sizeof title, "%s%s", base_name(doc->path),
                 ccr_editor_modified(&doc->ed) ? " *" : "");
        ccr_gui_text(title, 38.0f + ccr_gui_text_width("Ccratch", 22.0f) + 18.0f, 11.0f, 18.0f,
                     COL_BAR_TEXT);
    }

    char code[24];
    snprintf(code, sizeof code, "%s", ccr_lang_code());
    for (char *c = code; *c; ++c)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 'a' + 'A');
    const ccr_button_style *bs = button_style(false);
    const float tw = ccr_gui_text_width(ccr_tr(CCR_T_THEME_BUTTON), 16.0f) + 24.0f;
    if (ccr_gui_button((ccr_rect){ (float)width - 166.0f, 7.0f, 48.0f, 26.0f }, code, bs) && !modal)
        next_language(doc);
    if (ccr_gui_button((ccr_rect){ (float)width - 174.0f - tw, 7.0f, tw, 26.0f },
                       ccr_tr(CCR_T_THEME_BUTTON), bs) && !modal)
        next_theme(doc);
    const float fw = ccr_gui_text_width(ccr_tr(CCR_T_FILES_TITLE), 16.0f) + 24.0f;
    if (ccr_gui_button((ccr_rect){ (float)width - 222.0f - tw - fw, 7.0f, fw, 26.0f },
                       ccr_tr(CCR_T_FILES_TITLE), g_files_open ? button_style(true) : bs) && !modal)
        files_toggle(doc);
    if (ccr_gui_button((ccr_rect){ (float)width - 214.0f - tw, 7.0f, 32.0f, 26.0f }, "?", bs) &&
        !modal) {
        ev->open = true;   /* the agreement again, to read it at leisure */
        ev->scroll = 0.0f;
    }

    float x = (float)width - 110.0f;
    if (ccr_gui_button((ccr_rect){ x, 7.0f, 100.0f, 26.0f }, ccr_tr(CCR_T_BUILD), button_style(true)) &&
        !modal)
        printf("[ccratch] build: nothing to build yet. Go do some shrine work.\n");
}

static void draw_statusbar(int width, int height, const ccr_canvas *cv, const ccr_document *doc)
{
    float y = (float)height - STATUSBAR_H;
    ccr_gui_rect((ccr_rect){ 0, y, (float)width, STATUSBAR_H }, COL_BAR);

    const char *msg = doc->status[0] ? doc->status : ccr_tr(CCR_T_NO_FILE);
    ccr_color dot = doc->has_errors ? ccr_th(CCR_C_ERROR) : COL_OK;
    ccr_gui_circle((ccr_vec2){ 14.0f, y + STATUSBAR_H * 0.5f }, 4.0f, dot);
    ccr_gui_text(msg, 26.0f, y + 5.0f, 15.0f, COL_BAR_TEXT);

    char right[512], loose_txt[128] = "", zoom[64];
    uint32_t loose = doc->loaded ? ccr_editor_loose_count(&doc->ed) : 0;
    if (loose) {
        ccr_trnf(loose_txt, sizeof loose_txt - 8, CCR_T_BAR_LOOSE, loose, loose);
        strcat(loose_txt, "   |   ");
    }
    ccr_trf(zoom, sizeof zoom, CCR_T_BAR_ZOOM, (int)(cv->zoom * 100.0f + 0.5f));
    snprintf(right, sizeof right, "%s%s  |  %s", loose_txt, zoom, ccr_tr(CCR_T_BAR_HINTS));
    /* no room next to the message (long words, narrow window): hints go first */
    float room = (float)width - 26.0f - ccr_gui_text_width(msg, 15.0f) - 48.0f;
    if (ccr_gui_text_width(right, 15.0f) > room)
        snprintf(right, sizeof right, "%s%s", loose_txt, zoom);
    float rw = ccr_gui_text_width(right, 15.0f);
    ccr_gui_text(right, (float)width - rw - 12.0f, y + 5.0f, 15.0f, COL_BAR_TEXT);
}

/* ------------------------------------------------------------ palette */

static bool point_in(ccr_vec2 p, ccr_rect r)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

static ccr_rect palette_area(int height)
{
    return (ccr_rect){ 0, TOPBAR_H, PALETTE_W, (float)height - TOPBAR_H - STATUSBAR_H };
}

/* ------------------------------------------------- the operator menu */

/* A click on an operator offers the others of its kind: + for - * / %... */
typedef struct op_menu {
    bool               open;
    ccr_node_id        node;
    ccr_vec2           at;        /* screen: top middle */
    const char *const *choices;
    int                count;
    double             opened_at;
} op_menu;

#define OP_CELL_W  52.0f
#define OP_CELL_H  36.0f
#define OP_COLS    4

static ccr_rect op_cell(const op_menu *om, int i, int screen_w, int screen_h)
{
    const int cols = om->count < OP_COLS ? om->count : OP_COLS;
    const int rows = (om->count + OP_COLS - 1) / OP_COLS;
    const float w = (float)cols * OP_CELL_W + 12.0f, h = (float)rows * OP_CELL_H + 12.0f;
    float x = om->at.x - w * 0.5f, y = om->at.y;
    if (x < 4.0f) x = 4.0f;
    if (x + w > (float)screen_w - 4.0f) x = (float)screen_w - 4.0f - w;
    if (y + h > (float)screen_h - 4.0f) y = om->at.y - h - 40.0f;   /* no room under: above */
    if (i < 0)
        return (ccr_rect){ x, y, w, h };   /* the whole panel */
    return (ccr_rect){ x + 6.0f + (float)(i % OP_COLS) * OP_CELL_W,
                       y + 6.0f + (float)(i / OP_COLS) * OP_CELL_H, OP_CELL_W - 4.0f, OP_CELL_H - 4.0f };
}

static void op_menu_open(op_menu *om, const ccr_editor *ed, ccr_node_id node, ccr_vec2 mouse)
{
    const ccr_node *n = &ed->ast.nodes[node];
    om->choices = ccr_op_choices(ccr_ast_str(&ed->ast, n->op), n->kind == CCR_NODE_UNARY_OP,
                                 &om->count);
    om->open = om->choices != NULL;
    om->node = node;
    om->at = (ccr_vec2){ mouse.x, mouse.y + 18.0f };
    om->opened_at = ccr_gui_time();
}

/* Returns the operator picked this frame, or NULL; any click closes it. */
static const char *op_menu_update(op_menu *om, ccr_vec2 m, int w, int h)
{
    if (!om->open)
        return NULL;
    if (ccr_gui_key_pressed(CCR_KEY_ESCAPE) || ccr_gui_mouse_pressed(CCR_MOUSE_RIGHT)) {
        om->open = false;
        return NULL;
    }
    if (!ccr_gui_mouse_pressed(CCR_MOUSE_LEFT))
        return NULL;
    om->open = false;
    for (int i = 0; i < om->count; ++i)
        if (point_in(m, op_cell(om, i, w, h)))
            return om->choices[i];
    return NULL;
}

static void op_menu_draw(const op_menu *om, const ccr_editor *ed, ccr_vec2 m, int w, int h)
{
    if (!om->open)
        return;
    float k = g_animate ? (float)(ccr_gui_time() - om->opened_at) / 0.12f : 1.0f;
    if (k > 1.0f) k = 1.0f;
    const ccr_rect panel = op_cell(om, -1, w, h);
    const float drop = (1.0f - k) * -8.0f;   /* slides down a little as it opens */
    const unsigned a = (unsigned)(255.0f * k);
    const ccr_color edge = (ccr_th(CCR_C_PANEL_EDGE) & 0xFFFFFF00u) | a;
    ccr_gui_rect_rounded((ccr_rect){ panel.x - 1, panel.y - 1 + drop, panel.w + 2, panel.h + 2 }, 0.2f, edge);
    ccr_gui_rect_rounded((ccr_rect){ panel.x, panel.y + drop, panel.w, panel.h }, 0.2f,
                         (ccr_th(CCR_C_PANEL) & 0xFFFFFF00u) | (a * (ccr_th(CCR_C_PANEL) & 0xFFu) / 255u));
    const char *cur = ccr_ast_str(&ed->ast, ed->ast.nodes[om->node].op);
    for (int i = 0; i < om->count; ++i) {
        ccr_rect c = op_cell(om, i, w, h);
        c.y += drop;
        const bool now = strcmp(om->choices[i], cur) == 0, hot = point_in(m, c);
        if (now || hot)
            ccr_gui_rect_rounded(c, 0.3f, ((now ? ccr_th(CCR_C_HOT) : ccr_th(CCR_C_BUTTON_HOVER))
                                           & 0xFFFFFF00u) | a);
        const float tw = ccr_gui_text_width(om->choices[i], 20.0f);
        ccr_gui_text(om->choices[i], c.x + (c.w - tw) * 0.5f, c.y + (c.h - 20.0f) * 0.5f, 20.0f,
                     ((now ? 0xFFFFFFFFu : ccr_th(CCR_C_PANEL_TEXT)) & 0xFFFFFF00u) | a);
    }
}

/* ------------------------------------------------- actions (pie, keys) */

/* A text field being typed into. */
typedef struct text_edit {
    ccr_node_id node;
    char        buf[1024];
} text_edit;

static void start_editing(text_edit *te, const ccr_editor *ed, ccr_node_id slot)
{
    te->node = slot;
    ccr_gui_textbox_reset();
    snprintf(te->buf, sizeof te->buf, "%s", ccr_ast_str(&ed->ast, ed->ast.nodes[slot].name));
}

/* The field "Edit text" would open: the one under the mouse, or a code
 * line's only field. */
static ccr_node_id edit_target(const ccr_editor *ed, const ccr_context *ctx)
{
    if (ctx->text != CCR_NONE)
        return ctx->text;
    if (ctx->item != CCR_NONE && ed->ast.nodes[ctx->item].kind == CCR_NODE_TEXT)
        return ctx->item;
    return CCR_NONE;
}

static void pie_enabled(const ccr_editor *ed, const ccr_context *ctx, bool en[CCR_ACTION_COUNT])
{
    for (int i = 0; i < CCR_ACTION_COUNT; ++i)
        en[i] = false;
    en[CCR_ACTION_SAVE]      = true;
    en[CCR_ACTION_FIT_VIEW]  = true;
    en[CCR_ACTION_UNDO]      = ccr_editor_can_undo(ed);
    en[CCR_ACTION_REDO]      = ccr_editor_can_redo(ed);
    en[CCR_ACTION_DELETE]    = ctx->can_delete;   /* on ctx->target */
    en[CCR_ACTION_DUPLICATE] = ctx->can_duplicate;
    en[CCR_ACTION_COMMENT]   = ctx->can_comment;
    en[CCR_ACTION_EDIT_TEXT] = edit_target(ed, ctx) != CCR_NONE;
}

/* Everything the pie and the keyboard can do, in one place. `ctx` is what
 * was under the mouse when the action was asked for. */
static void run_action(ccr_action a, const ccr_context *ctx, ccr_vec2 world,
                       ccr_document *doc, const ccr_options *opt,
                       ccr_canvas *cv, ccr_rect viewport, text_edit *te)
{
    ccr_editor *ed = &doc->ed;
    switch (a) {
    case CCR_ACTION_SAVE:      document_save(doc, opt); break;
    case CCR_ACTION_UNDO:      ccr_editor_undo(ed); break;
    case CCR_ACTION_REDO:      ccr_editor_redo(ed); break;
    case CCR_ACTION_FIT_VIEW:   /* over a block that folds, that slice folds it */
        if (ctx->fold != CCR_NONE)
            ccr_editor_toggle_fold(ed, ctx->fold);
        else
            fit_view_smooth(cv, doc, viewport);
        break;
    case CCR_ACTION_DELETE:    ccr_editor_delete(ed, ctx->target); break;
    case CCR_ACTION_DUPLICATE: ccr_editor_duplicate(ed, ctx->target, world); break;
    case CCR_ACTION_COMMENT:   ccr_editor_toggle_comment(ed, ctx->item); break;
    case CCR_ACTION_EDIT_TEXT: {
        ccr_node_id slot = edit_target(ed, ctx);
        if (slot != CCR_NONE)
            start_editing(te, ed, slot);
        break;
    }
    case CCR_ACTION_NONE:
    case CCR_ACTION_COUNT:
        break;
    }
}

static void draw_empty_hint(ccr_rect viewport)
{
    const char *a = ccr_tr(CCR_T_EMPTY_TITLE);
    const char *b = ccr_tr(CCR_T_EMPTY_HINT);
    float cx = viewport.x + viewport.w * 0.5f, cy = viewport.y + viewport.h * 0.5f;
    ccr_gui_text(a, cx - ccr_gui_text_width(a, 30.0f) * 0.5f, cy - 30.0f, 30.0f, COL_HINT);
    ccr_gui_text(b, cx - ccr_gui_text_width(b, 20.0f) * 0.5f, cy + 10.0f, 20.0f, COL_HINT);
}

/* ------------------------------------------------------------ run */

int ccr_run(int argc, char **argv)
{
    ccr_options opt = { 0 };
    int early = parse_args(argc, argv, &opt);
    if (early >= 0)
        return early;

    if (opt.eula) {
        ccr_lang_pick(opt.lang, stderr);
        print_eula();
        ccr_lang_reset();
        return 0;
    }
    if (opt.dump_ast)
        return run_dump_ast(&opt);
    if (opt.make_blocks) {
        if (ccr_lib_make(opt.make_blocks, opt.clang_args, opt.clang_arg_count, stdout))
            return 0;
        fprintf(stderr, "ccratch: clang could not read '%s'\n", opt.make_blocks);
        return 1;
    }
    if (opt.roundtrip)
        return run_roundtrip(&opt);

    if (!ccr_gui_init(1280, 800, "Ccratch")) {
        fprintf(stderr, "ccratch: could not open a window. Opyat' rabotat'...\n");
        return 1;
    }
    ccr_lang_pick(opt.lang, stderr);
    ccr_theme_pick(opt.theme, stderr);
    ccr_lib_load(stderr);   /* block packs: libraries as blocks */
    char anim[16];
    g_animate = !opt.no_animations &&
                !(ccr_config_get("animations", anim, sizeof anim) &&
                  (strcmp(anim, "off") == 0 || strcmp(anim, "no") == 0 || strcmp(anim, "0") == 0));
    char answered[16];
    eula_view eula = { 0 };
    eula.open = eula.first_run =
        opt.smoke_frames == 0 && !ccr_config_get("eula", answered, sizeof answered);
    if (!load_font())
        fprintf(stderr, "ccratch: font did not load, falling back to raylib's (ASCII only)\n");
    if (!ccr_palette_init(&g_palette))
        fprintf(stderr, "ccratch: out of memory laying out the palette\n");
    if (!ccr_gui_set_icon_png(ccr_icon_png, ccr_icon_png_size))
        fprintf(stderr, "ccratch: icon did not load, carrying on without it\n");

    ccr_canvas canvas;
    ccr_canvas_init(&canvas);

    /* static: a document holds a 4 KB path and is reused all session */
    static ccr_document doc;
    ccr_rect viewport = { PALETTE_W, TOPBAR_H, (float)ccr_gui_width() - PALETTE_W,
                          (float)ccr_gui_height() - TOPBAR_H - STATUSBAR_H };
    const float start_zoom = opt.zoom_percent > 0 ? (float)opt.zoom_percent / 100.0f : 1.0f;
    if (opt.open && switch_to(&doc, &canvas, viewport, opt.open, &opt))
        fit_view(&canvas, &doc, viewport, start_zoom);
    else
        fit_view(&canvas, &doc, viewport, start_zoom);

    int frame = 0;
    char dropped[PATH_MAX_LEN];

    /* a text slot being typed into */
    static text_edit te = { .node = CCR_NONE };

    /* the pie menu, and the right click that may open it */
    ccr_pie pie = { 0 };
    op_menu opm = { 0 };
    ccr_context pie_ctx = { 0 };
    ccr_vec2 rmb_from = { 0 };
    bool rmb_armed = false;

    double last_time = ccr_gui_time();
    while (!ccr_gui_should_close()) {
        /* seconds since the last frame, for animations; a stall (window
         * dragged, laptop asleep) does not make things jump */
        const double now = ccr_gui_time();
        float dt = (float)(now - last_time);
        last_time = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.05f) dt = 0.05f;
        const int w = ccr_gui_width();
        const int h = ccr_gui_height();
        viewport = (ccr_rect){ PALETTE_W, TOPBAR_H, (float)w - PALETTE_W,
                               (float)h - TOPBAR_H - STATUSBAR_H };
        if (!doc.loaded)
            te.node = CCR_NONE;
        const bool was_open = pie.open || opm.open;

        if (ccr_gui_file_dropped(dropped, sizeof dropped) &&
            switch_to(&doc, &canvas, viewport, dropped, &opt)) {
            te.node = CCR_NONE;
            ccr_pie_close(&pie);
            opm.open = false;
        }
        if (ccr_gui_key_pressed(CCR_KEY_F5) && doc.loaded && !pie.open) {
            char again[PATH_MAX_LEN];
            snprintf(again, sizeof again, "%s", doc.path);
            document_open(&doc, again, &opt);
            te.node = CCR_NONE;
        }

        const ccr_vec2 m = ccr_gui_mouse();
        const ccr_vec2 world = ccr_canvas_to_world(&canvas, m);
        const bool inside = point_in(m, viewport) && !(g_files_t > 0.0f && point_in(m, files_area(w, h)));
        const bool over_palette = m.x < PALETTE_W && m.y > TOPBAR_H;
        palette_follow(&doc);
        const bool can_act = doc.loaded && te.node == CCR_NONE && !eula.open &&
                             g_fpanel.naming == CCR_NAMING_NONE;   /* typing a new file's name */

        /* the files panel slides in from the right, over the canvas */
        if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_P) && !eula.open)
            files_toggle(&doc);
        if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_TAB) && !eula.open && te.node == CCR_NONE &&
            doc.ed.drag == CCR_DRAG_IDLE && !pie.open && !opm.open)
            next_file(&doc, &canvas);
        {
            const float target = g_files_open ? 1.0f : 0.0f;
            const float step = g_animate ? dt / 0.18f : 1.0f;
            g_files_t += g_files_t < target ? step : -step;
            if ((g_files_t > target) == (target > 0.5f) || !g_animate)
                g_files_t = target;
            if (g_files_t < 0.0f) g_files_t = 0.0f;
        }
        const ccr_rect files_r = files_area(w, h);
        const bool over_files = g_files_t > 0.0f && point_in(m, files_r);
        if (g_files_open && !eula.open && !pie.open && !opm.open && te.node == CCR_NONE &&
            (!doc.loaded || doc.ed.drag == CCR_DRAG_IDLE)) {
            ccr_open_file of[MAX_OPEN + 1];
            const int nof = open_files(&doc, of);
            char path[CCR_FILES_PATH];
            ccr_files_event ev = ccr_files_panel_update(
                &g_fpanel, &g_files, files_r, m, over_files ? ccr_gui_wheel() : 0.0f,
                over_files && ccr_gui_mouse_pressed(CCR_MOUSE_LEFT), of, nof, path, (int)sizeof path);
            if (ev == CCR_FILES_OPEN && switch_to(&doc, &canvas, viewport, path, &opt))
                te.node = CCR_NONE;
            else if (ev == CCR_FILES_CLOSE)
                close_file(&doc, &canvas, path);
        }
        if (!pie.open && !eula.open)
            ccr_palette_update(&g_palette, palette_area(h), m, ccr_gui_wheel(),
                               over_palette && ccr_gui_mouse_pressed(CCR_MOUSE_LEFT), dt);

        /* the pie, while it is open, gets the mouse and Esc to itself */
        if (pie.open) {
            bool cancel = !can_act || ccr_gui_key_pressed(CCR_KEY_ESCAPE) ||
                          ccr_gui_mouse_pressed(CCR_MOUSE_RIGHT);
            ccr_action a = ccr_pie_update(&pie, m, ccr_gui_mouse_pressed(CCR_MOUSE_LEFT), cancel);
            if (a != CCR_ACTION_NONE)
                run_action(a, &pie_ctx, world, &doc, &opt, &canvas, viewport, &te);
        }

        /* the operator menu, the same way */
        if (opm.open) {
            const char *op = op_menu_update(&opm, m, w, h);
            if (!can_act)
                opm.open = false;
            else if (op)
                ccr_editor_set_op(&doc.ed, opm.node, op);
        }

        /* a right click that did not drag opens it; a drag pans */
        if (ccr_gui_mouse_pressed(CCR_MOUSE_RIGHT)) {
            rmb_from = m;
            rmb_armed = inside && !was_open;
        }
        if (ccr_gui_mouse_down(CCR_MOUSE_RIGHT) &&
            fabsf(m.x - rmb_from.x) + fabsf(m.y - rmb_from.y) > 4.0f)
            rmb_armed = false;
        if (ccr_gui_mouse_released(CCR_MOUSE_RIGHT) && rmb_armed && can_act &&
            doc.ed.drag == CCR_DRAG_IDLE) {
            bool en[CCR_ACTION_COUNT];
            pie_ctx = ccr_editor_context(&doc.ed, world);
            pie_enabled(&doc.ed, &pie_ctx, en);
            pie.animate = g_animate;
            ccr_pie_open(&pie, m, en, pie_ctx.commented);
            pie.fold = pie_ctx.fold == CCR_NONE ? 0 : pie_ctx.folded ? 2 : 1;
        }
        rmb_armed = rmb_armed && ccr_gui_mouse_down(CCR_MOUSE_RIGHT);

        if (can_act && !pie.open && !was_open && ccr_gui_ctrl_down()) {
            if (ccr_gui_key_pressed(CCR_KEY_S))
                document_save(&doc, &opt);
            /* Ctrl+Z undo; Ctrl+Y or Ctrl+Shift+Z redo */
            if (ccr_gui_key_pressed(CCR_KEY_Z)) {
                if (ccr_gui_shift_down())
                    ccr_editor_redo(&doc.ed);
                else
                    ccr_editor_undo(&doc.ed);
            }
            if (ccr_gui_key_pressed(CCR_KEY_Y))
                ccr_editor_redo(&doc.ed);
        }
        /* the block under the mouse: Delete, Ctrl+D, Ctrl+/ */
        if (can_act && !pie.open && !was_open && inside) {
            ccr_action a = CCR_ACTION_NONE;
            if (ccr_gui_key_pressed(CCR_KEY_DELETE))
                a = CCR_ACTION_DELETE;
            else if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_D))
                a = CCR_ACTION_DUPLICATE;
            else if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_SLASH))
                a = CCR_ACTION_COMMENT;
            if (a != CCR_ACTION_NONE) {
                ccr_context ctx = ccr_editor_context(&doc.ed, world);
                bool en[CCR_ACTION_COUNT];
                pie_enabled(&doc.ed, &ctx, en);
                if (en[a])
                    run_action(a, &ctx, world, &doc, &opt, &canvas, viewport, &te);
            }
        }
        if (ccr_gui_key_pressed(CCR_KEY_HOME) && !pie.open)
            fit_view_smooth(&canvas, &doc, viewport);

        const bool gliding = glide_step(&canvas, dt);
        if (!pie.open && !was_open && !eula.open) {
            const ccr_canvas before = canvas;
            ccr_canvas_update(&canvas, viewport);
            /* the user panned or zoomed: they have the wheel now */
            if (gliding && (canvas.zoom != before.zoom || canvas.offset.x != before.offset.x ||
                            canvas.offset.y != before.offset.y))
                g_glide.active = false;
        }

        if (can_act && !pie.open && !was_open) {
            /* a carried copy (Duplicate) lands on a click anywhere, the
             * palette included, where it is thrown away */
            const bool lmb = ccr_gui_mouse_pressed(CCR_MOUSE_LEFT);
            ccr_vec2 grab;
            int spawn = (over_palette && lmb && !doc.ed.carrying)
                      ? ccr_palette_hit(&g_palette, palette_area(h), m, &grab) : -1;
            if (spawn >= 0)
                ccr_editor_spawn_held(&doc.ed, (ccr_template)spawn, world, grab);
            ccr_editor_input in = {
                .mouse      = ccr_canvas_to_world(&canvas, m),
                .pressed    = lmb && (inside || doc.ed.carrying),
                .down       = ccr_gui_mouse_down(CCR_MOUSE_LEFT),
                .released   = ccr_gui_mouse_released(CCR_MOUSE_LEFT),
                .zoom       = canvas.zoom,
                .over_trash = over_palette,
                .dt         = dt,
            };
            ccr_editor_update(&doc.ed, &in);

            /* a click on a goto arrow: off to its other end */
            if (lmb && inside && doc.ed.drag == CCR_DRAG_IDLE && doc.ed.hover_arrow != CCR_NONE)
                glide_to_node(&canvas, &doc, viewport, doc.ed.hover_far);

            ccr_node_id req = ccr_editor_take_edit_request(&doc.ed);
            if (req != CCR_NONE)
                start_editing(&te, &doc.ed, req);
            ccr_node_id incq = ccr_editor_take_include_request(&doc.ed);
            if (incq != CCR_NONE)
                open_include(&doc, &canvas, viewport, incq, &opt);
            ccr_node_id opq = ccr_editor_take_op_request(&doc.ed);
            if (opq != CCR_NONE)
                op_menu_open(&opm, &doc.ed, opq, m);
        } else if (doc.loaded) {
            ccr_editor_tick(&doc.ed, dt);   /* menu open: things still move */
        }

        ccr_gui_begin_frame(COL_BG);
        ccr_canvas_draw_grid(&canvas, viewport);
        if (doc.loaded)
            ccr_editor_draw(&doc.ed, &canvas, viewport);
        else
            draw_empty_hint(viewport);
        ccr_palette_draw(&g_palette, palette_area(h),
                         doc.loaded && doc.ed.drag == CCR_DRAG_MOVING && over_palette);

        /* the text box over the slot being edited */
        if (te.node != CCR_NONE) {
            ccr_rect r;
            if (!ccr_editor_node_rect(&doc.ed, te.node, &r)) {
                te.node = CCR_NONE;
            } else {
                ccr_vec2 tl = ccr_canvas_to_screen(&canvas, (ccr_vec2){ r.x, r.y });
                float bw = r.w * canvas.zoom;
                ccr_rect box = { tl.x - 4.0f, tl.y - 4.0f, bw < 320.0f ? 320.0f : bw + 40.0f, 34.0f };
                if (ccr_gui_key_pressed(CCR_KEY_ESCAPE)) {
                    te.node = CCR_NONE;                     /* cancel */
                } else if (ccr_gui_textbox_colors(ccr_th(CCR_C_FIELD_EDGE), ccr_th(CCR_C_FIELD),
                                                  ccr_th(CCR_C_FIELD_TEXT)),
                           ccr_gui_textbox(box, te.buf, (int)sizeof te.buf, 20.0f)) {
                    ccr_editor_set_text(&doc.ed, te.node, te.buf);
                    te.node = CCR_NONE;
                }
            }
        }
        if (g_files_t > 0.0f) {
            ccr_open_file of[MAX_OPEN + 1];
            const int nof = open_files(&doc, of);
            ccr_files_panel_draw(&g_fpanel, &g_files, files_area(w, h), of, nof);
            if (g_files_open && te.node == CCR_NONE && !eula.open && !pie.open && !opm.open) {
                char made[CCR_FILES_PATH];
                if (ccr_files_panel_tools(&g_fpanel, &g_files, files_area(w, h), button_style(false),
                                          made, (int)sizeof made) == CCR_FILES_OPEN &&
                    switch_to(&doc, &canvas, viewport, made, &opt))
                    te.node = CCR_NONE;
            } else {
                g_fpanel.naming = CCR_NAMING_NONE;
            }
        }
        draw_topbar(w, &doc, &eula);
        draw_statusbar(w, h, &canvas, &doc);
        ccr_pie_draw(&pie);
        if (doc.loaded)
            op_menu_draw(&opm, &doc.ed, m, w, h);
        eula_frame(&eula, w, h, &doc);

        ++frame;
        const bool last = opt.smoke_frames > 0 && frame >= opt.smoke_frames;
        if (last && opt.screenshot && !ccr_gui_screenshot(opt.screenshot))
            fprintf(stderr, "ccratch: failed to save screenshot '%s'\n", opt.screenshot);

        ccr_gui_end_frame();
        if (last)
            break;
    }

    document_close(&doc);
    for (int i = 0; i < g_stash_n; ++i) {
        document_close(g_stash[i].doc);
        free(g_stash[i].doc);
    }
    g_stash_n = 0;
    ccr_files_free(&g_files);
    ccr_palette_free(&g_palette);
    ccr_lib_unload();
    ccr_gui_shutdown();
    ccr_lang_reset();
    return 0;
}
