#include "ccratch/ccratch.h"
#include "ccr_ast.h"
#include "ccr_build.h"
#include "ccr_canvas.h"
#include "ccr_cmake.h"
#include "ccr_codegen.h"
#include "ccr_config.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_lang.h"
#include "ccr_parse.h"
#include "ccr_files.h"
#include "ccr_job.h"
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
    const char *make_cmake;     /* --make-cmake DIR: a first CMakeLists.txt, printed */
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
    /* where every block is in the file as it was last written (or read):
     * what a compiler's "file.c:12:5" points at */
    ccr_out_range *place;
    uint32_t       place_n;
    uint32_t      *line_at;     /* byte offset of each line */
    uint32_t       line_n;
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
           "  --make-cmake DIR       print the CMakeLists.txt Ccratch would make for a project\n"
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

/* C through clang; CMakeLists.txt and *.cmake through Ccratch's own reader. */
static bool parse_any(const char *path, const char *const *args, int nargs, ccr_ast *out)
{
    if (ccr_cmake_is_cmake(path)) {
        ccr_cmake_parse_file(path, out);   /* its problems are in out->diags */
        return out->source != NULL;
    }
    return ccr_parse_file(path, args, nargs, out);
}

static int run_dump_ast(const ccr_options *opt)
{
    ccr_ast ast;
    if (!ccr_ast_init(&ast)) {
        fprintf(stderr, "ccratch: out of memory\n");
        return 2;
    }

    if (!parse_any(opt->dump_ast, opt->clang_args, opt->clang_arg_count, &ast)) {
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
    if (!parse_any(opt->roundtrip, opt->clang_args, opt->clang_arg_count, &ast) ||
        ccr_ast_count_diags(&ast, CCR_DIAG_ERROR) > (ccr_cmake_is_cmake(opt->roundtrip) ? 0u : ~0u)) {
        ccr_ast_dump_diags(&ast, stderr);
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
        } else if (strcmp(a, "--make-cmake") == 0 && i + 1 < argc) {
            opt->make_cmake = argv[++i];
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

static void forget_places(ccr_document *doc)
{
    free(doc->place);
    free(doc->line_at);
    doc->place = NULL;
    doc->line_at = NULL;
    doc->place_n = doc->line_n = 0;
}

/* Where the blocks are in the text saving writes now (the file, when it
 * was just saved or read). */
static void remember_places(ccr_document *doc)
{
    forget_places(doc);
    if (!doc->loaded)
        return;
    const ccr_ast *a = &doc->ed.ast;
    char *text = NULL, err[256];
    size_t len = 0;
    ccr_out_range *map = malloc((a->node_count ? a->node_count : 1) * sizeof *map);
    if (!map || !ccr_codegen_mapped(a, &text, &len, map, err, sizeof err)) {
        free(map);
        return;
    }
    uint32_t lines = 1;
    for (size_t i = 0; i < len; ++i)
        lines += text[i] == '\n';
    doc->line_at = malloc(lines * sizeof *doc->line_at);
    if (doc->line_at) {
        uint32_t k = 0;
        doc->line_at[k++] = 0;
        for (size_t i = 0; i < len; ++i)
            if (text[i] == '\n' && k < lines)
                doc->line_at[k++] = (uint32_t)(i + 1);
        doc->line_n = k;
    }
    free(text);
    doc->place = map;
    doc->place_n = a->node_count;
}

static void document_close(ccr_document *doc)
{
    if (doc->loaded)
        ccr_editor_free(&doc->ed);
    forget_places(doc);
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

/* What clang gets for a file of the project around `path`: the command
 * line's flags, then the project's root and its include/ (so its own
 * headers are found without -I). `args` holds CLANG_ARGS_MAX. */
#define CLANG_ARGS_MAX 66
static int clang_args_for(const char *path, const ccr_options *opt, const char **args)
{
    int nargs = 0;
    for (int i = 0; i < opt->clang_arg_count && nargs < CLANG_ARGS_MAX - 2; ++i)
        args[nargs++] = opt->clang_args[i];
    static char inc_root[CCR_FILES_PATH + 3], inc_dir[CCR_FILES_PATH + 11];
    char project[CCR_FILES_PATH];
    ccr_files_project_of(path, project, (int)sizeof project);
    snprintf(inc_root, sizeof inc_root, "-I%s", project);
    snprintf(inc_dir, sizeof inc_dir, "-I%s/include", project);
    args[nargs++] = inc_root;
    args[nargs++] = inc_dir;
    return nargs;
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

    const char *args[CLANG_ARGS_MAX];
    const int nargs = clang_args_for(path, opt, args);

    if (!parse_any(path, args, nargs, &ast) ||
        (ast.lang == CCR_LANG_CMAKE && ccr_ast_count_diags(&ast, CCR_DIAG_ERROR))) {
        if (ast.diag_count)
            ccr_ast_dump_diags(&ast, stderr);
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
    remember_places(doc);
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

/* Block packs again (the project's own first), and everything that shows
 * them laid out again: a pack came, went, or speaks another language. */
static void libs_reload(ccr_document *doc)
{
    ccr_lib_load_project(g_files.root[0] ? g_files.root : NULL, stderr);
    relayout_everything(doc);
    for (int k = 0; k < g_stash_n; ++k)
        ccr_editor_relayout_all(&g_stash[k].doc->ed);
}

/* A file from outside the project starts a new project, in its folder:
 * true then (its packs are not loaded yet). */
static bool project_follow(const char *path)
{
    if (g_files.root[0] && ccr_files_inside(g_files.root, path))
        return false;
    char dir[CCR_FILES_PATH];
    ccr_files_project_of(path, dir, (int)sizeof dir);
    ccr_files_scan(&g_files, dir, path);
    return true;
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
    if (project_follow(path))
        libs_reload(doc);
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
    if (g_palette.ready &&
        !ccr_palette_set_lang(&g_palette, doc->loaded ? doc->ed.ast.lang : CCR_LANG_C))
        fprintf(stderr, "ccratch: out of memory laying out the palette\n");
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
/* Writes the file; false (and why, in the status) if it could not. */
static bool document_save(ccr_document *doc, const ccr_options *opt)
{
    if (!doc->loaded)
        return false;
    ccr_editor_settle(&doc->ed);   /* a block still gliding in goes in first */
    const char *name = base_name(doc->path);

    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(&doc->ed.ast, &text, &len, err, sizeof err)) {
        ccr_trf(doc->status, sizeof doc->status, CCR_T_NOT_SAVED, err);
        doc->has_errors = true;
        fprintf(stderr, "[ccratch] %s not saved: %s\n", doc->path, err);
        return false;
    }

    char tmp[PATH_MAX_LEN + 32], backup[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.ccratch-tmp", doc->path);
    snprintf(backup, sizeof backup, "%s~", doc->path);

    if (!doc->backed_up) {
        if (!write_file(backup, doc->ed.ast.source, doc->ed.ast.source_len)) {
            ccr_trf(doc->status, sizeof doc->status, CCR_T_NO_BACKUP, name);
            doc->has_errors = true;
            free(text);
            return false;
        }
        doc->backed_up = true;
    }
    if (!write_file(tmp, text, len) || rename(tmp, doc->path) != 0) {
        remove(tmp);
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CANNOT_WRITE, name);
        doc->has_errors = true;
        free(text);
        return false;
    }
    free(text);
    ccr_editor_mark_saved(&doc->ed);
    remember_places(doc);

    /* read it back: did the move make code clang no longer accepts? */
    uint32_t errors = 0;
    ccr_ast check;
    if (ccr_ast_init(&check)) {
        if (parse_any(doc->path, opt->clang_args, opt->clang_arg_count, &check))
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
    return true;
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
            libs_reload(doc);
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

static bool point_in(ccr_vec2 p, ccr_rect r)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

/* ------------------------------------------- libraries, made into blocks */

/* A row of the dialog: a header the file includes that has no blocks yet,
 * or a pack the file does not include. */
typedef struct lib_row {
    char spelled[160];   /* <zlib.h>, "toy.h" */
    int  pack;           /* -1: no pack yet */
} lib_row;

#define LIB_ROWS 24

static struct {
    bool    open;
    char    name[256];   /* typed: any header */
    lib_row rows[LIB_ROWS];
    int     count, first_pack;   /* rows[first_pack..] are packs */
    char    note[600];
    bool    note_bad;
} g_libs;

static bool doc_includes_pack(const ccr_document *doc, const ccr_lib_pack *pk)
{
    if (!doc->loaded || !doc->ed.ast.node_count)
        return false;
    for (ccr_node_id c = doc->ed.ast.nodes[0].first_child; c != CCR_NONE;
         c = doc->ed.ast.nodes[c].next_sibling) {
        const ccr_node *k = &doc->ed.ast.nodes[c];
        if (k->kind == CCR_NODE_INCLUDE && !(k->flags & CCR_FLAG_DEAD) &&
            ccr_lib_pack_matches(pk, ccr_ast_str(&doc->ed.ast, k->name)))
            return true;
    }
    return false;
}

static void libs_rows(const ccr_document *doc)
{
    g_libs.count = 0;
    if (doc->loaded && doc->ed.ast.node_count)
        for (ccr_node_id c = doc->ed.ast.nodes[0].first_child; c != CCR_NONE && g_libs.count < LIB_ROWS / 2;
             c = doc->ed.ast.nodes[c].next_sibling) {
            const ccr_node *k = &doc->ed.ast.nodes[c];
            if (k->kind != CCR_NODE_INCLUDE || (k->flags & CCR_FLAG_DEAD))
                continue;
            const char *name = ccr_ast_str(&doc->ed.ast, k->name);
            bool has = false;
            for (int i = 0; i < ccr_lib_pack_count() && !has; ++i)
                has = ccr_lib_pack_at(i)->header[0] && ccr_lib_pack_matches(ccr_lib_pack_at(i), name);
            if (has || !*name)
                continue;
            lib_row *row = &g_libs.rows[g_libs.count++];
            const bool angled = (k->flags & CCR_FLAG_ANGLED) != 0;
            snprintf(row->spelled, sizeof row->spelled, "%c%.150s%c", angled ? '<' : '"', name,
                     angled ? '>' : '"');
            row->pack = -1;
        }
    g_libs.first_pack = g_libs.count;
    for (int i = 0; i < ccr_lib_pack_count() && g_libs.count < LIB_ROWS; ++i) {
        const ccr_lib_pack *pk = ccr_lib_pack_at(i);
        if (!pk->header[0] || !pk->include[0] || doc_includes_pack(doc, pk))
            continue;
        lib_row *row = &g_libs.rows[g_libs.count++];
        snprintf(row->spelled, sizeof row->spelled, "%s", pk->include);
        row->pack = i;
    }
}

static void libs_open(const ccr_document *doc)
{
    g_libs.open = true;
    g_libs.name[0] = '\0';
    g_libs.note[0] = '\0';
    ccr_gui_textbox_reset();
    libs_rows(doc);
}

static void libs_note(ccr_document *doc, bool bad, const char *text)
{
    snprintf(g_libs.note, sizeof g_libs.note, "%s", text);
    g_libs.note_bad = bad;
    snprintf(doc->status, sizeof doc->status, "%s", text);
    doc->has_errors = bad;
}

/* "~/..." for the home folder, "./..." inside the project */
static void short_path(const char *path, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    const size_t pl = strlen(g_files.root), hl = home ? strlen(home) : 0;
    if (pl && strncmp(path, g_files.root, pl) == 0 && path[pl] == '/')
        snprintf(out, cap, ".%s", path + pl);
    else if (hl && strncmp(path, home, hl) == 0 && path[hl] == '/')
        snprintf(out, cap, "~%s", path + hl);
    else
        snprintf(out, cap, "%s", path);
}

/* The file includes pack `i`'s header (one undo step). */
static void libs_include(ccr_document *doc, int i)
{
    const ccr_lib_pack *pk = ccr_lib_pack_at(i);
    if (!pk || !doc->loaded)
        return;
    if (ccr_editor_add_include(&doc->ed, pk->include)) {
        char msg[300];
        ccr_trf(msg, sizeof msg, CCR_T_LIBS_INCLUDED, pk->include);
        libs_note(doc, false, msg);
    }
    libs_rows(doc);
}

/* Any header, as typed or as the file includes it: read (the API, and the
 * ABI if its binary is found), made into a pack, loaded, included. */
static void libs_make(ccr_document *doc, const ccr_options *opt, const char *spelled)
{
    const char *project = g_files.root[0] ? g_files.root : NULL;
    const char *from = doc->loaded ? doc->path : NULL;
    const char *args[CLANG_ARGS_MAX];
    const int nargs = clang_args_for(from ? from : (project ? project : "."), opt, args);
    char header[CCR_FILES_PATH], msg[sizeof g_libs.note];
    bool found = ccr_lib_find_header(spelled, from, project, args, nargs, header, (int)sizeof header);
    if (!found && from && spelled[0] != '<') {   /* somewhere in the project */
        char name[256];
        snprintf(name, sizeof name, "%s", spelled + (spelled[0] == '"'));
        name[strcspn(name, "\"")] = '\0';
        found = ccr_files_find_include(&g_files, from, name, header, (int)sizeof header);
    }
    if (!found) {
        ccr_trf(msg, sizeof msg, CCR_T_LIBS_NOT_FOUND, spelled);
        libs_note(doc, true, msg);
        return;
    }

    /* how files include it: as this one does, if it does */
    char include[160];
    if (spelled[0] == '<' || spelled[0] == '"')
        snprintf(include, sizeof include, "%.150s", spelled);
    else
        ccr_lib_include_for(header, project, include, (int)sizeof include);
    /* the project's own library: its pack goes with the project */
    char dir[CCR_FILES_PATH + 32];
    if (project && ccr_files_inside(project, header))
        ccr_lib_project_dir(project, dir, (int)sizeof dir);
    else if (!ccr_lib_user_dir(dir, (int)sizeof dir) && project)
        ccr_lib_project_dir(project, dir, (int)sizeof dir);

    char path[CCR_FILES_PATH + 300], err[300];
    int count = 0;
    const char *hb = strrchr(header, '/') ? strrchr(header, '/') + 1 : header;
    if (!ccr_lib_create_pack(header, include, dir, args, nargs, project, path, (int)sizeof path,
                             &count, err, (int)sizeof err)) {
        ccr_trf(msg, sizeof msg, CCR_T_LIBS_FAILED, hb, err);
        libs_note(doc, true, msg);
        return;
    }
    libs_reload(doc);
    int at = -1;
    for (int i = 0; i < ccr_lib_pack_count(); ++i)
        if (ccr_files_same(ccr_lib_pack_at(i)->path, path))
            at = i;
    char where[CCR_FILES_PATH + 300];
    short_path(path, where, sizeof where);
    const ccr_lib_pack *pk = ccr_lib_pack_at(at);
    if (!pk) {   /* made, but too many packs to load it */
        ccr_trf(msg, sizeof msg, CCR_T_LIBS_FAILED, hb, where);
        libs_note(doc, true, msg);
        return;
    }
    ccr_trnf(msg, sizeof msg, CCR_T_LIBS_MADE, (unsigned long)pk->count, pk->name,
             (unsigned)pk->count, where);
    libs_note(doc, false, msg);
    if (doc->loaded && !doc_includes_pack(doc, pk))
        ccr_editor_add_include(&doc->ed, pk->include);
    libs_rows(doc);
    g_libs.name[0] = '\0';
}

/* -------------------------------------------------------- CMake */

/* The CMake button: the project's CMakeLists.txt, made first if missing. */
static void cmake_open(ccr_document *doc, ccr_canvas *cv, ccr_rect viewport, const ccr_options *opt)
{
    char project[CCR_FILES_PATH];
    if (doc->loaded) {
        ccr_files_project_of(doc->path, project, (int)sizeof project);
    } else if (g_files.root[0]) {
        snprintf(project, sizeof project, "%s", g_files.root);
    } else {
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CMAKE_NO_PROJECT);
        doc->has_errors = true;
        return;
    }
    char path[CCR_FILES_PATH + 32];
    int files = 0;
    const ccr_cmake_made r = ccr_build_make_cmake(project, doc->loaded ? doc->path : NULL, path,
                                                  (int)sizeof path, &files);
    char said[sizeof doc->status];
    switch (r) {
    case CCR_CMAKE_THERE:
        switch_to(doc, cv, viewport, path, opt);
        return;
    case CCR_CMAKE_MADE:
        ccr_files_scan(&g_files, project, path);   /* the panel shows it, the project is it */
        if (switch_to(doc, cv, viewport, path, opt)) {
            ccr_trnf(said, sizeof said, CCR_T_CMAKE_MADE, (unsigned long)files, (unsigned)files);
            snprintf(doc->status, sizeof doc->status, "%s", said);
            doc->has_errors = false;
        }
        return;
    case CCR_CMAKE_NO_SOURCES:
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CMAKE_NO_SOURCES);
        break;
    case CCR_CMAKE_CANNOT:
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CANNOT_WRITE, "CMakeLists.txt");
        break;
    }
    doc->has_errors = true;
}

#define LIBS_W 640.0f

static void libs_frame(int width, int height, ccr_document *doc, const ccr_options *opt)
{
    if (!g_libs.open)
        return;
    const ccr_vec2 m = ccr_gui_mouse();
    ccr_gui_rect((ccr_rect){ 0, TOPBAR_H, (float)width, (float)height - TOPBAR_H }, ccr_th(CCR_C_BACKDROP));
    const float pw = (float)width - 40.0f < LIBS_W ? (float)width - 40.0f : LIBS_W;
    const float pad = 24.0f, tw = pw - pad * 2.0f;
    const float explain_h = wrap_text(ccr_tr(CCR_T_LIBS_EXPLAIN), 0, 0, tw, 15.0f, 0, false);
    const int packs = g_libs.count - g_libs.first_pack, files = g_libs.first_pack;
    const float row_h = 34.0f;
    float ph = pad + 34.0f + explain_h + 14.0f + 2.0f * (26.0f + 6.0f) +
               row_h * (float)((files ? files : 1) + (packs ? packs : 1)) + 16.0f + 46.0f + 30.0f + 50.0f;
    if (ph > (float)height - TOPBAR_H - 20.0f)
        ph = (float)height - TOPBAR_H - 20.0f;
    const ccr_rect panel = { ((float)width - pw) * 0.5f, TOPBAR_H + 14.0f, pw, ph };
    ccr_gui_rect_rounded((ccr_rect){ panel.x - 2, panel.y - 2, panel.w + 4, panel.h + 4 }, 0.04f,
                         ccr_th_cat(CCR_CAT_LIBRARY, CCR_FILL));
    ccr_gui_rect_rounded(panel, 0.04f, ccr_th(CCR_C_PANEL) | 0xFFu);

    float x = panel.x + pad, y = panel.y + pad;
    ccr_gui_circle((ccr_vec2){ x + 9.0f, y + 12.0f }, 9.0f, ccr_th_cat(CCR_CAT_LIBRARY, CCR_FILL));
    ccr_gui_text(ccr_tr(CCR_T_LIBS_HEADING), x + 28.0f, y, 24.0f, ccr_th(CCR_C_PANEL_TEXT));
    y += 34.0f;
    y += wrap_text(ccr_tr(CCR_T_LIBS_EXPLAIN), x, y, tw, 15.0f, ccr_th(CCR_C_PANEL_HINT), true) + 14.0f;

    int make = -1, include = -1;
    const float bottom = panel.y + panel.h - 46.0f - 30.0f - 50.0f;
    for (int part = 0; part < 2; ++part) {
        if (y + 26.0f > bottom)
            break;
        ccr_gui_text(ccr_tr(part ? CCR_T_LIBS_HAVE : CCR_T_LIBS_IN_FILE), x, y, 16.0f,
                     ccr_th(CCR_C_PANEL_TEXT));
        y += 26.0f;
        const int from = part ? g_libs.first_pack : 0, to = part ? g_libs.count : g_libs.first_pack;
        if (from == to) {
            ccr_gui_text(ccr_tr(CCR_T_LIBS_NOTHING), x + 14.0f, y + 6.0f, 15.0f, ccr_th(CCR_C_DIM_TEXT));
            y += row_h;
        }
        for (int i = from; i < to && y + row_h <= bottom; ++i, y += row_h) {
            const lib_row *row = &g_libs.rows[i];
            const ccr_rect rr = { x, y, tw, row_h - 4.0f };
            if (point_in(m, rr))
                ccr_gui_rect_rounded(rr, 0.3f, ccr_th(CCR_C_HOT));
            ccr_gui_rect_rounded((ccr_rect){ x + 6.0f, y + 9.0f, 12.0f, 12.0f }, 0.4f,
                                 part ? ccr_th_cat(CCR_CAT_LIBRARY, CCR_FILL) : ccr_th_cat(CCR_CAT_PREPROC, CCR_FILL));
            char label[300];
            if (part)
                snprintf(label, sizeof label, "%.60s   %.150s", ccr_lib_pack_at(row->pack)->name, row->spelled);
            else
                snprintf(label, sizeof label, "#include %.150s", row->spelled);
            ccr_gui_text(label, x + 26.0f, y + 6.0f, 17.0f, ccr_th(CCR_C_PANEL_TEXT));
            const char *bl = ccr_tr(part ? CCR_T_LIBS_INCLUDE : CCR_T_LIBS_MAKE);
            const float bw = ccr_gui_text_width(bl, 15.0f) + 22.0f;
            if (ccr_gui_button((ccr_rect){ x + tw - bw - 4.0f, y + 2.0f, bw, row_h - 8.0f }, bl,
                               button_style(!part)) && doc->loaded) {
                if (part) include = row->pack;
                else make = i;
            }
        }
        y += 6.0f;
    }

    /* any header at all */
    const float fy = panel.y + panel.h - 46.0f - 30.0f - 44.0f;
    ccr_gui_text(ccr_tr(CCR_T_LIBS_ASK), x, fy, 15.0f, ccr_th(CCR_C_PANEL_HINT));
    bool typed = false;
    ccr_gui_textbox_colors(ccr_th(CCR_C_FIELD_EDGE), ccr_th(CCR_C_FIELD), ccr_th(CCR_C_FIELD_TEXT));
    if (ccr_gui_textbox((ccr_rect){ x, fy + 22.0f, tw, 36.0f }, g_libs.name, (int)sizeof g_libs.name, 19.0f))
        typed = g_libs.name[0] != '\0';
    const char *cl = ccr_tr(CCR_T_LIBS_CLOSE);
    const float cw = ccr_gui_text_width(cl, 16.0f) + 40.0f;
    if (g_libs.note[0]) {   /* what just happened, next to Close */
        const float nw = tw - cw - 16.0f;
        float size = 15.0f;
        while (size > 11.0f && ccr_gui_text_width(g_libs.note, size) > nw)
            size -= 0.5f;
        ccr_gui_clip_begin((ccr_rect){ x, panel.y + panel.h - 50.0f, nw, 40.0f });
        ccr_gui_text(g_libs.note, x, panel.y + panel.h - 30.0f - size * 0.5f, size,
                     g_libs.note_bad ? ccr_th(CCR_C_ERROR) : ccr_th(CCR_C_OK));
        ccr_gui_clip_end();
    }
    bool close = ccr_gui_button((ccr_rect){ panel.x + panel.w - pad - cw, panel.y + panel.h - 46.0f, cw, 32.0f },
                                cl, button_style(false)) ||
                 ccr_gui_key_pressed(CCR_KEY_ESCAPE) ||
                 (ccr_gui_mouse_pressed(CCR_MOUSE_LEFT) && !point_in(m, panel) && m.y > TOPBAR_H);

    /* the work, after the drawing: reading a header takes a moment */
    if (make >= 0) {
        char spelled[160];
        snprintf(spelled, sizeof spelled, "%s", g_libs.rows[make].spelled);
        libs_make(doc, opt, spelled);
    } else if (include >= 0) {
        libs_include(doc, include);
    } else if (typed) {
        char spelled[256];
        snprintf(spelled, sizeof spelled, "%s", g_libs.name);
        libs_make(doc, opt, spelled);
    }
    if (close)
        g_libs.open = false;
}

/* ---------------------------------------------------------- building */

/* The Build button's work, in the background; its log in a panel that
 * slides up from the bottom; its errors on the blocks they are about. */
static ccr_job g_job;
static struct {
    bool     open;
    float    t;            /* 0 hidden .. 1 up */
    float    scroll;       /* first line shown */
    bool     follow;       /* keep the last line in view */
    int      hot;          /* log line under the mouse, or -1 */
    ccr_job_result last;   /* to notice the end */
} g_bp = { .follow = true, .hot = -1 };

#define BUILD_H   236.0f
#define LOG_ROW   19.0f
#define LOG_FONT  14.0f

static ccr_rect build_area(int w, int h)
{
    const float k = g_bp.t;
    const float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
    const float right = g_files_t > 0.0f ? files_area(w, h).x : (float)w;
    return (ccr_rect){ PALETTE_W, (float)h - STATUSBAR_H - BUILD_H * e, right - PALETTE_W, BUILD_H };
}

/* Is a diagnostic's file this document's? Compilers give whole paths; the
 * linker only the name of the file the code came from. */
static bool diag_is_for(const ccr_build_diag *d, const ccr_document *doc)
{
    if (!d->file[0] || !doc->loaded)
        return false;
    if (d->file[0] == '/')
        return ccr_files_same(d->file, doc->path);
    const size_t n = strlen(d->file), pn = strlen(doc->path);
    return pn >= n && strcmp(doc->path + pn - n, d->file) == 0 && (pn == n || doc->path[pn - n - 1] == '/');
}

static bool is_statement(const ccr_ast *a, ccr_node_id id)
{
    const ccr_node_id p = a->nodes[id].parent;
    return p == CCR_NONE || a->nodes[p].kind == CCR_NODE_BLOCK ||
           a->nodes[p].kind == CCR_NODE_TRANSLATION_UNIT;
}

/* The block at line:col of the file as written: the smallest thing there;
 * with no column, the statement the line starts. */
static ccr_node_id locate(const ccr_document *doc, uint32_t line, uint32_t col)
{
    if (!doc->place || line == 0 || line > doc->line_n)
        return CCR_NONE;
    const ccr_ast *a = &doc->ed.ast;
    const uint32_t ls = doc->line_at[line - 1];
    const uint32_t le = line < doc->line_n ? doc->line_at[line] : UINT32_MAX;
    ccr_node_id best = CCR_NONE;
    uint32_t best_size = UINT32_MAX, best_b = UINT32_MAX;
    for (uint32_t i = 1; i < doc->place_n && i < a->node_count; ++i) {
        const ccr_out_range r = doc->place[i];
        const ccr_node_kind k = a->nodes[i].kind;
        if (r.b == UINT32_MAX || r.e <= r.b || k == CCR_NODE_BLOCK || k == CCR_NODE_TRANSLATION_UNIT)
            continue;
        if (col) {
            uint32_t off = ls + col - 1;
            if (off >= le)
                off = le - 1;
            if (r.b <= off && off < r.e && r.e - r.b < best_size) {
                best = i;
                best_size = r.e - r.b;
            }
        } else if (r.b >= ls && r.b < le &&
                   (r.b < best_b || (r.b == best_b && r.e - r.b > best_size))) {
            best = i;   /* the first thing on the line; of those, the biggest */
            best_b = r.b;
            best_size = r.e - r.b;
        }
    }
    if (best == CCR_NONE && !col)
        return locate(doc, line, 1);   /* inside a statement that began above */
    if (!col)
        while (best != CCR_NONE && !is_statement(a, best))
            best = a->nodes[best].parent;
    return best;
}

/* The build's errors and warnings, onto this document's blocks. */
static void apply_marks(ccr_document *doc)
{
    if (!doc->loaded)
        return;
    ccr_editor_mark *marks = malloc((size_t)(g_job.diag_count + 1) * 8 * sizeof *marks);
    uint32_t n = 0;
    for (int i = 0; i < g_job.diag_count && marks; ++i) {
        const ccr_build_diag *d = &g_job.diags[i];
        if (d->kind == CCR_BD_NOTE || !diag_is_for(d, doc))
            continue;
        const char *msg = d->message[0] ? d->message : "?";
        if (d->line) {
            const ccr_node_id at = locate(doc, d->line, d->col);
            if (at != CCR_NONE) {
                marks[n] = (ccr_editor_mark){ .node = at, .warning = d->kind == CCR_BD_WARNING };
                snprintf(marks[n++].text, sizeof marks[0].text, "%.290s", msg);
            }
        } else if (d->symbol[0]) {   /* the linker: every call to it */
            const ccr_ast *a = &doc->ed.ast;
            int found = 0;
            for (uint32_t k = 0; k < a->node_count && found < 8; ++k)
                if (a->nodes[k].kind == CCR_NODE_CALL && !(a->nodes[k].flags & CCR_FLAG_DEAD) &&
                    strcmp(ccr_ast_str(a, a->nodes[k].name), d->symbol) == 0) {
                    marks[n] = (ccr_editor_mark){ .node = k };
                    snprintf(marks[n++].text, sizeof marks[0].text, "%.290s", msg);
                    ++found;
                }
        }
    }
    ccr_editor_set_marks(&doc->ed, marks, n);
    free(marks);
}

static void marks_follow(ccr_document *doc)
{
    static uint32_t gen;
    static char path[PATH_MAX_LEN];
    static const void *ed_marks;
    /* a new build said something, or another file is on screen, or it was
     * opened again (its marks went with the old editor) */
    const bool reopened = doc->loaded && doc->ed.marks == NULL && ed_marks != NULL;
    if (gen == g_job.generation && strcmp(path, doc->loaded ? doc->path : "") == 0 && !reopened)
        return;
    const bool new_build = gen != g_job.generation;
    gen = g_job.generation;
    snprintf(path, sizeof path, "%s", doc->loaded ? doc->path : "");
    apply_marks(doc);
    if (new_build)
        for (int i = 0; i < g_stash_n; ++i)
            apply_marks(g_stash[i].doc);
    ed_marks = doc->loaded ? (const void *)doc->ed.marks : NULL;
}

/* Saves what changed (every open file), makes a CMakeLists.txt if the
 * project has none, and starts the build. */
static void build_start(ccr_document *doc, bool run, const ccr_options *opt)
{
    if (ccr_job_busy(&g_job)) {
        ccr_job_stop(&g_job);   /* a second press: stop; the next one builds */
        return;
    }
    g_bp.open = true;
    g_bp.follow = true;
    char project[CCR_FILES_PATH];
    if (doc->loaded)
        ccr_files_project_of(doc->path, project, (int)sizeof project);
    else if (g_files.root[0])
        snprintf(project, sizeof project, "%s", g_files.root);
    else {
        ccr_trf(doc->status, sizeof doc->status, CCR_T_CMAKE_NO_PROJECT);
        doc->has_errors = true;
        return;
    }
    /* what is on screen is what gets built */
    ccr_document *all[MAX_OPEN + 1];
    int n = 0;
    if (doc->loaded)
        all[n++] = doc;
    for (int i = 0; i < g_stash_n; ++i)
        all[n++] = g_stash[i].doc;
    for (int i = 0; i < n; ++i) {
        if (!ccr_editor_modified(&all[i]->ed))
            continue;
        if (!document_save(all[i], opt)) {
            char why[sizeof doc->status];
            ccr_trf(why, sizeof why, CCR_T_BUILD_NOT_SAVED, base_name(all[i]->path));
            snprintf(doc->status, sizeof doc->status, "%s", why);
            doc->has_errors = true;
            return;
        }
    }
    char cm[CCR_FILES_PATH + 32];
    int files = 0;
    const ccr_cmake_made made = ccr_build_make_cmake(project, doc->loaded ? doc->path : NULL, cm,
                                                     (int)sizeof cm, &files);
    if (made == CCR_CMAKE_NO_SOURCES || made == CCR_CMAKE_CANNOT) {
        ccr_trf(doc->status, sizeof doc->status,
                made == CCR_CMAKE_NO_SOURCES ? CCR_T_CMAKE_NO_SOURCES : CCR_T_CANNOT_WRITE, "CMakeLists.txt");
        doc->has_errors = true;
        return;
    }
    if (made == CCR_CMAKE_MADE)
        ccr_files_scan(&g_files, project, cm);
    for (int i = 0; i < n; ++i)
        remember_places(all[i]);   /* the files as the compiler will see them */
    ccr_job_build(&g_job, project, run);
    g_bp.last = CCR_JOB_RUNNING;
}

/* "3 errors, 1 warning" */
static void counts_text(char *out, size_t cap)
{
    char e[64] = "", w[64] = "";
    if (g_job.errors)
        ccr_trnf(e, sizeof e, CCR_T_BUILD_ERRORS, (unsigned long)g_job.errors, (unsigned)g_job.errors);
    if (g_job.warnings)
        ccr_trnf(w, sizeof w, CCR_T_BUILD_WARNINGS, (unsigned long)g_job.warnings, (unsigned)g_job.warnings);
    snprintf(out, cap, "%s%s%s", e, e[0] && w[0] ? ", " : "", w);
}

/* What the panel's header says, and its colour. */
static void build_headline(char *out, size_t cap, ccr_color *c)
{
    *c = COL_BAR_TEXT;
    switch (g_job.step) {
    case CCR_JOB_CONFIGURE: ccr_trf(out, cap, CCR_T_BUILD_CONFIGURING); return;
    case CCR_JOB_BUILD:     ccr_trf(out, cap, CCR_T_BUILD_BUILDING); return;
    case CCR_JOB_RUN:       ccr_trf(out, cap, CCR_T_BUILD_RUNNING, base_name(g_job.exe)); return;
    case CCR_JOB_IDLE:      break;
    }
    char counts[160], secs[32];
    counts_text(counts, sizeof counts);
    switch (g_job.result) {
    case CCR_JOB_RUNNING:
        ccr_trf(out, cap, CCR_T_BUILD_IDLE);
        return;
    case CCR_JOB_BUILT:
        *c = ccr_th(CCR_C_OK);
        if (g_job.ran) {
            ccr_trf(out, cap, CCR_T_BUILD_FINISHED, g_job.run_exit);
            if (g_job.run_exit)
                *c = ccr_th(CCR_C_ERROR);
        } else {
            snprintf(secs, sizeof secs, "%.1f", g_job.ended - g_job.started);
            ccr_trf(out, cap, CCR_T_BUILD_BUILT, secs);
        }
        if (counts[0]) {
            const size_t l = strlen(out);
            snprintf(out + l, cap - l, " (%s)", counts);
        }
        return;
    case CCR_JOB_FAILED:
        *c = ccr_th(CCR_C_ERROR);
        ccr_trf(out, cap, CCR_T_BUILD_FAILED, counts[0] ? counts : "...");
        return;
    case CCR_JOB_STOPPED:
        ccr_trf(out, cap, CCR_T_BUILD_STOPPED);
        return;
    case CCR_JOB_NO_CMAKE:
        *c = ccr_th(CCR_C_ERROR);
        ccr_trf(out, cap, CCR_T_BUILD_NO_CMAKE);
        return;
    }
}

/* Off to the block a diagnostic is about: its file opened if need be. */
static void go_to_diag(ccr_document *doc, ccr_canvas *cv, ccr_rect viewport, const ccr_build_diag *d,
                       const ccr_options *opt)
{
    char path[PATH_MAX_LEN] = "";
    if (d->file[0] == '/') {
        snprintf(path, sizeof path, "%s", d->file);
    } else if (d->file[0]) {   /* the linker's "main.c": an open file, or one of the project */
        if (doc->loaded && diag_is_for(d, doc))
            snprintf(path, sizeof path, "%s", doc->path);
        for (int i = 0; i < g_stash_n && !path[0]; ++i)
            if (diag_is_for(d, g_stash[i].doc))
                snprintf(path, sizeof path, "%s", g_stash[i].doc->path);
        if (!path[0] && doc->loaded)
            ccr_files_find_include(&g_files, doc->path, d->file, path, (int)sizeof path);
    }
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f)
        return;
    fclose(f);
    if (!switch_to(doc, cv, viewport, path, opt))
        return;
    marks_follow(doc);
    ccr_node_id at = CCR_NONE;
    if (d->line)
        at = locate(doc, d->line, d->col);
    for (uint32_t i = 0; at == CCR_NONE && i < doc->ed.mark_count; ++i)
        at = doc->ed.marks[i].node;   /* the linker's: the first call */
    if (at != CCR_NONE)
        glide_to_node(cv, doc, viewport, at);
}

static ccr_color log_color(ccr_log_kind k)
{
    switch (k) {
    case CCR_LOG_COMMAND: return ccr_th(CCR_C_ACCENT);
    case CCR_LOG_ERROR:   return ccr_th(CCR_C_ERROR);
    case CCR_LOG_WARNING: return 0xD98A1CFFu;
    case CCR_LOG_NOTE:    return ccr_th(CCR_C_PANEL_HINT);
    case CCR_LOG_QUIET:   return ccr_th(CCR_C_PANEL_HINT);
    case CCR_LOG_GOOD:    return ccr_th(CCR_C_OK);
    case CCR_LOG_BAD:     return ccr_th(CCR_C_ERROR);
    case CCR_LOG_TEXT:    break;
    }
    return ccr_th(CCR_C_PANEL_TEXT);
}

/* The panel: input and drawing in one go (after the canvas is drawn).
 * `input`: nothing else has the mouse (no menu, no dialog). */
static void build_panel(int w, int h, ccr_document *doc, ccr_canvas *cv, ccr_rect viewport,
                        const ccr_options *opt, bool input)
{
    if (g_bp.t <= 0.0f)
        return;
    const ccr_rect r = build_area(w, h);
    const ccr_vec2 m = ccr_gui_mouse();
    const bool over = input && point_in(m, r);
    ccr_gui_rect((ccr_rect){ r.x, r.y - 1.0f, r.w, 1.0f }, ccr_th(CCR_C_PANEL_EDGE));
    ccr_gui_rect(r, ccr_th(CCR_C_PANEL) | 0xFFu);

    /* the header: what is going on, and the buttons */
    const float hh = 38.0f;
    char head[400];
    ccr_color hc;
    build_headline(head, sizeof head, &hc);
    float x = r.x + 14.0f;
    if (ccr_job_busy(&g_job)) {   /* a little spinner */
        const float a = (float)ccr_gui_time() * 6.0f;
        for (int k = 0; k < 3; ++k) {
            const float ang = a + (float)k * 2.0944f;
            ccr_gui_circle((ccr_vec2){ x + 7.0f + cosf(ang) * 5.0f, r.y + hh * 0.5f + sinf(ang) * 5.0f },
                           2.2f, ccr_th(CCR_C_ACCENT));
        }
        x += 22.0f;
    } else {
        ccr_gui_circle((ccr_vec2){ x + 6.0f, r.y + hh * 0.5f }, 5.0f, hc == COL_BAR_TEXT ? ccr_th(CCR_C_PANEL_HINT) : hc);
        x += 18.0f;
    }
    ccr_gui_text(ccr_tr(CCR_T_BUILD_PANEL), x, r.y + 9.0f, 17.0f, ccr_th(CCR_C_PANEL_TEXT));
    x += ccr_gui_text_width(ccr_tr(CCR_T_BUILD_PANEL), 17.0f) + 14.0f;
    ccr_gui_text(head, x, r.y + 11.0f, 15.0f, hc == COL_BAR_TEXT ? ccr_th(CCR_C_PANEL_HINT) : hc);
    if (g_job.errors && !ccr_job_busy(&g_job)) {
        const float hx = x + ccr_gui_text_width(head, 15.0f) + 12.0f;
        ccr_gui_text(ccr_tr(CCR_T_BUILD_CLICK), hx, r.y + 12.0f, 13.0f, ccr_th(CCR_C_PANEL_HINT));
    }

    float bx = r.x + r.w - 10.0f;
    const ccr_button_style plain = *button_style(false);
    if (ccr_gui_button((ccr_rect){ bx - 28.0f, r.y + 7.0f, 28.0f, 24.0f }, "x", &plain) && input)
        g_bp.open = false;
    bx -= 36.0f;
    const bool busy = ccr_job_busy(&g_job);
    const char *bl = ccr_tr(busy ? CCR_T_BUILD_STOP : CCR_T_BUILD_RUN);
    const float bw = ccr_gui_text_width(bl, 15.0f) + 26.0f;
    if (ccr_gui_button((ccr_rect){ bx - bw, r.y + 7.0f, bw, 24.0f }, bl, busy ? &plain : button_style(true)) &&
        input) {
        if (busy)
            ccr_job_stop(&g_job);
        else
            build_start(doc, true, opt);
    }

    /* the log */
    const ccr_rect body = { r.x + 8.0f, r.y + hh, r.w - 16.0f, r.h - hh - 6.0f };
    const int rows = (int)(body.h / LOG_ROW);
    const float max_scroll = (float)(g_job.line_count > rows ? g_job.line_count - rows : 0);
    if (over && point_in(m, body)) {
        const float wheel = ccr_gui_wheel();
        if (wheel != 0.0f) {
            g_bp.scroll -= wheel * 3.0f;
            g_bp.follow = g_bp.scroll >= max_scroll;
        }
    }
    if (g_bp.follow)
        g_bp.scroll = max_scroll;
    if (g_bp.scroll > max_scroll) g_bp.scroll = max_scroll;
    if (g_bp.scroll < 0.0f) g_bp.scroll = 0.0f;

    g_bp.hot = -1;
    ccr_gui_clip_begin(body);
    const int first = (int)g_bp.scroll;
    for (int i = first; i < g_job.line_count && i < first + rows + 1; ++i) {
        const ccr_log_line *L = &g_job.lines[i];
        const float y = body.y + (float)(i - first) * LOG_ROW;
        const ccr_rect row = { body.x, y, body.w, LOG_ROW };
        const bool can_go = L->diag >= 0 && (g_job.diags[L->diag].file[0]);
        if (can_go && over && point_in(m, row)) {
            g_bp.hot = i;
            ccr_gui_rect_rounded(row, 0.3f, (ccr_th(CCR_C_PANEL_EDGE) & 0xFFFFFF00u) | 0x90u);
        }
        ccr_gui_text(L->text, body.x + 6.0f, y + 2.0f, LOG_FONT, log_color(L->kind));
    }
    if (g_job.line_count == 0)
        ccr_gui_text(ccr_tr(CCR_T_BUILD_IDLE), body.x + 6.0f, body.y + 4.0f, LOG_FONT, ccr_th(CCR_C_PANEL_HINT));
    ccr_gui_clip_end();
    if (max_scroll > 0.0f) {   /* a thin scroll bar */
        const float bh = body.h * (float)rows / (float)g_job.line_count;
        const float by = body.y + (body.h - bh) * (g_bp.scroll / max_scroll);
        ccr_gui_rect_rounded((ccr_rect){ body.x + body.w - 4.0f, by, 4.0f, bh }, 1.0f, ccr_th(CCR_C_PANEL_EDGE));
    }

    if (g_bp.hot >= 0 && ccr_gui_mouse_pressed(CCR_MOUSE_LEFT) && (!doc->loaded || doc->ed.drag == CCR_DRAG_IDLE)) {
        const ccr_build_diag d = g_job.diags[g_job.lines[g_bp.hot].diag];   /* a copy: switching files */
        ccr_rect seen = viewport;   /* the part of the canvas the panel leaves */
        seen.h = r.y - viewport.y;
        go_to_diag(doc, cv, seen, &d, opt);
    }
}

/* A mark under the mouse: what the compiler said, next to it. */
static void mark_tooltip(const ccr_document *doc, const ccr_canvas *cv, ccr_rect viewport)
{
    if (!doc->loaded || doc->ed.mark_count == 0 || doc->ed.drag != CCR_DRAG_IDLE)
        return;
    const ccr_vec2 m = ccr_gui_mouse();
    if (!point_in(m, viewport) || (g_bp.t > 0.0f && point_in(m, build_area(ccr_gui_width(), ccr_gui_height()))))
        return;
    const int i = ccr_editor_mark_at(&doc->ed, ccr_canvas_to_world(cv, m));
    if (i < 0)
        return;
    const ccr_editor_mark *mk = &doc->ed.marks[i];
    const float fs = 15.0f, pad = 10.0f;
    float tw = ccr_gui_text_width(mk->text, fs);
    const float maxw = 520.0f;
    const float wrap_w = tw > maxw ? maxw : tw;
    const float th = wrap_text(mk->text, 0, 0, wrap_w, fs, 0, false);
    ccr_rect box = { m.x + 16.0f, m.y + 18.0f, wrap_w + pad * 2.0f, th + pad * 2.0f - 4.0f };
    if (box.x + box.w > viewport.x + viewport.w)
        box.x = viewport.x + viewport.w - box.w - 4.0f;
    if (box.y + box.h > viewport.y + viewport.h)
        box.y = m.y - box.h - 10.0f;
    const ccr_color edge = mk->warning ? 0xD98A1CFFu : ccr_th(CCR_C_ERROR);
    ccr_gui_rect_rounded((ccr_rect){ box.x - 2, box.y - 2, box.w + 4, box.h + 4 }, 0.2f, edge);
    ccr_gui_rect_rounded(box, 0.2f, ccr_th(CCR_C_PANEL) | 0xFFu);
    wrap_text(mk->text, box.x + pad, box.y + pad - 2.0f, wrap_w, fs, ccr_th(CCR_C_PANEL_TEXT), true);
}

/* What was clicked in the top bar that the caller does (it has what
 * opening a file and building take). */
enum { TOP_CMAKE = 1, TOP_BUILD = 2 };

static int draw_topbar(int width, ccr_document *doc, eula_view *ev)
{
    int clicked = 0;
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
    const ccr_button_style plain = *button_style(false), *bs = &plain;   /* the style is static */
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
    const float lw = ccr_gui_text_width(ccr_tr(CCR_T_LIBS_TITLE), 16.0f) + 24.0f;
    if (ccr_gui_button((ccr_rect){ (float)width - 230.0f - tw - fw - lw, 7.0f, lw, 26.0f },
                       ccr_tr(CCR_T_LIBS_TITLE), g_libs.open ? button_style(true) : bs) && !modal) {
        if (g_libs.open) g_libs.open = false;
        else libs_open(doc);
    }
    const float cw = ccr_gui_text_width("CMake", 16.0f) + 24.0f;
    const bool on_cmake = doc->loaded && doc->ed.ast.lang == CCR_LANG_CMAKE;
    if (ccr_gui_button((ccr_rect){ (float)width - 238.0f - tw - fw - lw - cw, 7.0f, cw, 26.0f },
                       "CMake", on_cmake ? button_style(true) : bs) && !modal && !g_libs.open)
        clicked |= TOP_CMAKE;
    if (ccr_gui_button((ccr_rect){ (float)width - 214.0f - tw, 7.0f, 32.0f, 26.0f }, "?", bs) &&
        !modal) {
        ev->open = true;   /* the agreement again, to read it at leisure */
        ev->scroll = 0.0f;
    }

    float x = (float)width - 110.0f;
    const bool busy = ccr_job_busy(&g_job);
    if (ccr_gui_button((ccr_rect){ x, 7.0f, 100.0f, 26.0f }, ccr_tr(busy ? CCR_T_BUILD_STOP : CCR_T_BUILD),
                       busy ? bs : button_style(true)) &&
        !modal && !g_libs.open)
        clicked |= TOP_BUILD;
    return clicked;
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
    if (opt.make_cmake) {
        ccr_lib_load_project(opt.make_cmake, stderr);   /* what packs know about linking */
        char *text = ccr_build_cmake_text(opt.make_cmake, NULL, NULL);
        ccr_lib_unload();
        if (!text) {
            fprintf(stderr, "ccratch: no .c files in '%s'\n", opt.make_cmake);
            return 1;
        }
        fputs(text, stdout);
        free(text);
        return 0;
    }
    if (opt.make_blocks) {
        char project[CCR_FILES_PATH];
        ccr_files_project_of(opt.make_blocks, project, (int)sizeof project);
        if (ccr_lib_make(opt.make_blocks, opt.clang_args, opt.clang_arg_count, project, stdout))
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
        const bool modal = eula.open || g_libs.open;   /* a dialog has the mouse */

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

        /* the build: its output read, its errors onto the blocks */
        ccr_job_poll(&g_job);
        marks_follow(&doc);
        if (g_bp.last == CCR_JOB_RUNNING && g_job.result != CCR_JOB_RUNNING && !ccr_job_busy(&g_job)) {
            ccr_color unused;
            build_headline(doc.status, sizeof doc.status, &unused);
            doc.has_errors = g_job.result != CCR_JOB_BUILT || (g_job.ran && g_job.run_exit != 0);
            g_bp.last = g_job.result;
        } else if (ccr_job_busy(&g_job)) {
            g_bp.last = CCR_JOB_RUNNING;
        }
        {
            const float target = g_bp.open ? 1.0f : 0.0f;
            const float step = g_animate ? dt / 0.18f : 1.0f;
            g_bp.t += g_bp.t < target ? step : -step;
            if ((g_bp.t > target) == (target > 0.5f) || !g_animate)
                g_bp.t = target;
            if (g_bp.t < 0.0f) g_bp.t = 0.0f;
        }

        const ccr_vec2 m = ccr_gui_mouse();
        const ccr_vec2 world = ccr_canvas_to_world(&canvas, m);
        const bool inside = point_in(m, viewport) && !(g_files_t > 0.0f && point_in(m, files_area(w, h))) &&
                            !(g_bp.t > 0.0f && point_in(m, build_area(w, h)));
        const bool over_palette = m.x < PALETTE_W && m.y > TOPBAR_H;
        palette_follow(&doc);
        const bool can_act = doc.loaded && te.node == CCR_NONE && !modal &&
                             g_fpanel.naming == CCR_NAMING_NONE;   /* typing a new file's name */

        /* the files panel slides in from the right, over the canvas */
        if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_P) && !modal)
            files_toggle(&doc);
        if (ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_TAB) && !modal && te.node == CCR_NONE &&
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
        if (g_files_open && !modal && !pie.open && !opm.open && te.node == CCR_NONE &&
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
        if (!pie.open && !modal)
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
            if (ccr_gui_key_pressed(CCR_KEY_B) || ccr_gui_key_pressed(CCR_KEY_R))
                build_start(&doc, ccr_gui_key_pressed(CCR_KEY_R), &opt);
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
        if (!pie.open && !was_open && !modal) {
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
        mark_tooltip(&doc, &canvas, viewport);
        build_panel(w, h, &doc, &canvas, viewport, &opt,
                    !modal && !pie.open && !opm.open && te.node == CCR_NONE);
        if (g_files_t > 0.0f) {
            ccr_open_file of[MAX_OPEN + 1];
            const int nof = open_files(&doc, of);
            ccr_files_panel_draw(&g_fpanel, &g_files, files_area(w, h), of, nof);
            if (g_files_open && te.node == CCR_NONE && !modal && !pie.open && !opm.open) {
                char made[CCR_FILES_PATH];
                if (ccr_files_panel_tools(&g_fpanel, &g_files, files_area(w, h), button_style(false),
                                          made, (int)sizeof made) == CCR_FILES_OPEN &&
                    switch_to(&doc, &canvas, viewport, made, &opt))
                    te.node = CCR_NONE;
            } else {
                g_fpanel.naming = CCR_NAMING_NONE;
            }
        }
        const int top = draw_topbar(w, &doc, &eula);
        if ((top & TOP_CMAKE) && (!doc.loaded || doc.ed.drag == CCR_DRAG_IDLE)) {
            cmake_open(&doc, &canvas, viewport, &opt);
            te.node = CCR_NONE;
        }
        if ((top & TOP_BUILD) && (!doc.loaded || doc.ed.drag == CCR_DRAG_IDLE)) {
            te.node = CCR_NONE;
            build_start(&doc, false, &opt);
        }
        draw_statusbar(w, h, &canvas, &doc);
        ccr_pie_draw(&pie);
        if (doc.loaded)
            op_menu_draw(&opm, &doc.ed, m, w, h);
        if (!eula.open)
            libs_frame(w, h, &doc, &opt);
        eula_frame(&eula, w, h, &doc);

        ++frame;
        const bool last = opt.smoke_frames > 0 && frame >= opt.smoke_frames;
        if (last && opt.screenshot && !ccr_gui_screenshot(opt.screenshot))
            fprintf(stderr, "ccratch: failed to save screenshot '%s'\n", opt.screenshot);

        ccr_gui_end_frame();
        if (last)
            break;
    }

    ccr_job_free(&g_job);   /* a program still running goes too */
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
