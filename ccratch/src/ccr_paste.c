#include "ccr_paste.h"
#include "ccr_cmake.h"
#include "ccr_codegen.h"
#include "ccr_parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASTE_FN "ccratch_pasted_code__"

static const ccr_node *N(const ccr_snippet *s, ccr_node_id id) { return &s->ast.nodes[id]; }

static bool add_item(ccr_snippet *s, ccr_node_id id)
{
    if (s->count == CCR_SNIPPET_MAX || id == CCR_NONE)
        return false;
    s->items[s->count++] = id;
    return true;
}

/* One code line: a statement that is its own text. */
static void add_text(ccr_snippet *s, const char *text, size_t n, bool cmake)
{
    while (n && isspace((unsigned char)text[n - 1]))
        --n;
    while (n && isspace((unsigned char)*text)) {
        ++text;
        --n;
    }
    if (!cmake && n && text[n - 1] == ';')
        --n;   /* printing puts it back */
    if (!n)
        return;
    char *copy = malloc(n + 1);
    if (!copy)
        return;
    /* one line: the lines it was on, joined by a space */
    size_t k = 0;
    bool space = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = k > 0;
            continue;
        }
        if (space)
            copy[k++] = ' ';
        space = false;
        copy[k++] = c;
    }
    copy[k] = '\0';
    const ccr_node_id id = ccr_ast_add(&s->ast, CCR_NODE_TEXT, CCR_NONE);
    if (id != CCR_NONE) {
        s->ast.nodes[id].name = ccr_ast_intern(&s->ast, copy);
        s->ast.nodes[id].flags |= CCR_FLAG_NEW;
        if (add_item(s, id))
            ++s->as_text;
    }
    free(copy);
}

/* Text clang made nothing of, [b, e): code lines, a line per statement
 * (lines are joined until one ends a statement: ; { or }). */
static void add_gap(ccr_snippet *s, const char *src, uint32_t b, uint32_t e, bool cmake)
{
    uint32_t start = b;
    for (uint32_t i = b; i <= e; ++i) {
        if (i < e && src[i] != '\n')
            continue;
        /* the line [line_b, i) */
        uint32_t lb = i;
        while (lb > start && src[lb - 1] != '\n')
            --lb;
        uint32_t t = i;
        while (t > lb && isspace((unsigned char)src[t - 1]))
            --t;
        uint32_t f = lb;
        while (f < t && isspace((unsigned char)src[f]))
            ++f;
        const bool ends = t > f && (cmake || src[t - 1] == ';' || src[t - 1] == '{' || src[t - 1] == '}' ||
                                    (src[f] == '/' && f + 1 < t && src[f + 1] == '/') || src[f] == '#');
        if (ends || i == e) {
            add_text(s, src + start, i - start, cmake);
            start = i < e ? i + 1 : e;
        }
    }
}

/* Is there anything but space and comments in [b, e)? */
static bool has_code(const char *src, uint32_t b, uint32_t e)
{
    for (uint32_t i = b; i < e; ++i) {
        if (isspace((unsigned char)src[i]))
            continue;
        if (src[i] == '/' && i + 1 < e && src[i + 1] == '/') {
            while (i < e && src[i] != '\n')
                ++i;
            continue;
        }
        if (src[i] == '/' && i + 1 < e && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < e && !(src[i] == '*' && src[i + 1] == '/'))
                ++i;
            ++i;
            continue;
        }
        return true;
    }
    return false;
}

static ccr_node_id find_fn_body(const ccr_snippet *s)
{
    for (ccr_node_id i = 0; i < s->ast.node_count; ++i) {
        const ccr_node *n = N(s, i);
        if (n->kind != CCR_NODE_FUNCTION || strcmp(ccr_ast_str(&s->ast, n->name), PASTE_FN) != 0)
            continue;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(s, c)->next_sibling)
            if (N(s, c)->kind == CCR_NODE_BLOCK)
                return c;
    }
    return CCR_NONE;
}

static bool is_expr(ccr_node_kind k)
{
    switch (k) {
    case CCR_NODE_CALL: case CCR_NODE_BINARY_OP: case CCR_NODE_UNARY_OP: case CCR_NODE_TERNARY:
    case CCR_NODE_REF: case CCR_NODE_MEMBER: case CCR_NODE_INDEX: case CCR_NODE_CAST:
    case CCR_NODE_SIZEOF: case CCR_NODE_INT_LIT: case CCR_NODE_FLOAT_LIT: case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
        return true;
    default:
        return false;
    }
}

/* The file, then the code as the body of a function at its end. */
static bool read_c(const char *code, size_t code_len, bool as_expr, const char *context, size_t context_len,
                   const char *path, const char *const *args, int nargs, ccr_snippet *out)
{
    static const char head[] = "\nvoid " PASTE_FN "(void)\n{\n";
    static const char tail[] = "\n}\n";
    const size_t n = context_len + sizeof head - 1 + code_len + 1 + sizeof tail - 1;
    char *text = malloc(n + 1);
    if (!text)
        return false;
    size_t k = 0;
    if (context_len) {
        memcpy(text, context, context_len);
        k = context_len;
    }
    memcpy(text + k, head, sizeof head - 1);
    k += sizeof head - 1;
    const uint32_t code_b = (uint32_t)k;
    memcpy(text + k, code, code_len);
    k += code_len;
    if (as_expr)
        text[k++] = ';';
    const uint32_t code_e = (uint32_t)k;
    memcpy(text + k, tail, sizeof tail - 1);
    k += sizeof tail - 1;
    text[k] = '\0';

    memset(out, 0, sizeof *out);
    bool ok = ccr_ast_init(&out->ast) && ccr_parse_text(path, text, k, args, nargs, &out->ast);
    free(text);
    const ccr_node_id body = ok ? find_fn_body(out) : CCR_NONE;
    if (body == CCR_NONE)
        return false;
    const char *src = out->ast.source;
    if (!src || out->ast.source_len < code_e)
        return false;

    /* the statements clang read, and the text between them it did not */
    uint32_t cur = code_b;
    for (ccr_node_id c = N(out, body)->first_child; c != CCR_NONE; c = N(out, c)->next_sibling) {
        const ccr_node *st = N(out, c);
        const uint32_t b = st->ext_b < st->src_b && st->ext_e > st->ext_b ? st->ext_b : st->src_b;
        const uint32_t e = st->ext_e > st->src_e ? st->ext_e : st->src_e;
        if (b < cur || e > code_e)
            continue;   /* from somewhere else (a macro) */
        if (has_code(src, cur, b))
            add_gap(out, src, cur, b, false);
        add_item(out, c);
        cur = e;
    }
    if (has_code(src, cur, code_e))
        add_gap(out, src, cur, code_e, false);
    return out->count > 0;
}

/* Declarations for the file itself (a function copied whole): after the
 * file's text, at its top level. True only if all of it reads so. */
static bool read_toplevel(const char *code, size_t code_len, const char *context, size_t context_len,
                          const char *path, const char *const *args, int nargs, ccr_snippet *out)
{
    const size_t n = context_len + 1 + code_len + 1;
    char *text = malloc(n + 1);
    if (!text)
        return false;
    size_t k = 0;
    if (context_len) {
        memcpy(text, context, context_len);
        k = context_len;
    }
    text[k++] = '\n';
    const uint32_t code_b = (uint32_t)k;
    memcpy(text + k, code, code_len);
    k += code_len;
    const uint32_t code_e = (uint32_t)k;
    text[k++] = '\n';
    text[k] = '\0';
    memset(out, 0, sizeof *out);
    const bool ok = ccr_ast_init(&out->ast) && ccr_parse_text(path, text, k, args, nargs, &out->ast);
    free(text);
    if (!ok || out->ast.node_count == 0 || !out->ast.source)
        return false;
    uint32_t cur = code_b;
    for (ccr_node_id c = N(out, 0)->first_child; c != CCR_NONE; c = N(out, c)->next_sibling) {
        const ccr_node *d = N(out, c);
        if (d->src_b < code_b || d->kind == CCR_NODE_MACRO_USE)
            continue;   /* the file's own */
        const uint32_t b = d->ext_b < d->src_b && d->ext_e > d->ext_b ? d->ext_b : d->src_b;
        const uint32_t e = d->ext_e > d->src_e ? d->ext_e : d->src_e;
        if (has_code(out->ast.source, cur, b < cur ? cur : b))
            return false;   /* something it could not read: not this way */
        add_item(out, c);
        if (e > cur)
            cur = e;
    }
    if (has_code(out->ast.source, cur, code_e) || out->count == 0)
        return false;
    out->toplevel = true;
    return true;
}

bool ccr_snippet_parse(const char *code, const char *context, size_t context_len, const char *path, bool cmake,
                       const char *const *args, int nargs, ccr_snippet *out)
{
    memset(out, 0, sizeof *out);
    while (*code && isspace((unsigned char)*code))
        ++code;
    size_t n = strlen(code);
    while (n && isspace((unsigned char)code[n - 1]))
        --n;
    if (!n)
        return false;

    if (cmake) {
        if (!ccr_ast_init(&out->ast))
            return false;
        if (ccr_cmake_parse_text(code, n, "pasted", &out->ast)) {
            /* TU, its hat, the hat's block: the commands */
            const ccr_node_id hat = out->ast.nodes[0].first_child;
            const ccr_node_id top = hat != CCR_NONE ? out->ast.nodes[hat].first_child : CCR_NONE;
            for (ccr_node_id c = top != CCR_NONE ? N(out, top)->first_child : CCR_NONE; c != CCR_NONE;
                 c = N(out, c)->next_sibling)
                add_item(out, c);
        }
        if (out->count == 0) {   /* not CMake it reads: lines of text */
            ccr_ast_free(&out->ast);
            memset(out, 0, sizeof *out);
            if (!ccr_ast_init(&out->ast))
                return false;
            out->ast.lang = CCR_LANG_CMAKE;
            add_gap(out, code, 0, (uint32_t)n, true);
        }
        return out->count > 0;
    }

    /* one expression (no ; { }): for a slot, if it reads as one */
    const bool looks_expr = !memchr(code, ';', n) && !memchr(code, '{', n) && !memchr(code, '}', n) &&
                            code[0] != '#';
    if (looks_expr) {
        if (read_c(code, n, true, context, context_len, path, args, nargs, out) && out->count == 1 &&
            out->as_text == 0 && is_expr(N(out, out->items[0])->kind)) {
            out->reporter = true;
            return true;
        }
        ccr_snippet_free(out);
    }
    /* statements, for a function's body */
    const bool body = read_c(code, n, false, context, context_len, path, args, nargs, out);
    if (body && out->as_text == 0)
        return true;
    /* not all of it reads as statements: maybe it is a function, a type */
    ccr_snippet top;
    if (read_toplevel(code, n, context, context_len, path, args, nargs, &top)) {
        ccr_snippet_free(out);
        *out = top;
        return true;
    }
    ccr_snippet_free(&top);
    if (body)
        return true;   /* statements, some kept as code lines */
    ccr_snippet_free(out);
    return false;
}

void ccr_snippet_free(ccr_snippet *s)
{
    ccr_ast_free(&s->ast);
    memset(s, 0, sizeof *s);
}

/* ------------------------------------------------------------ copying */

typedef struct piece { uint32_t b, e; } piece;

static int by_start(const void *a, const void *b)
{
    const piece *x = a, *y = b;
    return (x->b > y->b) - (x->b < y->b);
}

/* `text` [b, e) with the indentation of its first line taken off every
 * line (as far as each line has it). */
static void put_dedented(char **out, size_t *len, size_t *cap, const char *text, uint32_t b, uint32_t e)
{
    uint32_t ls = b;
    while (ls > 0 && text[ls - 1] != '\n')
        --ls;
    uint32_t ind = 0;
    while (ls + ind < b && (text[ls + ind] == ' ' || text[ls + ind] == '\t'))
        ++ind;
    if (ls + ind != b)
        ind = 0;   /* something else before it on its line: nothing to take off */
    for (uint32_t i = b; i < e; ++i) {
        if (*len + 2 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
            char *g = realloc(*out, *cap);
            if (!g)
                return;
            *out = g;
        }
        (*out)[(*len)++] = text[i];
        if (text[i] == '\n')
            for (uint32_t k = 0; k < ind && i + 1 < e && (text[i + 1] == ' ' || text[i + 1] == '\t'); ++k)
                ++i;
    }
}

char *ccr_copy_text(const ccr_ast *ast, const ccr_node_id *nodes, int count)
{
    if (count <= 0 || ast->node_count == 0)
        return NULL;
    ccr_out_range *map = malloc(ast->node_count * sizeof *map);
    char *text = NULL, err[256];
    size_t tlen = 0;
    if (!map || !ccr_codegen_mapped(ast, &text, &tlen, map, err, sizeof err)) {
        free(map);
        return NULL;
    }
    piece *ps = malloc((size_t)count * sizeof *ps);
    int n = 0;
    for (int i = 0; ps && i < count; ++i)
        if (nodes[i] < ast->node_count && map[nodes[i]].b != UINT32_MAX && map[nodes[i]].e > map[nodes[i]].b)
            ps[n++] = (piece){ map[nodes[i]].b, map[nodes[i]].e };
    char *out = NULL;
    size_t len = 0, cap = 0;
    if (ps && n) {
        qsort(ps, (size_t)n, sizeof *ps, by_start);
        for (int i = 0; i < n; ++i) {
            if (i > 0 && ps[i].b < ps[i - 1].e)
                continue;   /* inside the one before */
            if (i > 0) {
                put_dedented(&out, &len, &cap, "\n", 0, 1);
            }
            put_dedented(&out, &len, &cap, text, ps[i].b, ps[i].e);
        }
        if (out)
            out[len] = '\0';
    }
    free(ps);
    free(map);
    free(text);
    return out;
}
