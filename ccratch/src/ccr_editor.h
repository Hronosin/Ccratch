/*
 * ccr_editor - an open file as scripts on a canvas, and dragging them.
 *
 * Scratch rules:
 *   - grabbing a statement takes it and everything under it in its stack;
 *   - grabbing a function hat, a struct or an #include moves the whole thing;
 *   - a stack dropped near a snap point joins that stack;
 *   - a stack dropped anywhere else lies loose on the canvas. Loose blocks
 *     are drawn faded and are NOT part of the program.
 *
 * Every move edits the ccr_ast, so what you see is what will be saved.
 * Input is passed in rather than read from the GUI, so tests can drive it.
 */
#ifndef CCR_EDITOR_H
#define CCR_EDITOR_H

#include "ccr_ast.h"
#include "ccr_blocks.h"
#include "ccr_canvas.h"

typedef struct ccr_script {
    ccr_node_id root;      /* top-level declaration or loose Block */
    ccr_vec2    pos;       /* world */
    ccr_blocks  blocks;
} ccr_script;

/* Where a script sits: enough to put the canvas back as it was. */
typedef struct ccr_placed {
    ccr_node_id root;
    ccr_vec2    pos;
} ccr_placed;

/* One undoable step (one drag): nodes and canvas, before and after. */
typedef struct ccr_change {
    ccr_node_links *before, *after;   /* same nodes, same order */
    uint32_t        node_count;
    ccr_placed     *scripts_before, *scripts_after;
    uint32_t        nb, na;
    uint32_t        nodes_before;     /* nodes from here on were created by it */
} ccr_change;

/* What the palette offers. */
typedef enum ccr_template {
    CCR_TEMPLATE_CODE,       /* a line of code: call, assignment, declaration */
    CCR_TEMPLATE_IF,
    CCR_TEMPLATE_IF_ELSE,
    CCR_TEMPLATE_WHILE,
    CCR_TEMPLATE_FOR,
    CCR_TEMPLATE_DO_WHILE,
    CCR_TEMPLATE_RETURN,
    CCR_TEMPLATE_BREAK,
    CCR_TEMPLATE_CONTINUE,
    CCR_TEMPLATE_COUNT
} ccr_template;

/* Palette label, e.g. "if / else". */
const char *ccr_template_label(ccr_template t);

typedef enum ccr_drag_state {
    CCR_DRAG_IDLE,
    CCR_DRAG_PRESSED,      /* button down, not moved far enough yet */
    CCR_DRAG_MOVING
} ccr_drag_state;

typedef struct ccr_editor_input {
    ccr_vec2 mouse;        /* world coordinates */
    bool     pressed;      /* left button went down this frame */
    bool     down;
    bool     released;
    float    zoom;         /* so "moved far enough" is in screen pixels */
    bool     over_trash;   /* over the palette: a stack dropped here is deleted */
} ccr_editor_input;

typedef struct ccr_editor {
    ccr_ast          ast;
    ccr_layout_cache cache;

    ccr_script *scripts;   /* in draw order: last is on top */
    uint32_t    script_count, script_cap;

    ccr_drag_state drag;
    ccr_vec2       press_at;     /* world */
    ccr_node_id    press_node;
    uint32_t       dragged;      /* script index while MOVING */
    ccr_vec2       grab;         /* mouse - script.pos */
    bool           snap_ok;
    ccr_snap       snap;         /* world coordinates in .at */

    /* undo history: [0, history_len) done, [history_len, history_count) redoable */
    ccr_change    *history;
    uint32_t       history_len, history_count, history_cap;
    int64_t        saved_at;     /* history_len when last saved, -1 if gone */

    /* the change being recorded while a drag is on */
    bool           recording;
    ccr_journal    journal;
    ccr_placed    *pending;
    uint32_t       pending_count;
    uint32_t       pending_nodes;

    /* a text slot was clicked: the app opens a text box for it */
    ccr_node_id    edit_request;

    /* a duplicate stuck to the mouse: dropped on the next click */
    bool           carrying;
} ccr_editor;

/* Takes ownership of `ast` (parsed, bodies wrapped). Needs an open window. */
bool ccr_editor_init(ccr_editor *ed, ccr_ast *ast);
void ccr_editor_free(ccr_editor *ed);

void ccr_editor_update(ccr_editor *ed, const ccr_editor_input *in);
void ccr_editor_draw(const ccr_editor *ed, const ccr_canvas *cv, ccr_rect viewport);

/* Everything, world coordinates. */
ccr_rect ccr_editor_bounds(const ccr_editor *ed);

/* World rectangle of a node's first block, for tests and "jump to". */
bool ccr_editor_node_rect(const ccr_editor *ed, ccr_node_id node, ccr_rect *out);

/* How many loose stacks lie on the canvas. */
uint32_t ccr_editor_loose_count(const ccr_editor *ed);

/*
 * Takes a new block from the palette: it appears under the mouse (world
 * coordinates) and is being dragged, as if picked up from the canvas.
 * Call while the mouse button is down; normal updates carry on the drag.
 */
bool ccr_editor_spawn(ccr_editor *ed, ccr_template t, ccr_vec2 mouse);

/* Text slot under edit: set its code (one undo step). */
bool ccr_editor_set_text(ccr_editor *ed, ccr_node_id slot, const char *text);

/* The text slot to edit after a click, or CCR_NONE. Clears the request. */
ccr_node_id ccr_editor_take_edit_request(ccr_editor *ed);

/* What is under the mouse, and what can be done to it (for the pie menu). */
typedef struct ccr_context {
    ccr_node_id node;        /* topmost node under the mouse, or CCR_NONE */
    ccr_node_id item;        /* the statement in a stack it belongs to */
    ccr_node_id target;      /* what Delete/Duplicate act on: the expression
                                under the mouse if it can come out of its
                                slot, else `item` */
    ccr_node_id text;        /* a white text field, if that is what was hit */
    bool        can_delete;
    bool        can_duplicate;
    bool        can_comment;
    bool        commented;   /* item is currently commented out */
} ccr_context;

ccr_context ccr_editor_context(const ccr_editor *ed, ccr_vec2 mouse);

/* Deletes one block from its stack; the blocks under it move up. A whole
 * loose stack goes if its top block is picked. An expression in a slot
 * goes and leaves the slot empty. One undo step. */
bool ccr_editor_delete(ccr_editor *ed, ccr_node_id item);

/* Copies a block (a statement, or an expression in a slot); the copy
 * follows the mouse until the next click, then snaps or lies loose like
 * any dropped block. */
bool ccr_editor_duplicate(ccr_editor *ed, ccr_node_id item, ccr_vec2 mouse);

/* Comments a block out (grey, saved as // lines), or back in. */
bool ccr_editor_toggle_comment(ccr_editor *ed, ccr_node_id item);

/* Undo/redo one drag. False if there is nothing to do (or a drag is on). */
bool ccr_editor_can_undo(const ccr_editor *ed);
bool ccr_editor_can_redo(const ccr_editor *ed);
bool ccr_editor_undo(ccr_editor *ed);
bool ccr_editor_redo(ccr_editor *ed);

/* Changed since loaded or last saved? Undoing back to the saved state
 * counts as unchanged. */
bool ccr_editor_modified(const ccr_editor *ed);
void ccr_editor_mark_saved(ccr_editor *ed);

#endif /* CCR_EDITOR_H */
