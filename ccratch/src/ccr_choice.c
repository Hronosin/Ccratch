#include "ccr_choice.h"
#include "ccr_lib.h"

#include <stdio.h>
#include <string.h>

static const ccr_node *N(const ccr_ast *a, ccr_node_id id) { return &a->nodes[id]; }
static const char *S(const ccr_ast *a, ccr_str s) { return ccr_ast_str(a, s); }

static void add(ccr_choices *c, const char *v)
{
    const size_t n = strlen(v) + 1;
    if (c->n == CCR_CHOICES_MAX || !*v || c->used + n > sizeof c->store)
        return;
    for (int i = 0; i < c->n; ++i)
        if (strcmp(c->v[i], v) == 0)
            return;
    memcpy(c->store + c->used, v, n);
    c->v[c->n++] = c->store + c->used;
    c->used += (int)n;
}

/* "const enum dir" -> "dir"; whether "enum" was said. */
static bool bare(const char *type, char *out, size_t cap)
{
    bool was_enum = false;
    while (*type == ' ')
        ++type;
    for (;;) {
        if (strncmp(type, "const ", 6) == 0)
            type += 6;
        else if (strncmp(type, "volatile ", 9) == 0)
            type += 9;
        else if (strncmp(type, "enum ", 5) == 0) {
            type += 5;
            was_enum = true;
        } else
            break;
        while (*type == ' ')
            ++type;
    }
    snprintf(out, cap, "%s", type);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ')
        out[--n] = '\0';
    return was_enum;
}

/* The file's top-level declaration of kind `k` called `name`. */
static ccr_node_id top_decl(const ccr_ast *a, ccr_node_kind k, const char *name)
{
    if (!a->node_count || N(a, 0)->kind != CCR_NODE_TRANSLATION_UNIT)
        return CCR_NONE;
    for (ccr_node_id c = N(a, 0)->first_child; c != CCR_NONE; c = N(a, c)->next_sibling)
        if (N(a, c)->kind == k && strcmp(S(a, N(a, c)->name), name) == 0)
            return c;
    return CCR_NONE;
}

bool ccr_type_choices(const ccr_ast *ast, const char *type, ccr_choices *out)
{
    out->n = 0;
    out->used = 0;
    out->title[0] = '\0';
    char t[96];
    bool said_enum = bare(type ? type : "", t, sizeof t);
    if (!*t || strchr(t, '*') || strchr(t, '['))
        return false;
    for (int hop = 0; hop < 4; ++hop) {
        const ccr_node_id e = top_decl(ast, CCR_NODE_ENUM, t);
        if (e != CCR_NONE) {
            snprintf(out->title, sizeof out->title, "%s%.50s", said_enum ? "enum " : "", t);
            for (ccr_node_id c = N(ast, e)->first_child; c != CCR_NONE; c = N(ast, c)->next_sibling)
                if (N(ast, c)->kind == CCR_NODE_ENUM_CONST)
                    add(out, S(ast, N(ast, c)->name));
            return out->n > 0;
        }
        const ccr_node_id td = top_decl(ast, CCR_NODE_TYPEDEF, t);
        if (td == CCR_NONE)
            break;
        said_enum = bare(S(ast, N(ast, td)->type), t, sizeof t);
    }
    if (strcmp(t, "bool") == 0 || strcmp(t, "_Bool") == 0) {
        snprintf(out->title, sizeof out->title, "bool");
        add(out, "true");
        add(out, "false");
        return true;
    }
    const char *vals[CCR_CHOICES_MAX];
    const int n = ccr_lib_enum_values(t, vals, CCR_CHOICES_MAX);
    for (int i = 0; i < n; ++i)
        add(out, vals[i]);
    if (out->n)
        snprintf(out->title, sizeof out->title, "%.60s", t);
    return out->n > 0;
}

/* The type of an expression next to the slot, as far as the file says. */
static const char *expr_type(const ccr_ast *a, ccr_node_id id)
{
    if (id == CCR_NONE)
        return "";
    const ccr_node *n = N(a, id);
    if (n->kind == CCR_NODE_REF || n->kind == CCR_NODE_MEMBER)
        return S(a, n->raw);
    if (n->kind == CCR_NODE_CALL) {   /* f(): what f returns */
        const ccr_node_id f = top_decl(a, CCR_NODE_FUNCTION, S(a, n->name));
        if (f != CCR_NONE)
            return S(a, N(a, f)->aux);
    }
    if (n->kind == CCR_NODE_CAST)
        return S(a, n->type);
    return "";
}

static int index_in(const ccr_ast *a, ccr_node_id parent, ccr_node_id kid, int *count)
{
    int i = 0, at = -1;
    for (ccr_node_id c = N(a, parent)->first_child; c != CCR_NONE; c = N(a, c)->next_sibling, ++i)
        if (c == kid)
            at = i;
    *count = i;
    return at;
}

bool ccr_slot_choices(const ccr_ast *ast, ccr_node_id slot, ccr_choices *out)
{
    out->n = 0;
    out->used = 0;
    out->title[0] = '\0';
    if (ast->lang != CCR_LANG_C || slot >= ast->node_count || (N(ast, slot)->flags & CCR_FLAG_WORD))
        return false;
    const ccr_node_id p = N(ast, slot)->parent;
    if (p == CCR_NONE)
        return false;
    const ccr_node *pn = N(ast, p);
    int count = 0;
    const int k = index_in(ast, p, slot, &count);

    switch (pn->kind) {
    case CCR_NODE_CALL: {
        bool word = false;
        for (ccr_node_id c = pn->first_child; c != CCR_NONE; c = N(ast, c)->next_sibling)
            word = word || (N(ast, c)->flags & CCR_FLAG_WORD);
        const char *fname = S(ast, pn->name);
        const ccr_lib_block *b = word ? NULL : ccr_lib_find(fname, count);
        if (b) {
            const char *vals[CCR_CHOICES_MAX];
            const int n = ccr_lib_param_choices(b, k, vals, CCR_CHOICES_MAX, out->title, (int)sizeof out->title);
            for (int i = 0; i < n; ++i)
                add(out, vals[i]);
            if (out->n)
                return true;
        }
        /* the file's own function: the type of that parameter */
        const ccr_node_id f = top_decl(ast, CCR_NODE_FUNCTION, fname);
        int i = 0;
        for (ccr_node_id c = f == CCR_NONE ? CCR_NONE : N(ast, f)->first_child; c != CCR_NONE;
             c = N(ast, c)->next_sibling) {
            if (N(ast, c)->kind != CCR_NODE_PARAM)
                continue;
            if (i++ == k)
                return ccr_type_choices(ast, S(ast, N(ast, c)->type), out);
        }
        return false;
    }
    case CCR_NODE_BINARY_OP: {
        const char *op = S(ast, pn->op);
        if (count != 2 || !(strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "=") == 0))
            return false;
        const ccr_node_id other = k == 1 ? pn->first_child : N(ast, pn->first_child)->next_sibling;
        if (k == 0 && strcmp(op, "=") == 0)
            return false;   /* what is assigned to is a variable, not a value */
        return ccr_type_choices(ast, expr_type(ast, other), out);
    }
    case CCR_NODE_VAR:
        return (N(ast, slot)->flags & CCR_FLAG_INIT) && ccr_type_choices(ast, S(ast, pn->type), out);
    case CCR_NODE_CASE: {
        if (k != 0)
            return false;
        for (ccr_node_id a = pn->parent; a != CCR_NONE; a = N(ast, a)->parent)
            if (N(ast, a)->kind == CCR_NODE_SWITCH)
                return ccr_type_choices(ast, expr_type(ast, N(ast, a)->first_child), out);
        return false;
    }
    case CCR_NODE_RETURN:
        for (ccr_node_id a = pn->parent; a != CCR_NONE; a = N(ast, a)->parent)
            if (N(ast, a)->kind == CCR_NODE_FUNCTION)
                return ccr_type_choices(ast, S(ast, N(ast, a)->aux), out);
        return false;
    default:
        return false;
    }
}
