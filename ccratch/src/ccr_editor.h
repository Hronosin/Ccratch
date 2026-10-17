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
#include "ccr_template.h"

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
    CCR_DRAG_MOVING,
    CCR_DRAG_SETTLING      /* let go over a snap point: gliding into place */
} ccr_drag_state;

typedef struct ccr_editor_input {
    ccr_vec2 mouse;        /* world coordinates */
    bool     pressed;      /* left button went down this frame */
    bool     down;
    bool     released;
    float    zoom;         /* so "moved far enough" is in screen pixels */
    bool     over_trash;   /* over the palette: a stack dropped here is deleted */
    float    dt;           /* seconds since the last update; 0: no animation */
} ccr_editor_input;

/* A bit of a block that was just deleted, flying off. */
typedef struct ccr_particle {
    ccr_vec2  pos, vel;    /* world */
    float     size, life, max_life;
    ccr_color color;
} ccr_particle;

#define CCR_MAX_PARTICLES 256

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
    ccr_node_id    press_op;       /* pressed on an operator that can change */
    ccr_node_id    op_request;     /* clicked one: the app shows the choices */
    ccr_node_id    hover_op;       /* an operator under the mouse */
    ccr_node_id    hover_include;  /* an #include under the mouse: a click opens it */
    ccr_node_id    include_request;
    ccr_rect       hover_op_rect;  /* world */

    /* a duplicate stuck to the mouse: dropped on the next click */
    bool           carrying;

    /* the node under the mouse when nothing is being dragged */
    ccr_node_id    hover;

    /* animations (off unless the app turns them on; tests leave them off) */
    bool           animate;
    ccr_vec2       settle_from, settle_to;
    float          settle_t;
    double         clock;          /* seconds of updates, for double clicks */
    double         last_click_at;
    ccr_node_id    last_click;
    ccr_node_id    hover_arrow;    /* the goto whose arrow is under the mouse */
    ccr_node_id    hover_far;      /* and the end a click there goes to */
    ccr_node_id    landed_first, landed_last;   /* just snapped in: a ripple */
    float          landed_t;
    ccr_particle   particles[CCR_MAX_PARTICLES];
    uint32_t       particle_count;
    uint32_t       rng;

    /* what the last build said about blocks of this file */
    struct ccr_editor_mark *marks;
    uint32_t       mark_count;

    /* the debugger: where the program stopped, its breakpoints, and the
     * values of what that statement uses */
    ccr_node_id    dbg_at;          /* CCR_NONE: not stopped in this file */
    bool           dbg_fault;       /* stopped by a crash (SIGSEGV...) */
    ccr_node_id   *dbg_breaks;
    uint32_t       dbg_break_count;
    char           dbg_values[600]; /* "i = 1\ntotal = 0": beside dbg_at */
} ccr_editor;

/* Blocks from another tree (ccr_paste): a stack of `items` (or one
 * reporter) stuck to the mouse, dropped on the next click like Ctrl+D's
 * copy. One undo step. */
bool ccr_editor_paste(ccr_editor *ed, const ccr_ast *src, const ccr_node_id *items, int count, bool reporter,
                      ccr_vec2 mouse);
/* Declarations (functions, types, #includes) from another tree, put
 * straight into the file where their kind goes, next to `at` on the
 * canvas. One undo step. */
bool ccr_editor_paste_decls(ccr_editor *ed, const ccr_ast *src, const ccr_node_id *items, int count, ccr_vec2 at);

/* An error or warning from a build, pinned to the block it is about. */
typedef struct ccr_editor_mark {
    ccr_node_id node;
    bool        warning;      /* else an error */
    char        text[300];
} ccr_editor_mark;

/* Takes ownership of `ast` (parsed, bodies wrapped). Needs an open window. */
bool ccr_editor_init(ccr_editor *ed, ccr_ast *ast);
void ccr_editor_free(ccr_editor *ed);

/* A word on the canvas that matched a search. */
typedef struct ccr_find_hit {
    ccr_node_id node;     /* the block it belongs to */
    ccr_rect    rect;     /* world: the word, or the folded block hiding it */
    bool        hidden;   /* inside a folded block (going there unfolds it) */
} ccr_find_hit;

/* Every word on the canvas containing `query` (any case, Latin and
 * Cyrillic), top to bottom: the blocks' own words, and names inside folded
 * blocks. Returns how many there are; up to `cap` go into `out`. */
uint32_t ccr_editor_find(const ccr_editor *ed, const char *query, ccr_find_hit *out, uint32_t cap);

/* The build's errors and warnings on this file's blocks (copied; replaces
 * what was there). Drawn as a red or orange outline with a badge. */
void ccr_editor_set_marks(ccr_editor *ed, const ccr_editor_mark *marks, uint32_t count);
/* The debugger's look on this file: the statement it stopped at (or
 * CCR_NONE), `fault` if a crash stopped it there, the statements with a
 * breakpoint (copied), and lines of values to show beside the one it
 * stopped at. */
void ccr_editor_set_debug(ccr_editor *ed, ccr_node_id at, bool fault, const ccr_node_id *breaks, uint32_t count,
                          const char *values);
/* The statement a breakpoint would go on for `id`: itself if it is one,
 * else the statement it is part of. CCR_NONE for declarations. */
ccr_node_id ccr_editor_statement_of(const ccr_editor *ed, ccr_node_id id);

/* The mark under a world point (its badge or its block), or -1. */
int  ccr_editor_mark_at(const ccr_editor *ed, ccr_vec2 world);
/* Where mark `i` is drawn (world): its block. */
bool ccr_editor_mark_rect(const ccr_editor *ed, int i, ccr_rect *out);

void ccr_editor_update(ccr_editor *ed, const ccr_editor_input *in);
void ccr_editor_draw(const ccr_editor *ed, const ccr_canvas *cv, ccr_rect viewport);

/* Everything, world coordinates. */
ccr_rect ccr_editor_bounds(const ccr_editor *ed);

/* World rectangle of a node's first block, for tests and "jump to". */
bool ccr_editor_node_rect(const ccr_editor *ed, ccr_node_id node, ccr_rect *out);

/* Lays every script out again (the theme changed its colours). */
void ccr_editor_relayout_all(ccr_editor *ed);

/* How many loose stacks lie on the canvas. */
uint32_t ccr_editor_loose_count(const ccr_editor *ed);

/*
 * Takes a new block from the palette: it appears under the mouse (world
 * coordinates) and is being dragged, as if picked up from the canvas.
 * Call while the mouse button is down; normal updates carry on the drag.
 */
bool ccr_editor_spawn(ccr_editor *ed, ccr_template t, ccr_vec2 mouse);
/* Adds `#include <x>` / `#include "x"` to the file, where includes go
 * (one undo step); false if it is there already. Library blocks dropped
 * into the file bring their pack's include by themselves. */
bool ccr_editor_add_include(ccr_editor *ed, const char *spelled);

/* A declaration from the palette that is now part of the file (it can
 * be deleted again; the file's own ones cannot, yet). */
bool ccr_editor_is_new_decl(const ccr_editor *ed, ccr_node_id id);

/* The same, held by `grab` (where in the block, in block units). */
bool ccr_editor_spawn_held(ccr_editor *ed, ccr_template t, ccr_vec2 mouse, ccr_vec2 grab);

/* Text slot under edit: set its code (one undo step). */
bool ccr_editor_set_text(ccr_editor *ed, ccr_node_id slot, const char *text);

/* A slot with a list to pick from (ccr_slot_choices) can be typed into
 * even when it holds a name or a number read from the file: the value
 * typed or picked takes its place (one undo step). For a text slot this
 * is ccr_editor_set_text. */
bool ccr_editor_set_value(ccr_editor *ed, ccr_node_id slot, const char *text);
/* Can a click on `id` type into it that way? */
bool ccr_editor_value_slot(const ccr_editor *ed, ccr_node_id id);

/* The text slot to edit after a click, or CCR_NONE. Clears the request. */
ccr_node_id ccr_editor_take_edit_request(ccr_editor *ed);

/* An operator was clicked (a + b, x += 1, !ok...): which node, or
 * CCR_NONE. Its choices are ccr_op_choices(). */
ccr_node_id ccr_editor_take_op_request(ccr_editor *ed);

/* An #include was clicked (not its name field): the app opens the file. */
ccr_node_id ccr_editor_take_include_request(ccr_editor *ed);

/* Swaps the operator of `node` (one undo step); saving prints it anew,
 * with parentheses if the new one binds differently. */
bool ccr_editor_set_op(ccr_editor *ed, ccr_node_id node, const char *op);

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
    ccr_node_id fold;        /* the block a fold would fold, or CCR_NONE */
    bool        folded;      /* and it is folded now */
} ccr_context;

ccr_context ccr_editor_context(const ccr_editor *ed, ccr_vec2 mouse);

/* Where a goto goes. Labels belong to their function, so a label of the
 * same name somewhere else (a loose stack, another function) does not
 * count: clang would say "use of undeclared label". */
typedef enum ccr_jump_state {
    CCR_JUMP_OK,          /* its label is in the same function */
    CCR_JUMP_ELSEWHERE,   /* a label of that name exists, but not there */
    CCR_JUMP_MISSING      /* no such label anywhere */
} ccr_jump_state;

ccr_jump_state ccr_editor_jump(const ccr_editor *ed, ccr_node_id go, ccr_node_id *label);

/* Deletes one block from its stack; the blocks under it move up. A whole
 * loose stack goes if its top block is picked. An expression in a slot
 * goes and leaves the slot empty. One undo step. */
bool ccr_editor_delete(ccr_editor *ed, ccr_node_id item);
/* Several at once (a selection), one undo step. */
bool ccr_editor_delete_many(ccr_editor *ed, const ccr_node_id *items, int count);

/* Copies a block (a statement, or an expression in a slot); the copy
 * follows the mouse until the next click, then snaps or lies loose like
 * any dropped block. */
bool ccr_editor_duplicate(ccr_editor *ed, ccr_node_id item, ccr_vec2 mouse);

/* Comments a block out (grey, saved as // lines), or back in. */
bool ccr_editor_toggle_comment(ccr_editor *ed, ccr_node_id item);

/* Undo/redo one drag. False if there is nothing to do (or a drag is on). */
bool ccr_editor_can_undo(const ccr_editor *ed);
bool ccr_editor_can_redo(const ccr_editor *ed);
/* Moves animations on by `dt` seconds. ccr_editor_update does this too;
 * call it on frames where update is not called (a menu is open). */
void ccr_editor_tick(ccr_editor *ed, float dt);
/* A stack still gliding into place is put there now. Every change
 * (undo, delete, ...) does this first; call it before saving. */
void ccr_editor_settle(ccr_editor *ed);

/* Can `node` fold to one line: C blocks, functions, structs. */
bool ccr_editor_can_fold(const ccr_editor *ed, ccr_node_id node);
/* Folds or unfolds it. A view, not an edit: no undo step, not saved. */
bool ccr_editor_toggle_fold(ccr_editor *ed, ccr_node_id node);
/* Unfolds whatever hides `node`, so it can be seen (and its rect found). */
void ccr_editor_reveal(ccr_editor *ed, ccr_node_id node);

/* The goto arrow within `tolerance` (world units) of `world`, or CCR_NONE.
 * `far_end` gets the end a click there takes you to: the one further from
 * the click, along the arrow (near the goto: its label; near the label:
 * the goto), so a click by mistake is undone by clicking again. */
ccr_node_id ccr_editor_arrow_at(const ccr_editor *ed, ccr_vec2 world, float tolerance,
                                ccr_node_id *far_end);

/* A ring round `node` once `delay` seconds have passed (after the view
 * has glided to it). Needs animations on. */
void ccr_editor_flash(ccr_editor *ed, ccr_node_id node, float delay);

bool ccr_editor_undo(ccr_editor *ed);
bool ccr_editor_redo(ccr_editor *ed);

/* Changed since loaded or last saved? Undoing back to the saved state
 * counts as unchanged. */
bool ccr_editor_modified(const ccr_editor *ed);
void ccr_editor_mark_saved(ccr_editor *ed);

#endif /* CCR_EDITOR_H */
