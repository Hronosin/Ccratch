#include "ccr_editor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_GAP     32.0f   /* between top-level scripts in a column */
#define COLUMN_GAP     96.0f
#define DRAG_THRESHOLD  4.0f   /* screen pixels before a press becomes a drag */
#define SNAP_RADIUS    36.0f   /* world units */
#define LOOSE_ALPHA     0.55f

static const ccr_node *N(const ccr_editor *ed, ccr_node_id id) { return &ed->ast.nodes[id]; }

/* -------------------------------------------------------- scripts */

static bool push_script(ccr_editor *ed, ccr_node_id root, ccr_vec2 pos)
{
    if (ed->script_count == ed->script_cap) {
        uint32_t cap = ed->script_cap ? ed->script_cap * 2 : 64;
        ccr_script *s = realloc(ed->scripts, cap * sizeof *s);
        if (!s)
            return false;
        ed->scripts = s;
        ed->script_cap = cap;
    }
    ccr_script *s = &ed->scripts[ed->script_count];
    memset(s, 0, sizeof *s);
    s->root = root;
    s->pos = pos;
    if (!ccr_blocks_build_script(&s->blocks, &ed->ast, root, &ed->cache))
        return false;
    ed->script_count++;
    return true;
}

static float bottom_of(const ccr_script *s)
{
    return s->pos.y + s->blocks.bounds.y + s->blocks.bounds.h;
}

/*
 * Re-lays out one script. If it grew taller, scripts sitting below it in
 * the same column are pushed down by the same amount, so a stack that got
 * a few more blocks does not end up on top of its neighbour.
 */
static void relayout(ccr_editor *ed, uint32_t index)
{
    ccr_script *s = &ed->scripts[index];
    ccr_blocks fresh;
    if (!ccr_blocks_build_script(&fresh, &ed->ast, s->root, &ed->cache))
        return;

    const float old_bottom = bottom_of(s);
    const ccr_rect old = s->blocks.bounds;
    ccr_blocks_free(&s->blocks);
    s->blocks = fresh;

    const float grew = bottom_of(s) - old_bottom;
    if (grew <= 0.0f)
        return;

    const float x0 = s->pos.x + (old.x < fresh.bounds.x ? old.x : fresh.bounds.x);
    const float x1 = s->pos.x + (old.x + old.w > fresh.bounds.x + fresh.bounds.w
                                 ? old.x + old.w : fresh.bounds.x + fresh.bounds.w);
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        ccr_script *o = &ed->scripts[i];
        if (i == index || i == ed->dragged)
            continue;
        const float ox0 = o->pos.x + o->blocks.bounds.x;
        const float ox1 = ox0 + o->blocks.bounds.w;
        const float otop = o->pos.y + o->blocks.bounds.y;
        if (ox1 > x0 && ox0 < x1 && otop >= old_bottom - 1.0f)
            o->pos.y += grew;
    }
}

static void remove_script(ccr_editor *ed, uint32_t index)
{
    ccr_blocks_free(&ed->scripts[index].blocks);
    memmove(&ed->scripts[index], &ed->scripts[index + 1],
            (ed->script_count - index - 1) * sizeof(ccr_script));
    ed->script_count--;
}

/* Moves a script to the end of the draw order, so it is on top. */
static uint32_t bring_to_front(ccr_editor *ed, uint32_t index)
{
    ccr_script tmp = ed->scripts[index];
    memmove(&ed->scripts[index], &ed->scripts[index + 1],
            (ed->script_count - index - 1) * sizeof(ccr_script));
    ed->scripts[ed->script_count - 1] = tmp;
    return ed->script_count - 1;
}

static bool is_loose_root(const ccr_editor *ed, ccr_node_id id)
{
    return N(ed, id)->parent == CCR_NONE && (N(ed, id)->flags & CCR_FLAG_LOOSE);
}

/* The top-level declaration or loose Block that `id` lives in. */
static ccr_node_id script_root_of(const ccr_editor *ed, ccr_node_id id)
{
    while (id != CCR_NONE) {
        ccr_node_id p = N(ed, id)->parent;
        if (p == CCR_NONE || N(ed, p)->kind == CCR_NODE_TRANSLATION_UNIT)
            return id;
        id = p;
    }
    return CCR_NONE;
}

static uint32_t script_index_of_root(const ccr_editor *ed, ccr_node_id root)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        if (ed->scripts[i].root == root)
            return i;
    return UINT32_MAX;
}

/*
 * The statement in a stack that a click on `id` grabs: climb until the
 * parent is a Block. CCR_NONE means the click was on something that only
 * moves as a whole (a hat, a struct, an #include).
 */
static ccr_node_id stack_item_of(const ccr_editor *ed, ccr_node_id id)
{
    while (id != CCR_NONE) {
        ccr_node_id p = N(ed, id)->parent;
        if (p == CCR_NONE || N(ed, p)->kind == CCR_NODE_TRANSLATION_UNIT)
            return CCR_NONE;
        if (N(ed, p)->kind == CCR_NODE_BLOCK)
            return id;
        id = p;
    }
    return CCR_NONE;
}

/* ----------------------------------------------------------- init */

bool ccr_editor_init(ccr_editor *ed, ccr_ast *ast)
{
    memset(ed, 0, sizeof *ed);
    ed->ast = *ast;
    memset(ast, 0, sizeof *ast);   /* ownership moved */
    ed->dragged = UINT32_MAX;
    if (ed->ast.node_count == 0)
        return true;

    /* Column 1: preprocessor and types. Column 2: functions. */
    const ccr_node_id tu = 0;
    float y = 0.0f, col1_w = 0.0f;
    for (ccr_node_id c = N(ed, tu)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
        const ccr_node *n = N(ed, c);
        if (n->kind == CCR_NODE_MACRO_USE)
            continue;   /* shown where they are used, not as scripts */
        if (n->kind == CCR_NODE_FUNCTION && (n->flags & CCR_FLAG_DEFINITION))
            continue;
        if (!push_script(ed, c, (ccr_vec2){ 0.0f, y }))
            return false;
        const ccr_rect b = ed->scripts[ed->script_count - 1].blocks.bounds;
        y += b.y + b.h + SCRIPT_GAP * 0.5f;
        if (b.x + b.w > col1_w)
            col1_w = b.x + b.w;
    }

    const float x2 = col1_w > 0.0f ? col1_w + COLUMN_GAP : 0.0f;
    y = 0.0f;
    for (ccr_node_id c = N(ed, tu)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
        const ccr_node *n = N(ed, c);
        if (n->kind != CCR_NODE_FUNCTION || !(n->flags & CCR_FLAG_DEFINITION))
            continue;
        if (!push_script(ed, c, (ccr_vec2){ x2, y }))
            return false;
        const ccr_rect b = ed->scripts[ed->script_count - 1].blocks.bounds;
        y += b.y + b.h + SCRIPT_GAP;
    }
    return true;
}

void ccr_editor_free(ccr_editor *ed)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        ccr_blocks_free(&ed->scripts[i].blocks);
    free(ed->scripts);
    ccr_layout_cache_free(&ed->cache);
    ccr_ast_free(&ed->ast);
    memset(ed, 0, sizeof *ed);
}

/* ---------------------------------------------------------- queries */

ccr_rect ccr_editor_bounds(const ccr_editor *ed)
{
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        const ccr_script *s = &ed->scripts[i];
        const ccr_rect b = s->blocks.bounds;
        if (b.w <= 0.0f && b.h <= 0.0f)
            continue;
        float bx0 = s->pos.x + b.x, by0 = s->pos.y + b.y;
        float bx1 = bx0 + b.w, by1 = by0 + b.h;
        if (!any) {
            x0 = bx0; y0 = by0; x1 = bx1; y1 = by1;
            any = true;
        } else {
            if (bx0 < x0) x0 = bx0;
            if (by0 < y0) y0 = by0;
            if (bx1 > x1) x1 = bx1;
            if (by1 > y1) y1 = by1;
        }
    }
    return (ccr_rect){ x0, y0, x1 - x0, y1 - y0 };
}

bool ccr_editor_node_rect(const ccr_editor *ed, ccr_node_id node, ccr_rect *out)
{
    if (node >= ed->ast.node_count)
        return false;
    uint32_t i = script_index_of_root(ed, script_root_of(ed, node));
    if (i == UINT32_MAX)
        return false;
    ccr_rect r;
    if (!ccr_blocks_find(&ed->scripts[i].blocks, node, &r))
        return false;
    *out = (ccr_rect){ r.x + ed->scripts[i].pos.x, r.y + ed->scripts[i].pos.y, r.w, r.h };
    return true;
}

uint32_t ccr_editor_loose_count(const ccr_editor *ed)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ed->script_count; ++i)
        if (is_loose_root(ed, ed->scripts[i].root))
            ++n;
    return n;
}

/* Topmost script and node under a world point. */
static ccr_node_id pick(const ccr_editor *ed, ccr_vec2 world, uint32_t *script)
{
    for (uint32_t i = ed->script_count; i-- > 0;) {
        const ccr_script *s = &ed->scripts[i];
        ccr_node_id hit = ccr_blocks_hit(&s->blocks,
                                         (ccr_vec2){ world.x - s->pos.x, world.y - s->pos.y });
        if (hit != CCR_NONE) {
            *script = i;
            return hit;
        }
    }
    return CCR_NONE;
}

/* --------------------------------------------------------- dragging */

/* Detaches `item` and every statement after it into a new loose stack. */
static uint32_t tear_off(ccr_editor *ed, ccr_node_id item, uint32_t from)
{
    ccr_rect at;
    if (!ccr_editor_node_rect(ed, item, &at))
        return UINT32_MAX;

    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE)
        return UINT32_MAX;
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE;

    for (ccr_node_id c = item; c != CCR_NONE;) {
        ccr_node_id next = N(ed, c)->next_sibling;
        ccr_ast_append(&ed->ast, loose, c);
        c = next;
    }

    relayout(ed, from);
    if (!push_script(ed, loose, (ccr_vec2){ at.x, at.y }))
        return UINT32_MAX;
    return ed->script_count - 1;
}

static void begin_drag(ccr_editor *ed, ccr_vec2 mouse)
{
    uint32_t si = UINT32_MAX;
    ccr_node_id hit = pick(ed, ed->press_at, &si);
    if (hit == CCR_NONE) {
        ed->drag = CCR_DRAG_IDLE;
        return;
    }

    ccr_node_id item = stack_item_of(ed, hit);
    uint32_t moving;
    const ccr_node_id root = ed->scripts[si].root;

    if (item != CCR_NONE &&
        !(is_loose_root(ed, root) && N(ed, root)->first_child == item)) {
        moving = tear_off(ed, item, si);    /* part of a stack: take the tail */
    } else {
        moving = bring_to_front(ed, si);    /* the whole script */
    }
    if (moving == UINT32_MAX) {
        ed->drag = CCR_DRAG_IDLE;
        return;
    }

    ed->dragged = moving;
    ed->grab = (ccr_vec2){ ed->press_at.x - ed->scripts[moving].pos.x,
                           ed->press_at.y - ed->scripts[moving].pos.y };
    ed->scripts[moving].pos = (ccr_vec2){ mouse.x - ed->grab.x, mouse.y - ed->grab.y };
    ed->drag = CCR_DRAG_MOVING;
}

/* Nearest snap point of any other script to the dragged stack's top-left. */
static void find_snap(ccr_editor *ed)
{
    ed->snap_ok = false;
    const ccr_script *d = &ed->scripts[ed->dragged];
    if (!is_loose_root(ed, d->root))
        return;   /* functions and structs do not snap into stacks */

    float best = SNAP_RADIUS * SNAP_RADIUS;
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        if (i == ed->dragged)
            continue;
        const ccr_script *s = &ed->scripts[i];
        for (uint32_t k = 0; k < s->blocks.snap_count; ++k) {
            ccr_snap sn = s->blocks.snaps[k];
            sn.at.x += s->pos.x;
            sn.at.y += s->pos.y;
            float dx = sn.at.x - d->pos.x, dy = sn.at.y - d->pos.y;
            float dist = dx * dx + dy * dy;
            if (dist < best) {
                best = dist;
                ed->snap = sn;
                ed->snap_ok = true;
            }
        }
    }
}

/* Moves the dragged loose stack into place at the current snap. */
static void drop_into_snap(ccr_editor *ed)
{
    const ccr_node_id loose = ed->scripts[ed->dragged].root;
    const ccr_node_id target_root = script_root_of(ed, ed->snap.ref);

    ccr_node_id prev = CCR_NONE;
    for (ccr_node_id c = N(ed, loose)->first_child; c != CCR_NONE;) {
        ccr_node_id next = N(ed, c)->next_sibling;
        if (prev != CCR_NONE)
            ccr_ast_insert_after(&ed->ast, prev, c);
        else if (ed->snap.kind == CCR_SNAP_AFTER)
            ccr_ast_insert_after(&ed->ast, ed->snap.ref, c);
        else
            ccr_ast_insert_first(&ed->ast, ed->snap.ref, c);
        prev = c;
        c = next;
    }

    ed->ast.nodes[loose].flags |= CCR_FLAG_DEAD;
    remove_script(ed, ed->dragged);
    ed->dragged = UINT32_MAX;

    uint32_t ti = script_index_of_root(ed, target_root);
    if (ti != UINT32_MAX)
        relayout(ed, ti);
}

void ccr_editor_update(ccr_editor *ed, const ccr_editor_input *in)
{
    switch (ed->drag) {
    case CCR_DRAG_IDLE:
        if (in->pressed) {
            uint32_t si;
            ed->press_node = pick(ed, in->mouse, &si);
            if (ed->press_node != CCR_NONE) {
                ed->press_at = in->mouse;
                ed->drag = CCR_DRAG_PRESSED;
            }
        }
        break;

    case CCR_DRAG_PRESSED: {
        if (!in->down) {
            ed->drag = CCR_DRAG_IDLE;   /* a click, not a drag */
            break;
        }
        float zoom = in->zoom > 0.0f ? in->zoom : 1.0f;
        float dx = (in->mouse.x - ed->press_at.x) * zoom;
        float dy = (in->mouse.y - ed->press_at.y) * zoom;
        if (dx * dx + dy * dy >= DRAG_THRESHOLD * DRAG_THRESHOLD)
            begin_drag(ed, in->mouse);
        if (ed->drag == CCR_DRAG_MOVING)
            find_snap(ed);
        break;
    }

    case CCR_DRAG_MOVING:
        ed->scripts[ed->dragged].pos =
            (ccr_vec2){ in->mouse.x - ed->grab.x, in->mouse.y - ed->grab.y };
        find_snap(ed);
        if (!in->down || in->released) {
            if (ed->snap_ok)
                drop_into_snap(ed);
            ed->snap_ok = false;
            ed->dragged = UINT32_MAX;
            ed->drag = CCR_DRAG_IDLE;
        }
        break;
    }
}

/* ---------------------------------------------------------- drawing */

void ccr_editor_draw(const ccr_editor *ed, const ccr_canvas *cv, ccr_rect viewport)
{
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        const ccr_script *s = &ed->scripts[i];
        bool loose = is_loose_root(ed, s->root);
        bool dragging = ed->drag == CCR_DRAG_MOVING && i == ed->dragged;
        float alpha = (loose && !(dragging && ed->snap_ok)) ? LOOSE_ALPHA : 1.0f;

        /* where the stack will land: a white bar under the snap point */
        if (dragging && ed->snap_ok) {
            ccr_vec2 p = ccr_canvas_to_screen(cv, ed->snap.at);
            float w = (ed->snap.width > 60.0f ? ed->snap.width : 60.0f) * cv->zoom;
            float h = 6.0f * (cv->zoom < 1.0f ? 1.0f : cv->zoom);
            ccr_gui_rect_rounded((ccr_rect){ p.x, p.y - h * 0.5f, w, h }, 1.0f, 0xFFFFFFDDu);
        }
        ccr_blocks_draw(&s->blocks, cv, viewport, s->pos, alpha);
    }
}
