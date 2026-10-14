#include "ccr_editor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_GAP     32.0f   /* between top-level scripts in a column */
#define COLUMN_GAP     96.0f
#define DRAG_THRESHOLD  4.0f   /* screen pixels before a press becomes a drag */
#define SNAP_RADIUS    36.0f   /* world units */
#define LOOSE_ALPHA     0.55f
#define HISTORY_MAX    500

static const ccr_node *N(const ccr_editor *ed, ccr_node_id id) { return &ed->ast.nodes[id]; }

static void begin_change(ccr_editor *ed);
static void find_snap(ccr_editor *ed);
static void commit_change(ccr_editor *ed);

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
    ed->edit_request = CCR_NONE;
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
    uint32_t moving;
    const ccr_node_id root = ed->scripts[si].root;

    begin_change(ed);   /* everything until the drop is one undo step */

    if (item != CCR_NONE &&
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

static const char *const g_template_labels[CCR_TEMPLATE_COUNT] = {
    [CCR_TEMPLATE_CODE]     = "code line",
    [CCR_TEMPLATE_IF]       = "if",
    [CCR_TEMPLATE_IF_ELSE]  = "if / else",
    [CCR_TEMPLATE_WHILE]    = "while",
    [CCR_TEMPLATE_FOR]      = "for",
    [CCR_TEMPLATE_DO_WHILE] = "do / while",
    [CCR_TEMPLATE_RETURN]   = "return",
    [CCR_TEMPLATE_BREAK]    = "break",
    [CCR_TEMPLATE_CONTINUE] = "continue",
};

const char *ccr_template_label(ccr_template t)
{
    return (unsigned)t < CCR_TEMPLATE_COUNT ? g_template_labels[t] : "?";
}

static ccr_node_id new_node(ccr_editor *ed, ccr_node_kind kind, ccr_node_id parent)
{
    ccr_node_id id = ccr_ast_add(&ed->ast, kind, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ed->ast.nodes[id].flags |= CCR_FLAG_NEW;
    if (kind == CCR_NODE_BLOCK) {
        /* printed with relative indents; the block it lands in shifts it */
        ed->ast.nodes[id].indent = ccr_ast_intern(&ed->ast, "    ");
        ed->ast.nodes[id].flags |= CCR_FLAG_DIRTY;
    }
    if (parent != CCR_NONE)
        ccr_ast_append(&ed->ast, parent, id);
    return id;
}

static ccr_node_id new_text(ccr_editor *ed, ccr_node_id parent, const char *text)
{
    ccr_node_id id = new_node(ed, CCR_NODE_TEXT, CCR_NONE);
    if (id == CCR_NONE)
        return CCR_NONE;
    ed->ast.nodes[id].name = ccr_ast_intern(&ed->ast, text);
    ccr_ast_append(&ed->ast, parent, id);
    return id;
}

/* Builds a template's nodes under `parent`. False if out of memory. */
static bool build_template(ccr_editor *ed, ccr_template t, ccr_node_id parent)
{
    ccr_node_id s = CCR_NONE;
    switch (t) {
    case CCR_TEMPLATE_CODE:
        return new_text(ed, parent, "") != CCR_NONE;
    case CCR_TEMPLATE_IF:
    case CCR_TEMPLATE_IF_ELSE:
        s = new_node(ed, CCR_NODE_IF, parent);
        new_text(ed, s, "");
        new_node(ed, CCR_NODE_BLOCK, s);
        if (t == CCR_TEMPLATE_IF_ELSE)
            new_node(ed, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_WHILE:
        s = new_node(ed, CCR_NODE_WHILE, parent);
        new_text(ed, s, "");
        new_node(ed, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_FOR:
        s = new_node(ed, CCR_NODE_FOR, parent);
        new_text(ed, s, "int i = 0");
        new_text(ed, s, "i < n");
        new_text(ed, s, "i++");
        new_node(ed, CCR_NODE_BLOCK, s);
        break;
    case CCR_TEMPLATE_DO_WHILE:
        s = new_node(ed, CCR_NODE_DO_WHILE, parent);
        new_node(ed, CCR_NODE_BLOCK, s);
        new_text(ed, s, "");
        break;
    case CCR_TEMPLATE_RETURN:
        s = new_node(ed, CCR_NODE_RETURN, parent);
        new_text(ed, s, "");
        break;
    case CCR_TEMPLATE_BREAK:
        s = new_node(ed, CCR_NODE_BREAK, parent);
        break;
    case CCR_TEMPLATE_CONTINUE:
        s = new_node(ed, CCR_NODE_CONTINUE, parent);
        break;
    case CCR_TEMPLATE_COUNT:
        break;
    }
    return s != CCR_NONE && !ed->journal.oom;
}

bool ccr_editor_spawn(ccr_editor *ed, ccr_template t, ccr_vec2 mouse)
{
    if (ed->drag != CCR_DRAG_IDLE || (unsigned)t >= CCR_TEMPLATE_COUNT)
        return false;
    begin_change(ed);

    ccr_node_id loose = ccr_ast_add(&ed->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (loose == CCR_NONE) {
        commit_change(ed);
        return false;
    }
    ccr_ast_touch(&ed->ast, loose);
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE;
    if (!build_template(ed, t, loose) || !push_script(ed, loose, mouse)) {
        commit_change(ed);
        return false;
    }

    /* held a little in from its corner, as if grabbed there */
    ed->dragged = ed->script_count - 1;
    ed->grab = (ccr_vec2){ 16.0f, 16.0f };
    ed->scripts[ed->dragged].pos = (ccr_vec2){ mouse.x - ed->grab.x, mouse.y - ed->grab.y };
    ed->drag = CCR_DRAG_MOVING;
    find_snap(ed);
    return true;
}

bool ccr_editor_set_text(ccr_editor *ed, ccr_node_id slot, const char *text)
{
    if (ed->drag != CCR_DRAG_IDLE || slot >= ed->ast.node_count ||
        N(ed, slot)->kind != CCR_NODE_TEXT)
        return false;
    if (strcmp(ccr_ast_str(&ed->ast, N(ed, slot)->name), text) == 0)
        return true;   /* unchanged: not worth an undo step */

    begin_change(ed);
    ccr_ast_touch(&ed->ast, slot);
    ed->ast.nodes[slot].name = ccr_ast_intern(&ed->ast, text);
    /* the block it sits in changes, so saving rebuilds that part */
    ccr_node_id p = N(ed, slot)->parent;
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

ccr_node_id ccr_editor_take_edit_request(ccr_editor *ed)
{
    ccr_node_id id = ed->edit_request;
    ed->edit_request = CCR_NONE;
    return id;
}

/* --------------------------------------------------- block actions */

ccr_context ccr_editor_context(const ccr_editor *ed, ccr_vec2 mouse)
{
    ccr_context c = { CCR_NONE, CCR_NONE, CCR_NONE, false, false, false, false };
    if (ed->drag != CCR_DRAG_IDLE)
        return c;
    uint32_t si;
    c.node = pick(ed, mouse, &si);
    if (c.node == CCR_NONE)
        return c;
    if (N(ed, c.node)->kind == CCR_NODE_TEXT)
        c.text = c.node;
    c.item = stack_item_of(ed, c.node);
    if (c.item == CCR_NONE) {
        /* a whole script: only loose stacks can go (the file's own
         * functions and structs stay; deleting those is not supported) */
        ccr_node_id root = ed->scripts[si].root;
        if (is_loose_root(ed, root) && N(ed, root)->first_child != CCR_NONE) {
            c.item = N(ed, root)->first_child;
            c.can_delete = true;
        }
        return c;
    }
    c.can_delete = c.can_duplicate = c.can_comment = true;
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
    if (ed->drag != CCR_DRAG_IDLE || !is_stack_item(ed, item))
        return false;
    const ccr_node_id block = N(ed, item)->parent;
    const ccr_node_id root = script_root_of(ed, item);
    uint32_t si = script_index_of_root(ed, root);
    if (si == UINT32_MAX)
        return false;

    begin_change(ed);
    if (is_loose_root(ed, block) && N(ed, block)->first_child == item) {
        /* the top of a loose stack: the whole stack goes */
        ccr_ast_touch(&ed->ast, block);
        ed->ast.nodes[block].flags |= CCR_FLAG_DEAD;
        remove_script(ed, si);
    } else {
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
    if (ed->drag != CCR_DRAG_IDLE || !is_stack_item(ed, item))
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
    ed->ast.nodes[loose].flags |= CCR_FLAG_LOOSE;
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
    if (ed->drag != CCR_DRAG_IDLE || !is_stack_item(ed, item))
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
    if (ed->drag != CCR_DRAG_IDLE || ed->history_len == 0)
        return false;
    const ccr_change *c = &ed->history[--ed->history_len];
    for (uint32_t i = c->node_count; i-- > 0;)
        ccr_ast_set_links(&ed->ast, &c->before[i]);
    for (uint32_t i = 0; i < c->node_count; ++i)
        if (c->before[i].name != c->after[i].name)
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
    if (ed->drag != CCR_DRAG_IDLE || ed->history_len == ed->history_count)
        return false;
    const ccr_change *c = &ed->history[ed->history_len++];
    for (uint32_t i = 0; i < c->node_count; ++i)
        ccr_ast_set_links(&ed->ast, &c->after[i]);
    for (uint32_t i = 0; i < c->node_count; ++i)
        if (c->before[i].name != c->after[i].name)
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
            if (ed->press_node != CCR_NONE && N(ed, ed->press_node)->kind == CCR_NODE_TEXT)
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
            if (in->over_trash && is_loose_root(ed, root)) {
                /* thrown back onto the palette: gone (Ctrl+Z brings it back) */
                ccr_ast_touch(&ed->ast, root);
                ed->ast.nodes[root].flags |= CCR_FLAG_DEAD;
                remove_script(ed, ed->dragged);
            } else if (ed->snap_ok) {
                drop_into_snap(ed);
            }
            ed->snap_ok = false;
            ed->dragged = UINT32_MAX;
            ed->drag = CCR_DRAG_IDLE;
            commit_change(ed);
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
