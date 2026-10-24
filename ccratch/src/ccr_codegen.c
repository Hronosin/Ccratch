#include "ccr_codegen.h"
#include "ccr_asm.h"
#include "ccr_lang.h"

#include <ctype.h>
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
    ccr_out_range *map; /* per node: where it went in the output (may be NULL) */
    int      asm_depth; /* inside an asm("...") being written, or one asm line */
    char     asm_indent[32];   /* the file's instructions' indent */
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
    bool d = (N(g, id)->flags & (CCR_FLAG_DIRTY | CCR_FLAG_NEW | CCR_FLAG_MOVED)) != 0;
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
    const char *mark = g->ast->lang == CCR_LANG_CMAKE ? "# " : "// ";
    const char *bare = g->ast->lang == CCR_LANG_CMAKE ? "#" : "//";
    if (g->ast->lang == CCR_LANG_ASM || g->asm_depth) {
        mark = ccr_asm_comment(g->ast->asm_syntax);
        bare = g->ast->asm_syntax == CCR_ASM_NASM ? ";" : g->ast->asm_syntax == CCR_ASM_ARM64 ? "//"
             : g->ast->asm_syntax == CCR_ASM_ARM32 ? "@" : "#";
    }
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
    puts_(g, mark);
    for (size_t i = 0; i < n; ++i) {
        put(g, copy + i, 1);
        if (copy[i] != '\n')
            continue;
        for (size_t k = 0; k < keep && i + 1 < n && (copy[i + 1] == ' ' || copy[i + 1] == '\t'); ++k)
            put(g, copy + ++i, 1);
        puts_(g, (i + 1 < n && copy[i + 1] != '\n') ? mark : bare);
    }
    free(copy);
}

/* CMake has no braces: a body is its commands, one per line, and the
 * end*() line after them; the file's own commands start at column 0. */
static void rebuild_cmake(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *b = N(g, id);
    const bool top = b->parent != CCR_NONE && N(g, b->parent)->kind == CCR_NODE_FUNCTION;
    const char *indent = ccr_ast_str(g->ast, b->indent);
    const char *close = ccr_ast_str(g->ast, b->close_indent);
    bool first = true;
    for (ccr_node_id c = b->first_child; c != CCR_NONE && g->ok; c = N(g, c)->next_sibling) {
        const ccr_node *k = N(g, c);
        uint32_t eb = k->ext_b, ee = k->ext_e;
        if (ee <= eb || eb > k->src_b || ee < k->src_e) {
            eb = k->src_b;
            ee = k->src_e;
        }
        char line_indent[512];
        snprintf(line_indent, sizeof line_indent, "%s%s", indent, ccr_ast_str(g->ast, k->extra_indent));
        if (top && first) {
            if (g->len > 0 && g->buf[g->len - 1] != '\n')
                puts_(g, "\n");   /* a file that did not end its last line */
        } else {
            if ((k->flags & CCR_FLAG_BLANK_BEFORE) && !first)
                puts_(g, "\n");
            puts_(g, "\n");
            puts_(g, line_indent);
        }
        first = false;
        size_t start = g->len;
        put_src(g, eb, k->src_b);
        emit(g, c, depth + 1);
        put_src(g, k->src_e, ee);
        /* the command's own indent is what moves; a comment above it that
         * sat elsewhere keeps its place relative to the line */
        uint32_t ib, ie;
        if (indent_before(g->ast, k->src_b, &ib, &ie) || indent_before(g->ast, eb, &ib, &ie))
            reindent(g, start, g->ast->source + ib, ie - ib, line_indent, strlen(line_indent));
        if (k->flags & CCR_FLAG_COMMENTED)
            comment_out(g, start, strlen(line_indent));
    }
    const char *tail = ccr_ast_str(g->ast, b->aux);
    if (*tail) {
        puts_(g, "\n");
        puts_(g, indent);
        puts_(g, tail);
    }
    if (!top) {
        puts_(g, "\n");
        puts_(g, close);
    }
}

static void rebuild_asm(gen *g, ccr_node_id id, unsigned depth);
static void rebuild_asm_literal(gen *g, ccr_node_id id, unsigned depth);

/* Which child of `parent` `id` is. */
static int kid_index(const gen *g, ccr_node_id parent, ccr_node_id id)
{
    int k = 0;
    for (ccr_node_id c = N(g, parent)->first_child; c != CCR_NONE && c != id; c = N(g, c)->next_sibling)
        ++k;
    return k;
}

/* Where the code on a line ends: after its last character that is not a
 * comment or a space (strings and characters are code). */
static size_t code_end_of_line(const char *s, size_t n)
{
    size_t end = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') {
            continue;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            break;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); ++i) {
            }
            ++i;
        } else if (s[i] == '"' || s[i] == '\'') {
            const char q = s[i];
            for (++i; i < n && s[i] != q; ++i)
                if (s[i] == '\\')
                    ++i;
            end = i + 1 < n ? i + 1 : n;
        } else {
            end = i + 1;
        }
    }
    return end;
}

/* buf[0..len) ends with code, maybe followed by comments (on its line, or
 * on lines of their own): where the " {" of a braceless body goes, right
 * after the code (not into a // comment, where it would be commented out). */
static size_t brace_spot(const char *buf, size_t len)
{
    size_t end = len;
    for (;;) {
        size_t ls = end;
        while (ls > 0 && buf[ls - 1] != '\n')
            --ls;
        const size_t ce = code_end_of_line(buf + ls, end - ls);
        if (ce > 0)
            return ls + ce;
        if (ls == 0)
            return len;
        end = ls - 1;   /* only comments on it: the line above */
        while (end > 0 && (buf[end - 1] == ' ' || buf[end - 1] == '\t' || buf[end - 1] == '\r' ||
                           buf[end - 1] == '\n'))
            --end;
    }
}

/* A Block whose statements moved: written anew from their original text. */
static void rebuild_block(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *b = N(g, id);
    if (g->ast->lang == CCR_LANG_CMAKE) {
        rebuild_cmake(g, id, depth);
        return;
    }
    if (g->ast->lang == CCR_LANG_ASM || g->asm_depth) {
        rebuild_asm(g, id, depth);
        return;
    }
    if (b->parent != CCR_NONE && N(g, b->parent)->kind == CCR_NODE_ASM) {
        rebuild_asm_literal(g, id, depth);
        return;
    }
    if (b->flags & CCR_FLAG_UNSAFE_GAP) {
        fail(g, ccr_tr(CCR_T_GEN_UNSAFE_GAP), b->line);
        return;
    }

    const bool implicit = (b->flags & CCR_FLAG_IMPLICIT) != 0;
    const char *indent = ccr_ast_str(g->ast, b->indent);
    const char *close = ccr_ast_str(g->ast, b->close_indent);

    /* `else if`: the else's body is the next if, and stays one, not
     * `else { if ... }` */
    if (implicit && b->first_child != CCR_NONE && N(g, b->first_child)->next_sibling == CCR_NONE &&
        N(g, b->first_child)->kind == CCR_NODE_IF && b->parent != CCR_NONE && N(g, b->parent)->kind == CCR_NODE_IF &&
        kid_index(g, b->parent, id) == 2) {
        const ccr_node *k = N(g, b->first_child);
        if (k->ext_b == k->src_b || k->ext_e <= k->ext_b) {   /* no comment of its own in front */
            while (g->len > 0 && (g->buf[g->len - 1] == ' ' || g->buf[g->len - 1] == '\t' ||
                                  g->buf[g->len - 1] == '\n' || g->buf[g->len - 1] == '\r'))
                g->len--;
            puts_(g, " ");
            emit(g, b->first_child, depth);
            if (k->ext_e > k->src_e)
                put_src(g, k->src_e, k->ext_e);   /* its comment at the end of the line */
            return;
        }
    }

    if (implicit) {
        /* `if (x)\n    y;` becomes `if (x) {`; and `if (x) // why` or a
         * comment on a line of its own under it: the brace goes after the
         * code, not into the comment (where it would be commented out) */
        while (g->len > 0 && (g->buf[g->len - 1] == ' ' || g->buf[g->len - 1] == '\t' ||
                              g->buf[g->len - 1] == '\n' || g->buf[g->len - 1] == '\r'))
            g->len--;
        const size_t at = brace_spot(g->buf, g->len);
        puts_(g, " {");
        if (g->ok && at < g->len - 2) {
            memmove(g->buf + at + 2, g->buf + at, g->len - 2 - at);
            memcpy(g->buf + at, " {", 2);
        }
    } else {
        puts_(g, "{");
    }

    for (ccr_node_id c = b->first_child; c != CCR_NONE && g->ok; c = N(g, c)->next_sibling) {
        const ccr_node *k = N(g, c);
        if ((k->flags & CCR_FLAG_BLANK_BEFORE) && c != b->first_child)
            puts_(g, "\n");
        uint32_t eb = k->ext_b, ee = k->ext_e;
        const bool asm_line = (k->flags & CCR_FLAG_ASM) != 0;   /* comes out as __asm__("...") */
        if (ee <= eb || eb > k->src_b || ee < k->src_e || asm_line) {   /* never had ext */
            eb = k->src_b;
            ee = k->src_e;
        }
        /* the block's indent, plus however much deeper this line was */
        char line_indent[512];
        snprintf(line_indent, sizeof line_indent, "%s%s", indent,
                 asm_line ? "" : ccr_ast_str(g->ast, k->extra_indent));
        puts_(g, "\n");
        puts_(g, line_indent);
        size_t start = g->len;
        put_src(g, eb, k->src_b);        /* comments above it */
        emit(g, c, depth + 1);           /* the statement itself, ';' included */
        put_src(g, k->src_e, ee);        /* comment at the end of its line */

        uint32_t ib, ie;
        if (!asm_line && indent_before(g->ast, eb, &ib, &ie))
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

/* The index-th child that is not a name or type field. */
static ccr_node_id arg(const gen *g, ccr_node_id id, uint32_t index)
{
    for (ccr_node_id c = N(g, id)->first_child; c != CCR_NONE; c = N(g, c)->next_sibling)
        if (!(N(g, c)->flags & CCR_FLAG_WORD) && index-- == 0)
            return c;
    return CCR_NONE;
}

/* ------------------------------------------------- operator precedence */

/* C's binding strength of a binary operator: higher binds tighter. */
static int binary_prec(const char *op)
{
    static const struct { const char *op; int prec; } table[] = {
        { "*", 13 }, { "/", 13 }, { "%", 13 }, { "+", 12 }, { "-", 12 },
        { "<<", 11 }, { ">>", 11 }, { "<", 10 }, { ">", 10 }, { "<=", 10 }, { ">=", 10 },
        { "==", 9 }, { "!=", 9 }, { "&", 8 }, { "^", 7 }, { "|", 6 }, { "&&", 5 }, { "||", 4 },
        { "=", 2 }, { "+=", 2 }, { "-=", 2 }, { "*=", 2 }, { "/=", 2 }, { "%=", 2 },
        { "<<=", 2 }, { ">>=", 2 }, { "&=", 2 }, { "^=", 2 }, { "|=", 2 }, { ",", 1 },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (strcmp(op, table[i].op) == 0)
            return table[i].prec;
    return 0;   /* unknown: always parenthesised */
}

/* How tightly typed text holds together, by its loosest operator outside
 * brackets: 16 for one thing on its own (a name, a call, p->x, a[i]), 14
 * for -x or (int)x, 12 for a + b... 0 if it cannot tell. */
static int text_prec(const char *s)
{
    static const char *const ops[] = {
        ">>=", "<<=", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
        "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
        "+", "-", "*", "/", "%", "<", ">", "&", "|", "^", "!", "~", "=", "?", ":", ",", ".",
    };
    int prec = 16;
    bool want_operand = true;   /* at the start, after an operator */
    bool after_group = false;   /* just closed a ( ) at the top */
    bool in_new = false;        /* new unsigned int[n]: its type's words side by side */
    bool after_string = false;  /* "a" "b": one literal, glued */
    const char *p = s;
    while (*p) {
        const char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++p;
            continue;
        }
        if (c == '/' && p[1] == '/') {           /* comments are space */
            while (*p && *p != '\n')
                ++p;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            if (!end)
                return 0;
            p = end + 2;
            continue;
        }
        if (c == '#' && (p == s || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t')) {
            while (*p && *p != '\n')            /* #if lines inside it */
                ++p;
            continue;
        }
        const bool word = c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') || (c == '.' && p[1] >= '0' && p[1] <= '9');   /* .5 */
        if (c == '"' && after_string) {   /* the next piece of the same literal */
            const char *q = p + 1;
            while (*q && *q != '"')
                q += (*q == '\\' && q[1]) ? 2 : 1;
            if (!*q)
                return 0;
            p = q + 1;
            continue;
        }
        if (word || c == '"' || c == '\'') {
            /* (int)sizeof x: a prefix word after a cast */
            bool prefix_next = false;
            if (word && !want_operand && after_group)
                for (const char *const *k = (const char *const[]){ "sizeof", "alignof", "_Alignof", NULL }; *k; ++k)
                    if (strncmp(p, *k, strlen(*k)) == 0 && !isalnum((unsigned char)p[strlen(*k)]) &&
                        p[strlen(*k)] != '_')
                        prefix_next = true;
            if (prefix_next) {
                want_operand = true;
                after_group = false;
            }
            if (!want_operand && !in_new) {
                if (!after_group)
                    return 0;           /* two things side by side */
                if (prec > 14)
                    prec = 14;          /* (int)x: a cast */
            }
            if (word) {
                const char *w = p;
                while (*p == '_' || *p == '.' || (*p >= '0' && *p <= '9') ||
                       (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (p[0] == ':' && p[1] == ':'))
                    p += p[0] == ':' ? 2 : 1;   /* std::move: one name */
                /* C++'s static_cast<T *>: its < > are brackets, not comparisons */
                static const char *const casts[] = { "static_cast", "dynamic_cast", "const_cast",
                                                     "reinterpret_cast" };
                const char *q = p;
                while (*q == ' ' || *q == '\t')
                    ++q;
                for (size_t k = 0; k < sizeof casts / sizeof casts[0] && *q == '<'; ++k)
                    if ((size_t)(p - w) == strlen(casts[k]) && strncmp(w, casts[k], (size_t)(p - w)) == 0) {
                        int angle = 0;
                        for (; *q; ++q) {
                            angle += *q == '<';
                            angle -= *q == '>';
                            if (angle == 0)
                                break;
                        }
                        if (!*q)
                            return 0;
                        p = q + 1;
                        break;
                    }
                const size_t wl = (size_t)(p - w);
                /* L"x", u8"x": the prefix is the literal's own */
                if ((*p == '"' || *p == '\'') &&
                    ((wl == 1 && (*w == 'L' || *w == 'u' || *w == 'U')) || (wl == 2 && strncmp(w, "u8", 2) == 0)))
                    continue;
                /* sizeof x, alignof, new T, delete p, throw e: words that work as prefix operators */
                static const char *const prefix_words[] = { "sizeof", "alignof", "_Alignof", "__alignof__",
                                                            "__real__", "__imag__", "__extension__", "new",
                                                            "delete", "throw" };
                bool is_prefix = false;
                for (size_t k = 0; k < sizeof prefix_words / sizeof prefix_words[0] && !is_prefix; ++k)
                    is_prefix = wl == strlen(prefix_words[k]) && strncmp(w, prefix_words[k], wl) == 0;
                if (is_prefix && want_operand) {
                    const int pp = wl == 5 && strncmp(w, "throw", 5) == 0 ? 2 : 14;
                    if (prec > pp)
                        prec = pp;
                    in_new = wl == 3 && strncmp(w, "new", 3) == 0;
                    after_group = false;
                    continue;   /* still wants its operand */
                }
            } else {
                const char q = *p++;
                while (*p && *p != q)
                    p += (*p == '\\' && p[1]) ? 2 : 1;
                if (!*p)
                    return 0;
                ++p;
            }
            after_string = c == '"';
            want_operand = false;
            after_group = false;
            continue;
        }
        after_string = false;
        if (c == '(' || c == '[' || c == '{') {
            /* a group: parentheses, a call, an index, a compound literal */
            int depth = 0;
            char quote = 0;
            for (; *p; ++p) {
                if (quote) {
                    if (*p == '\\' && p[1])
                        ++p;
                    else if (*p == quote)
                        quote = 0;
                } else if (*p == '"' || *p == '\'') {
                    quote = *p;
                } else if (*p == '(' || *p == '[' || *p == '{') {
                    ++depth;
                } else if (*p == ')' || *p == ']' || *p == '}') {
                    if (--depth == 0)
                        break;
                }
            }
            if (!*p)
                return 0;
            ++p;
            /* (int)x, and (void *)(uintptr_t)x: a cast after a cast */
            if (c == '(' && after_group && prec > 14)
                prec = 14;
            after_group = c == '(' && (want_operand || after_group);
            want_operand = false;
            continue;
        }
        const char *op = NULL;
        for (size_t i = 0; i < sizeof ops / sizeof ops[0] && !op; ++i)
            if (strncmp(p, ops[i], strlen(ops[i])) == 0)
                op = ops[i];
        if (!op)
            return 0;
        p += strlen(op);
        after_group = false;
        in_new = false;
        if (want_operand) {
            /* prefix: -x, !x, *p, &x, ++x */
            if (strchr("-+!~*&", op[0]) && (strlen(op) == 1 || strcmp(op, "++") == 0 ||
                                            strcmp(op, "--") == 0)) {
                if (prec > 14)
                    prec = 14;
                continue;
            }
            return 0;
        }
        if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0)
            continue;                   /* postfix */
        if (strcmp(op, "->") == 0 || strcmp(op, ".") == 0) {
            want_operand = true;        /* the member's name follows */
            continue;
        }
        const int bp = strcmp(op, "?") == 0 || strcmp(op, ":") == 0 ? 3 : binary_prec(op);
        if (bp < prec)
            prec = bp;
        want_operand = true;
    }
    return want_operand ? 0 : prec;
}

static int kind_prec(const gen *g, const ccr_node *n);

/* How tightly expression `id` holds together, as an operand. */
static int expr_prec(const gen *g, ccr_node_id id)
{
    const ccr_node *n = N(g, id);
    if (!(n->flags & CCR_FLAG_NEW) && n->src_e > n->src_b && n->src_e <= g->ast->source_len) {
        /* parsed: go by its text, which may be a macro call around it */
        const size_t len = n->src_e - n->src_b;
        char small[256];
        char *copy = len < sizeof small ? small : malloc(len + 1);
        if (!copy)
            return 0;
        memcpy(copy, g->ast->source + n->src_b, len);
        copy[len] = '\0';
        int prec = text_prec(copy);
        if (copy != small)
            free(copy);
        /* (float)-x reads like a subtraction; its kind knows better */
        const int kind = kind_prec(g, n);
        return kind > prec ? kind : prec;
    }
    return kind_prec(g, n);
}

static int kind_prec(const gen *g, const ccr_node *n)
{
    switch (n->kind) {
    case CCR_NODE_BINARY_OP: return binary_prec(ccr_ast_str(g->ast, n->op));
    case CCR_NODE_TERNARY:   return 3;
    case CCR_NODE_UNARY_OP:  return (n->flags & CCR_FLAG_POSTFIX) ? 15 : 14;
    case CCR_NODE_CAST:
    case CCR_NODE_SIZEOF:    return 14;
    case CCR_NODE_CALL: case CCR_NODE_INDEX: case CCR_NODE_MEMBER:
    case CCR_NODE_COMPOUND_LIT: return 15;
    case CCR_NODE_TEXT:      return text_prec(ccr_ast_str(g->ast, n->name));
    default:                 return 16;
    }
}

/* How tightly the operand at `index` of `parent` must hold together to need
 * no parentheses there; 0 for slots that are not operands (arguments,
 * conditions, a body's statements...). */
static int operand_need(const gen *g, ccr_node_id parent, uint32_t index)
{
    if (parent == CCR_NONE)
        return 0;
    const ccr_node *p = N(g, parent);
    switch (p->kind) {
    case CCR_NODE_BINARY_OP: {
        const int prec = binary_prec(ccr_ast_str(g->ast, p->op));
        if (prec == 2)   /* assignment: right to left */
            return index == 0 ? 14 : 2;
        return index == 0 ? prec : prec + 1;
    }
    case CCR_NODE_TERNARY:  return index == 0 ? 4 : index == 1 ? 0 : 3;
    case CCR_NODE_UNARY_OP: return (p->flags & CCR_FLAG_POSTFIX) ? 15 : 14;
    case CCR_NODE_CAST:
    case CCR_NODE_SIZEOF:   return 14;
    case CCR_NODE_MEMBER:   return 15;
    case CCR_NODE_INDEX:    return index == 0 ? 15 : 0;
    default:                return 0;
    }
}

/* An expression in a slot of its parent. Text and expressions that were
 * not parsed there get parentheses if C would otherwise bind them wrong. */
static void emit_operand(gen *g, ccr_node_id id, unsigned depth)
{
    if (g->ast->lang != CCR_LANG_C || g->asm_depth || (N(g, id)->flags & CCR_FLAG_ASM) ||
        N(g, id)->kind == CCR_NODE_BLOCK ||
        (N(g, id)->parent != CCR_NONE && N(g, N(g, id)->parent)->kind == CCR_NODE_ASM)) {
        emit(g, id, depth);   /* words, no precedence */
        return;
    }
    const ccr_node *n = N(g, id);
    uint32_t index = 0;
    if (n->parent != CCR_NONE)
        for (ccr_node_id c = N(g, n->parent)->first_child; c != id && c != CCR_NONE;
             c = N(g, c)->next_sibling)
            if (!(N(g, c)->flags & CCR_FLAG_WORD))
                ++index;
    /* where it was parsed, the text around it had whatever parentheses it
     * needed; somewhere new, or with its parent printed anew, it may not */
    const bool placed = (n->flags & (CCR_FLAG_NEW | CCR_FLAG_MOVED)) != 0 ||
                        (n->parent != CCR_NONE && (N(g, n->parent)->flags & CCR_FLAG_NEW));
    bool wrap = placed && expr_prec(g, id) < operand_need(g, n->parent, index);
    if (wrap && n->parent != CCR_NONE && !(N(g, n->parent)->flags & CCR_FLAG_NEW) &&
        n->place_e > n->place_b && n->place_e <= g->ast->source_len) {
        /* the parsed text around it has the parentheses already */
        const char *src = g->ast->source;
        uint32_t b = n->place_b, e = n->place_e;
        while (b > 0 && (src[b - 1] == ' ' || src[b - 1] == '	'))
            --b;
        while (e < g->ast->source_len && (src[e] == ' ' || src[e] == '	'))
            ++e;
        if (b > 0 && src[b - 1] == '(' && e < g->ast->source_len && src[e] == ')')
            wrap = false;
    }
    if (wrap)
        puts_(g, "(");
    emit(g, id, depth);
    if (wrap)
        puts_(g, ")");
}

/* What a slot holds: typed text, or whatever expression was put there. */
static void slot(gen *g, ccr_node_id id, unsigned depth)
{
    if (id != CCR_NONE)
        emit_operand(g, id, depth + 1);
}

/* A name or type that must not be left empty. */
static bool filled(gen *g, ccr_node_id id, const char *s)
{
    if (*s)
        return true;
    uint32_t line = 0;
    for (ccr_node_id a = id; a != CCR_NONE && !line; a = N(g, a)->parent)
        line = N(g, a)->line;
    fail(g, ccr_tr(CCR_T_GEN_EMPTY_SLOT), line);
    return false;
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
static void print_new_cmake(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    const char *name = ccr_ast_str(g->ast, n->name);
    switch (n->kind) {
    case CCR_NODE_TEXT:
        puts_(g, name);   /* an empty slot is no argument at all */
        break;
    case CCR_NODE_BLOCK:
        rebuild_cmake(g, id, depth);
        break;
    case CCR_NODE_CALL: {
        if (!filled(g, id, name))
            break;
        puts_(g, name);
        puts_(g, "(");
        bool first = true;
        for (ccr_node_id a = n->first_child; a != CCR_NONE; a = N(g, a)->next_sibling) {
            if (N(g, a)->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, a)->name))
                continue;
            if (!first)
                puts_(g, " ");
            first = false;
            slot(g, a, depth);
        }
        puts_(g, ")");
        break;
    }
    case CCR_NODE_IF:
    case CCR_NODE_SCOPE: {
        const bool is_if = n->kind == CCR_NODE_IF;
        const char *head = is_if ? ((n->flags & CCR_FLAG_ELSEIF) ? "elseif" : "if") : name;
        puts_(g, head);
        puts_(g, "(");
        slot(g, kid(g, id, 0), depth);
        puts_(g, ")");
        const ccr_node_id b1 = kid(g, id, 1), b2 = kid(g, id, 2);
        if (b1 != CCR_NONE)
            emit(g, b1, depth + 1);
        else
            puts_(g, "\n");
        if (is_if && b2 != CCR_NONE) {
            const ccr_node_id e = N(g, b2)->first_child;
            if (e != CCR_NONE && (N(g, e)->flags & CCR_FLAG_ELSEIF)) {
                emit(g, e, depth + 1);   /* elseif(...) ...: its own end is ours */
                break;
            }
            puts_(g, "else()");
            emit(g, b2, depth + 1);
        }
        if (n->flags & CCR_FLAG_ELSEIF)
            break;
        if (is_if) {
            puts_(g, "endif()");
        } else {
            puts_(g, "end");
            puts_(g, name);
            puts_(g, "()");
        }
        break;
    }
    default:
        fail(g, "internal: cannot print a new %s block in CMake", ccr_node_kind_name(n->kind));
        break;
    }
}


/* ------------------------------------------------------- assembly */

static const char *asm_default_indent(const gen *g)
{
    return g->asm_indent[0] ? g->asm_indent : g->ast->asm_syntax == CCR_ASM_NASM ? "    " : "\t";
}

/* The indent of statement `c`'s line in block `blk`: as it was written, or
 * the block's (labels: none). */
static void asm_line_indent(const gen *g, ccr_node_id blk, ccr_node_id c, char *out, size_t cap)
{
    const ccr_node *k = N(g, c);
    const char *was = ccr_ast_str(g->ast, k->extra_indent);
    if (!(k->flags & CCR_FLAG_NEW) && was[0] == '|') {
        snprintf(out, cap, "%s", was + 1);
        return;
    }
    if (k->kind == CCR_NODE_LABEL) {
        out[0] = '\0';
        return;
    }
    const char *bi = blk != CCR_NONE && !(N(g, blk)->flags & CCR_FLAG_NEW) ? ccr_ast_str(g->ast, N(g, blk)->indent)
                                                                         : "";
    snprintf(out, cap, "%s", *bi ? bi : asm_default_indent(g));
}

/* One statement per line; each keeps the indent it had. */
static void rebuild_asm(gen *g, ccr_node_id id, unsigned depth)
{
    bool first = true;
    for (ccr_node_id c = N(g, id)->first_child; c != CCR_NONE && g->ok; c = N(g, c)->next_sibling) {
        const ccr_node *k = N(g, c);
        uint32_t eb = k->ext_b, ee = k->ext_e;
        const bool written = !(k->flags & CCR_FLAG_NEW);
        if (!written || ee <= eb || eb > k->src_b || ee < k->src_e) {
            eb = k->src_b;
            ee = k->src_e;
        }
        char ind[160];
        asm_line_indent(g, id, c, ind, sizeof ind);
        if (first) {
            const char last = g->len ? g->buf[g->len - 1] : '\n';
            if (last != '\n' && last != ' ' && last != '\t')
                puts_(g, "\n");   /* right after `main:`, or the head of a .macro */
            if (!g->len || g->buf[g->len - 1] == '\n')
                puts_(g, ind);
        } else {
            if (k->flags & CCR_FLAG_BLANK_BEFORE)
                puts_(g, "\n");
            puts_(g, "\n");
            puts_(g, ind);
        }
        first = false;
        const size_t start = g->len;
        if (written)
            put_src(g, eb, k->src_b);   /* comments above it */
        emit(g, c, depth + 1);
        if (written)
            put_src(g, k->src_e, ee);   /* and the one at the end of its line */
        if (k->flags & CCR_FLAG_COMMENTED)
            comment_out(g, start, strlen(ind));
    }
}

/* The Text slots of a node, as operands: "a, b, c". */
static void asm_operands(gen *g, ccr_node_id id, unsigned depth, uint32_t from)
{
    bool first = true;
    uint32_t i = 0;
    for (ccr_node_id a = N(g, id)->first_child; a != CCR_NONE; a = N(g, a)->next_sibling, ++i) {
        const ccr_node *k = N(g, a);
        if (i < from || (k->flags & CCR_FLAG_WORD) || k->kind == CCR_NODE_BLOCK)
            continue;
        if (k->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, k->name)) {
            /* an empty slot is no operand; one between two (.p2align 4,,10) keeps its comma */
            bool more = false;
            for (ccr_node_id r = k->next_sibling; r != CCR_NONE && !more; r = N(g, r)->next_sibling)
                more = N(g, r)->kind != CCR_NODE_TEXT || *ccr_ast_str(g->ast, N(g, r)->name);
            if (!more || first)
                continue;
        }
        puts_(g, first ? " " : ", ");
        first = false;
        emit(g, a, depth + 1);
    }
}

static void print_new_asm(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    const char *name = ccr_ast_str(g->ast, n->name);
    char ind[160] = "";
    if (n->parent != CCR_NONE && N(g, n->parent)->kind == CCR_NODE_BLOCK)
        asm_line_indent(g, n->parent, id, ind, sizeof ind);
    switch (n->kind) {
    case CCR_NODE_TEXT:
        if (!(n->flags & CCR_FLAG_WORD))
            puts_(g, name);
        break;
    case CCR_NODE_BLOCK:
        rebuild_asm(g, id, depth);
        break;
    case CCR_NODE_CALL: {
        if (!filled(g, id, name))
            break;
        const char *label = ccr_ast_str(g->ast, n->type);
        if (*label) {
            puts_(g, label);
            puts_(g, " ");
        }
        puts_(g, name);
        asm_operands(g, id, depth, 0);
        break;
    }
    case CCR_NODE_LABEL:
        if (filled(g, id, name)) {
            puts_(g, name);
            puts_(g, ":");
        }
        break;
    case CCR_NODE_GOTO:
        if (filled(g, id, name)) {
            puts_(g, ccr_ast_str(g->ast, n->op));
            asm_operands(g, id, depth, 0);   /* cbz x0, ...: what it tests */
            puts_(g, N(g, id)->first_child != CCR_NONE && !(N(g, N(g, id)->first_child)->flags & CCR_FLAG_WORD)
                         ? ", " : " ");
            puts_(g, name);
        }
        break;
    case CCR_NODE_SCOPE:
    case CCR_NODE_IF: {
        puts_(g, name);
        const ccr_node_id head = kid(g, id, 0);
        if (head != CCR_NONE && *ccr_ast_str(g->ast, N(g, head)->name)) {
            puts_(g, " ");
            emit(g, head, depth + 1);
        }
        const ccr_node_id b1 = kid(g, id, 1), b2 = kid(g, id, 2);
        if (b1 != CCR_NONE)
            emit(g, b1, depth + 1);
        if (n->kind == CCR_NODE_IF && b2 != CCR_NONE) {
            puts_(g, "\n");
            puts_(g, ind);
            puts_(g, ccr_asm_else_word(name));
            emit(g, b2, depth + 1);
        }
        puts_(g, "\n");
        puts_(g, ind);
        puts_(g, ccr_asm_end_word(name));
        break;
    }
    case CCR_NODE_FUNCTION:
        if (filled(g, id, name)) {
            puts_(g, name);
            puts_(g, ":");
            if (kid(g, id, 0) != CCR_NONE)
                emit(g, N(g, id)->last_child, depth + 1);
        }
        break;
    default:
        fail(g, "internal: cannot print a new %s block in assembly", ccr_node_kind_name(n->kind));
        break;
    }
}

/* Takes buf[from..] out, as the inside of a C string ("\n" between
 * lines as `sep` says). */
static char *take_escaped(gen *g, size_t from, const char *sep)
{
    if (!g->ok)
        return NULL;
    const size_t n = g->len - from;
    char *raw = malloc(n + 1);
    char *out = malloc(n * 4 + 64);
    if (!raw || !out) {
        free(raw);
        free(out);
        fail(g, "out of memory");
        return NULL;
    }
    if (n)
        memcpy(raw, g->buf + from, n);   /* (nothing written yet: no buffer to copy from) */
    raw[n] = '\0';
    g->len = from;
    size_t o = 0;
    const char *line = raw;
    for (;;) {
        const char *nl = strchr(line, '\n');
        const size_t ln = nl ? (size_t)(nl - line) : strlen(line);
        size_t b = 0;
        if (line != raw)
            while (b < ln && (line[b] == ' ' || line[b] == '\t'))
                ++b;   /* the separator has the indent */
        ccr_asm_escape(line + b, ln - b, out + o, n * 4 + 64 - o);
        o += strlen(out + o);
        if (!nl)
            break;
        o += (size_t)snprintf(out + o, n * 4 + 64 - o, "%s", sep);
        line = nl + 1;
    }
    free(raw);
    return out;
}

/* An instruction among C statements: __asm__ volatile("..."); */
static void emit_asm_line(gen *g, ccr_node_id id, unsigned depth)
{
    const bool statement = N(g, id)->parent != CCR_NONE && N(g, N(g, id)->parent)->kind == CCR_NODE_BLOCK;
    const size_t from = g->len;
    ++g->asm_depth;
    if (N(g, id)->flags & CCR_FLAG_NEW)
        print_new_asm(g, id, depth);
    else
        emit(g, id, depth);
    --g->asm_depth;
    char *text = take_escaped(g, from, "\\n\\t");
    if (!text)
        return;
    puts_(g, "__asm__ volatile (\"");
    puts_(g, text);
    puts_(g, statement ? "\");" : "\")");
    free(text);
}

/* asm("..."): its Block written again, as C strings, laid out as they were
 * (one per line, or all on one), lines split as they were ("\n\t"...). */
static void rebuild_asm_literal(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *b = N(g, id);
    const char *src = g->ast->source;
    bool multi = false;
    const char *sep = "\\n\\t";
    char indent[64] = "    ";
    if (!(b->flags & CCR_FLAG_NEW) && b->src_e > b->src_b && b->src_e <= g->ast->source_len) {
        for (uint32_t i = b->src_b; i < b->src_e; ++i)
            multi |= src[i] == '\n';
        bool tab = false, nl = false;
        for (uint32_t i = b->src_b; i + 1 < b->src_e; ++i) {
            if (src[i] == '\\' && src[i + 1] == 'n') {
                nl = true;
                tab |= i + 3 < b->src_e && src[i + 2] == '\\' && src[i + 3] == 't';
            }
        }
        sep = nl ? (tab ? "\\n\\t" : "\\n") : "\\n\\t";
        /* the indent of the second string's line */
        uint32_t i = b->src_b;
        while (i < b->src_e && src[i] != '\n')
            ++i;
        if (i < b->src_e) {
            uint32_t k = i + 1, n = 0;
            while (k + n < b->src_e && (src[k + n] == ' ' || src[k + n] == '\t') && n + 1 < sizeof indent)
                ++n;
            memcpy(indent, src + k, n);
            indent[n] = '\0';
        }
    } else {
        uint32_t count = 0;
        for (ccr_node_id c = b->first_child; c != CCR_NONE; c = N(g, c)->next_sibling)
            ++count;
        multi = count > 2;
    }
    if (b->first_child == CCR_NONE) {
        puts_(g, "\"\"");
        return;
    }
    ++g->asm_depth;
    bool first = true;
    for (ccr_node_id c = b->first_child; c != CCR_NONE && g->ok; c = N(g, c)->next_sibling) {
        const ccr_node *k = N(g, c);
        const size_t from = g->len;
        if (k->flags & CCR_FLAG_NEW)
            print_new_asm(g, c, depth + 1);
        else
            emit(g, c, depth + 1);
        if (k->flags & CCR_FLAG_COMMENTED)
            comment_out(g, from, 0);
        char *text = take_escaped(g, from, sep);
        if (!text)
            break;
        const bool last = N(g, c)->next_sibling == CCR_NONE;
        if (first || multi) {
            if (!first) {
                puts_(g, "\n");
                puts_(g, indent);
            }
            puts_(g, "\"");
        }
        puts_(g, text);
        if (!last)
            puts_(g, sep);
        if (multi || last)
            puts_(g, "\"");
        free(text);
        first = false;
    }
    --g->asm_depth;
}

/* A new asm volatile("..." : out : in : clobbers) from the palette. */
static void print_new_inline(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    puts_(g, ccr_ast_str(g->ast, n->name));
    if (*ccr_ast_str(g->ast, n->op)) {
        puts_(g, " ");
        puts_(g, ccr_ast_str(g->ast, n->op));
    }
    puts_(g, " (");
    const ccr_node_id blk = kid(g, id, 0);
    if (blk != CCR_NONE)
        emit(g, blk, depth + 1);
    else
        puts_(g, "\"\"");
    /* the slots after it, the trailing empty ones left out */
    ccr_node_id slots[3] = { kid(g, id, 1), kid(g, id, 2), kid(g, id, 3) };
    int used = 0;
    for (int i = 0; i < 3; ++i)
        if (slots[i] != CCR_NONE && !(N(g, slots[i])->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, slots[i])->name)))
            used = i + 1;
    for (int i = 0; i < used; ++i) {
        puts_(g, " :");
        if (slots[i] != CCR_NONE && !(N(g, slots[i])->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, slots[i])->name))) {
            puts_(g, " ");
            emit(g, slots[i], depth + 1);
        }
    }
    puts_(g, ")");
}

static void print_new_inner(gen *g, ccr_node_id id, unsigned depth);

/* Where node `id` went: [b, e) of the output; `clean` when its own text was
 * copied as it was, so its children's places follow from their source. */
static void mark_out(gen *g, ccr_node_id id, size_t b, bool clean)
{
    if (!g->map || !g->ok)
        return;
    g->map[id].b = (uint32_t)b;
    g->map[id].e = (uint32_t)g->len;
    g->map[id].clean = clean;
}

static void print_new(gen *g, ccr_node_id id, unsigned depth)
{
    const size_t b = g->len;
    print_new_inner(g, id, depth);
    mark_out(g, id, b, false);
}

static void print_new_inner(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    if (g->ast->lang == CCR_LANG_CMAKE) {
        print_new_cmake(g, id, depth);
        return;
    }
    if (g->ast->lang == CCR_LANG_ASM || (n->flags & CCR_FLAG_ASM)) {
        print_new_asm(g, id, depth);
        return;
    }
    const ccr_node_id p = n->parent;
    const bool statement = p != CCR_NONE && N(g, p)->kind == CCR_NODE_BLOCK;

    const char *name = ccr_ast_str(g->ast, n->name);
    const char *type = ccr_ast_str(g->ast, n->type);
    const char *op = ccr_ast_str(g->ast, n->op);

    switch (n->kind) {
    case CCR_NODE_TEXT:
        if (n->flags & CCR_FLAG_WORD)
            break;   /* printed by its block, from the block's name/type */
        if (!statement && !*ccr_ast_str(g->ast, n->name) &&
            (p == CCR_NONE || N(g, p)->kind != CCR_NODE_FOR)) {   /* for (;;) is fine */
            uint32_t line = 0;
            for (ccr_node_id a = p; a != CCR_NONE && !line; a = N(g, a)->parent)
                line = N(g, a)->line;
            fail(g, ccr_tr(CCR_T_GEN_EMPTY_SLOT), line);
            break;
        }
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
    case CCR_NODE_SWITCH:
        puts_(g, "switch (");
        slot(g, kid(g, id, 0), depth);
        puts_(g, ") ");
        body(g, kid(g, id, 1), depth);
        break;
    case CCR_NODE_CASE:
        puts_(g, "case ");
        slot(g, kid(g, id, 0), depth);
        puts_(g, ": ");
        body(g, kid(g, id, 1), depth);
        break;
    case CCR_NODE_DEFAULT:
        puts_(g, "default: ");
        body(g, kid(g, id, 0), depth);
        break;
    case CCR_NODE_ASM:
        print_new_inline(g, id, depth);
        if (statement)
            puts_(g, ";");
        break;
    case CCR_NODE_GOTO:
        if (filled(g, id, name)) {
            puts_(g, "goto ");
            puts_(g, name);
            puts_(g, ";");
        }
        break;
    case CCR_NODE_LABEL:
        if (filled(g, id, name)) {
            puts_(g, name);
            puts_(g, ": ;");   /* a statement after it, so it may end a block */
        }
        break;
    case CCR_NODE_VAR: {
        if (!filled(g, id, type) || !filled(g, id, name))
            break;
        puts_(g, type);
        if (type[strlen(type) - 1] != '*')
            puts_(g, " ");   /* int *p, not int * p */
        puts_(g, name);
        ccr_node_id init = arg(g, id, 0);
        if (init != CCR_NONE) {
            puts_(g, " = ");
            slot(g, init, depth);
        }
        puts_(g, ";");
        break;
    }

    /* expressions, statements on their own or in slots */
    case CCR_NODE_BINARY_OP: {
        slot(g, arg(g, id, 0), depth);
        puts_(g, " ");
        puts_(g, op);
        puts_(g, " ");
        slot(g, arg(g, id, 1), depth);
        goto expression_end;
    }
    case CCR_NODE_UNARY_OP: {
        const ccr_node_id x = arg(g, id, 0);
        if (n->flags & CCR_FLAG_POSTFIX) {
            slot(g, x, depth);
            puts_(g, op);
        } else {
            puts_(g, op);
            /* - -x, not --x; & &x, not &&x */
            const size_t mark = g->len;
            slot(g, x, depth);
            if (g->ok && g->len > mark && *op && g->buf[mark] == op[strlen(op) - 1]) {
                put(g, " ", 1);
                memmove(g->buf + mark + 1, g->buf + mark, g->len - mark - 1);
                g->buf[mark] = ' ';
            }
        }
        goto expression_end;
    }
    case CCR_NODE_TERNARY:
        slot(g, arg(g, id, 0), depth);
        puts_(g, " ? ");
        slot(g, arg(g, id, 1), depth);
        puts_(g, " : ");
        slot(g, arg(g, id, 2), depth);
        goto expression_end;
    case CCR_NODE_INDEX:
        slot(g, arg(g, id, 0), depth);
        puts_(g, "[");
        slot(g, arg(g, id, 1), depth);
        puts_(g, "]");
        goto expression_end;
    case CCR_NODE_MEMBER:
        if (!filled(g, id, name))
            break;
        slot(g, arg(g, id, 0), depth);
        puts_(g, (n->flags & CCR_FLAG_ARROW) ? "->" : ".");
        puts_(g, name);
        goto expression_end;
    case CCR_NODE_CALL: {
        if (!filled(g, id, name))
            break;
        /* v.push_back(x): the object and its method, then the arguments */
        const ccr_node_id callee = arg(g, id, 0);
        const bool method = callee != CCR_NONE && N(g, callee)->kind == CCR_NODE_MEMBER;
        if (method)
            slot(g, callee, depth);
        else
            puts_(g, name);
        puts_(g, "(");
        bool first = true;
        const bool one = arg(g, id, method ? 2 : 1) == CCR_NONE;
        for (uint32_t i = method ? 1 : 0; arg(g, id, i) != CCR_NONE; ++i) {
            ccr_node_id a = arg(g, id, i);
            if (one && N(g, a)->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, a)->name))
                continue;   /* f( ) with nothing typed: f(); with more slots, each must be filled */
            if (!first)
                puts_(g, ", ");
            first = false;
            slot(g, a, depth);
        }
        puts_(g, ")");
        goto expression_end;
    }
    case CCR_NODE_CAST:
        if (!filled(g, id, type))
            break;
        if (strstr(ccr_ast_str(g->ast, n->raw), "_cast")) {   /* static_cast<int>(x) */
            puts_(g, ccr_ast_str(g->ast, n->raw));
            puts_(g, "<");
            puts_(g, type);
            puts_(g, ">(");
            slot(g, arg(g, id, 0), depth);
            puts_(g, ")");
            goto expression_end;
        }
        puts_(g, "(");
        puts_(g, type);
        puts_(g, ")");
        slot(g, arg(g, id, 0), depth);
        goto expression_end;
    case CCR_NODE_NEW: {
        if (!filled(g, id, type))
            break;
        puts_(g, "new ");
        puts_(g, type);
        puts_(g, "(");
        const ccr_node_id a = arg(g, id, 0);
        if (a != CCR_NONE && !(N(g, a)->kind == CCR_NODE_TEXT && !*ccr_ast_str(g->ast, N(g, a)->name)))
            slot(g, a, depth);
        puts_(g, ")");
        goto expression_end;
    }
    case CCR_NODE_DELETE:
        puts_(g, (n->flags & CCR_FLAG_ARRAY_FORM) ? "delete[] " : "delete ");
        slot(g, arg(g, id, 0), depth);
        goto expression_end;
    case CCR_NODE_FOR_RANGE:
        if (!filled(g, id, type) || !filled(g, id, name))
            break;
        puts_(g, "for (");
        puts_(g, type);
        if (type[strlen(type) - 1] != '&' && type[strlen(type) - 1] != '*')
            puts_(g, " ");
        puts_(g, name);
        puts_(g, " : ");
        slot(g, arg(g, id, 0), depth);
        puts_(g, ") ");
        body(g, arg(g, id, 1), depth);
        break;
    case CCR_NODE_NAMESPACE:
        if (!filled(g, id, name))
            break;
        puts_(g, "namespace ");
        puts_(g, name);
        puts_(g, " ");
        body(g, arg(g, id, 0), depth);
        break;
    case CCR_NODE_ACCESS:
        puts_(g, name);
        puts_(g, ":");
        break;
    case CCR_NODE_USING:
        if (!filled(g, id, name))
            break;
        puts_(g, "using namespace ");
        puts_(g, name);
        puts_(g, ";");
        break;
    case CCR_NODE_SIZEOF:
        if (arg(g, id, 0) != CCR_NONE) {
            puts_(g, "sizeof ");
            slot(g, arg(g, id, 0), depth);
        } else if (filled(g, id, type)) {
            puts_(g, "sizeof(");
            puts_(g, type);
            puts_(g, ")");
        }
        goto expression_end;
    /* the file's own declarations */
    case CCR_NODE_INCLUDE:
        if (!filled(g, id, name))
            break;
        puts_(g, "#include ");
        puts_(g, (n->flags & CCR_FLAG_ANGLED) ? "<" : "\"");
        puts_(g, name);
        puts_(g, (n->flags & CCR_FLAG_ANGLED) ? ">" : "\"");
        break;
    case CCR_NODE_MACRO_DEFINE: {
        if (!filled(g, id, name))
            break;
        puts_(g, "#define ");
        puts_(g, name);
        const ccr_node_id v = arg(g, id, 0);
        if (v != CCR_NONE && *ccr_ast_str(g->ast, N(g, v)->name)) {
            puts_(g, " ");
            emit(g, v, depth + 1);
        }
        break;
    }
    case CCR_NODE_TYPEDEF:
        if (!filled(g, id, type) || !filled(g, id, name))
            break;
        puts_(g, "typedef ");
        puts_(g, type);
        puts_(g, " ");
        puts_(g, name);
        puts_(g, ";");
        break;
    case CCR_NODE_ENUM: {
        if (!filled(g, id, name))
            break;
        puts_(g, "enum ");
        puts_(g, name);
        puts_(g, " { ");
        const ccr_node_id v = arg(g, id, 0);
        if (v != CCR_NONE)
            emit(g, v, depth + 1);
        puts_(g, " };");
        break;
    }
    case CCR_NODE_STRUCT:
    case CCR_NODE_UNION:
        if (!filled(g, id, name))
            break;
        puts_(g, n->kind == CCR_NODE_UNION ? "union " : (n->flags & CCR_FLAG_CLASS) ? "class " : "struct ");
        puts_(g, name);
        puts_(g, " ");
        body(g, arg(g, id, 0), depth);
        puts_(g, ";");
        break;
    case CCR_NODE_FUNCTION: {
        if (!filled(g, id, type) || !filled(g, id, name))
            break;
        puts_(g, type);
        if (type[strlen(type) - 1] != '*')
            puts_(g, " ");
        puts_(g, name);
        puts_(g, "(");
        const ccr_node_id params = arg(g, id, 0);
        if (params != CCR_NONE && N(g, params)->kind == CCR_NODE_TEXT)
            puts_(g, ccr_ast_str(g->ast, N(g, params)->name));   /* f() is fine too */
        else if (params != CCR_NONE)
            emit(g, params, depth + 1);
        puts_(g, ")");
        const ccr_node_id fbody = arg(g, id, 1);
        if (n->flags & CCR_FLAG_DEFINITION) {
            puts_(g, "\n");
            body(g, fbody, depth);
        } else {
            puts_(g, ";");
        }
        break;
    }

    case CCR_NODE_REF:
        puts_(g, name);
    expression_end:
        if (statement)
            puts_(g, ";");
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

/* Children sorted by the place they fill in the parent's text (insertion
 * sort: usually sorted already). */
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
        while (j > 0 && N(g, v[j - 1])->place_b > N(g, c)->place_b) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = c;
    }
    *count = n;
    return v;
}

static void emit_inner(gen *g, ccr_node_id id, unsigned depth);

/* A C++ class or namespace written in the file (members dropped in go
 * between the lines of its text). */
static bool cpp_container(const gen *g, ccr_node_id id)
{
    const ccr_node *n = N(g, id);
    return g->ast->cpp && !(n->flags & CCR_FLAG_NEW) &&
           (n->kind == CCR_NODE_NAMESPACE || n->kind == CCR_NODE_STRUCT || n->kind == CCR_NODE_UNION);
}

/* The indent of the members around `kid` in `parent`: the line of a
 * written member near it, else the container's plus 4. */
static void member_indent(const gen *g, ccr_node_id parent, ccr_node_id kid, char *out, size_t cap)
{
    const ccr_ast *a = g->ast;
    ccr_node_id near = CCR_NONE;
    for (ccr_node_id c = N(g, parent)->first_child; c != CCR_NONE; c = N(g, c)->next_sibling) {
        if (c == kid && near != CCR_NONE)
            break;
        const ccr_node *k = N(g, c);
        if (!(k->flags & CCR_FLAG_NEW) && k->kind != CCR_NODE_ACCESS && k->src_e > k->src_b)
            near = c;   /* public: is indented less than the members under it */
    }
    const uint32_t at = near != CCR_NONE ? N(g, near)->src_b : N(g, parent)->src_b;
    uint32_t b = at;
    while (b > 0 && a->source[b - 1] != '\n')
        --b;
    size_t n = 0;
    while (b + n < at && (a->source[b + n] == ' ' || a->source[b + n] == '\t') && n < cap - 5)
        ++n;
    memcpy(out, a->source + b, n);
    if (near == CCR_NONE) {   /* nothing written in it yet: one step in */
        memcpy(out + n, "    ", 4);
        n += 4;
    }
    out[n] = '\0';
}

/* The lines printed since `mark` (but the first) start with `indent`. */
static void indent_lines(gen *g, size_t mark, const char *indent)
{
    const size_t il = strlen(indent);
    if (!il || !g->ok)
        return;
    for (size_t i = mark; i < g->len; ++i) {
        if (g->buf[i] != '\n' || i + 1 >= g->len)
            continue;
        put(g, indent, il);   /* grow by il, then shift the rest right */
        if (!g->ok)
            return;
        memmove(g->buf + i + 1 + il, g->buf + i + 1, g->len - il - (i + 1));
        memcpy(g->buf + i + 1, indent, il);
        i += il;
    }
}

static void emit_asm_line(gen *g, ccr_node_id id, unsigned depth);

static void emit(gen *g, ccr_node_id id, unsigned depth)
{
    if (!g->ok)
        return;
    const size_t b = g->len;
    if ((N(g, id)->flags & CCR_FLAG_ASM) && g->ast->lang == CCR_LANG_C && !g->asm_depth) {
        emit_asm_line(g, id, depth);   /* an instruction among C statements */
        mark_out(g, id, b, false);
        return;
    }
    if (!has_dirty(g, id, 0)) {
        put_src(g, N(g, id)->src_b, N(g, id)->src_e);
        mark_out(g, id, b, true);
        return;
    }
    emit_inner(g, id, depth);
    if (g->map && g->map[id].b == UINT32_MAX)   /* print_new marked it already otherwise */
        mark_out(g, id, b, false);
}

static void emit_inner(gen *g, ccr_node_id id, unsigned depth)
{
    const ccr_node *n = N(g, id);
    if (depth > 4000) {
        fail(g, "code nested too deeply");
        return;
    }
    if (n->kind == CCR_NODE_BLOCK && n->parent != CCR_NONE && N(g, n->parent)->kind == CCR_NODE_ASM &&
        g->ast->lang == CCR_LANG_C) {
        rebuild_asm_literal(g, id, depth);   /* asm("..."): the string, written again */
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
     * its children's places, and fill each place with that child */
    uint32_t count;
    ccr_node_id *kids = sorted_children(g, id, &count);
    uint32_t cur = n->src_b;
    for (uint32_t i = 0; i < count && g->ok; ++i) {
        const ccr_node *k = N(g, kids[i]);
        const bool dirty = has_dirty(g, kids[i], 0);
        /* pasted: its text is after the file's end, its place a point in it */
        const bool pasted = !(k->flags & CCR_FLAG_NEW) && k->src_b >= n->src_e && n->src_e > 0;
        if (cpp_container(g, id) && ((k->flags & CCR_FLAG_NEW) || pasted) && k->place_b == k->place_e &&
            k->place_b >= cur) {
            /* dropped into a C++ class or namespace: a line of its own, indented
             * like the members around it */
            put_src(g, cur, k->place_b);
            cur = k->place_b;
            char indent[64];
            member_indent(g, id, kids[i], indent, sizeof indent);
            puts_(g, "\n");
            puts_(g, indent);
            const size_t mark = g->len;
            if (pasted)
                emit(g, kids[i], depth + 1);
            else
                print_new(g, kids[i], depth + 1);
            indent_lines(g, mark, indent);
            continue;
        }
        if (n->kind == CCR_NODE_TRANSLATION_UNIT && ((k->flags & CCR_FLAG_NEW) || pasted) &&
            k->place_b == k->place_e && k->place_b >= cur) {
            /* a declaration from the palette: on lines of its own, here */
            put_src(g, cur, k->place_b);
            cur = k->place_b;
            const bool directive = k->kind == CCR_NODE_INCLUDE || k->kind == CCR_NODE_MACRO_DEFINE;
            if (g->len > 0 && g->buf[g->len - 1] != '\n')
                puts_(g, "\n");
            if (!directive && g->len > 1 && !(g->buf[g->len - 1] == '\n' && g->buf[g->len - 2] == '\n'))
                puts_(g, "\n");   /* a blank line before a function or a type */
            if (pasted)
                emit(g, kids[i], depth + 1);
            else
                print_new(g, kids[i], depth + 1);
            puts_(g, "\n");
            continue;
        }
        if (lives_in_gaps(k->kind) ||
            (k->place_e <= k->place_b && !(g->ast->lang != CCR_LANG_C && k->place_b >= cur))) {
            if (dirty)
                fail(g, ccr_tr(CCR_T_GEN_LOST), k->line);
            continue;
        }
        if (k->place_b < cur) {
            if (k->place_e <= cur && !dirty)
                continue;   /* inside text already copied (struct inside typedef) */
            fail(g, "line %u: overlapping code (from a macro?) around a change", k->line);
            break;
        }
        if (k->place_e > n->src_e) {
            fail(g, "line %u: code outside its parent (from a macro?) around a change", k->line);
            break;
        }
        put_src(g, cur, k->place_b);
        emit_operand(g, kids[i], depth + 1);
        cur = k->place_e;
    }
    put_src(g, cur, n->src_e);
    free(kids);
}

/* --------------------------------------------- what may be moved */

static const ccr_node *AN(const ccr_ast *a, ccr_node_id id) { return &a->nodes[id]; }

static bool is_expr_kind(ccr_node_kind k)
{
    switch (k) {
    case CCR_NODE_CALL: case CCR_NODE_BINARY_OP: case CCR_NODE_UNARY_OP:
    case CCR_NODE_TERNARY: case CCR_NODE_REF: case CCR_NODE_MEMBER:
    case CCR_NODE_INDEX: case CCR_NODE_CAST: case CCR_NODE_SIZEOF:
    case CCR_NODE_INIT_LIST: case CCR_NODE_COMPOUND_LIT: case CCR_NODE_MACRO_VALUE:
    case CCR_NODE_TEXT:
    case CCR_NODE_INT_LIT: case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT: case CCR_NODE_STRING_LIT:
    case CCR_NODE_NEW: case CCR_NODE_DELETE:
        return true;
    default:
        return false;
    }
}

/*
 * Saving can rewrite one child of `id` only if its children sit in its
 * text one after another, each inside it: then the text between them is
 * the parent's own (operators, commas, parens). Code made by a macro
 * breaks this: tokens of the macro's body all claim the whole macro call,
 * its arguments claim nothing at all. That is left alone.
 */
static bool spliceable(const ccr_ast *ast, ccr_node_id id)
{
    const ccr_node *n = AN(ast, id);
    if (n->flags & CCR_FLAG_NEW)
        return true;   /* printed, not spliced */
    if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_DIRTY))
        return !(n->flags & CCR_FLAG_UNSAFE_GAP);   /* rebuilt statement by statement */
    if (n->src_e <= n->src_b)
        return false;
    uint32_t cur = n->src_b;
    const bool container = ast->cpp && (n->kind == CCR_NODE_NAMESPACE || n->kind == CCR_NODE_STRUCT ||
                                         n->kind == CCR_NODE_UNION);
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = AN(ast, c)->next_sibling) {
        const ccr_node *k = AN(ast, c);
        if (container && (k->flags & CCR_FLAG_NEW) && k->place_b == k->place_e && k->place_b >= cur)
            continue;   /* a member dropped in: printed on a line of its own */
        if (k->place_e <= k->place_b || k->place_b < cur || k->place_e > n->src_e)
            return false;
        /* a child that is all of its parent: the parent has no text of its
         * own (a macro), unless it is the Block around a braceless body */
        if (n->kind != CCR_NODE_BLOCK && !(k->flags & CCR_FLAG_NEW) &&
            k->place_b == n->src_b && k->place_e == n->src_e &&
            !(ast->lang != CCR_LANG_C && n->kind == CCR_NODE_FUNCTION))   /* a file of commands only */
            return false;
        cur = k->place_e;
    }
    return true;
}

/*
 * Can `id` be taken out of the slot it fills? It must be an expression in
 * an expression or statement (not a statement itself), with text of its own
 * to carry, and everything from its parent up to its function must be
 * safe to rewrite.
 */
static bool chain_ok(const ccr_ast *ast, ccr_node_id first);

bool ccr_codegen_can_reprint(const ccr_ast *ast, ccr_node_id id)
{
    const ccr_node *n = AN(ast, id);
    if (n->kind != CCR_NODE_BINARY_OP && n->kind != CCR_NODE_UNARY_OP)
        return false;
    if (n->flags & CCR_FLAG_NEW)
        return true;   /* printed anyway */
    /* its own text, its operands in it one after another (no macro) */
    if (n->src_e <= n->src_b || n->place_e <= n->place_b || !spliceable(ast, id))
        return false;
    uint32_t kids = 0;
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = AN(ast, c)->next_sibling, ++kids)
        if (!(AN(ast, c)->flags & (CCR_FLAG_NEW | CCR_FLAG_MOVED)) && AN(ast, c)->src_e <= AN(ast, c)->src_b)
            return false;
    if (kids != (n->kind == CCR_NODE_BINARY_OP ? 2u : 1u))
        return false;
    return chain_ok(ast, n->parent);
}

bool ccr_codegen_can_move(const ccr_ast *ast, ccr_node_id id)
{
    const ccr_node *n = AN(ast, id);
    if (!is_expr_kind(n->kind) || n->parent == CCR_NONE || (n->flags & CCR_FLAG_WORD))
        return false;   /* a name or type field stays with its block */
    if (AN(ast, n->parent)->kind == CCR_NODE_BLOCK)
        return false;   /* a statement, or a loose reporter itself */
    if (n->kind == CCR_NODE_TEXT ? !*ccr_ast_str(ast, n->name)
                                 : (!(n->flags & CCR_FLAG_NEW) && n->src_e <= n->src_b))
        return false;   /* an empty slot: nothing to take */
    return chain_ok(ast, n->parent);
}

/*
 * Everything from `first` up to its function is spliced on the way down
 * (blocks included: a block a macro made has statements that overlap);
 * the file itself and loose roots are not.
 */
static bool chain_ok(const ccr_ast *ast, ccr_node_id first)
{
    ccr_node_id top = CCR_NONE;
    for (ccr_node_id a = first; a != CCR_NONE && AN(ast, a)->parent != CCR_NONE;
         a = AN(ast, a)->parent) {
        if (!spliceable(ast, a))
            return false;
        top = a;
    }
    /* and the file must hold its top-level declaration on its own: not
     * `typedef enum {...} name;`, where the enum and the typedef share text */
    if (top != CCR_NONE && AN(ast, top)->parent != CCR_NONE &&
        AN(ast, AN(ast, top)->parent)->kind == CCR_NODE_TRANSLATION_UNIT) {
        const ccr_node *t = AN(ast, top);
        for (ccr_node_id c = AN(ast, t->parent)->first_child; c != CCR_NONE;
             c = AN(ast, c)->next_sibling) {
            const ccr_node *k = AN(ast, c);
            if (c == top || lives_in_gaps(k->kind) || k->place_e <= k->place_b)
                continue;
            if (k->place_b < t->place_e && t->place_b < k->place_e)
                return false;
        }
    }
    return true;
}

/* Nodes copied with their parent's text: where they are follows from it. */
static void derive_map(const ccr_ast *ast, ccr_out_range *map, ccr_node_id id, unsigned depth)
{
    if (depth > 4000)
        return;
    const ccr_node *n = &ast->nodes[id];
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling) {
        const ccr_node *k = &ast->nodes[c];
        if (map[c].b == UINT32_MAX && map[id].clean && map[id].b != UINT32_MAX &&
            k->src_b >= n->src_b && k->src_e <= n->src_e && k->src_e >= k->src_b) {
            map[c].b = map[id].b + (k->src_b - n->src_b);
            map[c].e = map[id].b + (k->src_e - n->src_b);
            map[c].clean = true;
        }
        derive_map(ast, map, c, depth + 1);
    }
}

/* What new instructions are indented with: what the file's are. */
static void find_asm_indent(gen *g)
{
    for (uint32_t i = 0; i < g->ast->node_count && !g->asm_indent[0]; ++i) {
        const ccr_node *n = &g->ast->nodes[i];
        if (n->kind == CCR_NODE_BLOCK && !(n->flags & CCR_FLAG_NEW) && n->first_child != CCR_NONE &&
            (g->ast->nodes[n->first_child].flags & CCR_FLAG_ASM))
            snprintf(g->asm_indent, sizeof g->asm_indent, "%s", ccr_ast_str(g->ast, n->indent));
    }
}

bool ccr_codegen(const ccr_ast *ast, char **out, size_t *out_len, char *err, size_t err_cap)
{
    return ccr_codegen_mapped(ast, out, out_len, NULL, err, err_cap);
}

bool ccr_codegen_mapped(const ccr_ast *ast, char **out, size_t *out_len, ccr_out_range *map,
                        char *err, size_t err_cap)
{
    *out = NULL;
    *out_len = 0;
    if (err_cap)
        err[0] = '\0';
    if (!ast->source || ast->node_count == 0) {
        snprintf(err, err_cap, "nothing loaded");
        return false;
    }

    gen g = { .ast = ast, .ok = true, .err = err, .err_cap = err_cap, .map = map };
    find_asm_indent(&g);
    for (uint32_t i = 0; map && i < ast->node_count; ++i)
        map[i] = (ccr_out_range){ UINT32_MAX, UINT32_MAX, false };
    g.dirty = calloc(ast->node_count, 1);
    if (!g.dirty) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }

    emit(&g, 0, 0);   /* node 0 is the translation unit: the whole file */
    free(g.dirty);
    if (g.ok && map)
        derive_map(ast, map, 0, 0);
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

bool ccr_codegen_node(const ccr_ast *ast, ccr_node_id id, char **out, size_t *out_len, char *err, size_t err_cap)
{
    *out = NULL;
    *out_len = 0;
    if (err_cap)
        err[0] = '\0';
    if (!ast->source || id >= ast->node_count) {
        snprintf(err, err_cap, "nothing loaded");
        return false;
    }
    gen g = { .ast = ast, .ok = true, .err = err, .err_cap = err_cap };
    find_asm_indent(&g);
    g.dirty = calloc(ast->node_count, 1);
    if (!g.dirty) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    emit(&g, id, 1);
    free(g.dirty);
    if (!g.ok || !g.buf) {
        free(g.buf);
        if (g.ok)
            snprintf(err, err_cap, "nothing to print");
        return false;
    }
    *out = g.buf;
    *out_len = g.len;
    return true;
}
