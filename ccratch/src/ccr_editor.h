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

/* Undo/redo one drag. False if there is nothing to do (or a drag is on). */
bool ccr_editor_undo(ccr_editor *ed);
bool ccr_editor_redo(ccr_editor *ed);

/* Changed since loaded or last saved? Undoing back to the saved state
 * counts as unchanged. */
bool ccr_editor_modified(const ccr_editor *ed);
void ccr_editor_mark_saved(ccr_editor *ed);

#endif /* CCR_EDITOR_H */
