#include "ccr_codegen.h"
#include "ccr_lang.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gen {
    const ccr_ast *ast;
    char    *buf;
    size_t   len, cap;
    bool     ok;
    char    *err;
    size_t   err_cap;
    uint8_t *dirty;     /* per node: 0 unknown, 1 clean, 2 something below changed */
} gen;

static const ccr_node *N(const gen *g, ccr_node_id id) { return &g->ast->nodes[id]; }

static void fail(gen *g, const char *fmt, ...)
{
    if (!g->ok)
        return;   /* keep the first reason */
    g->ok = false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->err, g->err_cap, fmt, ap);
    va_end(ap);
}

static void put(gen *g, const char *s, size_t n)
{
    if (!g->ok || n == 0)
        return;
    if (g->len + n + 1 > g->cap) {
        size_t cap = g->cap ? g->cap : 4096;
        while (cap < g->len + n + 1)
            cap *= 2;
        char *b = realloc(g->buf, cap);
        if (!b) {
            fail(g, "out of memory");
            return;
        }
        g->buf = b;
        g->cap = cap;
    }
    memcpy(g->buf + g->len, s, n);
    g->len += n;
    g->buf[g->len] = '\0';
}

static void puts_(gen *g, const char *s) { put(g, s, strlen(s)); }

static void put_src(gen *g, uint32_t b, uint32_t e)
{
    if (e > g->ast->source_len || b > e) {
        fail(g, "internal: bad source range [%u, %u)", b, e);
        return;
    }
    put(g, g->ast->source + b, e - b);
}

/* ------------------------------------------------------------ dirt */

static bool has_dirty(gen *g, ccr_node_id id, unsigned depth)
{
    if (g->dirty[id])
        return g->dirty[id] == 2;
    bool d = (N(g, id)->flags & (CCR_FLAG_DIRTY | CCR_FLAG_NEW)) != 0;
    if (depth < 4000)
        for (ccr_node_id c = N(g, id)->first_child; c != CCR_NONE && !d; c = N(g, c)->next_sibling)
            d = has_dirty(g, c, depth + 1);
    g->dirty[id] = d ? 2 : 1;
    return d;
}

/* ----------------------------------------------------------- emit */

static void emit(gen *g, ccr_node_id id, unsigned depth);

/* Whitespace before `at` on its line, if nothing else precedes it. */
static bool indent_before(const ccr_ast *ast, uint32_t at, uint32_t *b, uint32_t *e)
{
    uint32_t start = at;
    while (start > 0 && ast->source[start - 1] != '\n')
        --start;
    for (uint32_t i = start; i < at; ++i)
        if (ast->source[i] != ' ' && ast->source[i] != '\t')
            return false;
    *b = start;
    *e = at;
    return true;
}

/* In buf[from..], "\n" + old becomes "\n" + new at the start of each line. */
static void reindent(gen *g, size_t from, const char *old, size_t old_n, const char *nu, size_t nu_n)
{
    if (!g->ok || (old_n == nu_n && memcmp(old, nu, old_n) == 0))
        return;
    size_t n = g->len - from;
    char *copy = malloc(n + 1);
    if (!copy) {
        fail(g, "out of memory");
        return;
    }
    memcpy(copy, g->buf + from, n);
    g->len = from;
    for (size_t i = 0; i < n; ++i) {
        put(g, copy + i, 1);
        if (copy[i] == '\n' && n - (i + 1) >= old_n && memcmp(copy + i + 1, old, old_n) == 0) {
            put(g, nu, nu_n);
            i += old_n;
        }
    }
    free(copy);
}

/* buf[from..]: "// " at the start of every line, `keep` columns in (the
 * statement's own indent), so the slashes line up like an editor's. */
static void comment_out(gen *g, size_t from, size_t keep)
{
    if (!g->ok)
        return;
    size_t n = g->len - from;
    char *copy = malloc(n + 1);
    if (!copy) {
        fail(g, "out of memory");
        return;
    }
    memcpy(copy, g->buf + from, n);
    g->len = from;
    puts_(g, "// ");
    for (size_t i = 0; i < n; ++i) {
        put(g, copy + i, 1);
        if (copy[i] != '\n')
            continue;
        for (size_t k = 0; k < keep && i + 1 < n && (copy[i + 1] == ' ' || copy[i + 1] == '\t'); ++k)
            put(g, copy + ++i, 1);
        puts_(g, (i + 1 < n && copy[i + 1] != '\n') ? "// " : "//");
    }
    free(copy);
}

/* A Block whose statements moved: written anew from their original text. */
static void rebuild_block(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *b = N(g, id);
    if (b->flags & CCR_FLAG_UNSAFE_GAP) {
        fail(g, ccr_tr(CCR_T_GEN_UNSAFE_GAP), b->line);
        return;
    }

    const bool implicit = (b->flags & CCR_FLAG_IMPLICIT) != 0;
    const char *indent = ccr_ast_str(g->ast, b->indent);
    const char *close = ccr_ast_str(g->ast, b->close_indent);

    if (implicit) {
        /* `if (x)\n    y;` becomes `if (x) {` */
        while (g->len > 0 && (g->buf[g->len - 1] == ' ' || g->buf[g->len - 1] == '\t' ||
                              g->buf[g->len - 1] == '\n' || g->buf[g->len - 1] == '\r'))
            g->len--;
        puts_(g, " {");
    } else {
        puts_(g, "{");
    }

    for (ccr_node_id c = b->first_child; c != CCR_NONE && g->ok; c = N(g, c)->next_sibling) {
        const ccr_node *k = N(g, c);
        if ((k->flags & CCR_FLAG_BLANK_BEFORE) && c != b->first_child)
            puts_(g, "\n");
        uint32_t eb = k->ext_b, ee = k->ext_e;
        if (ee <= eb || eb > k->src_b || ee < k->src_e) {   /* never had ext */
            eb = k->src_b;
            ee = k->src_e;
        }
        /* the block's indent, plus however much deeper this line was */
        char line_indent[512];
        snprintf(line_indent, sizeof line_indent, "%s%s", indent,
                 ccr_ast_str(g->ast, k->extra_indent));
        puts_(g, "\n");
        puts_(g, line_indent);
        size_t start = g->len;
        put_src(g, eb, k->src_b);        /* comments above it */
        emit(g, c, depth + 1);           /* the statement itself, ';' included */
        put_src(g, k->src_e, ee);        /* comment at the end of its line */

        uint32_t ib, ie;
        if (indent_before(g->ast, eb, &ib, &ie))
            reindent(g, start, g->ast->source + ib, ie - ib, line_indent, strlen(line_indent));
        if (k->flags & CCR_FLAG_COMMENTED)   /* after reindent: final columns */
            comment_out(g, start, strlen(line_indent));
    }

    const char *tail = ccr_ast_str(g->ast, b->aux);
    if (*tail) {
        puts_(g, "\n");
        puts_(g, indent);
        puts_(g, tail);
    }
    puts_(g, "\n");
    puts_(g, close);
    puts_(g, "}");
}

/* --------------------------------------------------- new statements */

static ccr_node_id kid(const gen *g, ccr_node_id id, uint32_t index)
{
    ccr_node_id c = N(g, id)->first_child;
    while (c != CCR_NONE && index-- > 0)
        c = N(g, c)->next_sibling;
    return c;
}

/* What a slot holds: typed text, or whatever expression was put there. */
static void slot(gen *g, ccr_node_id id, unsigned depth)
{
    if (id != CCR_NONE)
        emit(g, id, depth + 1);
}

/* A body: a Block is printed `{ ... }`; anything else as one statement. */
static void body(gen *g, ccr_node_id id, unsigned depth)
{
    if (id == CCR_NONE)
        puts_(g, "{\n}");
    else
        emit(g, id, depth + 1);
}

/*
 * Code made in the editor has no source text, so it is printed. Indents
 * are relative (the first line starts at column 0, nested blocks add 4
 * spaces); the block it is dropped into shifts the whole thing into place.
 */
static void print_new(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    const ccr_node_id p = n->parent;
    const bool statement = p != CCR_NONE && N(g, p)->kind == CCR_NODE_BLOCK;

    switch (n->kind) {
    case CCR_NODE_TEXT:
        puts_(g, ccr_ast_str(g->ast, n->name));
        if (statement)
            puts_(g, ";");
        break;
    case CCR_NODE_BLOCK:
        rebuild_block(g, id, depth);
        break;
    case CCR_NODE_IF:
        puts_(g, "if (");
        slot(g, kid(g, id, 0), depth);
        puts_(g, ") ");
        body(g, kid(g, id, 1), depth);
        if (kid(g, id, 2) != CCR_NONE) {
            puts_(g, " else ");
            body(g, kid(g, id, 2), depth);
        }
        break;
    case CCR_NODE_WHILE:
        puts_(g, "while (");
        slot(g, kid(g, id, 0), depth);
        puts_(g, ") ");
        body(g, kid(g, id, 1), depth);
        break;
    case CCR_NODE_DO_WHILE:
        puts_(g, "do ");
        body(g, kid(g, id, 0), depth);
        puts_(g, " while (");
        slot(g, kid(g, id, 1), depth);
        puts_(g, ");");
        break;
    case CCR_NODE_FOR:
        puts_(g, "for (");
        slot(g, kid(g, id, 0), depth);
        puts_(g, "; ");
        slot(g, kid(g, id, 1), depth);
        puts_(g, "; ");
        slot(g, kid(g, id, 2), depth);
        puts_(g, ") ");
        body(g, kid(g, id, 3), depth);
        break;
    case CCR_NODE_RETURN: {
        ccr_node_id v = n->first_child;
        bool empty = v == CCR_NONE ||
                     (N(g, v)->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, v)->name));
        puts_(g, "return");
        if (!empty) {
            puts_(g, " ");
            slot(g, v, depth);
        }
        puts_(g, ";");
        break;
    }
    case CCR_NODE_BREAK:
        puts_(g, "break;");
        break;
    case CCR_NODE_CONTINUE:
        puts_(g, "continue;");
        break;
    default:
        fail(g, "internal: cannot print a new %s block yet", ccr_node_kind_name(n->kind));
        break;
    }
}

static bool lives_in_gaps(ccr_node_kind k)
{
    /* their text is copied with the gaps around real code */
    return k == CCR_NODE_MACRO_USE || k == CCR_NODE_INCLUDE || k == CCR_NODE_MACRO_DEFINE;
}

/* Children sorted by where they start in the file (insertion sort: usually sorted). */
static ccr_node_id *sorted_children(gen *g, ccr_node_id id, uint32_t *count)
{
    uint32_t n = 0;
    for (ccr_node_id c = N(g, id)->first_child; c != CCR_NONE; c = N(g, c)->next_sibling)
        ++n;
    ccr_node_id *v = malloc((n ? n : 1) * sizeof *v);
    if (!v) {
        fail(g, "out of memory");
        *count = 0;
        return NULL;
    }
    uint32_t i = 0;
    for (ccr_node_id c = N(g, id)->first_child; c != CCR_NONE; c = N(g, c)->next_sibling) {
        uint32_t j = i++;
        while (j > 0 && N(g, v[j - 1])->src_b > N(g, c)->src_b) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = c;
    }
    *count = n;
    return v;
}

static void emit(gen *g, ccr_node_id id, unsigned depth)
{
    if (!g->ok)
        return;
    const ccr_node *n = N(g, id);

    if (!has_dirty(g, id, 0)) {
        put_src(g, n->src_b, n->src_e);
        return;
    }
    if (depth > 4000) {
        fail(g, "code nested too deeply");
        return;
    }
    if (n->flags & CCR_FLAG_NEW) {
        print_new(g, id, depth);
        return;
    }
    if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_DIRTY)) {
        rebuild_block(g, id, depth);
        return;
    }

    /* unchanged node with a change somewhere inside: copy the text between
     * its children, and go down into them */
    uint32_t count;
    ccr_node_id *kids = sorted_children(g, id, &count);
    uint32_t cur = n->src_b;
    for (uint32_t i = 0; i < count && g->ok; ++i) {
        const ccr_node *k = N(g, kids[i]);
        const bool dirty = has_dirty(g, kids[i], 0);
        if (lives_in_gaps(k->kind) || k->src_e <= k->src_b) {
            if (dirty)
                fail(g, ccr_tr(CCR_T_GEN_LOST), k->line);
            continue;
        }
        if (k->src_b < cur) {
            if (k->src_e <= cur && !dirty)
                continue;   /* inside text already copied (struct inside typedef) */
            fail(g, "line %u: overlapping code (from a macro?) around a change", k->line);
            break;
        }
        if (k->src_e > n->src_e) {
            fail(g, "line %u: code outside its parent (from a macro?) around a change", k->line);
            break;
        }
        put_src(g, cur, k->src_b);
        emit(g, kids[i], depth + 1);
        cur = k->src_e;
    }
    put_src(g, cur, n->src_e);
    free(kids);
}

bool ccr_codegen(const ccr_ast *ast, char **out, size_t *out_len, char *err, size_t err_cap)
{
    *out = NULL;
    *out_len = 0;
    if (err_cap)
        err[0] = '\0';
    if (!ast->source || ast->node_count == 0) {
        snprintf(err, err_cap, "nothing loaded");
        return false;
    }

    gen g = { .ast = ast, .ok = true, .err = err, .err_cap = err_cap };
    g.dirty = calloc(ast->node_count, 1);
    if (!g.dirty) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }

    emit(&g, 0, 0);   /* node 0 is the translation unit: the whole file */
    free(g.dirty);
    if (!g.ok) {
        free(g.buf);
        return false;
    }
    if (!g.buf) {
        g.buf = calloc(1, 1);
        if (!g.buf) {
            snprintf(err, err_cap, "out of memory");
            return false;
        }
    }
    *out = g.buf;
    *out_len = g.len;
    return true;
}
