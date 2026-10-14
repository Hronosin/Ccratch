#include "ccr_blocks.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ look */

#define FONT        20.0f   /* default font is 10px; multiples stay crisp */
#define GAP          8.0f   /* between items on one line */
#define BLOCK_PAD_X 12.0f
#define BLOCK_PAD_Y  8.0f
#define BLOCK_MIN_H 44.0f
#define BLOCK_MIN_W 60.0f
#define EXPR_PAD_X   9.0f
#define EXPR_PAD_Y   4.0f
#define ARM_W       20.0f   /* left side of a C-block */
#define MOUTH_MIN_H 24.0f
#define FOOT_H      24.0f   /* bottom bar of a C-block */
#define HAT_BUMP    22.0f

typedef enum category {
    CAT_CONTROL, CAT_OPERATOR, CAT_VARIABLE, CAT_CALL, CAT_EVENT,
    CAT_TYPE, CAT_PREPROC, CAT_POINTER, CAT_LITERAL, CAT_DRAGON,
    CAT_COUNT
} category;

/* Scratch 3 palette, more or less. fill, edge, text */
static const ccr_color g_palette[CAT_COUNT][3] = {
    [CAT_CONTROL]  = { 0xFFAB19FF, 0xCF8B17FF, 0xFFFFFFFF },
    [CAT_OPERATOR] = { 0x59C059FF, 0x389438FF, 0xFFFFFFFF },
    [CAT_VARIABLE] = { 0xFF8C1AFF, 0xDB6E00FF, 0xFFFFFFFF },
    [CAT_CALL]     = { 0xFF6680FF, 0xFF3355FF, 0xFFFFFFFF },
    [CAT_EVENT]    = { 0xFFBF00FF, 0xCC9900FF, 0xFFFFFFFF },
    [CAT_TYPE]     = { 0x5CB1D6FF, 0x2E8EB8FF, 0xFFFFFFFF },
    [CAT_PREPROC]  = { 0x9966FFFF, 0x774DCBFF, 0xFFFFFFFF },
    [CAT_POINTER]  = { 0xE6544DFF, 0xB8372FFF, 0xFFFFFFFF },
    [CAT_LITERAL]  = { 0xFFFFFFFF, 0xC8CCD6FF, 0x575E75FF },
    [CAT_DRAGON]   = { 0x8A8F9CFF, 0x5F6470FF, 0xFFFFFFFF },
};

typedef enum shape { SHAPE_PILL, SHAPE_HEX, SHAPE_BLOCK } shape;

/* ------------------------------------------------------- the layout */

typedef struct layout {
    const ccr_ast *ast;
    ccr_blocks    *out;
    float         *ew, *eh;   /* cached expression sizes, per node */
    bool           oom;
} layout;

static void push_snap(layout *L, ccr_snap_kind kind, ccr_node_id ref, float x, float y, float w);

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

static void push_shape(layout *L, shape sh, category cat, ccr_rect r, ccr_node_id node)
{
    ccr_prim p = {
        .kind = (sh == SHAPE_HEX) ? CCR_PRIM_HEX : CCR_PRIM_BLOCK,
        .rect = r,
        .fill = g_palette[cat][0],
        .edge = g_palette[cat][1],
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
        .fill = color,
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

static void expr_look(const layout *L, ccr_node_id id, shape *sh, category *cat)
{
    const ccr_node *n = N(L, id);
    const char *op = S(L, n->op);
    *sh = SHAPE_PILL;

    switch (n->kind) {
    case CCR_NODE_REF:       *cat = CAT_VARIABLE; break;
    case CCR_NODE_MEMBER:
    case CCR_NODE_INDEX:     *cat = CAT_VARIABLE; break;
    case CCR_NODE_INT_LIT:
    case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT:
    case CCR_NODE_STRING_LIT:
    case CCR_NODE_INIT_LIST:
    case CCR_NODE_COMPOUND_LIT: *cat = CAT_LITERAL; break;
    case CCR_NODE_CALL:      *cat = CAT_CALL; break;
    case CCR_NODE_BINARY_OP:
        *cat = op_is_assign(op) ? CAT_VARIABLE : CAT_OPERATOR;
        if (op_is_bool(op))
            *sh = SHAPE_HEX;
        break;
    case CCR_NODE_UNARY_OP:
        if (strcmp(op, "&") == 0 || strcmp(op, "*") == 0) {
            *cat = CAT_POINTER;            /* future wires start here */
        } else {
            *cat = CAT_OPERATOR;
            if (op_is_bool(op))
                *sh = SHAPE_HEX;
        }
        break;
    case CCR_NODE_MACRO_VALUE: *cat = CAT_PREPROC; break;
    case CCR_NODE_OTHER:     *cat = CAT_DRAGON; break;
    default:                 *cat = CAT_OPERATOR; break;
    }
}

static void args_with_commas(const layout *L, ccr_node_id first, items *it)
{
    for (ccr_node_id c = first; c != CCR_NONE; c = N(L, c)->next_sibling) {
        /* `.x = 1` arrives as two siblings: the designator, then the value */
        if (N(L, c)->kind == CCR_NODE_DESIGNATOR) {
            it_fmt(it, ".%s =", S(L, N(L, c)->name));
            continue;
        }
        it_expr(it, c);
        if (N(L, c)->next_sibling != CCR_NONE)
            it_text(it, ",");
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

    switch (n->kind) {
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
        it_fmt(it, "%s (", *name ? name : "call");
        args_with_commas(L, c0, it);
        it_text(it, ")");
        break;
    case CCR_NODE_MEMBER:
        it_expr(it, c0);
        it_fmt(it, "%s%s", (n->flags & CCR_FLAG_ARROW) ? "->" : ".", name);
        break;
    case CCR_NODE_INDEX:
        it_expr(it, c0);
        it_text(it, "[");
        it_expr(it, c1);
        it_text(it, "]");
        break;
    case CCR_NODE_CAST:
    case CCR_NODE_COMPOUND_LIT:
        it_fmt(it, "(%s)", S(L, n->type));
        it_expr(it, c0);
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

    L->ew[id] = w;
    L->eh[id] = h;
}

static void emit_expr(layout *L, ccr_node_id id, float x, float y);

/* Lays out a line of items starting at (x, y), centered in height h. */
static void emit_items(layout *L, const items *it, float x, float y, float h,
                       category cat, ccr_node_id owner)
{
    const ccr_color text_color = g_palette[cat][2];
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

    items it = { .n = 0, .used = 0 };
    expr_items(L, id, &it);
    float pad = EXPR_PAD_X + (sh == SHAPE_HEX ? h * 0.25f : 0.0f);
    emit_items(L, &it, x + pad, y, h, cat, id);
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
        push_snap(L, CCR_SNAP_FIRST, body, x, y, 120.0f);
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

static size2 emit_c_block(layout *L, const items *head, const mouth *mouths, int mouth_count,
                          category cat, float x, float y, ccr_node_id owner, float *max_w)
{
    float hw, hh;
    measure_items(L, head, &hw, &hh);
    hw += BLOCK_PAD_X * 2.0f;
    hh += BLOCK_PAD_Y * 2.0f;
    if (hh < BLOCK_MIN_H) hh = BLOCK_MIN_H;
    float width = hw < 120.0f ? 120.0f : hw;

    push_shape(L, SHAPE_BLOCK, cat, (ccr_rect){ x, y, width, hh }, owner);
    emit_items(L, head, x + BLOCK_PAD_X, y, hh, cat, owner);

    float cy = y + hh;
    for (int m = 0; m < mouth_count; ++m) {
        float inner_w = 0.0f;
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
    case CCR_NODE_INT_LIT: case CCR_NODE_FLOAT_LIT:
    case CCR_NODE_CHAR_LIT: case CCR_NODE_STRING_LIT:
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

static float emit_stmt(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
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
        /* `done:` then the labelled statement right under it */
        it_fmt(&head, "%s:", S(L, n->name));
        sz = emit_line_block(L, &head, CAT_CONTROL, x, y, id);
        if (sz.w > *max_w) *max_w = sz.w;
        float h2 = 0.0f;
        if (n->first_child != CCR_NONE)
            h2 = emit_stmt(L, n->first_child, x, y + sz.h, max_w);
        return sz.h + h2;
    }
    case CCR_NODE_VAR: {
        ccr_node_id init = var_init(L, id);
        it_decl(&head, S(L, n->type), S(L, n->name), init != CCR_NONE);
        it_expr(&head, init);
        sz = emit_line_block(L, &head, CAT_VARIABLE, x, y, id);
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
        it_fmt(&head, "goto %s", S(L, n->name));
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
        if (is_expression(n->kind)) {
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
        push_snap(L, CCR_SNAP_AFTER, c, x, y + h, w);
        if (w > *max_w)
            *max_w = w;
    }
    return h;
}

/* -------------------------------------------- top-level declarations */

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

static size2 emit_hat(layout *L, ccr_node_id fn, float x, float y, float *max_w)
{
    items head = { .n = 0, .used = 0 };
    signature(L, fn, &head);

    float w, h;
    measure_items(L, &head, &w, &h);
    w += BLOCK_PAD_X * 2.0f;
    h += BLOCK_PAD_Y * 2.0f;
    if (h < BLOCK_MIN_H) h = BLOCK_MIN_H;

    /* the round "hat" bump, then the body of the block over it */
    push_shape(L, SHAPE_PILL, CAT_EVENT, (ccr_rect){ x, y, w * 0.5f, HAT_BUMP * 2.0f }, fn);
    push_shape(L, SHAPE_BLOCK, CAT_EVENT, (ccr_rect){ x, y + HAT_BUMP, w, h }, fn);
    emit_items(L, &head, x + BLOCK_PAD_X, y + HAT_BUMP, h, CAT_EVENT, fn);
    if (w > *max_w) *max_w = w;

    float body_h = 0.0f;
    for (ccr_node_id c = N(L, fn)->first_child; c != CCR_NONE; c = N(L, c)->next_sibling)
        if (N(L, c)->kind == CCR_NODE_BLOCK)
            body_h = emit_body(L, c, x, y + HAT_BUMP + h, max_w);

    return (size2){ *max_w, HAT_BUMP + h + body_h };
}

static size2 emit_decl_block(layout *L, ccr_node_id id, float x, float y, float *max_w)
{
    const ccr_node *n = N(L, id);
    items head = { .n = 0, .used = 0 };
    size2 sz;

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
        if (!(n->flags & CCR_FLAG_DEFINITION)) {
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

static void push_snap(layout *L, ccr_snap_kind kind, ccr_node_id ref, float x, float y, float w)
{
    ccr_blocks *b = L->out;
    if (!grow((void **)&b->snaps, &b->snap_cap, b->snap_count + 1, sizeof(ccr_snap))) {
        L->oom = true;
        return;
    }
    b->snaps[b->snap_count++] = (ccr_snap){ kind, ref, { x, y }, w };
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

    if (n->kind == CCR_NODE_BLOCK && (n->flags & CCR_FLAG_LOOSE))
        emit_stack(&L, n->first_child, 0.0f, 0.0f, &w);
    else if (n->kind == CCR_NODE_FUNCTION && (n->flags & CCR_FLAG_DEFINITION))
        emit_hat(&L, root, 0.0f, 0.0f, &w);
    else
        emit_decl_block(&L, root, 0.0f, 0.0f, &w);

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

bool ccr_blocks_find(const ccr_blocks *b, ccr_node_id node, ccr_rect *out)
{
    for (uint32_t i = 0; i < b->prim_count; ++i) {
        if (b->prims[i].node == node && b->prims[i].kind != CCR_PRIM_TEXT) {
            *out = b->prims[i].rect;
            return true;
        }
    }
    return false;
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
        case CCR_PRIM_TEXT:
            if (p->font * z >= 5.0f)  /* unreadable below that anyway */
                ccr_gui_text(b->text + p->text, r.x, r.y, p->font * z, fade(p->fill, alpha));
            break;
        }
    }
}
