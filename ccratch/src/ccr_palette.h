/*
 * ccr_palette - the block palette, Scratch-style.
 *
 * A strip of categories (coloured dots) on the left, and next to it every
 * block the editor can make, drawn as the real thing: each palette block
 * is a template built into the palette's own syntax tree and laid out by
 * ccr_blocks, so what you see is exactly what you will drag out. Scrolls
 * with the wheel; a click on a category glides to it.
 */
#ifndef CCR_PALETTE_H
#define CCR_PALETTE_H

#include "ccr_ast.h"
#include "ccr_blocks.h"
#include "ccr_lib.h"
#include "ccr_template.h"

#define CCR_PALETTE_STRIP_W 80.0f    /* the category strip */
#define CCR_PALETTE_FLY_W   256.0f   /* the blocks */
#define CCR_PALETTE_W       (CCR_PALETTE_STRIP_W + CCR_PALETTE_FLY_W)
#define CCR_PALETTE_ZOOM    0.78f    /* blocks are drawn a bit smaller here */

typedef struct ccr_palette_item {
    ccr_template t;
    ccr_node_id  root;
    ccr_blocks   blocks;
    float        y;          /* top, in the flyout's (unscaled) units */
} ccr_palette_item;

#define CCR_PALETTE_MAX_CATS (CCR_TCAT_COUNT + CCR_LIB_MAX_PACKS)
#define CCR_PALETTE_MAX_INCLUDES 64

/* A section of the palette: built in, or a block pack's. */
typedef struct ccr_palette_cat {
    char  name[64];
    int   color;              /* a ccr_theme_cat */
    float heading_y;
} ccr_palette_cat;

typedef struct ccr_palette {
    ccr_ast           ast;
    ccr_layout_cache  cache;
    ccr_palette_item *items;
    uint32_t          item_count, item_cap;
    ccr_palette_cat   cats[CCR_PALETTE_MAX_CATS];
    int               cat_count;
    float             content_h;
    /* what the open file includes: which packs show */
    char              includes[CCR_PALETTE_MAX_INCLUDES][128];
    int               include_count;
    float             scroll, scroll_to;   /* unscaled units from the top */
    int               hot;                 /* item under the mouse, or -1 */
    bool              ready;
} ccr_palette;

/* Builds the blocks. Needs the window (text is measured). */
bool ccr_palette_init(ccr_palette *p);
void ccr_palette_free(ccr_palette *p);
/* After a theme or language switch: colours and widths change. */
bool ccr_palette_relayout(ccr_palette *p);
/* The headers the file on screen includes: packs for them show (packs
 * that name no header always do). Lays out again if that changes. */
bool ccr_palette_set_includes(ccr_palette *p, const char *const *includes, int count);

/* Wheel scrolling, category clicks, the glide. `area` is the whole palette
 * (strip and flyout), screen coordinates. */
void ccr_palette_update(ccr_palette *p, ccr_rect area, ccr_vec2 mouse, float wheel,
                        bool click, float dt);

/* The template under the mouse, or -1; `grab` gets where in the block it
 * was hit, in block units (what a drag out of the palette holds it by). */
int  ccr_palette_hit(const ccr_palette *p, ccr_rect area, ccr_vec2 mouse, ccr_vec2 *grab);

/* `trash`: a block is being dragged over it (dropping it deletes it). */
void ccr_palette_draw(const ccr_palette *p, ccr_rect area, bool trash);

#endif /* CCR_PALETTE_H */
