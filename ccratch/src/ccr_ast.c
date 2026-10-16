#include "ccr_ast.h"

#include <stdlib.h>
#include <string.h>

static const char *const g_kind_names[CCR_NODE_KIND_COUNT] = {
#define X(k, name) name,
    CCR_NODE_KINDS(X)
#undef X
};

const char *ccr_node_kind_name(ccr_node_kind kind)
{
    if ((unsigned)kind >= CCR_NODE_KIND_COUNT)
        return "???";
    return g_kind_names[kind];
}

/* Grows *buf so it can hold `need` elements of `elem` bytes. */
static bool grow(void **buf, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint32_t cap2 = *cap ? *cap : 64;
    while (cap2 < need) {
        if (cap2 > UINT32_MAX / 2)
            return false;
        cap2 *= 2;
    }
    void *p = realloc(*buf, (size_t)cap2 * elem);
    if (!p)
        return false;
    *buf = p;
    *cap = cap2;
    return true;
}

bool ccr_ast_init(ccr_ast *ast)
{
    memset(ast, 0, sizeof *ast);
    /* offset 0 is reserved for the empty string */
    if (!grow((void **)&ast->strings, &ast->str_cap, 1024, 1))
        return false;
    ast->strings[0] = '\0';
    ast->str_len = 1;
    return true;
}

void ccr_ast_free(ccr_ast *ast)
{
    free(ast->nodes);
    free(ast->strings);
    free(ast->diags);
    free(ast->source);
    memset(ast, 0, sizeof *ast);
}

ccr_str ccr_ast_intern(ccr_ast *ast, const char *s)
{
    if (!s || !*s)
        return 0;
    size_t n = strlen(s) + 1;
    if (n > UINT32_MAX - ast->str_len)
        return 0;
    if (!grow((void **)&ast->strings, &ast->str_cap, ast->str_len + (uint32_t)n, 1))
        return 0;
    ccr_str at = ast->str_len;
    memcpy(ast->strings + at, s, n);
    ast->str_len += (uint32_t)n;
    return at;
}

const char *ccr_ast_str(const ccr_ast *ast, ccr_str s)
{
    return (s < ast->str_len) ? ast->strings + s : "";
}

ccr_node_id ccr_ast_add(ccr_ast *ast, ccr_node_kind kind, ccr_node_id parent)
{
    if (ast->node_count == CCR_NONE)
        return CCR_NONE;
    if (!grow((void **)&ast->nodes, &ast->node_cap, ast->node_count + 1, sizeof(ccr_node)))
        return CCR_NONE;

    ccr_node_id id = ast->node_count++;
    ccr_node *n = &ast->nodes[id];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->parent = CCR_NONE;
    n->first_child = n->last_child = n->next_sibling = CCR_NONE;

    if (parent != CCR_NONE)
        ccr_ast_append(ast, parent, id);
    return id;
}

/* ------------------------------------------------------------ editing */

ccr_node_links ccr_ast_links(const ccr_ast *ast, ccr_node_id id)
{
    const ccr_node *n = &ast->nodes[id];
    return (ccr_node_links){ id, n->parent, n->first_child, n->last_child,
                             n->next_sibling, n->flags, n->name, n->type,
                             n->place_b, n->place_e };
}

void ccr_ast_set_links(ccr_ast *ast, const ccr_node_links *l)
{
    ccr_node *n = &ast->nodes[l->id];
    n->parent = l->parent;
    n->first_child = l->first_child;
    n->last_child = l->last_child;
    n->next_sibling = l->next_sibling;
    n->flags = l->flags;
    n->name = l->name;
    n->type = l->type;
    n->place_b = l->place_b;
    n->place_e = l->place_e;
}

void ccr_ast_touch(ccr_ast *ast, ccr_node_id id)
{
    ccr_journal *j = ast->journal;
    if (!j || id == CCR_NONE)
        return;
    for (uint32_t i = 0; i < j->count; ++i)
        if (j->old[i].id == id)
            return;   /* the first state is the one to go back to */
    if (!grow((void **)&j->old, &j->cap, j->count + 1, sizeof(ccr_node_links))) {
        j->oom = true;
        return;
    }
    j->old[j->count++] = ccr_ast_links(ast, id);
}

void ccr_ast_unlink(ccr_ast *ast, ccr_node_id id)
{
    ccr_node *n = &ast->nodes[id];
    if (n->parent == CCR_NONE)
        return;
    ccr_ast_touch(ast, id);
    ccr_ast_touch(ast, n->parent);
    ccr_node *p = &ast->nodes[n->parent];

    ccr_node_id prev = CCR_NONE;
    if (p->first_child == id) {
        p->first_child = n->next_sibling;
    } else {
        for (prev = p->first_child; prev != CCR_NONE; prev = ast->nodes[prev].next_sibling)
            if (ast->nodes[prev].next_sibling == id)
                break;
        if (prev != CCR_NONE) {
            ccr_ast_touch(ast, prev);
            ast->nodes[prev].next_sibling = n->next_sibling;
        }
    }
    if (p->last_child == id)
        p->last_child = prev;

    n->parent = CCR_NONE;
    n->next_sibling = CCR_NONE;
}

void ccr_ast_append(ccr_ast *ast, ccr_node_id parent, ccr_node_id id)
{
    ccr_ast_unlink(ast, id);
    ccr_ast_touch(ast, id);
    ccr_ast_touch(ast, parent);
    ccr_ast_touch(ast, ast->nodes[parent].last_child);
    ccr_node *p = &ast->nodes[parent];
    ccr_node *n = &ast->nodes[id];
    n->parent = parent;
    n->next_sibling = CCR_NONE;
    if (p->last_child == CCR_NONE)
        p->first_child = id;
    else
        ast->nodes[p->last_child].next_sibling = id;
    p->last_child = id;
}

void ccr_ast_insert_first(ccr_ast *ast, ccr_node_id parent, ccr_node_id id)
{
    ccr_ast_unlink(ast, id);
    ccr_ast_touch(ast, id);
    ccr_ast_touch(ast, parent);
    ccr_node *p = &ast->nodes[parent];
    ccr_node *n = &ast->nodes[id];
    n->parent = parent;
    n->next_sibling = p->first_child;
    p->first_child = id;
    if (p->last_child == CCR_NONE)
        p->last_child = id;
}

void ccr_ast_insert_after(ccr_ast *ast, ccr_node_id ref, ccr_node_id id)
{
    if (ref == id)
        return;
    ccr_ast_unlink(ast, id);
    ccr_node_id parent = ast->nodes[ref].parent;
    ccr_ast_touch(ast, id);
    ccr_ast_touch(ast, ref);
    ccr_ast_touch(ast, parent);
    ccr_node *n = &ast->nodes[id];
    n->parent = parent;
    n->next_sibling = ast->nodes[ref].next_sibling;
    ast->nodes[ref].next_sibling = id;
    if (parent != CCR_NONE && ast->nodes[parent].last_child == ref)
        ast->nodes[parent].last_child = id;
}

static ccr_node_id nth_child(const ccr_ast *ast, ccr_node_id id, uint32_t index)
{
    ccr_node_id c = ast->nodes[id].first_child;
    while (c != CCR_NONE && index-- > 0)
        c = ast->nodes[c].next_sibling;
    return c;
}

/* Puts `child` inside a new Block that takes its place among the siblings. */
static void wrap_in_block(ccr_ast *ast, ccr_node_id child)
{
    ccr_node_id block = ccr_ast_add(ast, CCR_NODE_BLOCK, CCR_NONE);
    if (block == CCR_NONE)
        return;
    ast->nodes[block].flags |= CCR_FLAG_IMPLICIT;
    ast->nodes[block].line = ast->nodes[child].line;
    ast->nodes[block].column = ast->nodes[child].column;
    ast->nodes[block].src_b = ast->nodes[child].src_b;
    ast->nodes[block].src_e = ast->nodes[child].src_e;
    ccr_ast_insert_after(ast, child, block);
    ccr_ast_append(ast, block, child);
}

void ccr_ast_wrap_bodies(ccr_ast *ast)
{
    const uint32_t count = ast->node_count;  /* new Blocks need no wrapping */
    for (ccr_node_id id = 0; id < count; ++id) {
        ccr_node_id body[2] = { CCR_NONE, CCR_NONE };
        switch (ast->nodes[id].kind) {
        case CCR_NODE_IF:
            body[0] = nth_child(ast, id, 1);
            body[1] = nth_child(ast, id, 2);
            break;
        case CCR_NODE_WHILE:
            body[0] = nth_child(ast, id, 1);
            break;
        case CCR_NODE_DO_WHILE:
            body[0] = nth_child(ast, id, 0);
            break;
        case CCR_NODE_FOR:
            body[0] = ast->nodes[id].last_child;
            break;
        default:
            continue;
        }
        for (int i = 0; i < 2; ++i)
            if (body[i] != CCR_NONE && ast->nodes[body[i]].kind != CCR_NODE_BLOCK)
                wrap_in_block(ast, body[i]);
    }
}

bool ccr_ast_add_diag(ccr_ast *ast, ccr_diag d)
{
    if (!grow((void **)&ast->diags, &ast->diag_cap, ast->diag_count + 1, sizeof(ccr_diag)))
        return false;
    ast->diags[ast->diag_count++] = d;
    return true;
}

uint32_t ccr_ast_count_diags(const ccr_ast *ast, ccr_diag_severity at_least)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < ast->diag_count; ++i)
        if (ast->diags[i].severity >= at_least)
            ++count;
    return count;
}

/* ---------------------------------------------------------------- dump */

static void dump_node_line(const ccr_ast *ast, const ccr_node *n, FILE *out)
{
    const char *name = ccr_ast_str(ast, n->name);
    const char *type = ccr_ast_str(ast, n->type);
    const char *op   = ccr_ast_str(ast, n->op);

    fputs(ccr_node_kind_name(n->kind), out);

    switch (n->kind) {
    case CCR_NODE_INCLUDE:
        fprintf(out, n->flags & CCR_FLAG_ANGLED ? " <%s>" : " \"%s\"", name);
        break;
    case CCR_NODE_BINARY_OP:
        fprintf(out, " %s", op);
        break;
    case CCR_NODE_UNARY_OP:
        fprintf(out, (n->flags & CCR_FLAG_POSTFIX) ? " x%s" : " %sx", op);
        break;
    case CCR_NODE_OTHER:
        fprintf(out, " (%s)%s%s", ccr_ast_str(ast, n->raw), *name ? " " : "", name);
        break;
    case CCR_NODE_MEMBER:
        fprintf(out, " %s%s", (n->flags & CCR_FLAG_ARROW) ? "->" : ".", name);
        break;
    case CCR_NODE_DESIGNATOR:
        fprintf(out, " .%s", name);
        break;
    case CCR_NODE_SIZEOF:
        if (n->first_child == CCR_NONE)
            fprintf(out, " %s", ccr_ast_str(ast, n->aux));
        break;
    default:
        if (*name)
            fprintf(out, " %s", name);
        break;
    }

    if (*type && n->kind != CCR_NODE_INCLUDE)
        fprintf(out, " : %s", type);
    if (n->kind == CCR_NODE_FUNCTION && !(n->flags & CCR_FLAG_DEFINITION))
        fputs("  (declaration)", out);
    if (n->flags & CCR_FLAG_INIT)
        fputs("  (init)", out);
    if (n->flags & CCR_FLAG_IMPLICIT)
        fputs("  (implicit)", out);
    if (n->flags & CCR_FLAG_LOOSE)
        fputs("  (loose, not compiled)", out);
    if (n->line)
        fprintf(out, "  [%u:%u]", n->line, n->column);
    fputc('\n', out);
}

/* Iterative pre-order walk; the prefix stack stores "is last child?" bits. */
static void dump_tree(const ccr_ast *ast, ccr_node_id root, FILE *out)
{
    enum { MAX_DEPTH = 256 };
    bool last_at[MAX_DEPTH];

    ccr_node_id id = root;
    uint32_t depth = 0;

    while (id != CCR_NONE) {
        const ccr_node *n = &ast->nodes[id];

        if (depth > 0) {
            for (uint32_t d = 1; d < depth && d < MAX_DEPTH; ++d)
                fputs(last_at[d] ? "   " : "│  ", out);
            if (depth < MAX_DEPTH)
                last_at[depth] = (n->next_sibling == CCR_NONE);
            fputs(n->next_sibling == CCR_NONE ? "└─ " : "├─ ", out);
        }
        dump_node_line(ast, n, out);

        /* descend, else move to the next sibling, else climb */
        if (n->first_child != CCR_NONE) {
            id = n->first_child;
            ++depth;
            continue;
        }
        while (id != CCR_NONE) {
            if (id == root)
                return;
            if (ast->nodes[id].next_sibling != CCR_NONE) {
                id = ast->nodes[id].next_sibling;
                break;
            }
            id = ast->nodes[id].parent;
            --depth;
        }
    }
}

static const char *severity_name(ccr_diag_severity s)
{
    switch (s) {
    case CCR_DIAG_NOTE:    return "note";
    case CCR_DIAG_WARNING: return "warning";
    case CCR_DIAG_ERROR:   return "error";
    case CCR_DIAG_FATAL:   return "fatal";
    }
    return "?";
}

void ccr_ast_dump_diags(const ccr_ast *ast, FILE *out)
{
    for (uint32_t i = 0; i < ast->diag_count; ++i) {
        const ccr_diag *d = &ast->diags[i];
        const char *file = ccr_ast_str(ast, d->file);
        fprintf(out, "%s:%u:%u: %s: %s\n",
                *file ? file : "<unknown>", d->line, d->column,
                severity_name(d->severity), ccr_ast_str(ast, d->message));
    }
}

void ccr_ast_dump(const ccr_ast *ast, FILE *out)
{
    for (ccr_node_id id = 0; id < ast->node_count; ++id)
        if (ast->nodes[id].parent == CCR_NONE && !(ast->nodes[id].flags & CCR_FLAG_DEAD))
            dump_tree(ast, id, out);

    if (ast->diag_count) {
        fputc('\n', out);
        ccr_ast_dump_diags(ast, out);
    }

    fprintf(out, "\n%u nodes, %u errors, %u warnings\n",
            ast->node_count,
            ccr_ast_count_diags(ast, CCR_DIAG_ERROR),
            ccr_ast_count_diags(ast, CCR_DIAG_WARNING) - ccr_ast_count_diags(ast, CCR_DIAG_ERROR));
}

/* ------------------------------------------------------ source ranges */

static bool needs_semicolon(ccr_node_kind k)
{
    switch (k) {
    case CCR_NODE_RETURN: case CCR_NODE_BREAK: case CCR_NODE_CONTINUE:
    case CCR_NODE_GOTO: case CCR_NODE_DO_WHILE: case CCR_NODE_OTHER:
    case CCR_NODE_CALL: case CCR_NODE_BINARY_OP: case CCR_NODE_UNARY_OP:
    case CCR_NODE_TERNARY: case CCR_NODE_REF: case CCR_NODE_MEMBER:
    case CCR_NODE_INDEX: case CCR_NODE_CAST: case CCR_NODE_SIZEOF:
    case CCR_NODE_COMPOUND_LIT: case CCR_NODE_MACRO_VALUE:
    case CCR_NODE_INT_LIT: case CCR_NODE_FLOAT_LIT: case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
        return true;
    default:
        return false;
    }
}

static bool in_statement_position(const ccr_ast *ast, ccr_node_id id)
{
    ccr_node_id p = ast->nodes[id].parent;
    if (p == CCR_NONE)
        return false;
    switch (ast->nodes[p].kind) {
    case CCR_NODE_BLOCK: case CCR_NODE_CASE: case CCR_NODE_DEFAULT: case CCR_NODE_LABEL:
        return true;
    default:
        return false;
    }
}

static bool is_hspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool is_space(char c)  { return is_hspace(c) || c == '\n' || c == '\f' || c == '\v'; }

/* Children before parents, so every parent can cover its children. */
static void cover_children(ccr_ast *ast, ccr_node_id id, unsigned depth)
{
    if (depth > 4000)
        return;   /* absurd nesting: leave as is rather than blow the stack */
    for (ccr_node_id c = ast->nodes[id].first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling) {
        cover_children(ast, c, depth + 1);
        ccr_node *n = &ast->nodes[id];
        const ccr_node *k = &ast->nodes[c];
        if (n->kind == CCR_NODE_TRANSLATION_UNIT || k->src_e <= k->src_b)
            continue;
        if (k->src_e > n->src_e)
            n->src_e = k->src_e;
    }
    /* an implicit Block is exactly its one statement */
    ccr_node *n = &ast->nodes[id];
    if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_IMPLICIT) && n->first_child != CCR_NONE) {
        n->src_b = ast->nodes[n->first_child].src_b;
        n->src_e = ast->nodes[n->first_child].src_e;
    }
}

/* Leading whitespace of the line holding `at`, if only whitespace precedes
 * `at` on that line; otherwise NULL-length (len = UINT32_MAX). */
static uint32_t line_indent(const ccr_ast *ast, uint32_t at, uint32_t *len)
{
    uint32_t start = at;
    while (start > 0 && ast->source[start - 1] != '\n')
        --start;
    for (uint32_t i = start; i < at; ++i) {
        if (!is_hspace(ast->source[i])) {
            *len = UINT32_MAX;
            return start;
        }
    }
    *len = at - start;
    return start;
}

static ccr_str intern_n(ccr_ast *ast, const char *s, uint32_t n)
{
    char buf[256];
    if (n >= sizeof buf)
        n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return n ? ccr_ast_intern(ast, buf) : 0;
}

/* Indent of the line `at` is on, whatever precedes it on the line. */
static ccr_str indent_of_line(ccr_ast *ast, uint32_t at)
{
    uint32_t start = at;
    while (start > 0 && ast->source[start - 1] != '\n')
        --start;
    uint32_t end = start;
    while (end < ast->source_len && is_hspace(ast->source[end]) && ast->source[end] != '\r')
        ++end;
    return intern_n(ast, ast->source + start, end - start);
}

/*
 * Scans whitespace and comments in [from, to). Returns false if anything
 * else is there (a #directive, code from a macro...). `first_comment`
 * gets where the first comment after the last non-comment begins, or
 * UINT32_MAX if none.
 */
static bool scan_gap(const ccr_ast *ast, uint32_t from, uint32_t to, uint32_t *first_comment)
{
    const char *s = ast->source;
    bool clean = true;
    *first_comment = UINT32_MAX;
    uint32_t i = from;
    while (i < to) {
        if (is_space(s[i])) {
            ++i;
        } else if (i + 1 < to && s[i] == '/' && s[i + 1] == '/') {
            if (*first_comment == UINT32_MAX)
                *first_comment = i;
            while (i < to && s[i] != '\n')
                ++i;
        } else if (i + 1 < to && s[i] == '/' && s[i + 1] == '*') {
            if (*first_comment == UINT32_MAX)
                *first_comment = i;
            i += 2;
            while (i + 1 < to && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            i += 2;
        } else {
            clean = false;
            *first_comment = UINT32_MAX;   /* comments above junk are not ours */
            while (i < to && s[i] != '\n')
                ++i;
        }
    }
    return clean;
}

/* After `from`: spaces, then a comment on the same line, if any. */
static uint32_t trailing_comment_end(const ccr_ast *ast, uint32_t from, uint32_t limit)
{
    const char *s = ast->source;
    uint32_t i = from;
    while (i < limit && is_hspace(s[i]) && s[i] != '\r')
        ++i;
    if (i + 1 < limit && s[i] == '/' && s[i + 1] == '/') {
        while (i < limit && s[i] != '\n' && s[i] != '\r')
            ++i;
        return i;
    }
    if (i + 1 < limit && s[i] == '/' && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < limit && !(s[i] == '*' && s[i + 1] == '/'))
            ++i;
        return i + 2 <= limit ? i + 2 : limit;
    }
    return from;
}

static void finish_block(ccr_ast *ast, ccr_node_id id)
{
    ccr_node *b = &ast->nodes[id];
    const bool implicit = (b->flags & CCR_FLAG_IMPLICIT) != 0;
    const char *s = ast->source;

    uint32_t open = b->src_b, close = b->src_e;   /* interior is [open, close) */
    if (!implicit) {
        if (b->src_e <= b->src_b || s[b->src_b] != '{' || s[b->src_e - 1] != '}') {
            b->flags |= CCR_FLAG_UNSAFE_GAP;      /* a block made by a macro */
            return;
        }
        open = b->src_b + 1;
        close = b->src_e - 1;
    }

    /* closing brace indent; implicit: the owner's line */
    uint32_t len;
    if (implicit) {
        ccr_node_id owner = b->parent;
        uint32_t at = owner != CCR_NONE ? ast->nodes[owner].src_b : b->src_b;
        b->close_indent = indent_of_line(ast, at);
    } else {
        uint32_t start = line_indent(ast, close, &len);
        b->close_indent = len != UINT32_MAX ? intern_n(ast, s + start, len)
                                             : indent_of_line(ast, b->src_b);
    }

    /* statements: comments above belong to them, as does the rest of their line */
    uint32_t cur = open;
    bool first = true;
    for (ccr_node_id c = b->first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling) {
        ccr_node *k = &ast->nodes[c];
        k->ext_b = k->src_b;
        k->ext_e = k->src_e;
        if (k->src_b < cur) {
            b->flags |= CCR_FLAG_UNSAFE_GAP;      /* overlapping: macro magic */
            continue;
        }
        if (!implicit) {
            uint32_t comment;
            if (!scan_gap(ast, cur, k->src_b, &comment))
                b->flags |= CCR_FLAG_UNSAFE_GAP;
            if (comment != UINT32_MAX)
                k->ext_b = comment;
            /* an empty line between it and the statement above */
            unsigned newlines = 0;
            for (uint32_t i = cur; i < k->ext_b; ++i)
                if (s[i] == '\n')
                    ++newlines;
            if (!first && newlines >= 2)
                k->flags |= CCR_FLAG_BLANK_BEFORE;
        }
        ccr_node_id next = k->next_sibling;
        uint32_t limit = next != CCR_NONE ? ast->nodes[next].src_b : close;
        if (limit > k->src_e)
            k->ext_e = trailing_comment_end(ast, k->src_e, limit);
        if (first) {
            uint32_t start = line_indent(ast, k->src_b, &len);
            b->indent = len != UINT32_MAX ? intern_n(ast, s + start, len) : 0;
            first = false;
        }
        cur = k->ext_e;
    }

    if (!implicit) {
        /* comments after the last statement stay at the end of the block */
        uint32_t comment;
        if (!scan_gap(ast, cur, close, &comment))
            b->flags |= CCR_FLAG_UNSAFE_GAP;
        else if (comment != UINT32_MAX) {
            uint32_t end = close;
            while (end > comment && is_space(s[end - 1]))
                --end;
            /* stored as a string: it does not move with any statement.
             * Whole, however long (commented-out code can be kilobytes). */
            uint32_t n = end - comment;
            char *copy = malloc((size_t)n + 1);
            if (copy) {
                memcpy(copy, s + comment, n);
                copy[n] = '\0';
                b->aux = ccr_ast_intern(ast, copy);
                free(copy);
            }
            if (!copy || b->aux == 0)
                b->flags |= CCR_FLAG_UNSAFE_GAP;   /* would lose it: refuse instead */
        }
    }

    /* each statement's indent beyond the block's own */
    const char *base = ccr_ast_str(ast, b->indent);
    const uint32_t base_n = (uint32_t)strlen(base);
    for (ccr_node_id c = b->first_child; c != CCR_NONE; c = ast->nodes[c].next_sibling) {
        ccr_node *k = &ast->nodes[c];
        uint32_t start = line_indent(ast, k->ext_b, &len);
        if (len != UINT32_MAX && len > base_n && memcmp(s + start, base, base_n) == 0)
            k->extra_indent = intern_n(ast, s + start + base_n, len - base_n);
    }

    if (b->indent == 0) {   /* empty block: one level deeper than the brace */
        char buf[300];
        snprintf(buf, sizeof buf, "%s    ", ccr_ast_str(ast, b->close_indent));
        b->indent = ccr_ast_intern(ast, buf);
    }
}

void ccr_ast_finish_ranges(ccr_ast *ast)
{
    if (!ast->source || ast->node_count == 0)
        return;

    /* statements that end with ';' get it */
    for (ccr_node_id id = 0; id < ast->node_count; ++id) {
        ccr_node *n = &ast->nodes[id];
        if (!needs_semicolon(n->kind) || !in_statement_position(ast, id))
            continue;
        uint32_t i = n->src_e;
        while (i < ast->source_len && is_space(ast->source[i]))
            ++i;
        if (i < ast->source_len && ast->source[i] == ';')
            n->src_e = i + 1;
    }

    for (ccr_node_id id = 0; id < ast->node_count; ++id)
        if (ast->nodes[id].parent == CCR_NONE)
            cover_children(ast, id, 0);

    for (ccr_node_id id = 0; id < ast->node_count; ++id)
        if (ast->nodes[id].kind == CCR_NODE_BLOCK && !(ast->nodes[id].flags & CCR_FLAG_LOOSE))
            finish_block(ast, id);

    /* everything starts where it was written */
    for (ccr_node_id id = 0; id < ast->node_count; ++id) {
        ast->nodes[id].place_b = ast->nodes[id].src_b;
        ast->nodes[id].place_e = ast->nodes[id].src_e;
    }
}
