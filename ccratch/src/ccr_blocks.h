/*
 * ccr_blocks - turns a ccr_ast into Scratch-style blocks.
 *
 * Layout happens once, after a file is loaded: every block, reporter and
 * label becomes a primitive in one flat display list, in world
 * coordinates. Drawing a frame is then just a walk over that list with
 * the canvas transform applied and off-screen primitives culled.
 *
 * Read-only for now: blocks cannot be dragged yet.
 */
#ifndef CCR_BLOCKS_H
#define CCR_BLOCKS_H

#include "ccr_ast.h"
#include "ccr_canvas.h"
#include "ccr_gui.h"

typedef enum ccr_prim_kind {
    CCR_PRIM_BLOCK,   /* rounded rectangle with an outline */
    CCR_PRIM_HEX,     /* boolean reporter */
    CCR_PRIM_TEXT
} ccr_prim_kind;

typedef struct ccr_prim {
    ccr_prim_kind kind;
    ccr_rect  rect;        /* world coordinates */
    ccr_color fill;
    ccr_color edge;
    float     roundness;   /* BLOCK only, 0..1 */
    uint32_t  text;        /* TEXT only: offset into ccr_blocks.text */
    float     font;        /* TEXT only: size at zoom 1.0 */
    ccr_node_id node;      /* which AST node this came from */
} ccr_prim;

typedef struct ccr_blocks {
    ccr_prim *prims;
    uint32_t  prim_count, prim_cap;

    char     *text;
    uint32_t  text_len, text_cap;

    ccr_rect  bounds;      /* everything, world coordinates */
} ccr_blocks;

/* Needs an open window: text is measured with the GUI font. */
bool ccr_blocks_build(ccr_blocks *out, const ccr_ast *ast);
void ccr_blocks_free(ccr_blocks *b);
void ccr_blocks_draw(const ccr_blocks *b, const ccr_canvas *cv, ccr_rect viewport);

#endif /* CCR_BLOCKS_H */
