/*
 * ccr_blocks - turns one script of a ccr_ast into Scratch-style blocks.
 *
 * A script is one top-level thing (a function, a struct, an #include) or a
 * loose stack of statements lying on the canvas. Each script is laid out on
 * its own, at the origin, into a flat display list of blocks, hexagons and
 * labels. The editor places scripts by giving each an offset, so moving a
 * script never needs a new layout; only scripts whose contents changed do.
 *
 * Layout also records snap points: the places where a dragged stack of
 * blocks can be dropped (under a statement, or at the top of an empty or
 * non-empty mouth).
 */
#ifndef CCR_BLOCKS_H
#define CCR_BLOCKS_H

#include "ccr_ast.h"
#include "ccr_canvas.h"
#include "ccr_gui.h"

/* The puzzle tab under a stack block (world units): how far in from the
 * block's left edge, how wide, how deep. */
#define CCR_TAB_X 16.0f
#define CCR_TAB_W 32.0f
#define CCR_TAB_H  8.0f

typedef enum ccr_prim_kind {
    CCR_PRIM_BLOCK,   /* rounded rectangle with an outline */
    CCR_PRIM_HEX,     /* boolean reporter */
    CCR_PRIM_TEXT,
    CCR_PRIM_TAB      /* puzzle tab hanging under a block; rect.y = the block's bottom */
} ccr_prim_kind;

typedef struct ccr_prim {
    ccr_prim_kind kind;
    ccr_rect  rect;        /* script-local coordinates */
    ccr_color fill;
    ccr_color edge;
    float     roundness;   /* BLOCK only, 0..1 */
    uint32_t  text;        /* TEXT only: offset into ccr_blocks.text */
    float     font;        /* TEXT only: size at zoom 1.0 */
    ccr_node_id node;      /* which AST node this came from */
    bool      notch;       /* BLOCK: the puzzle notch is cut out of its top */
} ccr_prim;

typedef enum ccr_snap_kind {
    CCR_SNAP_AFTER,        /* insert right after statement `ref` */
    CCR_SNAP_FIRST,        /* insert as the first statement of Block `ref` */
    CCR_SNAP_SLOT          /* an expression replaces the empty slot `ref` */
} ccr_snap_kind;

typedef struct ccr_snap {
    ccr_snap_kind kind;
    ccr_node_id   ref;
    ccr_vec2      at;      /* script-local: where the dropped stack's top-left goes */
    float         width;   /* for the drop indicator */
    float         height;  /* SLOT: the slot's height */
} ccr_snap;

typedef struct ccr_blocks {
    ccr_prim *prims;
    uint32_t  prim_count, prim_cap;

    char     *text;
    uint32_t  text_len, text_cap;

    ccr_snap *snaps;
    uint32_t  snap_count, snap_cap;

    /* every goto and label in the script, for the arrows between them */
    ccr_node_id *jumps;
    uint32_t     jump_count, jump_cap;

    ccr_rect  bounds;      /* script-local */
} ccr_blocks;

/*
 * Measured expression sizes, per node. Expressions never change shape when
 * statements are dragged around, so this survives between layouts.
 */
typedef struct ccr_layout_cache {
    float   *w, *h;
    uint32_t cap;
} ccr_layout_cache;

bool ccr_layout_cache_reserve(ccr_layout_cache *c, uint32_t node_count);
void ccr_layout_cache_free(ccr_layout_cache *c);

/*
 * Lays out the script rooted at `root`: a top-level declaration, or a
 * Block flagged CCR_FLAG_LOOSE (a stack; with CCR_FLAG_REPORTER, one lone
 * expression). Needs an open window: text is measured
 * with the GUI font.
 */
bool ccr_blocks_build_script(ccr_blocks *out, const ccr_ast *ast, ccr_node_id root,
                             ccr_layout_cache *cache);
void ccr_blocks_free(ccr_blocks *b);

/* Topmost node under a script-local point, or CCR_NONE. */
ccr_node_id ccr_blocks_hit(const ccr_blocks *b, ccr_vec2 local);

/* The word under a script-local point, if any: which node's line it is
 * part of, the text, and where. */
bool ccr_blocks_hit_text(const ccr_blocks *b, ccr_vec2 local, ccr_node_id *node,
                         const char **text, ccr_rect *rect);

/* Rectangle of the first primitive drawn for `node`, if any. */
bool ccr_blocks_find(const ccr_blocks *b, ccr_node_id node, ccr_rect *out);

/* The script's outline in one flat colour: a shadow under a lifted stack. */
void ccr_blocks_draw_shadow(const ccr_blocks *b, const ccr_canvas *cv, ccr_rect viewport,
                            ccr_vec2 offset, ccr_color color);

/* Draws the script at `offset` (world), alpha scaled by `alpha` (0..1). */
void ccr_blocks_draw(const ccr_blocks *b, const ccr_canvas *cv, ccr_rect viewport,
                     ccr_vec2 offset, float alpha);

#endif /* CCR_BLOCKS_H */
