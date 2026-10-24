#include "ccr_blocks.h"
#include "ccr_asm.h"
#include "ccr_template.h"
#include "ccr_choice.h"
#include "ccr_cmake.h"
#include "ccr_lib.h"
#include "ccr_theme.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ look */

#define FONT        22.0f   /* line height; Nunito's letters are ~2/3 of it */
#define GAP          8.0f   /* between items on one line */
#define BLOCK_PAD_X 12.0f
#define BLOCK_PAD_Y  8.0f
#define BLOCK_MIN_H 44.0f
#define BLOCK_MIN_W 60.0f
#define EXPR_PAD_X   9.0f
#define ARROW_W     13.0f   /* room for a list slot's little triangle */
#define EXPR_PAD_Y   4.0f
#define ARM_W       20.0f   /* left side of a C-block */
#define MOUTH_MIN_H 24.0f
#define FOOT_H      24.0f   /* bottom bar of a C-block */
#define HAT_BUMP    22.0f
#define TAB_X       CCR_TAB_X
#define TAB_W       CCR_TAB_W
#define TAB_H       CCR_TAB_H

typedef enum category {
    CAT_CONTROL, CAT_OPERATOR, CAT_VARIABLE, CAT_CALL, CAT_EVENT,
    CAT_TYPE, CAT_PREPROC, CAT_POINTER, CAT_LITERAL, CAT_DRAGON, CAT_LIBRARY,
    CAT_COUNT
} category;

/* Colours come from the theme; its categories are in this same order. */
#define PAL(cat, role) ccr_th_cat((ccr_theme_cat)(cat), (role))

typedef enum shape { SHAPE_PILL, SHAPE_HEX, SHAPE_BLOCK } shape;

/* ------------------------------------------------------- the layout */

typedef struct layout {
    const ccr_ast *ast;
    ccr_blocks    *out;
    float         *ew, *eh;   /* cached expression sizes, per node */
    bool           oom;
    int            dim;       /* > 0 inside a commented-out statement */
    ccr_prim      *tabs;      /* puzzle tabs, drawn over everything at the end */
    uint32_t       tab_count, tab_cap;
} layout;

static void push_snap(layout *L, ccr_snap_kind kind, ccr_node_id ref, float x, float y, float w,
                      float h);

static void push_jump(layout *L, ccr_node_id id);
static void add_tab(layout *L, float x, float bottom, ccr_color fill, ccr_color edge, ccr_node_id node);

static const ccr_node *N(const layout *L, ccr_node_id id) { return &L->ast->nodes[id]; }
static const char *S(const layout *L, ccr_str s)         { return ccr_ast_str(L->ast, s); }

static uint32_t child_count(const layout *L, ccr_node_id id)
{
    uint32_t n = 0;
    for (ccr_node_id c = N(L, id)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        ++n;
    return n;
}

static ccr_node_id child_at(const layout *L, ccr_node_id id, uint32_t index)
{
    ccr_node_id c = N(L, id)->first_child;
    while (c != CCR_NONE && index-- > 0)
        c = N(L, c)->next_sibling;
    return c;
}

/* --------------------------------------------------- display list */

static bool grow(void **buf, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint32_t cap2 = *cap ? *cap : 256;
    while (cap2 < need)
        cap2 *= 2;
    void *p = realloc(*buf, (size_t)cap2 * elem);
    if (!p)
        return false;
    *buf = p;
    *cap = cap2;
    return true;
}

static void push_prim(layout *L, ccr_prim p)
{
    ccr_blocks *b = L->out;
    if (!grow((void **)&b->prims, &b->prim_cap, b->prim_count + 1, sizeof(ccr_prim))) {
        L->oom = true;
        return;
    }
    b->prims[b->prim_count++] = p;

    /* track bounds */
    ccr_rect *bb = &b->bounds;
    if (bb->w == 0.0f && bb->h == 0.0f) {
        *bb = p.rect;
    } else {
        float x2 = bb->x + bb->w, y2 = bb->y + bb->h;
        if (p.rect.x < bb->x) bb->x = p.rect.x;
        if (p.rect.y < bb->y) bb->y = p.rect.y;
        if (p.rect.x + p.rect.w > x2) x2 = p.rect.x + p.rect.w;
        if (p.rect.y + p.rect.h > y2) y2 = p.rect.y + p.rect.h;
        bb->w = x2 - bb->x;
        bb->h = y2 - bb->y;
    }
}

/* Commented-out code: the colour drained towards slate grey. */
static ccr_color dimmed(ccr_color c)
{
    const ccr_color grey = ccr_th(CCR_C_DIM);
    uint32_t out = c & 0xFFu;
    for (int i = 0; i < 3; ++i) {
        uint32_t ch = (c >> (24 - 8 * i)) & 0xFFu, g = (grey >> (24 - 8 * i)) & 0xFFu;
        out |= ((ch + g * 5u) / 6u) << (24 - 8 * i);
    }
    return out;
}

static void push_shape(layout *L, shape sh, category cat, ccr_rect r, ccr_node_id node)
{
    ccr_prim p = {
        .kind = (sh == SHAPE_HEX) ? CCR_PRIM_HEX : CCR_PRIM_BLOCK,
        .rect = r,
        .fill = L->dim ? dimmed(PAL(cat, CCR_FILL)) : PAL(cat, CCR_FILL),
        .edge = L->dim ? dimmed(PAL(cat, CCR_EDGE)) : PAL(cat, CCR_EDGE),
        .roundness = (sh == SHAPE_PILL) ? 1.0f : 0.18f,
        .node = node,
    };
    push_prim(L, p);
}

static void push_text(layout *L, const char *s, float x, float y, ccr_color color, ccr_node_id node)
{
    ccr_blocks *b = L->out;
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (!grow((void **)&b->text, &b->text_cap, b->text_len + n, 1)) {
        L->oom = true;
        return;
    }
    uint32_t at = b->text_len;
    memcpy(b->text + at, s, n);
    b->text_len += n;

    ccr_prim p = {
        .kind = CCR_PRIM_TEXT,
        .rect = { x, y, ccr_gui_text_width(s, FONT), FONT },
        .fill = L->dim ? ccr_th(CCR_C_DIM_TEXT) : color,
        .text = at,
        .font = FONT,
        .node = node,
    };
    push_prim(L, p);
}

/* ----------------------------------------------------- inline items */

/* One line of a block: a mix of plain words and nested expressions. */
enum { MAX_ITEMS = 48, SCRATCH = 1024 };

typedef struct item {
    const char *text;      /* NULL means: expression `expr` */
    ccr_node_id expr;
} item;

typedef struct items {
    item   v[MAX_ITEMS];
    int    n;
    char   scratch[SCRATCH];
    size_t used;
} items;

static void it_text(items *it, const char *s)
{
    if (it->n < MAX_ITEMS && s && *s)
        it->v[it->n++] = (item){ s, CCR_NONE };
}

static void it_fmt(items *it, const char *fmt, ...)
{
    if (it->used >= SCRATCH)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(it->scratch + it->used, SCRATCH - it->used, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    const char *s = it->scratch + it->used;
    size_t adv = (size_t)w + 1;
    it->used = (it->used + adv > SCRATCH) ? SCRATCH : it->used + adv;
    it_text(it, s);
}

static void it_expr(items *it, ccr_node_id e)
{
    if (it->n < MAX_ITEMS && e != CCR_NONE)
        it->v[it->n++] = (item){ NULL, e };
}

/* --------------------------------------------- expression appearance */

static bool op_is_assign(const char *op)
{
    size_t n = strlen(op);
    return n > 0 && op[n - 1] == '=' &&
           strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 &&
           strcmp(op, "<=") != 0 && strcmp(op, ">=") != 0;
}

static bool op_is_bool(const char *op)
{
    static const char *const ops[] = { "==", "!=", "<", ">", "<=", ">=", "&&", "||", "!" };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; ++i)
        if (strcmp(op, ops[i]) == 0)
            return true;
    return false;
}

/* The block pack block a call is drawn as, or NULL: same name, same
 * number of arguments. */
static const ccr_lib_block *lib_of(const layout *L, ccr_node_id id)
{
    const ccr_node *n = N(L, id);
    if (n->kind != CCR_NODE_CALL || L->ast->lang == CCR_LANG_CMAKE)
        return NULL;
    int args = 0;
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
        if (N(L, c)->flags & CCR_FLAG_WORD)
            return NULL;   /* f(...) from the palette, name typed in: plain */
        ++args;
    }
    return ccr_lib_find(S(L, n->name), args);
}

static void expr_look(const layout *L, ccr_node_id id, shape *sh, category *cat)
{
    const ccr_node *n = N(L, id);
    const char *op = S(L, n->op);
    *sh = SHAPE_PILL;

    switch (n->kind) {
    case CCR_NODE_REF:
        *cat = strcmp(S(L, n->name), "NULL") == 0 || strcmp(S(L, n->name), "nullptr") == 0 ? CAT_POINTER
                                                                                            : CAT_VARIABLE;
        break;
    case CCR_NODE_NEW:
    case CCR_NODE_DELETE:    *cat = CAT_POINTER; break;   /* memory, by hand */
    case CCR_NODE_CAST:
    case CCR_NODE_SIZEOF:    *cat = CAT_TYPE; break;
    case CCR_NODE_MEMBER:
    case CCR_NODE_INDEX:     *cat = CAT_VARIABLE; break;
    case CCR_NODE_INT_LIT:
    case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
    case CCR_NODE_INIT_LIST:
    case CCR_NODE_COMPOUND_LIT: *cat = CAT_LITERAL; break;
    case CCR_NODE_CALL: {
        const ccr_lib_block *lb = lib_of(L, id);
        *cat = CAT_CALL;
        if (ccr_std_header(S(L, n->name)))
            *cat = CAT_LIBRARY;   /* the standard library's, not yours */
        if (lb) {   /* a library's block: its pack's colour; true or false is pointy */
            const ccr_lib_pack *pk = ccr_lib_pack_at(lb->pack);
            *cat = pk ? (category)pk->category : CAT_LIBRARY;
            if (strcmp(lb->returns, "bool") == 0 || strcmp(lb->returns, "_Bool") == 0)
                *sh = SHAPE_HEX;
        }
        break;
    }
    case CCR_NODE_BINARY_OP:
        *cat = op_is_assign(op) ? CAT_VARIABLE : CAT_OPERATOR;
        if (op_is_bool(op))
            *sh = SHAPE_HEX;
        break;
    case CCR_NODE_UNARY_OP:
        if (strcmp(op, "&") == 0 || strcmp(op, "*") == 0) {
            *cat = CAT_POINTER;            /* future wires start here */
        } else if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
            *cat = CAT_VARIABLE;           /* Scratch's "change by 1" */
        } else {
            *cat = CAT_OPERATOR;
            if (op_is_bool(op))
                *sh = SHAPE_HEX;
        }
        break;
    case CCR_NODE_MACRO_VALUE: *cat = CAT_PREPROC; break;
    case CCR_NODE_TEXT:
        *cat = CAT_LITERAL;                /* a white field to type in */
        if (n->flags & CCR_FLAG_WORD)
            *sh = SHAPE_BLOCK;             /* a name, not a slot: square-ish */
        break;
    case CCR_NODE_OTHER:     *cat = CAT_DRAGON; break;
    default:                 *cat = CAT_OPERATOR; break;
    }
}

static bool is_word(const layout *L, ccr_node_id id)
{
    return id != CCR_NONE && (N(L, id)->flags & CCR_FLAG_WORD) != 0;
}

/* The name or type field of a new block. */
static ccr_node_id word_of(const layout *L, ccr_node_id id, uint32_t which)
{
    for (ccr_node_id c = N(L, id)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        if (N(L, c)->flags & which)
            return c;
    return CCR_NONE;
}

/* The index-th child that is not a name or type field. */
static ccr_node_id operand(const layout *L, ccr_node_id id, uint32_t index)
{
    for (ccr_node_id c = N(L, id)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        if (!is_word(L, c) && index-- == 0)
            return c;
    return CCR_NONE;
}

static void args_with_commas(const layout *L, ccr_node_id first, items *it)
{
    for (ccr_node_id c = first; c != CCR_NONE; c = N(L, c)->next_sibling) {
        if (is_word(L, c))
            continue;
        /* `.x = 1` arrives as two siblings: the designator, then the value */
        if (N(L, c)->kind == CCR_NODE_DESIGNATOR) {
            it_fmt(it, ".%s =", S(L, N(L, c)->name));
            continue;
        }
        it_expr(it, c);
        ccr_node_id next = N(L, c)->next_sibling;
        while (is_word(L, next))
            next = N(L, next)->next_sibling;
        if (next != CCR_NONE)
            it_text(it, ",");
    }
}

/* New blocks with name or type fields: the fields are typed into. */
static bool word_items(const layout *L, ccr_node_id id, items *it)
{
    const ccr_node *n = N(L, id);
    const ccr_node_id name = word_of(L, id, CCR_FLAG_WORD_NAME);
    const ccr_node_id type = word_of(L, id, CCR_FLAG_WORD_TYPE);
    if (name == CCR_NONE && type == CCR_NONE)
        return false;
    switch (n->kind) {
    case CCR_NODE_CALL:
        it_expr(it, name);
        it_text(it, "(");
        args_with_commas(L, n->first_child, it);
        it_text(it, ")");
        return true;
    case CCR_NODE_MEMBER:
        it_expr(it, operand(L, id, 0));
        it_text(it, (n->flags & CCR_FLAG_ARROW) ? "->" : ".");
        it_expr(it, name);
        return true;
    case CCR_NODE_CAST:
        if (strstr(S(L, n->raw), "_cast")) {   /* static_cast<T>(a) */
            it_fmt(it, "%s<", S(L, n->raw));
            it_expr(it, type);
            it_text(it, "> (");
            it_expr(it, operand(L, id, 0));
            it_text(it, ")");
            return true;
        }
        it_text(it, "(");
        it_expr(it, type);
        it_text(it, ")");
        it_expr(it, operand(L, id, 0));
        return true;
    case CCR_NODE_SIZEOF:
        it_text(it, "sizeof (");
        it_expr(it, type);
        it_text(it, ")");
        return true;
    default:
        return false;
    }
}

static void expr_items(const layout *L, ccr_node_id id, items *it)
{
    const ccr_node *n = N(L, id);
    const char *name = S(L, n->name);
    const char *op = S(L, n->op);
    ccr_node_id c0 = n->first_child;
    ccr_node_id c1 = c0 != CCR_NONE ? N(L, c0)->next_sibling : CCR_NONE;
    ccr_node_id c2 = c1 != CCR_NONE ? N(L, c1)->next_sibling : CCR_NONE;

    if ((n->flags & CCR_FLAG_NEW) && word_items(L, id, it))
        return;
    const ccr_lib_block *lb = lib_of(L, id);
    if (lb) {
        /* "open a window {width} x {height}": words, and the arguments
         * in the slots their names stand in */
        const char *t = lb->text;
        while (*t) {
            if (*t == '{') {
                const char *e = strchr(t, '}');
                if (!e)
                    break;
                int k = 0;
                while (k < lb->nparams && !(strlen(lb->params[k]) == (size_t)(e - t - 1) &&
                                            strncmp(lb->params[k], t + 1, (size_t)(e - t - 1)) == 0))
                    ++k;
                ccr_node_id a = n->first_child;
                for (int j = 0; j < k && a != CCR_NONE; ++j)
                    a = N(L, a)->next_sibling;
                it_expr(it, a);
                t = e + 1;
                continue;
            }
            const char *e = strchr(t, '{');
            size_t len = e ? (size_t)(e - t) : strlen(t);
            while (len > 0 && t[len - 1] == ' ')
                --len;
            const char *b = t;
            while (*b == ' ' && b < t + len)
                ++b;
            if (b < t + len)
                it_fmt(it, "%.*s", (int)(t + len - b), b);
            t = e ? e : t + strlen(t);
        }
        return;
    }

    switch (n->kind) {
    case CCR_NODE_TEXT:
        it_text(it, *name ? name : "   ");   /* empty slot: still something to click */
        break;
    case CCR_NODE_REF:
    case CCR_NODE_MACRO_VALUE:
    case CCR_NODE_INT_LIT:
    case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
        it_text(it, *name ? name : "?");
        break;
    case CCR_NODE_BINARY_OP:
        it_expr(it, c0);
        it_text(it, op);
        it_expr(it, c1);
        break;
    case CCR_NODE_UNARY_OP:
        if (n->flags & CCR_FLAG_POSTFIX) {
            it_expr(it, c0);
            it_text(it, op);
        } else {
            it_text(it, op);
            it_expr(it, c0);
        }
        break;
    case CCR_NODE_CALL:
        if (c0 != CCR_NONE && N(L, c0)->kind == CCR_NODE_MEMBER && !(N(L, c0)->flags & CCR_FLAG_WORD)) {
            /* obj.method(args): the object, then the call */
            const ccr_node *m = N(L, c0);
            if (m->first_child != CCR_NONE) {
                it_expr(it, m->first_child);
                it_fmt(it, "%s%s (", (m->flags & CCR_FLAG_ARROW) ? "->" : ".", S(L, m->name));
            } else {
                it_fmt(it, "%s (", S(L, m->name));   /* a method of this one's own class */
            }
            args_with_commas(L, c1, it);
            it_text(it, ")");
            break;
        }
        it_fmt(it, "%s (", *name ? name : "call");
        args_with_commas(L, c0, it);
        it_text(it, ")");
        break;
    case CCR_NODE_MEMBER:
        if (c0 == CCR_NONE) {   /* C++: a member of this object, no `this->` written */
            it_text(it, name);
            break;
        }
        it_expr(it, c0);
        it_fmt(it, "%s%s", (n->flags & CCR_FLAG_ARROW) ? "->" : ".", name);
        break;
    case CCR_NODE_NEW:
        if (n->flags & CCR_FLAG_NEW) {
            it_text(it, "new");
            it_expr(it, word_of(L, id, CCR_FLAG_WORD_TYPE));
            it_text(it, "(");
            args_with_commas(L, operand(L, id, 0), it);
            it_text(it, ")");
            break;
        }
        it_fmt(it, "new %s", S(L, n->type));
        it_text(it, (n->flags & CCR_FLAG_ARRAY_FORM) ? "[" : "(");
        args_with_commas(L, c0, it);
        it_text(it, (n->flags & CCR_FLAG_ARRAY_FORM) ? "]" : ")");
        break;
    case CCR_NODE_DELETE:
        it_text(it, (n->flags & CCR_FLAG_ARRAY_FORM) ? "delete[]" : "delete");
        it_expr(it, c0);
        break;
    case CCR_NODE_INDEX:
        it_expr(it, c0);
        it_text(it, "[");
        it_expr(it, c1);
        it_text(it, "]");
        break;
    case CCR_NODE_CAST:
    case CCR_NODE_COMPOUND_LIT:
        if (n->kind == CCR_NODE_CAST && strcmp(S(L, n->raw), "functional") == 0) {
            it_fmt(it, "%s (", S(L, n->type));   /* int(x) */
            it_expr(it, c0);
            it_text(it, ")");
        } else if (n->kind == CCR_NODE_CAST && strstr(S(L, n->raw), "_cast")) {
            it_fmt(it, "%s<%s> (", S(L, n->raw), S(L, n->type));   /* static_cast<int>(x) */
            it_expr(it, c0);
            it_text(it, ")");
        } else {
            it_fmt(it, "(%s)", S(L, n->type));
            it_expr(it, c0);
        }
        break;
    case CCR_NODE_DESIGNATOR:
        it_fmt(it, ".%s", name);
        break;
    case CCR_NODE_SIZEOF:
        if (c0 != CCR_NONE) {
            it_text(it, "sizeof");
            it_expr(it, c0);
        } else {
            it_text(it, S(L, n->aux));
        }
        break;
    case CCR_NODE_TERNARY:
        it_expr(it, c0);
        it_text(it, "?");
        it_expr(it, c1);
        it_text(it, ":");
        it_expr(it, c2);
        break;
    case CCR_NODE_INIT_LIST:
        it_text(it, "{");
        args_with_commas(L, c0, it);
        it_text(it, "}");
        break;
    case CCR_NODE_OTHER:
        it_text(it, *S(L, n->aux) ? S(L, n->aux) : S(L, n->raw));
        break;
    default:
        it_text(it, ccr_node_kind_name(n->kind));
        break;
    }
}

/* ------------------------------------------- measuring and emitting */

static void measure_expr(layout *L, ccr_node_id id);

static void measure_items(layout *L, const items *it, float *w, float *h)
{
    float tw = 0.0f, th = FONT;
    for (int i = 0; i < it->n; ++i) {
        const item *x = &it->v[i];
        if (x->text) {
            tw += ccr_gui_text_width(x->text, FONT);
        } else {
            measure_expr(L, x->expr);
            tw += L->ew[x->expr];
            if (L->eh[x->expr] > th)
                th = L->eh[x->expr];
        }
        if (i + 1 < it->n)
            tw += GAP;
    }
    *w = tw;
    *h = th;
}

/* A slot holding one word or number that could be picked from a list:
 * an enum's values, true/false. */
static bool has_list(const layout *L, ccr_node_id id)
{
    const ccr_node *n = N(L, id);
    if (n->first_child != CCR_NONE || (n->flags & CCR_FLAG_WORD) || L->ast->lang != CCR_LANG_C)
        return false;
    switch (n->kind) {
    case CCR_NODE_TEXT:
    case CCR_NODE_REF:
    case CCR_NODE_INT_LIT:
    case CCR_NODE_MACRO_VALUE:
    case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
    case CCR_NODE_FLOAT_LIT:
        break;
    default:
        return false;
    }
    if (n->parent == CCR_NONE || N(L, n->parent)->kind == CCR_NODE_BLOCK)
        return false;   /* a code line, or a reporter on its own */
    static ccr_choices c;   /* big: off the stack */
    return ccr_slot_choices(L->ast, id, &c);
}

static void measure_expr(layout *L, ccr_node_id id)
{
    if (L->eh[id] > 0.0f)
        return;  /* cached */

    items it = { .n = 0, .used = 0 };
    expr_items(L, id, &it);

    float w, h;
    measure_items(L, &it, &w, &h);
    h += EXPR_PAD_Y * 2.0f;
    w += EXPR_PAD_X * 2.0f;

    shape sh;
    category cat;
    expr_look(L, id, &sh, &cat);
    if (sh == SHAPE_HEX)
        w += h * 0.5f;  /* room for the pointy ends */
    if (has_list(L, id))
        w += ARROW_W;

    L->ew[id] = w;
    L->eh[id] = h;
}

static void emit_expr(layout *L, ccr_node_id id, float x, float y);

/* Lays out a line of items starting at (x, y), centered in height h. */
static void emit_items(layout *L, const items *it, float x, float y, float h,
                       category cat, ccr_node_id owner)
{
    const ccr_color text_color = PAL(cat, CCR_TEXT);
    for (int i = 0; i < it->n; ++i) {
        const item *e = &it->v[i];
        if (e->text) {
            push_text(L, e->text, x, y + (h - FONT) * 0.5f, text_color, owner);
            x += ccr_gui_text_width(e->text, FONT);
        } else {
            emit_expr(L, e->expr, x, y + (h - L->eh[e->expr]) * 0.5f);
            x += L->ew[e->expr];
        }
        x += GAP;
    }
}

static void emit_expr(layout *L, ccr_node_id id, float x, float y)
{
    measure_expr(L, id);
    shape sh;
    category cat;
    expr_look(L, id, &sh, &cat);

    const float w = L->ew[id], h = L->eh[id];
    push_shape(L, sh, cat, (ccr_rect){ x, y, w, h }, id);

    /* a text slot, empty or typed into (not a code line, that is a
     * statement): reporters drop in, Scratch-style, over what was typed */
    const ccr_node *n = N(L, id);
    if (n->kind == CCR_NODE_TEXT && n->parent != CCR_NONE &&
        N(L, n->parent)->kind != CCR_NODE_BLOCK && !(n->flags & CCR_FLAG_WORD))
        push_snap(L, CCR_SNAP_SLOT, id, x, y, w, h);

    items it = { .n = 0, .used = 0 };
    expr_items(L, id, &it);
    float pad = EXPR_PAD_X + (sh == SHAPE_HEX ? h * 0.25f : 0.0f);
    emit_items(L, &it, x + pad, y, h, cat, id);
    if (has_list(L, id)) {
        ccr_prim p = {
            .kind = CCR_PRIM_ARROW,
            .rect = { x + w - pad - ARROW_W + 4.0f, y + h * 0.5f - 2.5f, 8.0f, 5.0f },
            .fill = L->dim ? ccr_th(CCR_C_DIM_TEXT) : PAL(cat, CCR_TEXT),
            .node = id,
        };
        push_prim(L, p);
    }
}

/* --------------------------------------------------------- blocks */

typedef struct size2 { float w, h; } size2;

/* A plain stack block: one line of items. */
static size2 emit_line_block(layout *L, const items *it, category cat, float x, float y,
                             ccr_node_id owner)
{
    float w, h;
    measure_items(L, it, &w, &h);
    w += BLOCK_PAD_X * 2.0f;
    h += BLOCK_PAD_Y * 2.0f;
    if (w < BLOCK_MIN_W) w = BLOCK_MIN_W;
    if (h < BLOCK_MIN_H) h = BLOCK_MIN_H;

    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y, w, h }, owner);
    emit_items(L, it, x + BLOCK_PAD_X, y, h, cat, owner);
    return (size2){ w, h };
}

static float emit_stack(layout *L, ccr_node_id first, float x, float y, float *max_w);
static float emit_stmt(layout *L, ccr_node_id id, float x, float y, float *max_w);

/* What goes inside a mouth: the children of a Block, or one statement. */
static float emit_body(layout *L, ccr_node_id body, float x, float y, float *max_w)
{
    if (body == CCR_NONE)
        return 0.0f;
    if (N(L, body)->kind == CCR_NODE_BLOCK) {
        push_snap(L, CCR_SNAP_FIRST, body, x, y, 120.0f, 0.0f);
        return emit_stack(L, N(L, body)->first_child, x, y, max_w);
    }
    return emit_stmt(L, body, x, y, max_w);
}

/*
 * A C-block: header line, then for each mouth its contents next to the
 * left arm, separated by `between` labels (e.g. "else"), then a foot
 * (optionally with its own label line, for do-while).
 */
typedef struct mouth {
    ccr_node_id body;
    const items *label_after;  /* line under this mouth, may be NULL */
} mouth;

/* How many statements there are under `id`, at any depth: what a folded
 * block hides. */
static uint32_t hidden_count(const layout *L, ccr_node_id id)
{
    uint32_t n = 0;
    for (ccr_node_id c = N(L, id)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
        if (N(L, id)->kind == CCR_NODE_BLOCK || N(L, c)->kind == CCR_NODE_FIELD ||
            N(L, c)->kind == CCR_NODE_ENUM_CONST)
            ++n;
        n += hidden_count(L, c);
    }
    return n;
}

static bool folded(const layout *L, ccr_node_id id)
{
    return (N(L, id)->flags & CCR_FLAG_COLLAPSED) != 0;
}

static size2 emit_line_block(layout *L, const items *it, category cat, float x, float y,
                             ccr_node_id owner);

/* A folded C block: its head, a count of what is inside, and a do-while's
 * foot, on one line. */
static size2 emit_folded(layout *L, const items *head, const items *foot, category cat,
                         float x, float y, ccr_node_id owner, float *max_w)
{
    items line = *head;   /* its words still point into `head`, alive meanwhile */
    it_fmt(&line, "{ %u }", hidden_count(L, owner));
    for (int i = 0; foot && i < foot->n && line.n < MAX_ITEMS; ++i)
        line.v[line.n++] = foot->v[i];
    size2 sz = emit_line_block(L, &line, cat, x, y, owner);
    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz;
}

static size2 emit_c_block(layout *L, const items *head, const mouth *mouths, int mouth_count,
                          category cat, float x, float y, ccr_node_id owner, float *max_w)
{
    if (folded(L, owner))
        return emit_folded(L, head, mouth_count ? mouths[mouth_count - 1].label_after : NULL,
                           cat, x, y, owner, max_w);
    float hw, hh;
    measure_items(L, head, &hw, &hh);
    hw += BLOCK_PAD_X * 2.0f;
    hh += BLOCK_PAD_Y * 2.0f;
    if (hh < BLOCK_MIN_H) hh = BLOCK_MIN_H;
    float width = hw < 120.0f ? 120.0f : hw;

    const uint32_t head_prim = L->out->prim_count;
    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y, width, hh }, owner);
    emit_items(L, head, x + BLOCK_PAD_X, y, hh, cat, owner);
    const ccr_color tab_fill = L->out->prims[head_prim].fill, tab_edge = L->out->prims[head_prim].edge;

    float cy = y + hh;
    for (int m = 0; m < mouth_count; ++m) {
        float inner_w = 0.0f;
        add_tab(L, x + ARM_W, cy, tab_fill, tab_edge, owner);   /* into the mouth */
        float inner_h = emit_body(L, mouths[m].body, x + ARM_W, cy, &inner_w);
        if (inner_h < MOUTH_MIN_H)
            inner_h = MOUTH_MIN_H;
        if (x + ARM_W + inner_w - x > *max_w)
            *max_w = ARM_W + inner_w;

        push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, cy, ARM_W, inner_h }, owner);
        cy += inner_h;

        const items *after = mouths[m].label_after;
        if (after && after->n > 0) {
            float lw, lh;
            measure_items(L, after, &lw, &lh);
            lw += BLOCK_PAD_X * 2.0f;
            lh += BLOCK_PAD_Y * 2.0f;
            if (lh < BLOCK_MIN_H * 0.8f) lh = BLOCK_MIN_H * 0.8f;
            float bw = lw > width ? lw : width;
            push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, cy, bw, lh }, owner);
            emit_items(L, after, x + BLOCK_PAD_X, cy, lh, cat, owner);
            cy += lh;
        } else if (m + 1 == mouth_count) {
            push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, cy, width, FOOT_H }, owner);
            cy += FOOT_H;
        }
    }

    if (width > *max_w)
        *max_w = width;
    return (size2){ width, cy - y };
}

static category stmt_expr_category(const layout *L, ccr_node_id id)
{
    shape sh;
    category cat;
    expr_look(L, id, &sh, &cat);
    if (cat == CAT_LITERAL)
        cat = CAT_OPERATOR;   /* a lone `42;` should not look like a hole */
    return cat;
}

static bool is_expression(ccr_node_kind k)
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

/* "int[4]" + "numbers" -> "int numbers[4]", the way C spells it. */
static void it_decl(items *it, const char *type, const char *name, bool with_eq)
{
    const char *br = strchr(type, '[');
    const char *eq = with_eq ? " =" : "";
    if (br && br > type) {
        int base = (int)(br - type);
        while (base > 0 && type[base - 1] == ' ')
            --base;
        it_fmt(it, "%.*s %s%s%s", base, type, name, br, eq);
    } else {
        it_fmt(it, "%s %s%s", type, name, eq);
    }
}

static ccr_node_id var_init(const layout *L, ccr_node_id var)
{
    for (ccr_node_id c = N(L, var)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        if (N(L, c)->flags & CCR_FLAG_INIT)
            return c;
    return CCR_NONE;
}

static size2 emit_decl_block(layout *L, ccr_node_id id, float x, float y, float *max_w);

static float emit_stmt_body(layout *L, ccr_node_id id, float x, float y, float *max_w);

/* A statement; a commented-out one, and everything in it, drawn grey. */
/* ---------------------------------------------------- puzzle shapes */

/*
 * Scratch's stack blocks fit together like a puzzle: a tab under each
 * block, a notch on top of the next one. Here a notch is a dent drawn in
 * the canvas colour, and the tab of the block above is drawn over it at
 * the end, so the two lock. Where nothing is above, the dent stays empty;
 * blocks after which nothing runs (return, break, continue, goto) have no
 * tab, like Scratch's "stop" blocks: their flat bottom says it.
 */
static void add_tab(layout *L, float x, float bottom, ccr_color fill, ccr_color edge, ccr_node_id node)
{
    if (!grow((void **)&L->tabs, &L->tab_cap, L->tab_count + 1, sizeof(ccr_prim))) {
        L->oom = true;
        return;
    }
    L->tabs[L->tab_count++] = (ccr_prim){ .kind = CCR_PRIM_TAB, .node = node,
        .rect = { x + TAB_X, bottom, TAB_W, TAB_H }, .fill = fill, .edge = edge };
}

static bool ends_flow(ccr_node_kind k)
{
    return k == CCR_NODE_RETURN || k == CCR_NODE_BREAK || k == CCR_NODE_CONTINUE ||
           k == CCR_NODE_GOTO;
}

/* Nothing runs after this statement: no tab under it. Assembly says so by
 * its mnemonic (jmp, ret; jne goes on). */
static bool stmt_ends_flow(const layout *L, ccr_node_id id)
{
    const ccr_node *n = N(L, id);
    if (n->flags & CCR_FLAG_ASM) {
        if (n->kind == CCR_NODE_CALL && (strcmp(S(L, n->name), "pop") == 0 || strncmp(S(L, n->name), "ldm", 3) == 0))
            for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
                if (strstr(S(L, N(L, c)->name), "pc"))
                    return true;   /* pop {r4, pc}: ARM's way back */
        return (n->kind == CCR_NODE_GOTO && ccr_asm_ends_flow(S(L, n->op))) ||
               (n->kind == CCR_NODE_CALL && ccr_asm_ends_flow(S(L, n->name)));
    }
    return ends_flow(n->kind);
}

static float emit_stmt(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const bool off = (N(L, id)->flags & CCR_FLAG_COMMENTED) != 0;
    const uint32_t first = L->out->prim_count;
    L->dim += off;
    float h = emit_stmt_body(L, id, x, y, max_w);
    L->dim -= off;

    /* a block of its own (not a group of others): notch on top, tab under */
    if (!L->oom && first < L->out->prim_count) {
        const ccr_prim top = L->out->prims[first];
        if (top.node == id && top.kind == CCR_PRIM_BLOCK && top.rect.x == x && top.rect.y == y) {
            L->out->prims[first].notch = true;   /* a real hole: what is under shows */
            if (!stmt_ends_flow(L, id) && (N(L, id)->kind != CCR_NODE_LABEL || (N(L, id)->flags & CCR_FLAG_ASM)))
                add_tab(L, x, y + h, top.fill, top.edge, id);
        }
    }
    return h;
}

/* A CMake command: its name, then its arguments: keywords (PRIVATE,
 * REQUIRED...) as words, everything else a slot. */
static void cmake_items(const layout *L, ccr_node_id id, items *it)
{
    const ccr_node *n = N(L, id);
    it_text(it, S(L, n->name));
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
        const ccr_node *k = N(L, c);
        if (k->kind == CCR_NODE_TEXT && ccr_cmake_is_keyword(S(L, k->name)))
            it_text(it, S(L, k->name));
        else
            it_expr(it, c);
    }
}

static float emit_cmake_stmt(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, id);
    items head = { .n = 0, .used = 0 };
    size2 sz = { 0, 0 };
    switch (n->kind) {
    case CCR_NODE_IF: {
        /* if(), then each elseif() and else() under its mouth, one block */
        enum { MAX_ARMS = 16 };
        items labels[MAX_ARMS];
        mouth m[MAX_ARMS];
        int count = 0;
        it_text(&head, "if");
        it_expr(&head, child_at(L, id, 0));
        ccr_node_id cur = id;
        while (cur != CCR_NONE && count < MAX_ARMS) {
            m[count] = (mouth){ child_at(L, cur, 1), NULL };
            const ccr_node_id other = child_at(L, cur, 2);
            if (other == CCR_NONE)
                break;
            labels[count] = (items){ .n = 0, .used = 0 };
            const ccr_node_id e = N(L, other)->first_child;
            if (e != CCR_NONE && (N(L, e)->flags & CCR_FLAG_ELSEIF)) {
                it_text(&labels[count], "elseif");
                it_expr(&labels[count], child_at(L, e, 0));
                m[count].label_after = &labels[count];
                ++count;
                cur = e;
                continue;
            }
            it_text(&labels[count], "else");
            m[count].label_after = &labels[count];
            ++count;
            if (count < MAX_ARMS)
                m[count] = (mouth){ other, NULL };
            break;
        }
        sz = emit_c_block(L, &head, m, count + 1 > MAX_ARMS ? MAX_ARMS : count + 1, CAT_CONTROL,
                          x, y, id, max_w);
        break;
    }
    case CCR_NODE_SCOPE: {
        it_text(&head, S(L, n->name));
        it_expr(&head, child_at(L, id, 0));
        mouth m = { child_at(L, id, 1), NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_CALL:
        cmake_items(L, id, &head);
        sz = emit_line_block(L, &head, (category)ccr_cmake_category(S(L, n->name)), x, y, id);
        break;
    case CCR_NODE_TEXT:
        it_expr(&head, id);
        sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
        break;
    default:
        it_text(&head, ccr_node_kind_name(n->kind));
        sz = emit_line_block(L, &head, CAT_DRAGON, x, y, id);
        break;
    }
    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz.h;
}

/* An assembly statement: its mnemonic, then each operand a slot. */
static float emit_asm_stmt(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, id);
    items head = { .n = 0, .used = 0 };
    size2 sz = { 0, 0 };
    switch (n->kind) {
    case CCR_NODE_CALL: {
        it_text(&head, S(L, n->type));   /* msg db ..., x = ... */
        it_text(&head, S(L, n->name));
        bool first = true;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
            if (N(L, c)->flags & CCR_FLAG_WORD)
                continue;
            if (!first)
                it_text(&head, ",");
            it_expr(&head, c);
            first = false;
        }
        sz = emit_line_block(L, &head, (category)ccr_asm_category(S(L, n->name)), x, y, id);
        break;
    }
    case CCR_NODE_LABEL:
        push_jump(L, id);
        if (n->flags & CCR_FLAG_NEW) {
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
            it_text(&head, ":");
        } else {
            it_fmt(&head, "%s:", S(L, n->name));
        }
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        break;
    case CCR_NODE_GOTO:
        push_jump(L, id);
        it_text(&head, S(L, n->op));
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
            if (!(N(L, c)->flags & CCR_FLAG_WORD)) {   /* cbz x0, ...: what it tests */
                it_expr(&head, c);
                it_text(&head, ",");
            }
        if (n->flags & CCR_FLAG_NEW)
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
        else
            it_text(&head, S(L, n->name));
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        break;
    case CCR_NODE_SCOPE:
    case CCR_NODE_IF: {
        /* .macro / .rept / .if: a C block, its end written under its mouth */
        const char *name = S(L, n->name);
        it_text(&head, name);
        it_expr(&head, child_at(L, id, 0));
        items els = { .n = 0, .used = 0 }, end = { .n = 0, .used = 0 };
        it_text(&els, ccr_asm_else_word(name));
        it_text(&end, ccr_asm_end_word(name));
        const ccr_node_id other = n->kind == CCR_NODE_IF ? child_at(L, id, 2) : CCR_NONE;
        mouth m[2] = { { child_at(L, id, 1), other != CCR_NONE ? &els : &end }, { other, &end } };
        const bool macro = strstr(name, "macro") != NULL;
        sz = emit_c_block(L, &head, m, other != CCR_NONE ? 2 : 1, macro ? CAT_PREPROC : CAT_CONTROL, x, y, id,
                          max_w);
        break;
    }
    case CCR_NODE_TEXT:
        it_expr(&head, id);
        sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
        break;
    default:
        it_text(&head, ccr_node_kind_name(n->kind));
        sz = emit_line_block(L, &head, CAT_DRAGON, x, y, id);
        break;
    }
    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz.h;
}

/* asm volatile ( instructions ) out: "=r" x  in: "r" a  clobbers: "cc" */
static float emit_inline_asm(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, id);
    items head = { .n = 0, .used = 0 }, foot = { .n = 0, .used = 0 };
    it_text(&head, S(L, n->name));
    it_text(&head, S(L, n->op));
    const ccr_node_id body = child_at(L, id, 0);
    if (n->flags & CCR_FLAG_NEW) {
        static const char *const words[] = { "out:", "in:", "clobbers:" };
        for (uint32_t k = 0; k < 3; ++k) {
            it_text(&foot, words[k]);
            it_expr(&foot, child_at(L, id, 1 + k));
        }
    } else if (n->src_e > n->src_b && n->src_e <= L->ast->source_len) {
        ccr_asm_parts p;
        const char *t = L->ast->source + n->src_b;
        if (ccr_asm_parts_of(t, n->src_e - n->src_b, &p)) {
            int section = -1;
            for (int k = 0; k < p.op_count; ++k) {
                if (p.op_section[k] != section) {
                    section = p.op_section[k];
                    it_text(&foot, section == 0 ? "out:" : "in:");
                }
                it_fmt(&foot, "%.*s", (int)(p.ops[k].ce - p.ops[k].cb), t + p.ops[k].cb);
                const ccr_node_id e = child_at(L, id, 1 + (uint32_t)k);
                if (e != CCR_NONE)
                    it_expr(&foot, e);
                else
                    it_fmt(&foot, "(%.*s)", (int)(p.ops[k].ee - p.ops[k].eb), t + p.ops[k].eb);
            }
            for (int sct = 2; sct < p.sections; ++sct) {
                uint32_t b = p.sec_b[sct], e = p.sec_e[sct];
                while (b < e && isspace((unsigned char)t[b])) ++b;
                while (e > b && isspace((unsigned char)t[e - 1])) --e;
                if (e > b)
                    it_fmt(&foot, "%s %.*s", sct == 2 ? "clobbers:" : "goto:", (int)(e - b), t + b);
            }
        }
    }
    mouth m = { body, foot.n ? &foot : NULL };
    const size2 sz = emit_c_block(L, &head, &m, 1, CAT_PREPROC, x, y, id, max_w);
    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz.h;
}

static float emit_stmt_body(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    if (N(L, id)->flags & CCR_FLAG_ASM)
        return emit_asm_stmt(L, id, x, y, max_w);
    if (N(L, id)->kind == CCR_NODE_ASM)
        return emit_inline_asm(L, id, x, y, max_w);
    if (L->ast->lang == CCR_LANG_CMAKE)
        return emit_cmake_stmt(L, id, x, y, max_w);
    const ccr_node *n = N(L, id);
    const uint32_t kids = child_count(L, id);
    items head = { .n = 0, .used = 0 };
    size2 sz = { 0, 0 };

    switch (n->kind) {
    case CCR_NODE_IF: {
        it_text(&head, "if");
        it_expr(&head, child_at(L, id, 0));
        items els = { .n = 0, .used = 0 };
        it_text(&els, "else");
        mouth m[2] = {
            { child_at(L, id, 1), kids > 2 ? &els : NULL },
            { child_at(L, id, 2), NULL },
        };
        sz = emit_c_block(L, &head, m, kids > 2 ? 2 : 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_WHILE: {
        it_text(&head, "while");
        it_expr(&head, child_at(L, id, 0));
        mouth m = { child_at(L, id, 1), NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_DO_WHILE: {
        it_text(&head, "do");
        items foot = { .n = 0, .used = 0 };
        it_text(&foot, "while");
        it_expr(&foot, child_at(L, id, 1));
        mouth m = { child_at(L, id, 0), &foot };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_FOR: {
        /* the body is always last; the clauses that exist come before it */
        it_text(&head, "for");
        for (uint32_t i = 0; i + 1 < kids; ++i) {
            ccr_node_id c = child_at(L, id, i);
            if (N(L, c)->kind == CCR_NODE_VAR) {
                ccr_node_id init = var_init(L, c);
                it_decl(&head, S(L, N(L, c)->type), S(L, N(L, c)->name), init != CCR_NONE);
                it_expr(&head, init);
            } else {
                it_expr(&head, c);
            }
            if (i + 2 < kids)
                it_text(&head, ";");
        }
        mouth m = { kids ? child_at(L, id, kids - 1) : CCR_NONE, NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_FOR_RANGE: {
        /* for (auto &p : team): the variable, a colon, what it walks */
        it_text(&head, "for");
        const ccr_node_id var = child_at(L, id, 0);
        if (n->flags & CCR_FLAG_NEW) {
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_TYPE));
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
            it_text(&head, ":");
            it_expr(&head, operand(L, id, 0));
            mouth m = { operand(L, id, 1), NULL };
            sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
            break;
        }
        if (var != CCR_NONE && N(L, var)->kind == CCR_NODE_VAR)
            it_decl(&head, S(L, N(L, var)->type), S(L, N(L, var)->name), false);
        it_text(&head, ":");
        if (kids > 2)
            it_expr(&head, child_at(L, id, 1));
        mouth m = { kids ? child_at(L, id, kids - 1) : CCR_NONE, NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_USING:
        if (n->flags & CCR_FLAG_NEW) {
            it_text(&head, "using namespace");
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
        } else {
            it_text(&head, S(L, n->aux));
        }
        sz = emit_line_block(L, &head, CAT_PREPROC, x, y, id);
        break;
    case CCR_NODE_ACCESS:
    case CCR_NODE_NAMESPACE:
    case CCR_NODE_FUNCTION:
    case CCR_NODE_FIELD:
        sz = emit_decl_block(L, id, x, y, max_w);
        break;
    case CCR_NODE_SWITCH: {
        it_text(&head, "switch");
        it_expr(&head, child_at(L, id, 0));
        mouth m = { child_at(L, id, 1), NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_CASE: {
        it_text(&head, "case");
        it_expr(&head, child_at(L, id, 0));
        it_text(&head, ":");
        mouth m = { child_at(L, id, 1), NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_DEFAULT: {
        it_text(&head, "default:");
        mouth m = { child_at(L, id, 0), NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_BLOCK: {
        it_text(&head, "{ }");
        mouth m = { id, NULL };
        sz = emit_c_block(L, &head, &m, 1, CAT_CONTROL, x, y, id, max_w);
        break;
    }
    case CCR_NODE_LABEL: {
        if (n->flags & CCR_FLAG_NEW) {
            /* a new label: its name to type, and nothing hanging off it */
            push_jump(L, id);
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
            it_text(&head, ":");
            const uint32_t line = L->out->prim_count;
            sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
            if (line < L->out->prim_count)
                add_tab(L, x, y + sz.h, L->out->prims[line].fill, L->out->prims[line].edge, id);
            break;
        }
        /* `done:` then the labelled statement right under it */
        push_jump(L, id);
        it_fmt(&head, "%s:", S(L, n->name));
        const uint32_t line = L->out->prim_count;
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        if (n->first_child != CCR_NONE && line < L->out->prim_count)
            add_tab(L, x, y + sz.h, L->out->prims[line].fill, L->out->prims[line].edge, id);
        if (sz.w > *max_w) *max_w = sz.w;
        float h2 = 0.0f;
        if (n->first_child != CCR_NONE)
            h2 = emit_stmt(L, n->first_child, x, y + sz.h, max_w);
        return sz.h + h2;
    }
    case CCR_NODE_VAR: {
        ccr_node_id init = var_init(L, id);
        if (n->flags & CCR_FLAG_NEW) {
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_TYPE));
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
            if (init != CCR_NONE)
                it_text(&head, "=");
            it_expr(&head, init);
            sz = emit_line_block(L, &head, CAT_VARIABLE, x, y, id);
            break;
        }
        it_decl(&head, S(L, n->type), S(L, n->name), init != CCR_NONE);
        it_expr(&head, init);
        sz = emit_line_block(L, &head, CAT_VARIABLE, x, y, id);
        break;
    }
    case CCR_NODE_DECL_GROUP: {
        /* `int a = 1, b = 2;` on one line; with a struct inside, one under another */
        bool all_vars = true;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
            if (N(L, c)->kind != CCR_NODE_VAR)
                all_vars = false;
        if (all_vars) {
            for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
                ccr_node_id init = var_init(L, c);
                if (c != n->first_child)
                    it_text(&head, ",");
                it_decl(&head, S(L, N(L, c)->type), S(L, N(L, c)->name), init != CCR_NONE);
                it_expr(&head, init);
            }
            sz = emit_line_block(L, &head, CAT_VARIABLE, x, y, id);
        } else {
            float h = 0.0f;
            for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
                h += emit_stmt(L, c, x, y + h, max_w);
            return h;
        }
        break;
    }
    case CCR_NODE_RETURN:
        it_text(&head, "return");
        it_expr(&head, n->first_child);
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        break;
    case CCR_NODE_BREAK:    it_text(&head, "break");    goto control_line;
    case CCR_NODE_CONTINUE: it_text(&head, "continue"); goto control_line;
    case CCR_NODE_EMPTY:    it_text(&head, ";");        goto control_line;
    case CCR_NODE_GOTO:
        push_jump(L, id);
        if (n->flags & CCR_FLAG_NEW) {
            it_text(&head, "goto");
            it_expr(&head, word_of(L, id, CCR_FLAG_WORD_NAME));
        } else {
            it_fmt(&head, "goto %s", S(L, n->name));
        }
    control_line:
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        break;
    case CCR_NODE_STRUCT:
    case CCR_NODE_UNION:
    case CCR_NODE_ENUM:
    case CCR_NODE_TYPEDEF:
        sz = emit_decl_block(L, id, x, y, max_w);
        break;
    default:
        if (n->kind == CCR_NODE_TEXT) {
            /* a line of code from the palette: a block holding one text field */
            it_expr(&head, id);
            sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
        } else if (is_expression(n->kind)) {
            /* expression statement: the expression's own items, block-shaped */
            expr_items(L, id, &head);
            sz = emit_line_block(L, &head, stmt_expr_category(L, id), x, y, id);
        } else {
            it_text(&head, *S(L, n->aux) ? S(L, n->aux) : ccr_node_kind_name(n->kind));
            sz = emit_line_block(L, &head, CAT_DRAGON, x, y, id);
        }
        break;
    }

    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz.h;
}

/* Statements one under another, Scratch-style, touching. Each one gets a
 * snap point under it, so a dragged stack can be dropped there. */
static float emit_stack(layout *L, ccr_node_id first, float x, float y, float *max_w)
{
    float h = 0.0f;
    for (ccr_node_id c = first; c != CCR_NONE && !L->oom; c = N(L, c)->next_sibling) {
        float w = 0.0f;
        h += emit_stmt(L, c, x, y + h, &w);
        push_snap(L, CCR_SNAP_AFTER, c, x, y + h, w, 0.0f);
        if (w > *max_w)
            *max_w = w;
    }
    return h;
}

/* -------------------------------------------- top-level declarations */

/* C++ says much more in a head than C (templates, const, initialiser
 * lists, base classes...): the head as written, from the node's start up
 * to `end`, on one line, without its comments and the `{`. */
static bool written_head(const layout *L, ccr_node_id id, uint32_t end, items *it)
{
    const ccr_ast *a = L->ast;
    const ccr_node *n = N(L, id);
    if (!a->cpp || (n->flags & CCR_FLAG_NEW) || !a->source || n->src_e <= n->src_b || end > a->source_len ||
        end <= n->src_b)
        return false;
    char buf[240];
    size_t len = 0;
    bool space = false;
    for (uint32_t i = n->src_b; i < end && len < sizeof buf - 4; ++i) {
        const char c = a->source[i];
        if (c == '/' && i + 1 < end && a->source[i + 1] == '/') {   /* a comment: to its line's end */
            while (i < end && a->source[i] != '\n')
                ++i;
            space = true;
            continue;
        }
        if (c == '/' && i + 1 < end && a->source[i + 1] == '*') {
            i += 2;
            while (i + 1 < end && !(a->source[i] == '*' && a->source[i + 1] == '/'))
                ++i;
            ++i;
            space = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = len > 0;
            continue;
        }
        if (space && len > 0)
            buf[len++] = ' ';
        space = false;
        buf[len++] = c;
    }
    while (len > 0 && (buf[len - 1] == '{' || buf[len - 1] == ';' || buf[len - 1] == ' '))
        --len;
    if (len == 0)
        return false;
    if (len >= sizeof buf - 4)
        memcpy(buf + len, "...", 3), len += 3;
    buf[len] = '\0';
    it_fmt(it, "%s", buf);
    return true;
}

/* Where a C++ function's body starts (its head ends there), or its end. */
static uint32_t head_end(const layout *L, ccr_node_id fn)
{
    for (ccr_node_id c = N(L, fn)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        if (N(L, c)->kind == CCR_NODE_BLOCK && N(L, c)->src_e > N(L, c)->src_b)
            return N(L, c)->src_b;
    return N(L, fn)->src_e;
}

/* Where a C++ class's members start: its first `{`. */
static uint32_t brace_of(const layout *L, ccr_node_id id)
{
    const ccr_node *n = N(L, id);
    for (uint32_t i = n->src_b; i < n->src_e && i < L->ast->source_len; ++i)
        if (L->ast->source[i] == '{')
            return i;
    return n->src_e;
}

static void signature(const layout *L, ccr_node_id fn, items *it)
{
    const ccr_node *n = N(L, fn);
    char buf[512];
    size_t len = 0;
    int w = snprintf(buf, sizeof buf, "%s %s(", S(L, n->aux), S(L, n->name));
    len = (w > 0) ? (size_t)w : 0;

    bool any = false;
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
        if (N(L, c)->kind != CCR_NODE_PARAM || len >= sizeof buf)
            continue;
        w = snprintf(buf + len, sizeof buf - len, "%s%s %s", any ? ", " : "",
                     S(L, N(L, c)->type), S(L, N(L, c)->name));
        if (w > 0) len += (size_t)w;
        any = true;
    }
    if (len < sizeof buf) {
        const char *type = S(L, n->type);
        bool is_void = strstr(type, "(void)") != NULL;
        snprintf(buf + len, sizeof buf - len, "%s)", (!any && is_void) ? "void" : "");
    }
    it_fmt(it, "%s", buf);
}

/* A new function's head: return type, name and parameters, all typed in. */
static void new_signature(const layout *L, ccr_node_id fn, items *it)
{
    it_expr(it, word_of(L, fn, CCR_FLAG_WORD_TYPE));
    it_expr(it, word_of(L, fn, CCR_FLAG_WORD_NAME));
    it_text(it, "(");
    it_expr(it, operand(L, fn, 0));
    it_text(it, ")");
}

static size2 emit_hat(layout *L, ccr_node_id fn, float x, float y, float *max_w)
{
    items head = { .n = 0, .used = 0 };
    if (L->ast->lang == CCR_LANG_CMAKE)
        it_text(&head, S(L, N(L, fn)->name));   /* the file: CMake runs it top to bottom */
    else if (L->ast->lang == CCR_LANG_ASM && strcmp(S(L, N(L, fn)->type), "function") == 0)
        it_text(&head, S(L, N(L, fn)->name));   /* its label is in the stack, after its .globl and .type */
    else if (L->ast->lang == CCR_LANG_ASM && strcmp(S(L, N(L, fn)->type), "label") == 0)
        it_fmt(&head, "%s:", S(L, N(L, fn)->name));   /* where code starts: a global label */
    else if (L->ast->lang == CCR_LANG_ASM)
        it_text(&head, S(L, N(L, fn)->name));   /* a section, or the file */
    else if (N(L, fn)->flags & CCR_FLAG_NEW)
        new_signature(L, fn, &head);
    else if (!written_head(L, fn, head_end(L, fn), &head))
        signature(L, fn, &head);
    if (folded(L, fn))
        it_fmt(&head, "{ %u }", hidden_count(L, fn));   /* the body is folded away */

    float w, h;
    measure_items(L, &head, &w, &h);
    w += BLOCK_PAD_X * 2.0f;
    h += BLOCK_PAD_Y * 2.0f;
    if (h < BLOCK_MIN_H) h = BLOCK_MIN_H;

    /* the round "hat" bump, then the body of the block over it */
    push_shape(L, SHAPE_PILL, CAT_EVENT, (ccr_rect){ x, y, w * 0.5f, HAT_BUMP * 2.0f }, fn);
    const uint32_t hat_prim = L->out->prim_count;
    push_shape(L, SHAPE_BLOCK, CAT_EVENT, (ccr_rect){ x, y + HAT_BUMP, w, h }, fn);
    add_tab(L, x, y + HAT_BUMP + h, L->out->prims[hat_prim].fill, L->out->prims[hat_prim].edge, fn);
    emit_items(L, &head, x + BLOCK_PAD_X, y + HAT_BUMP, h, CAT_EVENT, fn);
    if (w > *max_w) *max_w = w;

    float body_h = 0.0f;
    for (ccr_node_id c = N(L, fn)->first_child; c != CCR_NONE && !folded(L, fn); c = N(L, c)->next_sibling)
        if (N(L, c)->kind == CCR_NODE_BLOCK)
            body_h = emit_body(L, c, x, y + HAT_BUMP + h, max_w);

    return (size2){ *max_w, HAT_BUMP + h + body_h };
}

/* A new declaration from the palette: its names, types and values are
 * fields to type into. False if `id` is not one. */
static bool emit_new_decl(layout *L, ccr_node_id id, float x, float y, float *max_w, size2 *sz)
{
    const ccr_node *n = N(L, id);
    if (!(n->flags & CCR_FLAG_NEW))
        return false;
    items head = { .n = 0, .used = 0 };
    const ccr_node_id name = word_of(L, id, CCR_FLAG_WORD_NAME);
    switch (n->kind) {
    case CCR_NODE_INCLUDE: {
        const bool angled = (n->flags & CCR_FLAG_ANGLED) != 0;
        it_text(&head, "#include");
        it_text(&head, angled ? "<" : "\"");
        it_expr(&head, name);
        it_text(&head, angled ? ">" : "\"");
        *sz = emit_line_block(L, &head, CAT_PREPROC, x, y, id);
        return true;
    }
    case CCR_NODE_MACRO_DEFINE:
        it_text(&head, "#define");
        it_expr(&head, name);
        it_expr(&head, operand(L, id, 0));
        *sz = emit_line_block(L, &head, CAT_PREPROC, x, y, id);
        return true;
    case CCR_NODE_TYPEDEF:
        it_text(&head, "typedef");
        it_expr(&head, word_of(L, id, CCR_FLAG_WORD_TYPE));
        it_expr(&head, name);
        *sz = emit_line_block(L, &head, CAT_TYPE, x, y, id);
        return true;
    case CCR_NODE_ENUM:
        it_text(&head, "enum");
        it_expr(&head, name);
        it_text(&head, "{");
        it_expr(&head, operand(L, id, 0));
        it_text(&head, "}");
        *sz = emit_line_block(L, &head, CAT_TYPE, x, y, id);
        return true;
    case CCR_NODE_STRUCT:
    case CCR_NODE_UNION: {
        /* a C block: the fields are declarations dropped into its mouth */
        it_text(&head, n->kind == CCR_NODE_UNION ? "union" : (n->flags & CCR_FLAG_CLASS) ? "class" : "struct");
        it_expr(&head, name);
        mouth m = { operand(L, id, 0), NULL };
        *sz = emit_c_block(L, &head, &m, 1, CAT_TYPE, x, y, id, max_w);
        return true;
    }
    case CCR_NODE_NAMESPACE: {
        it_text(&head, "namespace");
        it_expr(&head, name);
        mouth m = { operand(L, id, 0), NULL };
        *sz = emit_c_block(L, &head, &m, 1, CAT_PREPROC, x, y, id, max_w);
        return true;
    }
    case CCR_NODE_FUNCTION:
        if (n->flags & CCR_FLAG_DEFINITION) {
            /* a method (in a class's mouth): its head, its body in its own mouth */
            new_signature(L, id, &head);
            mouth m = { operand(L, id, 1), NULL };
            *sz = emit_c_block(L, &head, &m, 1, CAT_EVENT, x, y, id, max_w);
            return true;
        }
        it_text(&head, "declare");   /* a prototype; a definition at the top is a hat */
        new_signature(L, id, &head);
        *sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
        return true;
    default:
        return false;
    }
}

/* One member of a C++ class or namespace, in its mouth. */
static size2 emit_member(layout *L, ccr_node_id c, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, c);
    items line = { .n = 0, .used = 0 };
    size2 sz;
    switch (n->kind) {
    case CCR_NODE_FIELD: {
        ccr_node_id init = CCR_NONE;   /* int hp = 100; */
        for (ccr_node_id k = n->first_child; k != CCR_NONE; k = N(L, k)->next_sibling)
            if (is_expression(N(L, k)->kind))
                init = k;
        it_decl(&line, S(L, n->type), S(L, n->name), init != CCR_NONE);
        it_expr(&line, init);
        sz = emit_line_block(L, &line, CAT_TYPE, x, y, c);
        break;
    }
    case CCR_NODE_ACCESS:
        it_fmt(&line, "%s:", S(L, n->name));
        sz = emit_line_block(L, &line, CAT_TYPE, x, y, c);
        break;
    case CCR_NODE_FUNCTION:
        if ((n->flags & CCR_FLAG_DEFINITION) && !(n->flags & CCR_FLAG_NEW)) {
            /* a method: its head, its body in the mouth, like a hat folded in */
            if (!written_head(L, c, head_end(L, c), &line))
                signature(L, c, &line);
            ccr_node_id body = CCR_NONE;
            for (ccr_node_id k = n->first_child; k != CCR_NONE; k = N(L, k)->next_sibling)
                if (N(L, k)->kind == CCR_NODE_BLOCK)
                    body = k;
            mouth m = { body, NULL };
            sz = emit_c_block(L, &line, &m, 1, CAT_EVENT, x, y, c, max_w);
            break;
        }
        return emit_decl_block(L, c, x, y, max_w);
    default:
        return emit_decl_block(L, c, x, y, max_w);
    }
    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz;
}

/* A C++ class or namespace: its head, its members one under another in
 * its mouth (not a stack: they stay where the file has them). */
static size2 emit_container(layout *L, ccr_node_id id, const items *head, category cat, float x, float y,
                            float *max_w)
{
    if (folded(L, id))
        return emit_folded(L, head, NULL, cat, x, y, id, max_w);
    float hw, hh;
    measure_items(L, head, &hw, &hh);
    hw += BLOCK_PAD_X * 2.0f;
    hh += BLOCK_PAD_Y * 2.0f;
    if (hh < BLOCK_MIN_H) hh = BLOCK_MIN_H;
    const float width = hw < 160.0f ? 160.0f : hw;
    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y, width, hh }, id);
    emit_items(L, head, x + BLOCK_PAD_X, y, hh, cat, id);
    float cy = y + hh;
    /* members can be dropped in: at its top, or under any of them */
    push_snap(L, CCR_SNAP_FIRST, id, x + ARM_W, cy, 120.0f, 0.0f);
    for (ccr_node_id c = N(L, id)->first_child; c != CCR_NONE && !L->oom; c = N(L, c)->next_sibling) {
        const ccr_node_kind k = N(L, c)->kind;
        if (k == CCR_NODE_MACRO_USE || k == CCR_NODE_PARAM || k == CCR_NODE_DESIGNATOR)
            continue;
        float w = 0.0f;
        const size2 ms = (N(L, c)->flags & CCR_FLAG_NEW) ? (size2){ 0, emit_stmt(L, c, x + ARM_W, cy, &w) }
                                                        : emit_member(L, c, x + ARM_W, cy, &w);
        if (ARM_W + w > *max_w)
            *max_w = ARM_W + w;
        cy += ms.h;
        push_snap(L, CCR_SNAP_AFTER, c, x + ARM_W, cy, w, 0.0f);
    }
    float inner = cy - (y + hh);
    if (inner < MOUTH_MIN_H) inner = MOUTH_MIN_H;
    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y + hh, ARM_W, inner }, id);
    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y + hh + inner, width, FOOT_H }, id);
    if (width > *max_w)
        *max_w = width;
    return (size2){ width, hh + inner + FOOT_H };
}

static size2 emit_decl_block(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, id);
    items head = { .n = 0, .used = 0 };
    size2 sz;

    if (n->kind == CCR_NODE_ACCESS || n->kind == CCR_NODE_FIELD)
        return emit_member(L, id, x, y, max_w);
    /* C++: classes and namespaces hold members; heads as written */
    if (!(n->flags & CCR_FLAG_NEW) && L->ast->cpp) {
        switch (n->kind) {
        case CCR_NODE_NAMESPACE:
            if (*S(L, n->name))
                it_fmt(&head, "namespace %s", S(L, n->name));
            else
                it_text(&head, "namespace");
            sz = emit_container(L, id, &head, CAT_PREPROC, x, y, max_w);
            if (sz.w > *max_w)
                *max_w = sz.w;
            return sz;
        case CCR_NODE_STRUCT:
        case CCR_NODE_UNION:
            if (!(n->flags & CCR_FLAG_DEFINITION))
                break;
            if (!written_head(L, id, brace_of(L, id), &head))
                it_fmt(&head, "%s %s", (n->flags & CCR_FLAG_CLASS) ? "class" : "struct", S(L, n->name));
            sz = emit_container(L, id, &head, CAT_TYPE, x, y, max_w);
            if (sz.w > *max_w)
                *max_w = sz.w;
            return sz;
        case CCR_NODE_FUNCTION:
            if (n->flags & CCR_FLAG_DEFINITION)
                break;
            it_text(&head, "declare");
            if (!written_head(L, id, n->src_e, &head))
                signature(L, id, &head);
            sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
            if (sz.w > *max_w)
                *max_w = sz.w;
            return sz;
        case CCR_NODE_FIELD:
        case CCR_NODE_ACCESS:
            return emit_member(L, id, x, y, max_w);
        default:
            break;
        }
    }

    if (emit_new_decl(L, id, x, y, max_w, &sz)) {
        if (sz.w > *max_w)
            *max_w = sz.w;
        return sz;
    }

    switch (n->kind) {
    case CCR_NODE_INCLUDE:
        it_fmt(&head, (n->flags & CCR_FLAG_ANGLED) ? "#include <%s>" : "#include \"%s\"",
               S(L, n->name));
        sz = emit_line_block(L, &head, CAT_PREPROC, x, y, id);
        break;
    case CCR_NODE_MACRO_DEFINE:
        it_fmt(&head, "#define %s", S(L, n->name));
        sz = emit_line_block(L, &head, CAT_PREPROC, x, y, id);
        break;
    case CCR_NODE_TYPEDEF:
        it_fmt(&head, "typedef %s %s", S(L, n->type), S(L, n->name));
        sz = emit_line_block(L, &head, CAT_TYPE, x, y, id);
        break;
    case CCR_NODE_STRUCT:
    case CCR_NODE_UNION:
    case CCR_NODE_ENUM: {
        const char *word = n->kind == CCR_NODE_STRUCT ? "struct"
                         : n->kind == CCR_NODE_UNION  ? "union" : "enum";
        it_fmt(&head, "%s %s", word, S(L, n->name));
        if ((n->flags & CCR_FLAG_DEFINITION) && folded(L, id))
            it_fmt(&head, "{ %u }", hidden_count(L, id));
        if (!(n->flags & CCR_FLAG_DEFINITION) || folded(L, id)) {
            sz = emit_line_block(L, &head, CAT_TYPE, x, y, id);
            break;
        }
        /* fields / constants as small blocks in the mouth */
        float hw, hh;
        measure_items(L, &head, &hw, &hh);
        hw += BLOCK_PAD_X * 2.0f;
        hh += BLOCK_PAD_Y * 2.0f;
        if (hh < BLOCK_MIN_H) hh = BLOCK_MIN_H;
        float width = hw < 120.0f ? 120.0f : hw;
        push_shape(L, SHAPE_BLOCK, CAT_TYPE, (ccr_rect){ x, y, width, hh }, id);
        emit_items(L, &head, x + BLOCK_PAD_X, y, hh, CAT_TYPE, id);

        float cy = y + hh;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(L, c)->next_sibling) {
            items line = { .n = 0, .used = 0 };
            const ccr_node *f = N(L, c);
            if (f->kind == CCR_NODE_FIELD) {
                it_decl(&line, S(L, f->type), S(L, f->name), false);
            } else if (f->kind == CCR_NODE_DECL_GROUP) {   /* int x, y; on one line */
                for (ccr_node_id g = f->first_child; g != CCR_NONE; g = N(L, g)->next_sibling) {
                    if (N(L, g)->kind != CCR_NODE_FIELD)
                        continue;
                    if (g != f->first_child)
                        it_text(&line, ",");
                    it_decl(&line, S(L, N(L, g)->type), S(L, N(L, g)->name), false);
                }
            } else if (f->kind == CCR_NODE_ENUM_CONST) {
                it_fmt(&line, "%s%s", S(L, f->name), f->first_child != CCR_NONE ? " =" : "");
                it_expr(&line, f->first_child);
            } else {
                continue; /* nested definitions are drawn on their own */
            }
            size2 fs = emit_line_block(L, &line, CAT_TYPE, x + ARM_W, cy, c);
            if (ARM_W + fs.w > *max_w) *max_w = ARM_W + fs.w;
            cy += fs.h;
        }
        float inner = cy - (y + hh);
        if (inner < MOUTH_MIN_H) inner = MOUTH_MIN_H;
        push_shape(L, SHAPE_BLOCK, CAT_TYPE, (ccr_rect){ x, y + hh, ARM_W, inner }, id);
        push_shape(L, SHAPE_BLOCK, CAT_TYPE, (ccr_rect){ x, y + hh + inner, width, FOOT_H }, id);
        sz = (size2){ width, hh + inner + FOOT_H };
        break;
    }
    case CCR_NODE_FUNCTION:
        it_text(&head, "declare");
        signature(L, id, &head);
        sz = emit_line_block(L, &head, CAT_CALL, x, y, id);
        break;
    default: {
        float h = emit_stmt(L, id, x, y, max_w);
        sz = (size2){ *max_w, h };
        break;
    }
    }

    if (sz.w > *max_w)
        *max_w = sz.w;
    return sz;
}

/* ---------------------------------------------------------- public */

static void push_snap(layout *L, ccr_snap_kind kind, ccr_node_id ref, float x, float y, float w,
                      float h)
{
    ccr_blocks *b = L->out;
    if (!grow((void **)&b->snaps, &b->snap_cap, b->snap_count + 1, sizeof(ccr_snap))) {
        L->oom = true;
        return;
    }
    b->snaps[b->snap_count++] = (ccr_snap){ kind, ref, { x, y }, w, h };
}

static void push_jump(layout *L, ccr_node_id id)
{
    ccr_blocks *b = L->out;
    if (!grow((void **)&b->jumps, &b->jump_cap, b->jump_count + 1, sizeof(ccr_node_id))) {
        L->oom = true;
        return;
    }
    b->jumps[b->jump_count++] = id;
}

bool ccr_layout_cache_reserve(ccr_layout_cache *c, uint32_t node_count)
{
    if (node_count <= c->cap)
        return true;
    uint32_t cap = c->cap ? c->cap : 256;
    while (cap < node_count)
        cap *= 2;
    float *w = realloc(c->w, (size_t)cap * sizeof(float));
    if (w) c->w = w;
    float *h = realloc(c->h, (size_t)cap * sizeof(float));
    if (h) c->h = h;
    if (!w || !h)
        return false;
    /* 0 = not measured yet */
    memset(c->w + c->cap, 0, (size_t)(cap - c->cap) * sizeof(float));
    memset(c->h + c->cap, 0, (size_t)(cap - c->cap) * sizeof(float));
    c->cap = cap;
    return true;
}

void ccr_layout_cache_free(ccr_layout_cache *c)
{
    free(c->w);
    free(c->h);
    memset(c, 0, sizeof *c);
}

bool ccr_blocks_build_script(ccr_blocks *out, const ccr_ast *ast, ccr_node_id root,
                             ccr_layout_cache *cache)
{
    memset(out, 0, sizeof *out);
    if (root == CCR_NONE || root >= ast->node_count)
        return true;
    if (!ccr_layout_cache_reserve(cache, ast->node_count))
        return false;

    layout L = { .ast = ast, .out = out, .ew = cache->w, .eh = cache->h };
    const ccr_node *n = N(&L, root);
    float w = 0.0f;

    if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_REPORTER)) {
        if (n->first_child != CCR_NONE)
            emit_expr(&L, n->first_child, 0.0f, 0.0f);
    } else if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_TOPLEVEL)) {
        /* a declaration for the file, on its way there from the palette */
        const ccr_node_id d = n->first_child;
        if (d != CCR_NONE && N(&L, d)->kind == CCR_NODE_FUNCTION &&
            (N(&L, d)->flags & CCR_FLAG_DEFINITION))
            emit_hat(&L, d, 0.0f, 0.0f, &w);
        else if (d != CCR_NONE)
            emit_decl_block(&L, d, 0.0f, 0.0f, &w);
    } else if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_LOOSE))
        emit_stack(&L, n->first_child, 0.0f, 0.0f, &w);
    else if (n->kind == CCR_NODE_FUNCTION && (n->flags & CCR_FLAG_DEFINITION))
        emit_hat(&L, root, 0.0f, 0.0f, &w);
    else
        emit_decl_block(&L, root, 0.0f, 0.0f, &w);

    for (uint32_t i = 0; i < L.tab_count && !L.oom; ++i)
        push_prim(&L, L.tabs[i]);
    free(L.tabs);
    if (L.oom) {
        ccr_blocks_free(out);
        return false;
    }
    return true;
}

void ccr_blocks_free(ccr_blocks *b)
{
    free(b->prims);
    free(b->text);
    free(b->snaps);
    free(b->jumps);
    memset(b, 0, sizeof *b);
}

static bool contains(ccr_rect r, ccr_vec2 p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

ccr_node_id ccr_blocks_hit(const ccr_blocks *b, ccr_vec2 local)
{
    if (!contains(b->bounds, local))
        return CCR_NONE;
    /* drawn last = on top */
    for (uint32_t i = b->prim_count; i-- > 0;)
        if (b->prims[i].kind != CCR_PRIM_TEXT && contains(b->prims[i].rect, local))
            return b->prims[i].node;
    return CCR_NONE;
}

bool ccr_blocks_hit_text(const ccr_blocks *b, ccr_vec2 local, ccr_node_id *node,
                         const char **text, ccr_rect *rect)
{
    if (!contains(b->bounds, local))
        return false;
    for (uint32_t i = b->prim_count; i-- > 0;) {
        const ccr_prim *p = &b->prims[i];
        /* a little room around the word: operators are small */
        const ccr_rect r = { p->rect.x - 4.0f, p->rect.y - 3.0f, p->rect.w + 8.0f, p->rect.h + 6.0f };
        if (p->kind == CCR_PRIM_TEXT && contains(r, local)) {
            *node = p->node;
            *text = b->text + p->text;
            *rect = p->rect;
            return true;
        }
    }
    return false;
}

bool ccr_blocks_find(const ccr_blocks *b, ccr_node_id node, ccr_rect *out)
{
    for (uint32_t i = 0; i < b->prim_count; ++i) {
        if (b->prims[i].node == node && b->prims[i].kind != CCR_PRIM_TEXT &&
            b->prims[i].kind != CCR_PRIM_TAB) {
            *out = b->prims[i].rect;
            return true;
        }
    }
    return false;
}

bool ccr_blocks_find_head(const ccr_blocks *b, ccr_node_id node, ccr_rect *out)
{
    bool found = false;
    ccr_rect u = { 0 }, first = { 0 };
    for (uint32_t i = 0; i < b->prim_count; ++i) {
        const ccr_prim *p = &b->prims[i];
        if (p->node != node || p->kind == CCR_PRIM_TEXT || p->kind == CCR_PRIM_TAB)
            continue;
        if (!found) {
            first = u = p->rect;
            found = true;
            continue;
        }
        if (p->rect.y >= first.y + first.h - 0.5f)
            continue;   /* starts below the first piece: an arm, a foot */
        const float x1 = fmaxf(u.x + u.w, p->rect.x + p->rect.w), y1 = fmaxf(u.y + u.h, p->rect.y + p->rect.h);
        u.x = fminf(u.x, p->rect.x);
        u.y = fminf(u.y, p->rect.y);
        u.w = x1 - u.x;
        u.h = y1 - u.y;
    }
    if (found)
        *out = u;
    return found;
}

/* --------------------------------------------------------- drawing */

static bool overlaps(ccr_rect a, ccr_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static ccr_color fade(ccr_color c, float alpha)
{
    if (alpha >= 1.0f)
        return c;
    uint32_t a = (uint32_t)((float)(c & 0xFFu) * alpha);
    return (c & 0xFFFFFF00u) | (a & 0xFFu);
}

/*
 * The block shape with the puzzle notch cut out of its top, as a real hole
 * (what is under the block shows through it), in one pass: three pieces
 * that do not overlap - the column left of the notch with the block's two
 * left corners rounded, the column right of it with the right corners,
 * and the part under the notch. With an outline the same is done again,
 * inset by its width, so the notch gets walls and a floor for free.
 */
static void notched_shape(ccr_rect r, float rad, float z, float inset, ccr_color c)
{
    const float x1 = r.x + TAB_X * z, x2 = r.x + (TAB_X + TAB_W) * z, y1 = r.y + TAB_H * z;
    const float e = inset;
    const ccr_rect in = { r.x + e, r.y + e, r.w - e * 2.0f, r.h - e * 2.0f };
    if (in.w <= 0.0f || in.h <= 0.0f)
        return;
    const float rr = rad - e > 0.0f ? rad - e : 0.0f;
    ccr_gui_rect_corners((ccr_rect){ in.x, in.y, (x1 - e) - in.x, in.h }, rr,
                         CCR_CORNER_TL | CCR_CORNER_BL, c);
    ccr_gui_rect_corners((ccr_rect){ x2 + e, in.y, in.x + in.w - (x2 + e), in.h }, rr,
                         CCR_CORNER_TR | CCR_CORNER_BR, c);
    ccr_gui_rect((ccr_rect){ x1 - e, y1 + e, (x2 + e) - (x1 - e), in.y + in.h - (y1 + e) }, c);
}

static void draw_notched(ccr_rect r, float roundness, float z, float e, ccr_color edge, ccr_color fill,
                         bool outline)
{
    /* the radius raylib's rounded rectangles use, so the corners match */
    const float rad = roundness * (r.w < r.h ? r.w : r.h) * 0.5f;
    notched_shape(r, rad, z, 0.0f, outline ? edge : fill);
    if (outline)
        notched_shape(r, rad, z, e, fill);
}

void ccr_blocks_draw_shadow(const ccr_blocks *b, const ccr_canvas *cv, ccr_rect viewport,
                            ccr_vec2 offset, ccr_color color)
{
    const float z = cv->zoom;
    for (uint32_t i = 0; i < b->prim_count; ++i) {
        const ccr_prim *p = &b->prims[i];
        if (p->kind == CCR_PRIM_TEXT)
            continue;
        ccr_vec2 tl = ccr_canvas_to_screen(cv, (ccr_vec2){ p->rect.x + offset.x, p->rect.y + offset.y });
        ccr_rect r = { tl.x, tl.y, p->rect.w * z, p->rect.h * z };
        if (!overlaps(r, viewport))
            continue;
        if (p->kind == CCR_PRIM_HEX)
            ccr_gui_hex(r, color);
        else if (p->notch)
            draw_notched(r, p->roundness, z, 0.0f, color, color, false);
        else
            ccr_gui_rect_rounded(r, p->roundness, color);
    }
}

void ccr_blocks_draw(const ccr_blocks *b, const ccr_canvas *cv, ccr_rect viewport,
                     ccr_vec2 offset, float alpha)
{
    const float z = cv->zoom;
    const float edge = z < 1.0f ? 1.0f : z;   /* outline at least 1px */

    /* whole script off screen: nothing to do */
    ccr_vec2 btl = ccr_canvas_to_screen(cv, (ccr_vec2){ b->bounds.x + offset.x, b->bounds.y + offset.y });
    if (!overlaps((ccr_rect){ btl.x, btl.y, b->bounds.w * z, b->bounds.h * z }, viewport))
        return;

    for (uint32_t i = 0; i < b->prim_count; ++i) {
        const ccr_prim *p = &b->prims[i];
        ccr_vec2 tl = ccr_canvas_to_screen(cv, (ccr_vec2){ p->rect.x + offset.x, p->rect.y + offset.y });
        ccr_rect r = { tl.x, tl.y, p->rect.w * z, p->rect.h * z };
        if (!overlaps(r, viewport))
            continue;

        switch (p->kind) {
        case CCR_PRIM_BLOCK: {
            if (p->notch) {
                draw_notched(r, p->roundness, z, edge, fade(p->edge, alpha), fade(p->fill, alpha), true);
                break;
            }
            ccr_rect inner = { r.x + edge, r.y + edge, r.w - edge * 2.0f, r.h - edge * 2.0f };
            ccr_gui_rect_rounded(r, p->roundness, fade(p->edge, alpha));
            if (inner.w > 0.0f && inner.h > 0.0f)
                ccr_gui_rect_rounded(inner, p->roundness, fade(p->fill, alpha));
            break;
        }
        case CCR_PRIM_HEX: {
            ccr_rect inner = { r.x + edge * 1.5f, r.y + edge, r.w - edge * 3.0f, r.h - edge * 2.0f };
            ccr_gui_hex(r, fade(p->edge, alpha));
            if (inner.w > 0.0f && inner.h > 0.0f)
                ccr_gui_hex(inner, fade(p->fill, alpha));
            break;
        }
        case CCR_PRIM_TAB: {
            /* The outline follows the tab: sides and a rounded bottom. Its
             * top is open, and filled up past both blocks' edges so the
             * seam between them does not cut straight across it. */
            ccr_gui_rect_rounded((ccr_rect){ r.x, r.y - edge, r.w, r.h + edge }, 0.7f,
                                 fade(p->edge, alpha));
            ccr_gui_rect_rounded((ccr_rect){ r.x + edge, r.y - edge, r.w - edge * 2.0f, r.h }, 0.7f,
                                 fade(p->fill, alpha));
            ccr_gui_rect((ccr_rect){ r.x + edge, r.y - edge * 2.0f - 1.0f, r.w - edge * 2.0f,
                                     edge * 3.0f + 1.0f }, fade(p->fill, alpha));
            break;
        }
        case CCR_PRIM_ARROW:
            if (z >= 0.35f)
                ccr_gui_triangle((ccr_vec2){ r.x, r.y }, (ccr_vec2){ r.x + r.w, r.y },
                                 (ccr_vec2){ r.x + r.w * 0.5f, r.y + r.h }, fade(p->fill, alpha * 0.75f));
            break;
        case CCR_PRIM_TEXT:
            if (p->font * z >= 5.0f)  /* unreadable below that anyway */
                ccr_gui_text(b->text + p->text, r.x, r.y, p->font * z, fade(p->fill, alpha));
            break;
        }
    }
}
