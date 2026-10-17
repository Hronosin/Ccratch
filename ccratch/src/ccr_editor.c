#include "ccr_editor.h"
#include "ccr_codegen.h"
#include "ccr_theme.h"
#include "ccr_lang.h"
#include "ccr_lib.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_GAP     32.0f   /* between top-level scripts in a column */
#define COLUMN_GAP     96.0f
#define DRAG_THRESHOLD  4.0f   /* screen pixels before a press becomes a drag */
#define SNAP_RADIUS    36.0f   /* world units */
#define LOOSE_ALPHA     0.55f
#define SETTLE_TIME     0.13f   /* seconds a dropped stack takes to glide in */
#define RIPPLE_TIME     0.35f   /* and how long the ring round it lasts */
#define ARROW_GRIP      7.0f    /* pixels from a goto arrow that still hit it */
#define DOUBLE_CLICK    0.35    /* seconds between the clicks of a double click */
#define HISTORY_MAX    500

static const ccr_node *N(const ccr_editor *ed, ccr_node_id id) { return &ed->ast.nodes[id]; }

static void begin_change(ccr_editor *ed);
static void find_snap(ccr_editor *ed);
static void drop_into_snap(ccr_editor *ed);
static void poof_node(ccr_editor *ed, ccr_node_id node);
static void poof_script(ccr_editor *ed, uint32_t si);
static void remove_new_decl(ccr_editor *ed, ccr_node_id d);
static void commit_change(ccr_editor *ed);
static void forget_sizes(ccr_editor *ed, ccr_node_id id);
static ccr_node_id new_node(ccr_editor *ed, ccr_node_kind kind, ccr_node_id parent);

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
        if (N(ed, p)->kind == CCR_NODE_BLOCK && !(N(ed, id)->flags & CCR_FLAG_ELSEIF))
            return id;   /* an elseif() is part of its if(): that moves */
        id = p;
    }
    return CCR_NONE;
}

/* ------------------------------------------------------ expressions */

/* Saving decides what can come out of a slot (ccr_codegen_can_move). */
static bool expr_movable(const ccr_editor *ed, ccr_node_id id)
{
    return ccr_codegen_can_move(&ed->ast, id);
}

/* The expression a press on `hit` grabs: the innermost movable one around
 * it, below its statement. CCR_NONE: grab the statement instead. */
static ccr_node_id expr_to_grab(const ccr_editor *ed, ccr_node_id hit)
{
    for (ccr_node_id a = hit; a != CCR_NONE; a = N(ed, a)->parent) {
        ccr_node_id p = N(ed, a)->parent;
        if (p == CCR_NONE || N(ed, p)->kind == CCR_NODE_BLOCK)
            return CCR_NONE;
        if (expr_movable(ed, a))
            return a;
    }
    return CCR_NONE;
}

/* A new loose root for one expression, lying on the canvas. */
static ccr_node_id new_reporter_root(ccr_editor *ed)
{
    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE)
        return CCR_NONE;
    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE | CCR_FLAG_REPORTER;
    return loose;
}

/* Takes `x` out of its slot, leaving an empty one (a white field) where it
 * was. Returns the empty slot, or CCR_NONE. */
static ccr_node_id vacate(ccr_editor *ed, ccr_node_id x)
{
    ccr_node_id hole = new_node(ed, CCR_NODE_TEXT, CCR_NONE);
    if (hole == CCR_NONE)
        return CCR_NONE;
    ccr_ast_touch(&ed->ast, hole);
    ed->ast.nodes[hole].place_b = N(ed, x)->place_b;
    ed->ast.nodes[hole].place_e = N(ed, x)->place_e;
    ed->ast.nodes[hole].flags |= N(ed, x)->flags & CCR_FLAG_INIT;   /* still the initializer */
    const ccr_node_id parent = N(ed, x)->parent;
    ccr_ast_insert_after(&ed->ast, x, hole);
    ccr_ast_unlink(&ed->ast, x);
    forget_sizes(ed, parent);
    return hole;
}

/* Lifts expression `x` out of script `from` into a loose reporter. */
static uint32_t lift_expr(ccr_editor *ed, ccr_node_id x, uint32_t from)
{
    ccr_rect at;
    if (!ccr_editor_node_rect(ed, x, &at))
        return UINT32_MAX;
    ccr_node_id loose = new_reporter_root(ed);
    if (loose == CCR_NONE || vacate(ed, x) == CCR_NONE)
        return UINT32_MAX;
    ccr_ast_append(&ed->ast, loose, x);
    relayout(ed, from);
    if (!push_script(ed, loose, (ccr_vec2){ at.x, at.y }))
        return UINT32_MAX;
    return ed->script_count - 1;
}

/* ----------------------------------------------------------- init */

bool ccr_editor_init(ccr_editor *ed, ccr_ast *ast)
{
    memset(ed, 0, sizeof *ed);
    ed->ast = *ast;
    memset(ast, 0, sizeof *ast);   /* ownership moved */
    ed->dragged = UINT32_MAX;
    ed->edit_request = CCR_NONE;
    ed->hover = ed->hover_arrow = ed->hover_far = CCR_NONE;
    ed->press_op = ed->op_request = ed->hover_op = CCR_NONE;
    ed->hover_include = ed->include_request = CCR_NONE;
    ed->last_click = CCR_NONE;
    ed->landed_first = ed->landed_last = CCR_NONE;
    ed->rng = 0x9E3779B9u;
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

static void free_change(ccr_change *c)
{
    free(c->before);
    free(c->after);
    free(c->scripts_before);
    free(c->scripts_after);
    memset(c, 0, sizeof *c);
}

void ccr_editor_free(ccr_editor *ed)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        ccr_blocks_free(&ed->scripts[i].blocks);
    free(ed->scripts);
    for (uint32_t i = 0; i < ed->history_count; ++i)
        free_change(&ed->history[i]);
    free(ed->history);
    free(ed->journal.old);
    free(ed->pending);
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

void ccr_editor_relayout_all(ccr_editor *ed)
{
    ccr_editor_settle(ed);
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        ccr_blocks fresh;
        if (ccr_blocks_build_script(&fresh, &ed->ast, ed->scripts[i].root, &ed->cache)) {
            ccr_blocks_free(&ed->scripts[i].blocks);
            ed->scripts[i].blocks = fresh;
        }
    }
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

    /* the stack it leaves will be written anew on save */
    ccr_node_id from_block = N(ed, item)->parent;
    if (from_block != CCR_NONE && !(N(ed, from_block)->flags & CCR_FLAG_LOOSE)) {
        ccr_ast_touch(&ed->ast, from_block);
        ed->ast.nodes[from_block].flags |= CCR_FLAG_DIRTY;
    }

    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE)
        return UINT32_MAX;
    ccr_ast_touch(&ed->ast, loose);
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
    ccr_node_id expr = expr_to_grab(ed, hit);
    uint32_t moving;
    const ccr_node_id root = ed->scripts[si].root;

    begin_change(ed);   /* everything until the drop is one undo step */

    if (expr != CCR_NONE) {
        moving = lift_expr(ed, expr, si);   /* Scratch: a reporter comes out of its slot */
    } else if (item != CCR_NONE &&
        !(is_loose_root(ed, root) && N(ed, root)->first_child == item)) {
        moving = tear_off(ed, item, si);    /* part of a stack: take the tail */
    } else {
        moving = bring_to_front(ed, si);    /* the whole script */
    }
    if (moving == UINT32_MAX) {
        commit_change(ed);   /* keep whatever did happen undoable */
        ed->drag = CCR_DRAG_IDLE;
        return;
    }

    ed->dragged = moving;
    ed->grab = (ccr_vec2){ ed->press_at.x - ed->scripts[moving].pos.x,
                           ed->press_at.y - ed->scripts[moving].pos.y };
    ed->scripts[moving].pos = (ccr_vec2){ mouse.x - ed->grab.x, mouse.y - ed->grab.y };
    ed->drag = CCR_DRAG_MOVING;
}

/* ---------------------------------------------------------- history */

static ccr_placed *snapshot_scripts(const ccr_editor *ed, uint32_t *count)
{
    ccr_placed *v = malloc((ed->script_count ? ed->script_count : 1) * sizeof *v);
    if (!v) {
        *count = 0;
        return NULL;
    }
    for (uint32_t i = 0; i < ed->script_count; ++i)
        v[i] = (ccr_placed){ ed->scripts[i].root, ed->scripts[i].pos };
    *count = ed->script_count;
    return v;
}

static bool same_placement(const ccr_placed *a, uint32_t na, const ccr_placed *b, uint32_t nb)
{
    if (na != nb)
        return false;
    for (uint32_t i = 0; i < na; ++i)
        if (a[i].root != b[i].root || a[i].pos.x != b[i].pos.x || a[i].pos.y != b[i].pos.y)
            return false;
    return true;
}

/* Starts recording: from here every tree edit and the canvas are tracked. */
static void begin_change(ccr_editor *ed)
{
    free(ed->pending);
    ed->pending = snapshot_scripts(ed, &ed->pending_count);
    ed->pending_nodes = ed->ast.node_count;
    ed->journal.count = 0;
    ed->journal.oom = false;
    ed->ast.journal = &ed->journal;
    ed->recording = true;
}

static void forget_redo(ccr_editor *ed)
{
    for (uint32_t i = ed->history_len; i < ed->history_count; ++i)
        free_change(&ed->history[i]);
    ed->history_count = ed->history_len;
    if (ed->saved_at > (int64_t)ed->history_len)
        ed->saved_at = -1;   /* the saved state was on the branch just cut off */
}

static void forget_all(ccr_editor *ed)
{
    for (uint32_t i = 0; i < ed->history_count; ++i)
        free_change(&ed->history[i]);
    ed->history_len = ed->history_count = 0;
    ed->saved_at = -1;
}

/* Stops recording and files what changed as one undoable step. */
static void commit_change(ccr_editor *ed)
{
    if (!ed->recording)
        return;
    ed->recording = false;
    ed->ast.journal = NULL;

    uint32_t na = 0;
    ccr_placed *after_scripts = snapshot_scripts(ed, &na);
    const bool canvas_same = same_placement(ed->pending, ed->pending_count, after_scripts, na);
    if (ed->journal.count == 0 && canvas_same) {
        free(after_scripts);   /* picked up and put back exactly: nothing to undo */
        return;
    }
    if (ed->journal.oom || !after_scripts || !ed->pending) {
        free(after_scripts);
        forget_all(ed);        /* cannot record it: better no history than a wrong one */
        return;
    }

    ccr_change c = {
        .node_count = ed->journal.count,
        .scripts_before = ed->pending,
        .scripts_after = after_scripts,
        .nb = ed->pending_count,
        .na = na,
        .nodes_before = ed->pending_nodes,
    };
    ed->pending = NULL;
    c.before = malloc((c.node_count ? c.node_count : 1) * sizeof(ccr_node_links));
    c.after = malloc((c.node_count ? c.node_count : 1) * sizeof(ccr_node_links));
    if (!c.before || !c.after) {
        free_change(&c);
        forget_all(ed);
        return;
    }
    for (uint32_t i = 0; i < c.node_count; ++i) {
        c.before[i] = ed->journal.old[i];
        c.after[i] = ccr_ast_links(&ed->ast, ed->journal.old[i].id);
    }

    forget_redo(ed);
    if (ed->history_count == HISTORY_MAX) {   /* drop the oldest */
        free_change(&ed->history[0]);
        memmove(&ed->history[0], &ed->history[1], (HISTORY_MAX - 1) * sizeof(ccr_change));
        ed->history_len--;
        ed->history_count--;
        ed->saved_at = ed->saved_at > 0 ? ed->saved_at - 1 : -1;
    }
    if (ed->history_count == ed->history_cap) {
        uint32_t cap = ed->history_cap ? ed->history_cap * 2 : 32;
        ccr_change *h = realloc(ed->history, cap * sizeof *h);
        if (!h) {
            free_change(&c);
            forget_all(ed);
            return;
        }
        ed->history = h;
        ed->history_cap = cap;
    }
    ed->history[ed->history_count++] = c;
    ed->history_len = ed->history_count;
}

/* Text changed under `id`: its measured size, and every container's, is stale. */
static void forget_sizes(ccr_editor *ed, ccr_node_id id)
{
    for (; id != CCR_NONE && id < ed->cache.cap; id = N(ed, id)->parent) {
        ed->cache.w[id] = 0.0f;
        ed->cache.h[id] = 0.0f;
    }
}

/* Puts the canvas back: these scripts, these positions, laid out anew. */
static void restore_scripts(ccr_editor *ed, const ccr_placed *v, uint32_t n)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        ccr_blocks_free(&ed->scripts[i].blocks);
    ed->script_count = 0;
    for (uint32_t i = 0; i < n; ++i)
        push_script(ed, v[i].root, v[i].pos);
}

/* ---------------------------------------------------------- palette */

static ccr_node_id new_node(ccr_editor *ed, ccr_node_kind kind, ccr_node_id parent)
{
    return ccr_template_node(&ed->ast, kind, parent);
}

bool ccr_editor_spawn(ccr_editor *ed, ccr_template t, ccr_vec2 mouse)
{
    return ccr_editor_spawn_held(ed, t, mouse, (ccr_vec2){ 16.0f, 16.0f });
}

bool ccr_editor_spawn_held(ccr_editor *ed, ccr_template t, ccr_vec2 mouse, ccr_vec2 grab)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || (int)t < 0 || (int)t >= ccr_template_total())
        return false;
    begin_change(ed);

    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE) {
        commit_change(ed);
        return false;
    }
    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE;
    if (ccr_template_is_reporter(t))
        ed->ast.nodes[loose].flags |= CCR_FLAG_REPORTER;
    if (ccr_template_is_toplevel(t))
        ed->ast.nodes[loose].flags |= CCR_FLAG_TOPLEVEL;
    if (ccr_template_build(&ed->ast, t, loose) == CCR_NONE || ed->journal.oom ||
        !push_script(ed, loose, mouse)) {
        commit_change(ed);
        return false;
    }

    /* held where it was grabbed */
    ed->dragged = ed->script_count - 1;
    ed->grab = grab;
    ed->scripts[ed->dragged].pos = (ccr_vec2){ mouse.x - ed->grab.x, mouse.y - ed->grab.y };
    ed->drag = CCR_DRAG_MOVING;
    find_snap(ed);
    return true;
}

bool ccr_editor_set_text(ccr_editor *ed, ccr_node_id slot, const char *text)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || slot >= ed->ast.node_count ||
        N(ed, slot)->kind != CCR_NODE_TEXT)
        return false;
    if (strcmp(ccr_ast_str(&ed->ast, N(ed, slot)->name), text) == 0)
        return true;   /* unchanged: not worth an undo step */

    begin_change(ed);
    ccr_ast_touch(&ed->ast, slot);
    ed->ast.nodes[slot].name = ccr_ast_intern(&ed->ast, text);
    /* a word read from the file (CMake's arguments): printed from now on */
    ed->ast.nodes[slot].flags |= CCR_FLAG_NEW;
    /* the block it sits in changes, so saving rebuilds that part */
    ccr_node_id p = N(ed, slot)->parent;
    const uint32_t word = N(ed, slot)->flags & CCR_FLAG_WORD;
    if (word && p != CCR_NONE) {
        /* a name or type: its block keeps it too (goto arrows, saving) */
        ccr_ast_touch(&ed->ast, p);
        if (word & CCR_FLAG_WORD_NAME)
            ed->ast.nodes[p].name = N(ed, slot)->name;
        else
            ed->ast.nodes[p].type = N(ed, slot)->name;
    }
    if (p != CCR_NONE && N(ed, p)->kind == CCR_NODE_BLOCK && !(N(ed, p)->flags & CCR_FLAG_LOOSE)) {
        ccr_ast_touch(&ed->ast, p);
        ed->ast.nodes[p].flags |= CCR_FLAG_DIRTY;
    }
    forget_sizes(ed, slot);
    uint32_t i = script_index_of_root(ed, script_root_of(ed, slot));
    if (i != UINT32_MAX)
        relayout(ed, i);
    commit_change(ed);
    return true;
}

/* An operator under a world point that can be swapped for another. */
static ccr_node_id op_under(const ccr_editor *ed, ccr_vec2 world, ccr_rect *rect)
{
    uint32_t si;
    if (pick(ed, world, &si) == CCR_NONE)
        return CCR_NONE;
    const ccr_script *s = &ed->scripts[si];
    ccr_node_id node;
    const char *text;
    ccr_rect r;
    if (!ccr_blocks_hit_text(&s->blocks, (ccr_vec2){ world.x - s->pos.x, world.y - s->pos.y },
                             &node, &text, &r))
        return CCR_NONE;
    const ccr_node *n = N(ed, node);
    if ((n->kind != CCR_NODE_BINARY_OP && n->kind != CCR_NODE_UNARY_OP) ||
        strcmp(text, ccr_ast_str(&ed->ast, n->op)) != 0)
        return CCR_NONE;
    int count;
    if (!ccr_op_choices(text, n->kind == CCR_NODE_UNARY_OP, &count) ||
        !ccr_codegen_can_reprint(&ed->ast, node))
        return CCR_NONE;
    if (rect)
        *rect = (ccr_rect){ r.x + s->pos.x, r.y + s->pos.y, r.w, r.h };
    return node;
}

/* The #include a click on `hit` opens: its block, not its name field. */
static ccr_node_id include_of(const ccr_editor *ed, ccr_node_id hit)
{
    if (hit == CCR_NONE || (N(ed, hit)->flags & CCR_FLAG_WORD))
        return CCR_NONE;
    const ccr_node_id root = script_root_of(ed, hit);
    return root != CCR_NONE && N(ed, root)->kind == CCR_NODE_INCLUDE &&
           *ccr_ast_str(&ed->ast, N(ed, root)->name) ? root : CCR_NONE;
}

ccr_node_id ccr_editor_take_include_request(ccr_editor *ed)
{
    ccr_node_id id = ed->include_request;
    ed->include_request = CCR_NONE;
    return id;
}

ccr_node_id ccr_editor_take_op_request(ccr_editor *ed)
{
    ccr_node_id id = ed->op_request;
    ed->op_request = CCR_NONE;
    return id;
}

bool ccr_editor_set_op(ccr_editor *ed, ccr_node_id node, const char *op)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || node >= ed->ast.node_count ||
        !ccr_codegen_can_reprint(&ed->ast, node))
        return false;
    const ccr_node *n = N(ed, node);
    int count;
    const char *const *choices = ccr_op_choices(ccr_ast_str(&ed->ast, n->op),
                                                n->kind == CCR_NODE_UNARY_OP, &count);
    bool allowed = false;
    for (int i = 0; choices && i < count; ++i)
        allowed = allowed || strcmp(choices[i], op) == 0;
    if (!allowed)
        return false;
    if (strcmp(ccr_ast_str(&ed->ast, n->op), op) == 0)
        return true;   /* the same one: not an undo step */

    begin_change(ed);
    ccr_ast_touch(&ed->ast, node);
    ed->ast.nodes[node].op = ccr_ast_intern(&ed->ast, op);
    ed->ast.nodes[node].flags |= CCR_FLAG_NEW;   /* printed anew, around its operands */
    forget_sizes(ed, node);
    uint32_t i = script_index_of_root(ed, script_root_of(ed, node));
    if (i != UINT32_MAX)
        relayout(ed, i);
    commit_change(ed);
    return true;
}

ccr_node_id ccr_editor_take_edit_request(ccr_editor *ed)
{
    ccr_node_id id = ed->edit_request;
    ed->edit_request = CCR_NONE;
    return id;
}

/* ------------------------------------------------------------ folding */

bool ccr_editor_can_fold(const ccr_editor *ed, ccr_node_id node)
{
    if (node >= ed->ast.node_count)
        return false;
    const ccr_node *n = N(ed, node);
    switch (n->kind) {
    case CCR_NODE_IF:
        return !(n->flags & CCR_FLAG_ELSEIF);   /* the if() it is part of folds */
    case CCR_NODE_WHILE: case CCR_NODE_DO_WHILE: case CCR_NODE_FOR: case CCR_NODE_SCOPE:
    case CCR_NODE_SWITCH: case CCR_NODE_CASE: case CCR_NODE_DEFAULT:
        return true;
    case CCR_NODE_BLOCK:   /* a bare { } in a stack; not a body, not a loose root */
        return n->parent != CCR_NONE && N(ed, n->parent)->kind == CCR_NODE_BLOCK;
    case CCR_NODE_FUNCTION: case CCR_NODE_STRUCT: case CCR_NODE_UNION: case CCR_NODE_ENUM:
        return (n->flags & CCR_FLAG_DEFINITION) && !(n->kind == CCR_NODE_ENUM && (n->flags & CCR_FLAG_NEW));
    default:
        return false;
    }
}

/* What a click on `hit` would fold: its statement, or its script's
 * declaration, if that folds. Not the if around a statement in its body. */
static ccr_node_id fold_target(const ccr_editor *ed, ccr_node_id hit)
{
    ccr_node_id s = stack_item_of(ed, hit);
    if (s == CCR_NONE)
        s = script_root_of(ed, hit);
    if (s != CCR_NONE && N(ed, s)->kind == CCR_NODE_BLOCK && (N(ed, s)->flags & CCR_FLAG_TOPLEVEL))
        s = N(ed, s)->first_child;
    return ccr_editor_can_fold(ed, s) ? s : CCR_NONE;
}

bool ccr_editor_toggle_fold(ccr_editor *ed, ccr_node_id node)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || !ccr_editor_can_fold(ed, node))
        return false;
    ed->ast.nodes[node].flags ^= CCR_FLAG_COLLAPSED;   /* not journaled: a view */
    forget_sizes(ed, node);
    uint32_t i = script_index_of_root(ed, script_root_of(ed, node));
    if (i != UINT32_MAX)
        relayout(ed, i);
    return true;
}

void ccr_editor_reveal(ccr_editor *ed, ccr_node_id node)
{
    if (node >= ed->ast.node_count)
        return;
    bool changed = false;
    for (ccr_node_id a = N(ed, node)->parent; a != CCR_NONE; a = N(ed, a)->parent)
        if (N(ed, a)->flags & CCR_FLAG_COLLAPSED) {
            ed->ast.nodes[a].flags &= ~CCR_FLAG_COLLAPSED;
            changed = true;
        }
    if (changed) {
        uint32_t i = script_index_of_root(ed, script_root_of(ed, node));
        if (i != UINT32_MAX)
            relayout(ed, i);
    }
}

/* --------------------------------------------------- block actions */

ccr_context ccr_editor_context(const ccr_editor *ed, ccr_vec2 mouse)
{
    ccr_context c = { CCR_NONE, CCR_NONE, CCR_NONE, CCR_NONE, false, false, false, false,
                      CCR_NONE, false };
    if (ed->drag != CCR_DRAG_IDLE)
        return c;
    uint32_t si;
    c.node = pick(ed, mouse, &si);
    if (c.node == CCR_NONE)
        return c;
    c.fold = fold_target(ed, c.node);
    c.folded = c.fold != CCR_NONE && (N(ed, c.fold)->flags & CCR_FLAG_COLLAPSED);
    if (N(ed, c.node)->kind == CCR_NODE_TEXT)
        c.text = c.node;
    c.item = stack_item_of(ed, c.node);
    const ccr_node_id expr = expr_to_grab(ed, c.node);
    c.target = expr != CCR_NONE ? expr : c.item;
    if (c.item == CCR_NONE) {
        /* a whole script: only loose stacks can go (the file's own
         * functions and structs stay; deleting those is not supported) */
        ccr_node_id root = ed->scripts[si].root;
        if (is_loose_root(ed, root) && N(ed, root)->first_child != CCR_NONE) {
            c.item = c.target = N(ed, root)->first_child;
            c.can_delete = true;
        } else if (ccr_editor_is_new_decl(ed, root)) {
            c.item = c.target = root;   /* made here: it may go again */
            c.can_delete = true;
        }
        return c;
    }
    const bool reporter = (N(ed, N(ed, c.item)->parent)->flags & CCR_FLAG_REPORTER) != 0;
    c.can_delete = c.can_duplicate = true;
    c.can_comment = !reporter;   /* a lone expression is no statement to switch off */
    c.commented = (N(ed, c.item)->flags & CCR_FLAG_COMMENTED) != 0;
    return c;
}

static void mark_dirty(ccr_editor *ed, ccr_node_id block)
{
    if (block != CCR_NONE && N(ed, block)->kind == CCR_NODE_BLOCK &&
        !(N(ed, block)->flags & CCR_FLAG_LOOSE)) {
        ccr_ast_touch(&ed->ast, block);
        ed->ast.nodes[block].flags |= CCR_FLAG_DIRTY;
    }
}

static bool is_stack_item(const ccr_editor *ed, ccr_node_id id)
{
    return id < ed->ast.node_count && N(ed, id)->parent != CCR_NONE &&
           N(ed, N(ed, id)->parent)->kind == CCR_NODE_BLOCK;
}

bool ccr_editor_delete(ccr_editor *ed, ccr_node_id item)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE)
        return false;
    if (ccr_editor_is_new_decl(ed, item)) {
        begin_change(ed);
        remove_new_decl(ed, item);
        commit_change(ed);
        return true;
    }
    if (!is_stack_item(ed, item)) {
        /* an expression in a slot: the slot stays, empty */
        if (item >= ed->ast.node_count || !expr_movable(ed, item))
            return false;
        uint32_t si = script_index_of_root(ed, script_root_of(ed, item));
        poof_node(ed, item);
        begin_change(ed);
        if (vacate(ed, item) != CCR_NONE) {
            ccr_ast_touch(&ed->ast, item);
            ed->ast.nodes[item].flags |= CCR_FLAG_DEAD;
        }
        if (si != UINT32_MAX)
            relayout(ed, si);
        commit_change(ed);
        return true;
    }
    const ccr_node_id block = N(ed, item)->parent;
    const ccr_node_id root = script_root_of(ed, item);
    uint32_t si = script_index_of_root(ed, root);
    if (si == UINT32_MAX)
        return false;

    begin_change(ed);
    if (is_loose_root(ed, block) && N(ed, block)->first_child == item) {
        /* the top of a loose stack: the whole stack goes */
        poof_script(ed, si);
        ccr_ast_touch(&ed->ast, block);
        ed->ast.nodes[block].flags |= CCR_FLAG_DEAD;
        remove_script(ed, si);
    } else {
        poof_node(ed, item);
        mark_dirty(ed, block);
        ccr_ast_unlink(&ed->ast, item);   /* the rest of the stack closes up */
        ccr_ast_touch(&ed->ast, item);
        ed->ast.nodes[item].flags |= CCR_FLAG_DEAD;
        relayout(ed, si);
    }
    commit_change(ed);
    return true;
}

/* Deep copy of a subtree; the copy has no parent. */
static ccr_node_id clone(ccr_editor *ed, ccr_node_id src, unsigned depth)
{
    if (depth > 4000)
        return CCR_NONE;
    ccr_node_id id = ccr_ast_add(&ed->ast, N(ed, src)->kind, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ccr_node *n = &ed->ast.nodes[id];
    *n = ed->ast.nodes[src];   /* everything: name, ranges, indents, flags */
    n->parent = n->first_child = n->last_child = n->next_sibling = CCR_NONE;
    n->flags &= ~(CCR_FLAG_LOOSE | CCR_FLAG_DEAD | CCR_FLAG_BLANK_BEFORE);
    ccr_ast_touch(&ed->ast, id);
    for (ccr_node_id c = ed->ast.nodes[src].first_child; c != CCR_NONE;
         c = ed->ast.nodes[c].next_sibling) {
        ccr_node_id k = clone(ed, c, depth + 1);
        if (k == CCR_NONE)
            return CCR_NONE;
        ccr_ast_append(&ed->ast, id, k);
    }
    return id;
}

bool ccr_editor_duplicate(ccr_editor *ed, ccr_node_id item, ccr_vec2 mouse)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || item >= ed->ast.node_count)
        return false;
    /* a statement copies as a stack, an expression (in a slot, or lying
     * loose) as a reporter */
    const bool stmt = is_stack_item(ed, item);
    const bool reporter = !stmt ||
        (N(ed, N(ed, item)->parent)->flags & CCR_FLAG_REPORTER) != 0;
    if (!stmt && !expr_movable(ed, item))
        return false;
    begin_change(ed);
    ccr_node_id copy = clone(ed, item, 0);
    ccr_node_id loose = copy == CCR_NONE ? CCR_NONE
                      : ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE) {
        commit_change(ed);
        return false;
    }
    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE | (reporter ? CCR_FLAG_REPORTER : 0u);
    ccr_ast_append(&ed->ast, loose, copy);
    if (!push_script(ed, loose, mouse)) {
        commit_change(ed);
        return false;
    }
    ed->dragged = ed->script_count - 1;
    ed->grab = (ccr_vec2){ 16.0f, 16.0f };
    ed->scripts[ed->dragged].pos = (ccr_vec2){ mouse.x - ed->grab.x, mouse.y - ed->grab.y };
    ed->drag = CCR_DRAG_MOVING;
    ed->carrying = true;
    find_snap(ed);
    return true;
}

bool ccr_editor_toggle_comment(ccr_editor *ed, ccr_node_id item)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || !is_stack_item(ed, item) ||
        (N(ed, N(ed, item)->parent)->flags & CCR_FLAG_REPORTER))
        return false;
    uint32_t si = script_index_of_root(ed, script_root_of(ed, item));
    begin_change(ed);
    mark_dirty(ed, N(ed, item)->parent);
    ccr_ast_touch(&ed->ast, item);
    ed->ast.nodes[item].flags ^= CCR_FLAG_COMMENTED;
    if (si != UINT32_MAX)
        relayout(ed, si);
    commit_change(ed);
    return true;
}

bool ccr_editor_can_undo(const ccr_editor *ed)
{
    return ed->drag == CCR_DRAG_IDLE && ed->history_len > 0;
}

bool ccr_editor_can_redo(const ccr_editor *ed)
{
    return ed->drag == CCR_DRAG_IDLE && ed->history_len < ed->history_count;
}

bool ccr_editor_undo(ccr_editor *ed)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || ed->history_len == 0)
        return false;
    const ccr_change *c = &ed->history[--ed->history_len];
    for (uint32_t i = c->node_count; i-- > 0;) {
        /* folding is a view, not an edit: undo leaves it as it is */
        const uint32_t folded = c->before[i].id < ed->ast.node_count
                              ? N(ed, c->before[i].id)->flags & CCR_FLAG_COLLAPSED : 0;
        ccr_ast_set_links(&ed->ast, &c->before[i]);
        ed->ast.nodes[c->before[i].id].flags = (N(ed, c->before[i].id)->flags & ~CCR_FLAG_COLLAPSED) | folded;
    }
    for (uint32_t i = 0; i < c->node_count; ++i)   /* slots refilled: sizes change */
        forget_sizes(ed, c->before[i].id);
    /* nodes this step created (a loose stack's root) are gone again */
    for (uint32_t i = 0; i < c->node_count; ++i)
        if (c->before[i].id >= c->nodes_before)
            ed->ast.nodes[c->before[i].id].flags |= CCR_FLAG_DEAD;
    restore_scripts(ed, c->scripts_before, c->nb);
    return true;
}

bool ccr_editor_redo(ccr_editor *ed)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE || ed->history_len == ed->history_count)
        return false;
    const ccr_change *c = &ed->history[ed->history_len++];
    for (uint32_t i = 0; i < c->node_count; ++i) {
        /* folding is a view, not an edit: undo leaves it as it is */
        const uint32_t folded = c->after[i].id < ed->ast.node_count
                              ? N(ed, c->after[i].id)->flags & CCR_FLAG_COLLAPSED : 0;
        ccr_ast_set_links(&ed->ast, &c->after[i]);
        ed->ast.nodes[c->after[i].id].flags = (N(ed, c->after[i].id)->flags & ~CCR_FLAG_COLLAPSED) | folded;
    }
    for (uint32_t i = 0; i < c->node_count; ++i)
        forget_sizes(ed, c->after[i].id);
    restore_scripts(ed, c->scripts_after, c->na);
    return true;
}

bool ccr_editor_modified(const ccr_editor *ed)
{
    return ed->saved_at != (int64_t)ed->history_len;
}

void ccr_editor_mark_saved(ccr_editor *ed)
{
    ed->saved_at = (int64_t)ed->history_len;
}

/* Nearest snap point of any other script to the dragged stack's top-left. */
static void find_snap(ccr_editor *ed)
{
    ed->snap_ok = false;
    const ccr_script *d = &ed->scripts[ed->dragged];
    if (!is_loose_root(ed, d->root))
        return;   /* functions and structs do not snap into stacks */

    if (N(ed, d->root)->flags & CCR_FLAG_TOPLEVEL)
        return;   /* a declaration for the file finds its own place there */
    const bool reporter = (N(ed, d->root)->flags & CCR_FLAG_REPORTER) != 0;
    float best = SNAP_RADIUS * SNAP_RADIUS;
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        if (i == ed->dragged)
            continue;
        const ccr_script *s = &ed->scripts[i];
        for (uint32_t k = 0; k < s->blocks.snap_count; ++k) {
            ccr_snap sn = s->blocks.snaps[k];
            if ((sn.kind == CCR_SNAP_SLOT) != reporter)
                continue;   /* expressions go into slots, statements into stacks */
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

/* The dragged reporter fills the empty slot at the current snap. */
static void drop_into_slot(ccr_editor *ed)
{
    const ccr_node_id loose = ed->scripts[ed->dragged].root;
    const ccr_node_id hole = ed->snap.ref;
    const ccr_node_id x = N(ed, loose)->first_child;
    const ccr_node_id target_root = script_root_of(ed, hole);
    const ccr_node_id parent = N(ed, hole)->parent;
    if (x == CCR_NONE || parent == CCR_NONE)
        return;

    /* it now stands where the slot was in its parent's text */
    ccr_ast_touch(&ed->ast, x);
    ed->ast.nodes[x].place_b = N(ed, hole)->place_b;
    ed->ast.nodes[x].place_e = N(ed, hole)->place_e;
    ed->ast.nodes[x].flags |= CCR_FLAG_MOVED;
    ed->ast.nodes[x].flags = (N(ed, x)->flags & ~CCR_FLAG_INIT) | (N(ed, hole)->flags & CCR_FLAG_INIT);
    ccr_ast_insert_after(&ed->ast, hole, x);
    ccr_ast_unlink(&ed->ast, hole);
    ccr_ast_touch(&ed->ast, hole);
    ed->ast.nodes[hole].flags |= CCR_FLAG_DEAD;
    forget_sizes(ed, parent);

    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_DEAD;
    remove_script(ed, ed->dragged);
    ed->dragged = UINT32_MAX;

    uint32_t ti = script_index_of_root(ed, target_root);
    if (ti != UINT32_MAX)
        relayout(ed, ti);
}

/* Moves the dragged loose stack into place at the current snap. */
static void drop_raw(ccr_editor *ed)
{
    if (ed->snap.kind == CCR_SNAP_SLOT) {
        drop_into_slot(ed);
        return;
    }
    const ccr_node_id loose = ed->scripts[ed->dragged].root;
    const ccr_node_id target_root = script_root_of(ed, ed->snap.ref);

    ccr_node_id into = ed->snap.kind == CCR_SNAP_AFTER ? N(ed, ed->snap.ref)->parent : ed->snap.ref;
    if (into != CCR_NONE && !(N(ed, into)->flags & CCR_FLAG_LOOSE)) {
        ccr_ast_touch(&ed->ast, into);
        ed->ast.nodes[into].flags |= CCR_FLAG_DIRTY;
    }

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

    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_DEAD;
    remove_script(ed, ed->dragged);
    ed->dragged = UINT32_MAX;

    uint32_t ti = script_index_of_root(ed, target_root);
    if (ti != UINT32_MAX)
        relayout(ed, ti);
}

/* ------------------------------------------------------- animations */

static float frand(ccr_editor *ed)
{
    ed->rng = ed->rng * 1664525u + 1013904223u;
    return (float)(ed->rng >> 8) / 16777216.0f;
}

/* Poof: a deleted block breaks into bits of its own colours that fly
 * off. `r` is the world rectangle going away, inside script `si`. */
static void poof(ccr_editor *ed, uint32_t si, ccr_rect r)
{
    if (!ed->animate || si >= ed->script_count || r.w <= 0.0f || r.h <= 0.0f)
        return;
    const ccr_script *s = &ed->scripts[si];

    /* the colours showing in that rectangle, fills and edges */
    enum { MAX_COLORS = 32 };
    ccr_color colors[MAX_COLORS];
    int nc = 0;
    for (uint32_t i = 0; i < s->blocks.prim_count && nc < MAX_COLORS - 1; ++i) {
        const ccr_prim *pr = &s->blocks.prims[i];
        if (pr->kind == CCR_PRIM_TEXT)
            continue;
        const float x = pr->rect.x + s->pos.x, y = pr->rect.y + s->pos.y;
        if (x > r.x + r.w || y > r.y + r.h || x + pr->rect.w < r.x || y + pr->rect.h < r.y)
            continue;
        colors[nc++] = pr->fill;
        colors[nc++] = pr->edge;
    }
    if (nc == 0)
        colors[nc++] = ccr_th(CCR_C_HINT);

    const ccr_vec2 c = { r.x + r.w * 0.5f, r.y + r.h * 0.5f };
    float area = (r.w * r.h) / 700.0f;
    const int n = 16 + (int)(area > 48.0f ? 48.0f : area);
    for (int i = 0; i < n && ed->particle_count < CCR_MAX_PARTICLES; ++i) {
        ccr_particle *p = &ed->particles[ed->particle_count++];
        p->pos = (ccr_vec2){ r.x + frand(ed) * r.w, r.y + frand(ed) * r.h };
        float dx = p->pos.x - c.x, dy = p->pos.y - c.y;
        float len = sqrtf(dx * dx + dy * dy) + 1.0f;
        float speed = 120.0f + frand(ed) * 220.0f;
        p->vel = (ccr_vec2){ dx / len * speed, dy / len * speed - 160.0f * frand(ed) };
        p->size = 6.0f + frand(ed) * 8.0f;
        p->max_life = p->life = 0.4f + frand(ed) * 0.35f;
        p->color = colors[(int)(frand(ed) * (float)nc) % nc];
    }
}

static void poof_script(ccr_editor *ed, uint32_t si)
{
    const ccr_script *s = &ed->scripts[si];
    const ccr_rect b = s->blocks.bounds;
    poof(ed, si, (ccr_rect){ s->pos.x + b.x, s->pos.y + b.y, b.w, b.h });
}

static void poof_node(ccr_editor *ed, ccr_node_id node)
{
    ccr_rect r;
    if (ccr_editor_node_rect(ed, node, &r))
        poof(ed, script_index_of_root(ed, script_root_of(ed, node)), r);
}

static void step_particles(ccr_editor *ed, float dt)
{
    uint32_t keep = 0;
    for (uint32_t i = 0; i < ed->particle_count; ++i) {
        ccr_particle p = ed->particles[i];
        p.life -= dt;
        if (p.life <= 0.0f)
            continue;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.vel.y += 520.0f * dt;              /* they fall */
        p.vel.x *= 1.0f - 2.5f * dt;         /* and slow down */
        ed->particles[keep++] = p;
    }
    ed->particle_count = keep;
}

static float ease_out_cubic(float k)
{
    float u = 1.0f - k;
    return 1.0f - u * u * u;
}

/* The dropped stack has reached its snap point: put it in for real. */
static void finish_drop(ccr_editor *ed)
{
    /* what lands, for the ripple: the stack's statements, or the whole
     * thing when it moves as one (an expression, a function) */
    const ccr_node_id root = ed->scripts[ed->dragged].root;
    ccr_node_id first = root, last = root;
    if (ed->snap.kind != CCR_SNAP_SLOT && is_loose_root(ed, root) && N(ed, root)->first_child != CCR_NONE) {
        first = last = N(ed, root)->first_child;
        while (N(ed, last)->next_sibling != CCR_NONE)
            last = N(ed, last)->next_sibling;
    }
    drop_into_snap(ed);
    if (ed->animate) {
        ed->landed_first = first;
        ed->landed_last = last;
        ed->landed_t = 0.0f;
    }
    ed->snap_ok = false;
    ed->dragged = UINT32_MAX;
    ed->drag = CCR_DRAG_IDLE;
    commit_change(ed);
}

/* ------------------------------------------- declarations for the file */

/* Where a kind of declaration goes in a C file, top to bottom. */
static int decl_rank(const ccr_node *n)
{
    switch (n->kind) {
    case CCR_NODE_INCLUDE:      return 0;
    case CCR_NODE_MACRO_DEFINE: return 1;
    case CCR_NODE_STRUCT: case CCR_NODE_UNION: case CCR_NODE_ENUM:
    case CCR_NODE_TYPEDEF:      return 2;
    case CCR_NODE_FUNCTION:     return (n->flags & CCR_FLAG_DEFINITION) ? 5 : 3;
    case CCR_NODE_VAR: case CCR_NODE_DECL_GROUP: return 4;
    default:                    return 5;
    }
}

/* Just after the end of the line `at` is on. */
static uint32_t after_line(const ccr_ast *a, uint32_t at)
{
    while (at < a->source_len && a->source[at] != '\n')
        ++at;
    return at < a->source_len ? at + 1 : a->source_len;
}

/* The start of the line `at` is on. */
static uint32_t line_start(const ccr_ast *a, uint32_t at)
{
    if (at > a->source_len)
        at = a->source_len;
    while (at > 0 && a->source[at - 1] != '\n')
        --at;
    return at;
}

/* `#define X_H` right after `#ifndef X_H`: the guard of a header. */
static bool is_guard(const ccr_ast *a, const ccr_node *k)
{
    if (k->kind != CCR_NODE_MACRO_DEFINE || (k->flags & CCR_FLAG_NEW) || k->src_b > a->source_len)
        return false;
    char want[256];
    snprintf(want, sizeof want, "#ifndef %s", ccr_ast_str(a, k->name));
    const size_t n = strlen(want);
    /* the line before the #define's */
    uint32_t end = line_start(a, k->src_b);
    while (end > 0 && (a->source[end - 1] == '\n' || a->source[end - 1] == ' '))
        --end;
    const uint32_t start = line_start(a, end);
    return end - start >= n && memcmp(a->source + start, want, n) == 0;
}

bool ccr_editor_is_new_decl(const ccr_editor *ed, ccr_node_id id)
{
    return id < ed->ast.node_count && N(ed, id)->parent != CCR_NONE &&
           N(ed, N(ed, id)->parent)->kind == CCR_NODE_TRANSLATION_UNIT &&
           (N(ed, id)->flags & CCR_FLAG_NEW) && !(N(ed, id)->flags & CCR_FLAG_DEAD);
}

/*
 * A declaration from the palette was let go: it joins the file where C
 * wants it (#include, then #define, types, prototypes, globals, then
 * functions), after the last one of its kind or earlier. Inside a header's
 * guard that is before the #endif, as the guard's #define comes first.
 * On the canvas it stays where it was dropped.
 */
static void place_in_file(ccr_editor *ed, uint32_t si)
{
    const ccr_node_id loose = ed->scripts[si].root;
    const ccr_node_id d = N(ed, loose)->first_child;
    const ccr_vec2 pos = ed->scripts[si].pos;
    const ccr_node_id tu = 0;
    if (d == CCR_NONE || ed->ast.node_count == 0 || N(ed, tu)->kind != CCR_NODE_TRANSLATION_UNIT)
        return;
    const int rank = decl_rank(N(ed, d));

    ccr_node_id anchor = CCR_NONE, first = CCR_NONE;
    for (ccr_node_id c = N(ed, tu)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
        const ccr_node *k = N(ed, c);
        if ((k->flags & CCR_FLAG_DEAD) || k->kind == CCR_NODE_MACRO_USE)
            continue;
        /* a header's guard: `#ifndef X_H` then `#define X_H` comes first */
        const bool guard = first == CCR_NONE && is_guard(&ed->ast, k);
        if (first == CCR_NONE)
            first = c;
        if (guard || decl_rank(k) <= rank)
            anchor = c;
    }

    uint32_t at;
    ccr_ast_unlink(&ed->ast, d);
    if (anchor != CCR_NONE) {
        const ccr_node *a = N(ed, anchor);
        at = (a->flags & CCR_FLAG_NEW) ? a->place_b
           : after_line(&ed->ast, a->ext_e > a->src_e ? a->ext_e : a->src_e);
        ccr_ast_insert_after(&ed->ast, anchor, d);
    } else if (first != CCR_NONE) {
        const ccr_node *f = N(ed, first);
        at = (f->flags & CCR_FLAG_NEW) ? f->place_b
           : line_start(&ed->ast, f->ext_b < f->src_b && f->ext_e > f->ext_b ? f->ext_b : f->src_b);
        ccr_ast_insert_first(&ed->ast, tu, d);
    } else {
        at = ed->ast.source_len;   /* an empty file */
        ccr_ast_append(&ed->ast, tu, d);
    }
    ccr_ast_touch(&ed->ast, d);
    ed->ast.nodes[d].place_b = ed->ast.nodes[d].place_e = at;

    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_DEAD;
    remove_script(ed, si);
    push_script(ed, d, pos);
}

/* -------------------------------------------- a library's #include */

/* Does the file include something `pack` takes as its header? */
static bool file_includes(const ccr_editor *ed, const ccr_lib_pack *pk)
{
    if (ed->ast.node_count == 0)
        return false;
    for (ccr_node_id c = N(ed, 0)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (N(ed, c)->kind == CCR_NODE_INCLUDE && !(N(ed, c)->flags & CCR_FLAG_DEAD) &&
            ccr_lib_pack_matches(pk, ccr_ast_str(&ed->ast, N(ed, c)->name)))
            return true;
    return false;
}

/* Adds `#include <x>` or `#include "x"` to the file (inside a change). */
static bool add_include(ccr_editor *ed, const char *spelled)
{
    char name[256];
    const bool angled = spelled[0] == '<';
    snprintf(name, sizeof name, "%s", spelled + (spelled[0] == '<' || spelled[0] == '"'));
    size_t n = strlen(name);
    if (n && (name[n - 1] == '>' || name[n - 1] == '"'))
        name[--n] = '\0';
    if (!n || ed->ast.node_count == 0 || N(ed, 0)->kind != CCR_NODE_TRANSLATION_UNIT)
        return false;
    for (ccr_node_id c = N(ed, 0)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (N(ed, c)->kind == CCR_NODE_INCLUDE && !(N(ed, c)->flags & CCR_FLAG_DEAD) &&
            strcmp(ccr_ast_str(&ed->ast, N(ed, c)->name), name) == 0)
            return false;   /* there already */

    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE)
        return false;
    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE | CCR_FLAG_TOPLEVEL;
    ccr_node_id inc = ccr_template_build(&ed->ast, angled ? CCR_TEMPLATE_INCLUDE_SYS
                                                          : CCR_TEMPLATE_INCLUDE_LOC, loose);
    if (inc == CCR_NONE)
        return false;
    for (ccr_node_id w = N(ed, inc)->first_child; w != CCR_NONE; w = N(ed, w)->next_sibling)
        if (N(ed, w)->flags & CCR_FLAG_WORD_NAME)
            ed->ast.nodes[w].name = ccr_ast_intern(&ed->ast, name);
    ed->ast.nodes[inc].name = ccr_ast_intern(&ed->ast, name);

    /* on the canvas: above everything, at the left */
    ccr_vec2 at = { 0.0f, 0.0f };
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        const ccr_script *sc = &ed->scripts[i];
        if (i == 0 || sc->pos.x < at.x) at.x = sc->pos.x;
        if (i == 0 || sc->pos.y + sc->blocks.bounds.y - 70.0f < at.y)
            at.y = sc->pos.y + sc->blocks.bounds.y - 70.0f;
    }
    if (!push_script(ed, loose, at))
        return false;
    place_in_file(ed, ed->script_count - 1);

    /* better: right under the include before it, if nothing is there */
    ccr_script *me = &ed->scripts[ed->script_count - 1];
    ccr_node_id prev = CCR_NONE;
    for (ccr_node_id c = N(ed, 0)->first_child; c != CCR_NONE && c != inc; c = N(ed, c)->next_sibling)
        if (!(N(ed, c)->flags & CCR_FLAG_DEAD))
            prev = c;
    for (uint32_t i = 0; i + 1 < ed->script_count && prev != CCR_NONE; ++i) {
        const ccr_script *sp = &ed->scripts[i];
        if (sp->root != prev)
            continue;
        const ccr_vec2 p = { sp->pos.x + sp->blocks.bounds.x - me->blocks.bounds.x,
                             sp->pos.y + sp->blocks.bounds.y + sp->blocks.bounds.h + 16.0f -
                                 me->blocks.bounds.y };
        const ccr_rect r = { p.x + me->blocks.bounds.x, p.y + me->blocks.bounds.y - 8.0f,
                             me->blocks.bounds.w, me->blocks.bounds.h + 16.0f };
        bool free_spot = true;
        for (uint32_t k = 0; k + 1 < ed->script_count && free_spot; ++k) {
            const ccr_script *o = &ed->scripts[k];
            const ccr_rect q = { o->pos.x + o->blocks.bounds.x, o->pos.y + o->blocks.bounds.y,
                                 o->blocks.bounds.w, o->blocks.bounds.h };
            free_spot = r.x + r.w <= q.x || q.x + q.w <= r.x || r.y + r.h <= q.y || q.y + q.h <= r.y;
        }
        if (free_spot)
            me->pos = p;
        break;
    }
    return true;
}

/* Library calls just dropped into the file: their headers come along. */
static void include_libraries(ccr_editor *ed, ccr_node_id id)
{
    const ccr_node *n = N(ed, id);
    if (n->kind == CCR_NODE_CALL) {
        int args = 0;
        bool word = false;
        for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
            word = word || (N(ed, c)->flags & CCR_FLAG_WORD);
            ++args;
        }
        const ccr_lib_block *b = word ? NULL : ccr_lib_find(ccr_ast_str(&ed->ast, n->name), args);
        const ccr_lib_pack *pk = b ? ccr_lib_pack_at(b->pack) : NULL;
        if (pk && pk->include[0] && !file_includes(ed, pk))
            add_include(ed, pk->include);
    }
    for (ccr_node_id c = n->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        include_libraries(ed, c);
}

static bool in_file(const ccr_editor *ed, ccr_node_id id)
{
    const ccr_node_id root = script_root_of(ed, id);
    return root != CCR_NONE && N(ed, root)->parent != CCR_NONE;   /* a child of the file */
}

static void drop_into_snap(ccr_editor *ed)
{
    /* what is dropped: a reporter, or the statements of a stack */
    const ccr_node_id loose = ed->scripts[ed->dragged].root;
    ccr_node_id moved[256];
    int n = 0;
    for (ccr_node_id c = N(ed, loose)->first_child; c != CCR_NONE && n < 256; c = N(ed, c)->next_sibling)
        moved[n++] = c;
    drop_raw(ed);
    for (int i = 0; i < n; ++i)
        if (in_file(ed, moved[i]))
            include_libraries(ed, moved[i]);
}

bool ccr_editor_add_include(ccr_editor *ed, const char *spelled)
{
    ccr_editor_settle(ed);
    if (ed->drag != CCR_DRAG_IDLE)
        return false;
    begin_change(ed);
    const bool added = add_include(ed, spelled);
    commit_change(ed);
    return added;
}

/* A declaration from the palette taken back out of the file. */
static void remove_new_decl(ccr_editor *ed, ccr_node_id d)
{
    const uint32_t si = script_index_of_root(ed, d);
    if (si != UINT32_MAX) {
        poof_script(ed, si);
        remove_script(ed, si);
    }
    ccr_ast_unlink(&ed->ast, d);
    ccr_ast_touch(&ed->ast, d);
    ed->ast.nodes[d].flags |= CCR_FLAG_DEAD;
}

void ccr_editor_tick(ccr_editor *ed, float dt)
{
    ed->clock += dt;
    if (dt > 0.0f) {
        step_particles(ed, dt);
        ed->landed_t += dt;
    }
    if (ed->drag != CCR_DRAG_SETTLING)
        return;
    ed->settle_t += dt > 0.0f ? dt : SETTLE_TIME;
    float k = ed->settle_t / SETTLE_TIME;
    if (k > 1.0f)
        k = 1.0f;
    const float e = ease_out_cubic(k);
    ed->scripts[ed->dragged].pos = (ccr_vec2){
        ed->settle_from.x + (ed->settle_to.x - ed->settle_from.x) * e,
        ed->settle_from.y + (ed->settle_to.y - ed->settle_from.y) * e,
    };
    if (k >= 1.0f)
        finish_drop(ed);
}

void ccr_editor_settle(ccr_editor *ed)
{
    if (ed->drag == CCR_DRAG_SETTLING) {
        ed->scripts[ed->dragged].pos = ed->settle_to;
        finish_drop(ed);
    }
}

void ccr_editor_update(ccr_editor *ed, const ccr_editor_input *in)
{
    ccr_editor_tick(ed, in->dt);
    if (ed->drag == CCR_DRAG_SETTLING) {
        if (!in->pressed)
            return;              /* busy gliding */
        ccr_editor_settle(ed);   /* grabbing something else: in it goes, now */
    }

    ed->hover_arrow = ed->hover_far = ed->hover_op = ed->hover_include = CCR_NONE;
    if (ed->drag != CCR_DRAG_MOVING) {
        uint32_t si;
        ed->hover = pick(ed, in->mouse, &si);
        if (ed->hover != CCR_NONE)
            ed->hover_op = op_under(ed, in->mouse, &ed->hover_op_rect);
        ed->hover_include = include_of(ed, ed->hover);
        if (ed->hover == CCR_NONE)   /* blocks first; between them, arrows */
            ed->hover_arrow = ccr_editor_arrow_at(ed, in->mouse,
                                                  ARROW_GRIP / (in->zoom > 0.0f ? in->zoom : 1.0f),
                                                  &ed->hover_far);
    } else {
        ed->hover = CCR_NONE;
    }

    switch (ed->drag) {
    case CCR_DRAG_IDLE:
        if (in->pressed) {
            uint32_t si;
            ed->press_node = pick(ed, in->mouse, &si);
            ed->press_op = op_under(ed, in->mouse, NULL);
            if (ed->press_node != CCR_NONE) {
                ed->press_at = in->mouse;
                ed->drag = CCR_DRAG_PRESSED;
            }
        }
        break;

    case CCR_DRAG_PRESSED: {
        if (!in->down) {
            ed->drag = CCR_DRAG_IDLE;   /* a click, not a drag */
            /* a second click on the same block, quickly: fold or unfold it */
            const ccr_node_id fold = fold_target(ed, ed->press_node);
            const bool twice = fold != CCR_NONE && fold == ed->last_click &&
                               ed->clock - ed->last_click_at < DOUBLE_CLICK && in->dt > 0.0f;
            ed->last_click = fold;
            ed->last_click_at = ed->clock;
            if (twice && N(ed, ed->press_node)->kind != CCR_NODE_TEXT && ed->press_op == CCR_NONE) {
                ed->last_click = CCR_NONE;
                ccr_editor_toggle_fold(ed, fold);
            } else if (ed->press_op != CCR_NONE)
                ed->op_request = ed->press_op;
            else if (include_of(ed, ed->press_node) != CCR_NONE)
                ed->include_request = include_of(ed, ed->press_node);
            else if (ed->press_node != CCR_NONE && N(ed, ed->press_node)->kind == CCR_NODE_TEXT)
                ed->edit_request = ed->press_node;
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
        if (in->over_trash)
            ed->snap_ok = false;
        /* a carried copy lands on a click; a dragged block when let go */
        if (ed->carrying ? in->pressed : (!in->down || in->released)) {
            const ccr_node_id root = ed->scripts[ed->dragged].root;
            ed->carrying = false;
            if (ed->snap_ok && !in->over_trash && ed->animate && in->dt > 0.0f) {
                /* glide the last bit into place, then drop */
                ed->settle_from = ed->scripts[ed->dragged].pos;
                ed->settle_to = ed->snap.at;
                ed->settle_t = 0.0f;
                ed->drag = CCR_DRAG_SETTLING;
                break;
            }
            if (in->over_trash && ccr_editor_is_new_decl(ed, root)) {
                remove_new_decl(ed, root);   /* a new declaration, thrown away */
            } else if (in->over_trash && is_loose_root(ed, root)) {
                /* thrown back onto the palette: gone (Ctrl+Z brings it back) */
                poof_script(ed, ed->dragged);
                ccr_ast_touch(&ed->ast, root);
                ed->ast.nodes[root].flags |= CCR_FLAG_DEAD;
                remove_script(ed, ed->dragged);
            } else if (ed->snap_ok) {
                drop_into_snap(ed);
            } else if (N(ed, root)->flags & CCR_FLAG_TOPLEVEL) {
                place_in_file(ed, ed->dragged);
            }
            ed->snap_ok = false;
            ed->dragged = UINT32_MAX;
            ed->drag = CCR_DRAG_IDLE;
            commit_change(ed);
        }
        break;

    case CCR_DRAG_SETTLING:
        break;
    }
}

/* ---------------------------------------------------------- drawing */

/* A label named like goto `go` in script `si`, or CCR_NONE. */
static ccr_node_id label_in(const ccr_editor *ed, uint32_t si, ccr_node_id go)
{
    const ccr_blocks *b = &ed->scripts[si].blocks;
    const char *name = ccr_ast_str(&ed->ast, N(ed, go)->name);
    for (uint32_t k = 0; k < b->jump_count; ++k) {
        const ccr_node *n = N(ed, b->jumps[k]);
        if (n->kind == CCR_NODE_LABEL && strcmp(ccr_ast_str(&ed->ast, n->name), name) == 0)
            return b->jumps[k];
    }
    return CCR_NONE;
}

ccr_jump_state ccr_editor_jump(const ccr_editor *ed, ccr_node_id go, ccr_node_id *label)
{
    *label = CCR_NONE;
    if (go >= ed->ast.node_count || N(ed, go)->kind != CCR_NODE_GOTO)
        return CCR_JUMP_MISSING;
    const uint32_t own = script_index_of_root(ed, script_root_of(ed, go));
    if (own != UINT32_MAX && (*label = label_in(ed, own, go)) != CCR_NONE)
        return CCR_JUMP_OK;
    for (uint32_t i = 0; i < ed->script_count; ++i)
        if (i != own && (*label = label_in(ed, i, go)) != CCR_NONE)
            return CCR_JUMP_ELSEWHERE;
    return CCR_JUMP_MISSING;
}

static ccr_color alpha_of(ccr_color c, uint32_t a) { return (c & 0xFFFFFF00u) | (a & 0xFFu); }

static void dashed_line(ccr_vec2 a, ccr_vec2 b, float thick, ccr_color c)
{
    const float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    const float dash = 8.0f, gap = 6.0f;
    for (float t = 0.0f; t < len; t += dash + gap) {
        float t2 = t + dash < len ? t + dash : len;
        ccr_gui_line((ccr_vec2){ a.x + dx * t / len, a.y + dy * t / len },
                     (ccr_vec2){ a.x + dx * t2 / len, a.y + dy * t2 / len }, thick, c);
    }
}

static void polyline(const ccr_vec2 *p, int n, float thick, ccr_color c, bool dashed)
{
    for (int i = 0; i + 1 < n; ++i) {
        if (dashed)
            dashed_line(p[i], p[i + 1], thick, c);
        else
            ccr_gui_line(p[i], p[i + 1], thick, c);
    }
    for (int i = 1; i + 1 < n; ++i)   /* round the corners */
        ccr_gui_circle(p[i], thick * 0.5f, c);
}

/* Dots running along a polyline, from its start to its end. */
static void march(const ccr_vec2 *p, int n, float spacing, float phase, float radius, ccr_color c)
{
    float carry = phase;   /* distance to the next dot */
    for (int i = 0; i + 1 < n; ++i) {
        const float dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y;
        const float len = sqrtf(dx * dx + dy * dy);
        float t = carry;
        for (; t < len; t += spacing)
            ccr_gui_circle((ccr_vec2){ p[i].x + dx * t / len, p[i].y + dy * t / len }, radius, c);
        carry = t - len;
    }
}

/*
 * goto arrows: out of the right side of the goto, along a lane right of
 * the whole function, into the right side of its label. Laid out in world
 * units, for drawing and for clicking alike.
 */
typedef struct jump_arrow {
    ccr_node_id    go, label;
    ccr_jump_state st;
    bool           path;      /* false: no label to point at (a red mark instead) */
    ccr_vec2       p[4];      /* world: goto, lane, lane, label */
} jump_arrow;

enum { MAX_ARROWS = 256 };

/* Where `node` is drawn, or the folded block hiding it. */
static bool shown_rect(const ccr_editor *ed, ccr_node_id node, ccr_rect *r)
{
    for (ccr_node_id a = node; a != CCR_NONE; a = N(ed, a)->parent)
        if (ccr_editor_node_rect(ed, a, r))
            return true;
    return false;
}

/* The arrows of script `si`; shorter jumps get the lanes nearer the
 * blocks, so arrows nest like brackets instead of crossing. */
static uint32_t script_arrows(const ccr_editor *ed, uint32_t si, jump_arrow *out)
{
    const ccr_script *s = &ed->scripts[si];
    struct { ccr_node_id go, label; ccr_jump_state st; float span; } v[MAX_ARROWS];
    uint32_t na = 0;
    for (uint32_t k = 0; k < s->blocks.jump_count && na < MAX_ARROWS; ++k) {
        const ccr_node_id go = s->blocks.jumps[k];
        if (N(ed, go)->kind != CCR_NODE_GOTO || !*ccr_ast_str(&ed->ast, N(ed, go)->name))
            continue;
        ccr_rect f, t;
        ccr_node_id label;
        ccr_jump_state st = ccr_editor_jump(ed, go, &label);
        float span = 1e9f;
        if (shown_rect(ed, go, &f) && label != CCR_NONE && shown_rect(ed, label, &t))
            span = fabsf(t.y - f.y);
        uint32_t j = na++;
        while (j > 0 && v[j - 1].span > span) {
            v[j] = v[j - 1];
            --j;
        }
        v[j].go = go;
        v[j].label = label;
        v[j].st = st;
        v[j].span = span;
    }

    uint32_t n = 0, lane = 0;
    for (uint32_t k = 0; k < na; ++k) {
        ccr_rect from;
        if (!shown_rect(ed, v[k].go, &from))
            continue;
        jump_arrow *a = &out[n];
        a->go = v[k].go;
        a->label = v[k].label;
        a->st = v[k].st;
        a->path = false;
        a->p[0] = (ccr_vec2){ from.x + from.w, from.y + from.h * 0.5f };
        if (a->st != CCR_JUMP_MISSING) {
            ccr_rect to;
            const uint32_t ls = script_index_of_root(ed, script_root_of(ed, a->label));
            if (!shown_rect(ed, a->label, &to) || ls == UINT32_MAX)
                continue;
            if (to.x == from.x && to.y == from.y)
                continue;   /* both folded away in the same block */
            /* the lane: right of both scripts, one lane per arrow */
            float right = s->pos.x + s->blocks.bounds.x + s->blocks.bounds.w;
            const ccr_script *o = &ed->scripts[ls];
            float oright = o->pos.x + o->blocks.bounds.x + o->blocks.bounds.w;
            if (oright > right)
                right = oright;
            const float lx = right + 18.0f + 12.0f * (float)(lane++ % 8);
            const float by = to.y + to.h * 0.5f;
            a->p[1] = (ccr_vec2){ lx, a->p[0].y };
            a->p[2] = (ccr_vec2){ lx, by };
            a->p[3] = (ccr_vec2){ to.x + to.w + 9.0f, by };
            a->path = true;
        }
        ++n;
    }
    return n;
}

/* How far `m` is from the arrow's path, and where along it (0 at the
 * goto, 1 at the label). */
static float arrow_distance(const jump_arrow *a, ccr_vec2 m, float *along)
{
    float total = 0.0f, best = 1e30f, best_at = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const ccr_vec2 p = a->p[i], q = a->p[i + 1];
        const float dx = q.x - p.x, dy = q.y - p.y, len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? ((m.x - p.x) * dx + (m.y - p.y) * dy) / len2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float cx = p.x + dx * t - m.x, cy = p.y + dy * t - m.y;
        const float d = sqrtf(cx * cx + cy * cy), len = sqrtf(len2);
        if (d < best) {
            best = d;
            best_at = total + len * t;
        }
        total += len;
    }
    *along = total > 0.0f ? best_at / total : 0.0f;
    return best;
}

ccr_node_id ccr_editor_arrow_at(const ccr_editor *ed, ccr_vec2 world, float tolerance,
                                ccr_node_id *far_end)
{
    static jump_arrow arrows[MAX_ARROWS];
    ccr_node_id found = CCR_NONE;
    float best = tolerance;
    for (uint32_t si = 0; si < ed->script_count; ++si) {
        const uint32_t n = script_arrows(ed, si, arrows);
        for (uint32_t k = 0; k < n; ++k) {
            if (!arrows[k].path)
                continue;
            float along;
            const float d = arrow_distance(&arrows[k], world, &along);
            if (d <= best) {
                best = d;
                found = arrows[k].go;
                /* the end that is further from where it was clicked: a
                 * click near the goto follows the jump, a click near the
                 * label comes back */
                if (far_end)
                    *far_end = along < 0.5f ? arrows[k].label : arrows[k].go;
            }
        }
    }
    return found;
}

void ccr_editor_flash(ccr_editor *ed, ccr_node_id node, float delay)
{
    if (node >= ed->ast.node_count)
        return;
    ed->landed_first = ed->landed_last = node;
    ed->landed_t = -delay;   /* the ring waits until the view gets there */
}

/*
 * Faint until the mouse is on one end or on the arrow itself; red and
 * dashed when the label is not where the goto can reach it, and a red mark
 * when there is no label at all.
 */
static void draw_jumps(const ccr_editor *ed, const ccr_canvas *cv)
{
    static jump_arrow arrows[MAX_ARROWS];
    const float z = cv->zoom;
    const float thick = 2.5f * (z < 0.6f ? 0.6f : z);
    for (uint32_t si = 0; si < ed->script_count; ++si) {
        const ccr_script *s = &ed->scripts[si];
        const uint32_t n = script_arrows(ed, si, arrows);
        for (uint32_t k = 0; k < n; ++k) {
            const jump_arrow *ar = &arrows[k];
            const ccr_node_id go = ar->go, label = ar->label;
            const ccr_jump_state st = ar->st;
            const bool hot = ed->hover_arrow == go ||
                             (ed->hover != CCR_NONE && (ed->hover == go || ed->hover == label));
            const ccr_vec2 a = ccr_canvas_to_screen(cv, ar->p[0]);

            if (st == CCR_JUMP_MISSING) {
                ccr_rect from;
                ccr_editor_node_rect(ed, go, &from);
                ccr_vec2 tl = ccr_canvas_to_screen(cv, (ccr_vec2){ from.x, from.y });
                ccr_rect r = { tl.x - 3, tl.y - 3, from.w * z + 6, from.h * z + 6 };
                ccr_gui_rect_rounded(r, 0.3f, alpha_of(ccr_th(CCR_C_ERROR), 0xC0u));
                ccr_gui_circle((ccr_vec2){ a.x + 14.0f * z, a.y }, 9.0f * z, ccr_th(CCR_C_ERROR));
                ccr_gui_text("!", a.x + 11.5f * z, a.y - 8.0f * z, 16.0f * z, 0xFFFFFFFFu);
            } else if (ar->path) {
                ccr_vec2 pts[4];
                for (int i = 0; i < 4; ++i)
                    pts[i] = ccr_canvas_to_screen(cv, ar->p[i]);
                const ccr_vec2 b = { pts[3].x - 9.0f * z, pts[3].y };
                const bool broken = st != CCR_JUMP_OK;
                const bool loose = is_loose_root(ed, s->root);
                const ccr_color line = ccr_th_cat(CCR_CAT_CONTROL, CCR_EDGE);
                ccr_color c = broken ? ccr_th(CCR_C_ERROR) : hot ? ccr_th(CCR_C_GLOW)
                            : alpha_of(line, loose ? 0x60u : 0xB0u);
                polyline(pts, 4, hot ? thick * 1.6f : thick, c, broken);
                if (hot && !broken && ed->animate) {
                    /* the way the jump goes */
                    const float spacing = 16.0f * (z < 0.6f ? 0.6f : z);
                    const float phase = fmodf((float)ccr_gui_time() * 3.0f * spacing, spacing);
                    march(pts, 4, spacing, phase, thick * 1.3f, c);   /* beads */
                }
                if (ed->hover_arrow == go) {
                    /* a click goes to the other end: show which one */
                    const ccr_vec2 tip = ed->hover_far == go ? a : pts[3];
                    ccr_gui_circle(tip, thick * 3.2f, alpha_of(ccr_th(CCR_C_GLOW), 0x70u));
                }
                ccr_gui_circle(a, thick * 1.4f, c);
                ccr_gui_triangle((ccr_vec2){ b.x + 1.0f, b.y },
                                 (ccr_vec2){ b.x + 12.0f * z, b.y - 7.0f * z },
                                 (ccr_vec2){ b.x + 12.0f * z, b.y + 7.0f * z }, c);
            }

            /* what is wrong, next to the goto, while the mouse is on it */
            if (st != CCR_JUMP_OK && ed->hover == go) {
                char msg[256];
                ccr_trf(msg, sizeof msg, st == CCR_JUMP_MISSING ? CCR_T_JUMP_MISSING
                                                                : CCR_T_JUMP_ELSEWHERE,
                        ccr_ast_str(&ed->ast, N(ed, go)->name));
                const float tw = ccr_gui_text_width(msg, 16.0f);
                ccr_rect box = { a.x + 30.0f * z, a.y - 14.0f, tw + 16.0f, 28.0f };
                ccr_gui_rect_rounded(box, 0.4f, ccr_th(CCR_C_ERROR));
                ccr_gui_text(msg, box.x + 8.0f, box.y + 5.0f, 16.0f, 0xFFFFFFFFu);
            }
        }
    }
}

void ccr_editor_draw(const ccr_editor *ed, const ccr_canvas *cv, ccr_rect viewport)
{
    for (uint32_t i = 0; i < ed->script_count; ++i) {
        const ccr_script *s = &ed->scripts[i];
        bool loose = is_loose_root(ed, s->root);
        bool dragging = (ed->drag == CCR_DRAG_MOVING || ed->drag == CCR_DRAG_SETTLING) &&
                        i == ed->dragged;
        float alpha = (loose && !(dragging && ed->snap_ok)) ? LOOSE_ALPHA : 1.0f;

        /* where a reporter will land: the slot lights up */
        if (dragging && ed->snap_ok && ed->snap.kind == CCR_SNAP_SLOT) {
            ccr_vec2 p = ccr_canvas_to_screen(cv, ed->snap.at);
            const float w = ed->snap.width * cv->zoom, h = ed->snap.height * cv->zoom;
            const float m = 5.0f * (cv->zoom < 1.0f ? 1.0f : cv->zoom);
            ccr_gui_rect_rounded((ccr_rect){ p.x - m, p.y - m, w + 2 * m, h + 2 * m }, 1.0f,
                                 ccr_th(CCR_C_GLOW));
            ccr_gui_rect_rounded((ccr_rect){ p.x, p.y, w, h }, 1.0f, ccr_th(CCR_C_BG));
        } else if (dragging && ed->snap_ok) {
            /* where the stack will land: the seam it will make, puzzle tab
             * included, so it reads as "clicks in here" */
            const float z = cv->zoom;
            const ccr_vec2 p = ccr_canvas_to_screen(cv, ed->snap.at);
            const float w = (ed->snap.width > 60.0f ? ed->snap.width : 60.0f) * z;
            const float t = 5.0f * (z < 1.0f ? 1.0f : z);
            const float tx = p.x + CCR_TAB_X * z, tw = CCR_TAB_W * z, th = CCR_TAB_H * z;
            const ccr_vec2 seam[6] = {
                { p.x, p.y }, { tx, p.y }, { tx, p.y + th },
                { tx + tw, p.y + th }, { tx + tw, p.y }, { p.x + w, p.y },
            };
            const ccr_color c = ccr_th(CCR_C_GLOW);
            for (int k = 0; k + 1 < 6; ++k)
                ccr_gui_line(seam[k], seam[k + 1], t, c);
            for (int k = 0; k < 6; ++k)   /* round joints and ends */
                ccr_gui_circle(seam[k], t * 0.5f, c);
        }
        if (dragging) {
            /* lifted off the canvas: a shadow, as one solid colour so the
             * blocks nested inside do not darken it twice */
            const ccr_color sh = ccr_th(CCR_C_SHADOW), bg = ccr_th(CCR_C_BG);
            const uint32_t a = sh & 0xFFu;
            ccr_color flat = 0xFFu;
            for (int k = 0; k < 3; ++k) {
                uint32_t sc = (sh >> (24 - 8 * k)) & 0xFFu, bc = (bg >> (24 - 8 * k)) & 0xFFu;
                flat |= ((sc * a + bc * (255u - a)) / 255u) << (24 - 8 * k);
            }
            /* while it glides in, it also comes down onto the canvas */
            float lift = 7.0f;
            if (ed->drag == CCR_DRAG_SETTLING) {
                float k = ed->settle_t / SETTLE_TIME;
                lift *= 1.0f - ease_out_cubic(k > 1.0f ? 1.0f : k);
            }
            ccr_blocks_draw_shadow(&s->blocks, cv, viewport,
                                   (ccr_vec2){ s->pos.x + lift * 0.6f, s->pos.y + lift }, flat);
        }
        ccr_blocks_draw(&s->blocks, cv, viewport, s->pos, alpha);
    }
    draw_jumps(ed, cv);

    /* an #include under the mouse: a click opens the file, underlined */
    ccr_rect inc;
    if (ed->hover_include != CCR_NONE && ed->drag == CCR_DRAG_IDLE &&
        ccr_editor_node_rect(ed, ed->hover_include, &inc)) {
        const ccr_vec2 a = ccr_canvas_to_screen(cv, (ccr_vec2){ inc.x + 10.0f, inc.y + inc.h - 7.0f });
        const ccr_vec2 b = ccr_canvas_to_screen(cv, (ccr_vec2){ inc.x + inc.w - 10.0f, inc.y + inc.h - 7.0f });
        ccr_gui_line(a, b, 2.0f * (cv->zoom < 1.0f ? 1.0f : cv->zoom), alpha_of(ccr_th(CCR_C_GLOW), 0xD0u));
    }

    /* an operator under the mouse: a click there offers the others */
    if (ed->hover_op != CCR_NONE && ed->drag == CCR_DRAG_IDLE) {
        const ccr_rect r = ed->hover_op_rect;
        const ccr_vec2 tl = ccr_canvas_to_screen(cv, (ccr_vec2){ r.x - 4.0f, r.y - 2.0f });
        const ccr_rect box = { tl.x, tl.y, (r.w + 8.0f) * cv->zoom, (r.h + 4.0f) * cv->zoom };
        ccr_gui_rect_rounded(box, 0.5f, alpha_of(0xFFFFFFFFu, 0x55u));
        ccr_gui_rect_rounded((ccr_rect){ box.x, box.y + box.h - 2.0f, box.w, 2.0f }, 1.0f,
                             alpha_of(ccr_th(CCR_C_GLOW), 0xD0u));
    }

    /* a ripple round what just snapped in */
    if (ed->animate && ed->landed_first != CCR_NONE && ed->landed_t >= 0.0f &&
        ed->landed_t < RIPPLE_TIME) {
        ccr_rect a, b;
        if (ccr_editor_node_rect(ed, ed->landed_first, &a) &&
            ccr_editor_node_rect(ed, ed->landed_last, &b)) {
            const float k = ed->landed_t / RIPPLE_TIME;
            const float grow = (2.0f + 9.0f * ease_out_cubic(k)) / cv->zoom;
            float x0 = fminf(a.x, b.x) - grow, y0 = a.y - grow;
            float x1 = fmaxf(a.x + a.w, b.x + b.w) + grow, y1 = b.y + b.h + grow;
            const ccr_vec2 p0 = ccr_canvas_to_screen(cv, (ccr_vec2){ x0, y0 });
            const ccr_vec2 p1 = ccr_canvas_to_screen(cv, (ccr_vec2){ x1, y1 });
            const ccr_vec2 pts[5] = { p0, { p1.x, p0.y }, p1, { p0.x, p1.y }, p0 };
            const ccr_color c = alpha_of(ccr_th(CCR_C_GLOW), (uint32_t)(200.0f * (1.0f - k)));
            const float th = 3.0f * (1.0f - k) + 1.0f;
            polyline(pts, 5, th, c, false);
            ccr_gui_circle(p0, th * 0.5f, c);
        }
    }

    /* bits of deleted blocks */
    for (uint32_t i = 0; i < ed->particle_count; ++i) {
        const ccr_particle *p = &ed->particles[i];
        const float k = p->life / p->max_life;
        const float s = p->size * cv->zoom * (0.4f + 0.6f * k);
        const ccr_vec2 c = ccr_canvas_to_screen(cv, p->pos);
        ccr_gui_rect_rounded((ccr_rect){ c.x - s * 0.5f, c.y - s * 0.5f, s, s }, 0.4f,
                             alpha_of(p->color, (uint32_t)(255.0f * k)));
    }
}
