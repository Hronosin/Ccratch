#include "ccratch/ccratch.h"
#include "ccr_ast.h"
#include "ccr_canvas.h"
#include "ccr_codegen.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_lang.h"
#include "ccr_parse.h"
#include "ccr_pie.h"

#include <math.h>
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
#define PALETTE_W   176.0f
#define COL_PALETTE  0x1A1C23FFu
#define COL_CONTROL  0xFFAB19FFu
#define COL_CONTROL_EDGE 0xCF8B17FFu
#define COL_CODE     0xFF6680FFu
#define COL_CODE_EDGE 0xFF3355FFu
#define STATUSBAR_H  26.0f
#define PATH_MAX_LEN 4096

typedef struct ccr_options {
    int         smoke_frames;   /* >0: quit after N frames (CI, headless) */
    const char *screenshot;     /* save the last frame here */
    const char *dump_ast;       /* parse this file, print the tree, exit */
    const char *roundtrip;      /* parse, write back unchanged, compare */
    const char *open;           /* file to open in the editor */
    int         zoom_percent;   /* initial zoom, 0 = 100% */
    const char *lang;           /* --lang CODE, NULL: pick one */
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
           "  --lang CODE            interface language (en, ru...); remembered when\n"
           "                         picked with the button in the window\n"
           "  --list-langs           show the language packs found, and where\n"
           "  --lang-template        print the English text as a pack to translate\n"
           "  --check-lang FILE      check a language pack for missing or broken text\n"
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
            ccr_trf(doc->status, sizeof doc->status, CCR_T_LANGUAGE, ccr_lang_name());
            doc->has_errors = false;
            return;   /* blocks show C, not words: nothing to lay out again */
        }
    }
}

static void draw_topbar(int width, ccr_document *doc)
{
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
    if (ccr_gui_button((ccr_rect){ (float)width - 166.0f, 7.0f, 48.0f, 26.0f }, code))
        next_language(doc);

    float x = (float)width - 110.0f;
    if (ccr_gui_button((ccr_rect){ x, 7.0f, 100.0f, 26.0f }, ccr_tr(CCR_T_BUILD)))
        printf("[ccratch] build: nothing to build yet. Go do some shrine work.\n");
}

static void draw_statusbar(int width, int height, const ccr_canvas *cv, const ccr_document *doc)
{
    float y = (float)height - STATUSBAR_H;
    ccr_gui_rect((ccr_rect){ 0, y, (float)width, STATUSBAR_H }, COL_BAR);

    const char *msg = doc->status[0] ? doc->status : ccr_tr(CCR_T_NO_FILE);
    ccr_color dot = doc->has_errors ? COL_ACCENT : COL_OK;
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

#define PALETTE_ITEM_H   34.0f
#define PALETTE_GAP      10.0f
#define PALETTE_TOP     (TOPBAR_H + 44.0f)

static ccr_rect palette_item(int i)
{
    return (ccr_rect){ 14.0f, PALETTE_TOP + (float)i * (PALETTE_ITEM_H + PALETTE_GAP),
                       PALETTE_W - 28.0f, PALETTE_ITEM_H };
}

static bool point_in(ccr_vec2 p, ccr_rect r)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

/* Which palette block is under the mouse, or -1. */
static int palette_hit(ccr_vec2 m)
{
    for (int i = 0; i < CCR_TEMPLATE_COUNT; ++i)
        if (point_in(m, palette_item(i)))
            return i;
    return -1;
}

static void draw_palette(int height, bool dragging_over)
{
    ccr_gui_rect((ccr_rect){ 0, TOPBAR_H, PALETTE_W, (float)height - TOPBAR_H - STATUSBAR_H },
                 dragging_over ? 0x2A1E22FFu : COL_PALETTE);
    ccr_gui_text(dragging_over ? ccr_tr(CCR_T_PALETTE_DROP) : ccr_tr(CCR_T_PALETTE_TITLE), 16.0f, TOPBAR_H + 12.0f, 19.0f,
                 dragging_over ? COL_ACCENT : COL_BAR_TEXT);

    for (int i = 0; i < CCR_TEMPLATE_COUNT; ++i) {
        ccr_rect r = palette_item(i);
        bool code = i == CCR_TEMPLATE_CODE;
        bool mouth = i >= CCR_TEMPLATE_IF && i <= CCR_TEMPLATE_DO_WHILE;
        ccr_color fill = code ? COL_CODE : COL_CONTROL;
        ccr_color edge = code ? COL_CODE_EDGE : COL_CONTROL_EDGE;
        ccr_gui_rect_rounded(r, 0.25f, edge);
        ccr_gui_rect_rounded((ccr_rect){ r.x + 1, r.y + 1, r.w - 2, r.h - 2 }, 0.25f, fill);
        if (mouth)   /* a hint of the C shape: there is room inside */
            ccr_gui_rect((ccr_rect){ r.x + 14.0f, r.y + r.h - 7.0f, r.w - 28.0f, 4.0f }, edge);
        ccr_gui_text(ccr_template_label((ccr_template)i), r.x + 12.0f, r.y + 6.0f, 19.0f, 0xFFFFFFFFu);
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
    en[CCR_ACTION_DELETE]    = ctx->can_delete;
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
    case CCR_ACTION_FIT_VIEW:  fit_view(cv, doc, viewport, 1.0f); break;
    case CCR_ACTION_DELETE:    ccr_editor_delete(ed, ctx->item); break;
    case CCR_ACTION_DUPLICATE: ccr_editor_duplicate(ed, ctx->item, world); break;
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

    if (opt.dump_ast)
        return run_dump_ast(&opt);
    if (opt.roundtrip)
        return run_roundtrip(&opt);

    if (!ccr_gui_init(1280, 800, "Ccratch")) {
        fprintf(stderr, "ccratch: could not open a window. Opyat' rabotat'...\n");
        return 1;
    }
    ccr_lang_pick(opt.lang, stderr);
    if (!load_font())
        fprintf(stderr, "ccratch: font did not load, falling back to raylib's (ASCII only)\n");
    if (!ccr_gui_set_icon_png(ccr_icon_png, ccr_icon_png_size))
        fprintf(stderr, "ccratch: icon did not load, carrying on without it\n");

    ccr_canvas canvas;
    ccr_canvas_init(&canvas);

    /* static: a document holds a 4 KB path and is reused all session */
    static ccr_document doc;
    ccr_rect viewport = { PALETTE_W, TOPBAR_H, (float)ccr_gui_width() - PALETTE_W,
                          (float)ccr_gui_height() - TOPBAR_H - STATUSBAR_H };
    const float start_zoom = opt.zoom_percent > 0 ? (float)opt.zoom_percent / 100.0f : 1.0f;
    if (opt.open)
        document_open(&doc, opt.open, &opt);
    fit_view(&canvas, &doc, viewport, start_zoom);

    int frame = 0;
    char dropped[PATH_MAX_LEN];

    /* a text slot being typed into */
    static text_edit te = { .node = CCR_NONE };

    /* the pie menu, and the right click that may open it */
    ccr_pie pie = { 0 };
    ccr_context pie_ctx = { 0 };
    ccr_vec2 rmb_from = { 0 };
    bool rmb_armed = false;

    while (!ccr_gui_should_close()) {
        const int w = ccr_gui_width();
        const int h = ccr_gui_height();
        viewport = (ccr_rect){ PALETTE_W, TOPBAR_H, (float)w - PALETTE_W,
                               (float)h - TOPBAR_H - STATUSBAR_H };
        if (!doc.loaded)
            te.node = CCR_NONE;
        const bool was_open = pie.open;

        if (ccr_gui_file_dropped(dropped, sizeof dropped) && document_open(&doc, dropped, &opt)) {
            fit_view(&canvas, &doc, viewport, 1.0f);
            te.node = CCR_NONE;
            ccr_pie_close(&pie);
        }
        if (ccr_gui_key_pressed(CCR_KEY_F5) && doc.loaded && !pie.open) {
            char again[PATH_MAX_LEN];
            snprintf(again, sizeof again, "%s", doc.path);
            document_open(&doc, again, &opt);
            te.node = CCR_NONE;
        }

        const ccr_vec2 m = ccr_gui_mouse();
        const ccr_vec2 world = ccr_canvas_to_world(&canvas, m);
        const bool inside = point_in(m, viewport);
        const bool over_palette = m.x < PALETTE_W && m.y > TOPBAR_H;
        const bool can_act = doc.loaded && te.node == CCR_NONE;

        /* the pie, while it is open, gets the mouse and Esc to itself */
        if (pie.open) {
            bool cancel = !can_act || ccr_gui_key_pressed(CCR_KEY_ESCAPE) ||
                          ccr_gui_mouse_pressed(CCR_MOUSE_RIGHT);
            ccr_action a = ccr_pie_update(&pie, m, ccr_gui_mouse_pressed(CCR_MOUSE_LEFT), cancel);
            if (a != CCR_ACTION_NONE)
                run_action(a, &pie_ctx, world, &doc, &opt, &canvas, viewport, &te);
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
            ccr_pie_open(&pie, m, en, pie_ctx.commented);
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
            fit_view(&canvas, &doc, viewport, 1.0f);

        if (!pie.open && !was_open)
            ccr_canvas_update(&canvas, viewport);

        if (doc.loaded && te.node == CCR_NONE && !pie.open && !was_open) {
            /* a carried copy (Duplicate) lands on a click anywhere, the
             * palette included, where it is thrown away */
            const bool lmb = ccr_gui_mouse_pressed(CCR_MOUSE_LEFT);
            int spawn = (over_palette && lmb) ? palette_hit(m) : -1;
            if (spawn >= 0)
                ccr_editor_spawn(&doc.ed, (ccr_template)spawn, world);
            ccr_editor_input in = {
                .mouse      = ccr_canvas_to_world(&canvas, m),
                .pressed    = lmb && (inside || doc.ed.carrying),
                .down       = ccr_gui_mouse_down(CCR_MOUSE_LEFT),
                .released   = ccr_gui_mouse_released(CCR_MOUSE_LEFT),
                .zoom       = canvas.zoom,
                .over_trash = over_palette,
            };
            ccr_editor_update(&doc.ed, &in);

            ccr_node_id req = ccr_editor_take_edit_request(&doc.ed);
            if (req != CCR_NONE)
                start_editing(&te, &doc.ed, req);
        }

        ccr_gui_begin_frame(COL_BG);
        ccr_canvas_draw_grid(&canvas, viewport);
        if (doc.loaded)
            ccr_editor_draw(&doc.ed, &canvas, viewport);
        else
            draw_empty_hint(viewport);
        if (doc.loaded)
            draw_palette(h, doc.ed.drag == CCR_DRAG_MOVING && over_palette);

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
                } else if (ccr_gui_textbox(box, te.buf, (int)sizeof te.buf, 20.0f)) {
                    ccr_editor_set_text(&doc.ed, te.node, te.buf);
                    te.node = CCR_NONE;
                }
            }
        }
        draw_topbar(w, &doc);
        draw_statusbar(w, h, &canvas, &doc);
        ccr_pie_draw(&pie);

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
    ccr_lang_reset();
    return 0;
}
