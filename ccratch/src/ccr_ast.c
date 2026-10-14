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

void ccr_ast_unlink(ccr_ast *ast, ccr_node_id id)
{
    ccr_node *n = &ast->nodes[id];
    if (n->parent == CCR_NONE)
        return;
    ccr_node *p = &ast->nodes[n->parent];

    ccr_node_id prev = CCR_NONE;
    if (p->first_child == id) {
        p->first_child = n->next_sibling;
    } else {
        for (prev = p->first_child; prev != CCR_NONE; prev = ast->nodes[prev].next_sibling)
            if (ast->nodes[prev].next_sibling == id)
                break;
        if (prev != CCR_NONE)
            ast->nodes[prev].next_sibling = n->next_sibling;
    }
    if (p->last_child == id)
        p->last_child = prev;

    n->parent = CCR_NONE;
    n->next_sibling = CCR_NONE;
}

void ccr_ast_append(ccr_ast *ast, ccr_node_id parent, ccr_node_id id)
{
    ccr_ast_unlink(ast, id);
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
