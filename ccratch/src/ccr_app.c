#include "ccratch/ccratch.h"
#include "ccr_ast.h"
#include "ccr_canvas.h"
#include "ccr_codegen.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette */
#define COL_BG        0x1E2028FFu
#define COL_BAR       0x16181EFFu
#define COL_BAR_TEXT  0x9AA3B5FFu
#define COL_TITLE     0xE8ECF4FFu
#define COL_HINT      0x5A6170FFu
#define COL_ACCENT    0xD9464BFFu   /* shrine red */
#define COL_OK        0x59C059FFu

/* Generated from assets/icon.png at build time */
extern const unsigned char ccr_icon_png[];
extern const unsigned int  ccr_icon_png_size;

#define TOPBAR_H     40.0f
#define STATUSBAR_H  26.0f
#define PATH_MAX_LEN 4096

typedef struct ccr_options {
    int         smoke_frames;   /* >0: quit after N frames (CI, headless) */
    const char *screenshot;     /* save the last frame here */
    const char *dump_ast;       /* parse this file, print the tree, exit */
    const char *roundtrip;      /* parse, write back unchanged, compare */
    const char *open;           /* file to open in the editor */
    int         zoom_percent;   /* initial zoom, 0 = 100% */
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
           "  --roundtrip FILE.c     check that saving an unchanged FILE gives it back\n"
           "                         byte for byte\n"
           "  -- ARGS...             pass ARGS to clang (e.g. -- -std=c99 -Iinclude)\n"
           "  --zoom PERCENT         start at this zoom (25..300)\n"
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

static void fit_view(ccr_canvas *cv, const ccr_document *doc, ccr_rect viewport, float zoom)
{
    if (zoom < 0.25f) zoom = 0.25f;
    if (zoom > 3.0f)  zoom = 3.0f;
    cv->zoom = zoom;
    ccr_rect b = doc->loaded ? ccr_editor_bounds(&doc->ed) : (ccr_rect){ 0, 0, 0, 0 };
    cv->offset = (ccr_vec2){ viewport.x + 48.0f - b.x * zoom, viewport.y + 32.0f - b.y * zoom };
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

    if (!ccr_parse_file(path, opt->clang_args, opt->clang_arg_count, &ast)) {
        ccr_ast_free(&ast);
        snprintf(doc->status, sizeof doc->status,
                 "Could not read %s. Opyat' rabotat'...", base_name(path));
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
        snprintf(doc->status, sizeof doc->status, "Out of memory while laying out blocks.");
        doc->has_errors = true;
        return false;
    }
    next.loaded = true;
    next.has_errors = errors > 0;
    if (errors)
        snprintf(next.status, sizeof next.status,
                 "%s: %u error%s (details in the terminal). Opyat' rabotat'...",
                 base_name(path), errors, errors == 1 ? "" : "s");
    else
        snprintf(next.status, sizeof next.status, "%s: no incidents. Donation box: still empty.",
                 base_name(path));

    document_close(doc);
    *doc = next;
    return true;
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
    const char *name = base_name(doc->path);

    char *text = NULL, err[256];
    size_t len = 0;
    if (!ccr_codegen(&doc->ed.ast, &text, &len, err, sizeof err)) {
        snprintf(doc->status, sizeof doc->status, "Not saved: %s", err);
        doc->has_errors = true;
        fprintf(stderr, "[ccratch] %s not saved: %s\n", doc->path, err);
        return;
    }

    char tmp[PATH_MAX_LEN + 32], backup[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.ccratch-tmp", doc->path);
    snprintf(backup, sizeof backup, "%s~", doc->path);

    if (!doc->backed_up) {
        if (!write_file(backup, doc->ed.ast.source, doc->ed.ast.source_len)) {
            snprintf(doc->status, sizeof doc->status, "Not saved: could not write the backup %s~", name);
            doc->has_errors = true;
            free(text);
            return;
        }
        doc->backed_up = true;
    }
    if (!write_file(tmp, text, len) || rename(tmp, doc->path) != 0) {
        remove(tmp);
        snprintf(doc->status, sizeof doc->status, "Not saved: could not write %s", name);
        doc->has_errors = true;
        free(text);
        return;
    }
    free(text);
    doc->ed.modified = false;

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
    char extra[96] = "";
    if (loose)
        snprintf(extra, sizeof extra, " %u loose stack%s left out.", loose, loose == 1 ? "" : "s");
    if (errors > doc->errors_at_load) {
        snprintf(doc->status, sizeof doc->status,
                 "Saved %s, but clang now sees %u error%s (details in the terminal).%s",
                 name, errors, errors == 1 ? "" : "s", extra);
        doc->has_errors = true;
    } else {
        snprintf(doc->status, sizeof doc->status, "Saved %s (original in %s~).%s",
                 name, name, extra);
        doc->has_errors = false;
    }
}

/* -------------------------------------------------------- drawing */

static void draw_topbar(int width, const ccr_document *doc)
{
    ccr_gui_rect((ccr_rect){ 0, 0, (float)width, TOPBAR_H }, COL_BAR);
    ccr_gui_circle((ccr_vec2){ 22.0f, TOPBAR_H * 0.5f }, 8.0f, COL_ACCENT);
    ccr_gui_text("Ccratch", 38.0f, 10.0f, 20.0f, COL_TITLE);
    if (doc->loaded) {
        char title[300];
        snprintf(title, sizeof title, "%s%s", base_name(doc->path), doc->ed.modified ? " *" : "");
        ccr_gui_text(title, 140.0f, 12.0f, 16.0f, COL_BAR_TEXT);
    }

    float x = (float)width - 110.0f;
    if (ccr_gui_button((ccr_rect){ x, 7.0f, 100.0f, 26.0f }, "Build"))
        printf("[ccratch] build: nothing to build yet. Go do some shrine work.\n");
}

static void draw_statusbar(int width, int height, const ccr_canvas *cv, const ccr_document *doc)
{
    float y = (float)height - STATUSBAR_H;
    ccr_gui_rect((ccr_rect){ 0, y, (float)width, STATUSBAR_H }, COL_BAR);

    const char *msg = doc->status[0] ? doc->status : "No file. Drop a .c file onto the window.";
    ccr_color dot = doc->has_errors ? COL_ACCENT : COL_OK;
    ccr_gui_circle((ccr_vec2){ 14.0f, y + STATUSBAR_H * 0.5f }, 4.0f, dot);
    ccr_gui_text(msg, 26.0f, y + 7.0f, 12.0f, COL_BAR_TEXT);

    char right[192];
    uint32_t loose = doc->loaded ? ccr_editor_loose_count(&doc->ed) : 0;
    char loose_txt[64] = "";
    if (loose)
        snprintf(loose_txt, sizeof loose_txt, "%u loose stack%s (not compiled)   |   ",
                 loose, loose == 1 ? "" : "s");
    snprintf(right, sizeof right,
             "%szoom %d%%   |   LMB drag: blocks   RMB: pan   wheel: zoom   Ctrl+S: save   Home: fit   F5: reload",
             loose_txt, (int)(cv->zoom * 100.0f + 0.5f));
    float rw = ccr_gui_text_width(right, 12.0f);
    ccr_gui_text(right, (float)width - rw - 12.0f, y + 7.0f, 12.0f, COL_BAR_TEXT);
}

static void draw_empty_hint(ccr_rect viewport)
{
    const char *a = "Drop a .c file here";
    const char *b = "(or: ccratch file.c)";
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

    if (opt.dump_ast)
        return run_dump_ast(&opt);
    if (opt.roundtrip)
        return run_roundtrip(&opt);

    if (!ccr_gui_init(1280, 800, "Ccratch")) {
        fprintf(stderr, "ccratch: could not open a window. Opyat' rabotat'...\n");
        return 1;
    }
    if (!ccr_gui_set_icon_png(ccr_icon_png, ccr_icon_png_size))
        fprintf(stderr, "ccratch: icon did not load, carrying on without it\n");

    ccr_canvas canvas;
    ccr_canvas_init(&canvas);

    /* static: a document holds a 4 KB path and is reused all session */
    static ccr_document doc;
    ccr_rect viewport = { 0, TOPBAR_H, (float)ccr_gui_width(),
                          (float)ccr_gui_height() - TOPBAR_H - STATUSBAR_H };
    const float start_zoom = opt.zoom_percent > 0 ? (float)opt.zoom_percent / 100.0f : 1.0f;
    if (opt.open)
        document_open(&doc, opt.open, &opt);
    fit_view(&canvas, &doc, viewport, start_zoom);

    int frame = 0;
    char dropped[PATH_MAX_LEN];
    while (!ccr_gui_should_close()) {
        const int w = ccr_gui_width();
        const int h = ccr_gui_height();
        viewport = (ccr_rect){ 0, TOPBAR_H, (float)w, (float)h - TOPBAR_H - STATUSBAR_H };

        if (ccr_gui_file_dropped(dropped, sizeof dropped) && document_open(&doc, dropped, &opt))
            fit_view(&canvas, &doc, viewport, 1.0f);
        if (ccr_gui_key_pressed(CCR_KEY_F5) && doc.loaded) {
            char again[PATH_MAX_LEN];
            snprintf(again, sizeof again, "%s", doc.path);
            document_open(&doc, again, &opt);
        }
        if (doc.loaded && ccr_gui_ctrl_down() && ccr_gui_key_pressed(CCR_KEY_S))
            document_save(&doc, &opt);
        if (ccr_gui_key_pressed(CCR_KEY_HOME))
            fit_view(&canvas, &doc, viewport, 1.0f);

        ccr_canvas_update(&canvas, viewport);

        if (doc.loaded) {
            ccr_vec2 m = ccr_gui_mouse();
            bool inside = m.x >= viewport.x && m.y >= viewport.y &&
                          m.x < viewport.x + viewport.w && m.y < viewport.y + viewport.h;
            ccr_editor_input in = {
                .mouse    = ccr_canvas_to_world(&canvas, m),
                .pressed  = inside && ccr_gui_mouse_pressed(CCR_MOUSE_LEFT),
                .down     = ccr_gui_mouse_down(CCR_MOUSE_LEFT),
                .released = ccr_gui_mouse_released(CCR_MOUSE_LEFT),
                .zoom     = canvas.zoom,
            };
            ccr_editor_update(&doc.ed, &in);
        }

        ccr_gui_begin_frame(COL_BG);
        ccr_canvas_draw_grid(&canvas, viewport);
        if (doc.loaded)
            ccr_editor_draw(&doc.ed, &canvas, viewport);
        else
            draw_empty_hint(viewport);
        draw_topbar(w, &doc);
        draw_statusbar(w, h, &canvas, &doc);

        ++frame;
        const bool last = opt.smoke_frames > 0 && frame >= opt.smoke_frames;
        if (last && opt.screenshot && !ccr_gui_screenshot(opt.screenshot))
            fprintf(stderr, "ccratch: failed to save screenshot '%s'\n", opt.screenshot);

        ccr_gui_end_frame();
        if (last)
            break;
    }

    document_close(&doc);
    ccr_gui_shutdown();
    return 0;
}
