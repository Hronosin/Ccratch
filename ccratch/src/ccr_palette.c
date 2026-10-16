#include "ccr_palette.h"
#include "ccr_canvas.h"
#include "ccr_gui.h"
#include "ccr_lang.h"
#include "ccr_theme.h"

#include <string.h>

#define PAD_X        14.0f   /* flyout units (scaled by CCR_PALETTE_ZOOM) */
#define PAD_TOP      10.0f
#define HEADING_H    40.0f
#define GAP          12.0f
#define CAT_GAP      18.0f   /* after a category's last block */
#define HEADING_SIZE 17.0f   /* screen pixels */

#define DOT_R        11.0f
#define STRIP_ROW_H  60.0f
#define LABEL_SIZE   12.0f

static const ccr_text g_cat_names[CCR_TCAT_COUNT] = {
    [CCR_TCAT_CONTROL]   = CCR_T_CAT_CONTROL,
    [CCR_TCAT_OPERATORS] = CCR_T_CAT_OPERATORS,
    [CCR_TCAT_VARIABLES] = CCR_T_CAT_VARIABLES,
    [CCR_TCAT_POINTERS]  = CCR_T_CAT_POINTERS,
    [CCR_TCAT_FUNCTIONS] = CCR_T_CAT_FUNCTIONS,
    [CCR_TCAT_TYPES]     = CCR_T_CAT_TYPES,
    [CCR_TCAT_DECLS]     = CCR_T_CAT_DECLS,
};

static const ccr_theme_cat g_cat_colors[CCR_TCAT_COUNT] = {
    [CCR_TCAT_CONTROL]   = CCR_CAT_CONTROL,
    [CCR_TCAT_OPERATORS] = CCR_CAT_OPERATOR,
    [CCR_TCAT_VARIABLES] = CCR_CAT_VARIABLE,
    [CCR_TCAT_POINTERS]  = CCR_CAT_POINTER,
    [CCR_TCAT_FUNCTIONS] = CCR_CAT_CALL,
    [CCR_TCAT_TYPES]     = CCR_CAT_TYPE,
    [CCR_TCAT_DECLS]     = CCR_CAT_EVENT,   /* the hat's colour: where code starts */
};

/* Halfway between two colours (the flyout: between palette and canvas). */
static ccr_color mix(ccr_color a, ccr_color b)
{
    ccr_color out = 0xFFu;
    for (int sh = 8; sh < 32; sh += 8)
        out |= ((((a >> sh) & 0xFFu) + ((b >> sh) & 0xFFu)) / 2u) << sh;
    return out;
}

static ccr_color fly_color(void) { return mix(ccr_th(CCR_C_PALETTE), ccr_th(CCR_C_BG)); }

static ccr_rect strip_of(ccr_rect area)
{
    return (ccr_rect){ area.x, area.y, CCR_PALETTE_STRIP_W, area.h };
}

static ccr_rect fly_of(ccr_rect area)
{
    return (ccr_rect){ area.x + CCR_PALETTE_STRIP_W, area.y, area.w - CCR_PALETTE_STRIP_W, area.h };
}

/* The flyout as a little canvas: unscaled units, scrolled. */
static ccr_canvas fly_canvas(const ccr_palette *p, ccr_rect fly)
{
    return (ccr_canvas){ .offset = { fly.x, fly.y - p->scroll * CCR_PALETTE_ZOOM },
                         .zoom = CCR_PALETTE_ZOOM };
}

static float max_scroll(const ccr_palette *p, ccr_rect area)
{
    /* far enough that the last category's heading can reach the top */
    float m = p->content_h - area.h / CCR_PALETTE_ZOOM;
    const float last = p->heading_y[CCR_TCAT_COUNT - 1] - PAD_TOP;
    if (m < last)
        m = last;
    return m > 0.0f ? m : 0.0f;
}

/* ------------------------------------------------------------ building */

static void free_items(ccr_palette *p)
{
    for (uint32_t i = 0; i < p->item_count; ++i)
        ccr_blocks_free(&p->items[i].blocks);
    p->item_count = 0;
    ccr_layout_cache_free(&p->cache);
    ccr_ast_free(&p->ast);
}

bool ccr_palette_init(ccr_palette *p)
{
    const float scroll = p->ready ? p->scroll : 0.0f;
    memset(p, 0, sizeof *p);
    p->hot = -1;
    if (!ccr_ast_init(&p->ast))
        return false;

    float y = PAD_TOP;
    for (int c = 0; c < CCR_TCAT_COUNT; ++c) {
        p->heading_y[c] = y;
        y += HEADING_H;
        for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t) {
            if (ccr_template_category((ccr_template)t) != (ccr_tcat)c)
                continue;
            ccr_node_id root = ccr_ast_add(&p->ast, CCR_NODE_BLOCK, CCR_NONE);
            if (root == CCR_NONE)
                goto oom;
            p->ast.nodes[root].flags |= CCR_FLAG_LOOSE;
            if (ccr_template_is_reporter((ccr_template)t))
                p->ast.nodes[root].flags |= CCR_FLAG_REPORTER;
            if (ccr_template_is_toplevel((ccr_template)t))
                p->ast.nodes[root].flags |= CCR_FLAG_TOPLEVEL;
            ccr_node_id made = ccr_template_build(&p->ast, (ccr_template)t, root);
            if (made == CCR_NONE)
                goto oom;
            if (t == CCR_TEMPLATE_CODE)   /* an empty white bar says nothing: name it */
                p->ast.nodes[made].name = ccr_ast_intern(&p->ast, ccr_tr(CCR_T_TEMPLATE_CODE));

            ccr_palette_item *it = &p->items[p->item_count];
            it->t = (ccr_template)t;
            it->root = root;
            it->y = y;
            if (!ccr_layout_cache_reserve(&p->cache, p->ast.node_count) ||
                !ccr_blocks_build_script(&it->blocks, &p->ast, root, &p->cache))
                goto oom;
            ++p->item_count;
            /* the block's top may be above 0 (a notch), its bottom below
             * bounds (a tab): bounds covers both */
            y += it->blocks.bounds.y + it->blocks.bounds.h + GAP;
        }
        y += CAT_GAP;
    }
    p->content_h = y;
    p->scroll = p->scroll_to = scroll;
    p->ready = true;
    return true;
oom:
    free_items(p);
    return false;
}

void ccr_palette_free(ccr_palette *p)
{
    free_items(p);
    p->ready = false;
}

bool ccr_palette_relayout(ccr_palette *p)
{
    const float scroll = p->scroll, to = p->scroll_to;
    free_items(p);
    p->ready = true;   /* keep the scroll */
    p->scroll = scroll;
    bool ok = ccr_palette_init(p);
    p->scroll_to = to;
    return ok;
}

/* ------------------------------------------------------------ input */

/* The category the flyout is showing now: the last heading above its top. */
static int current_cat(const ccr_palette *p)
{
    int c = 0;
    for (int i = 0; i < CCR_TCAT_COUNT; ++i)
        if (p->heading_y[i] <= p->scroll + PAD_TOP + 1.0f)
            c = i;
    return c;
}

static ccr_rect strip_row(ccr_rect strip, int c)
{
    return (ccr_rect){ strip.x, strip.y + 6.0f + (float)c * STRIP_ROW_H, strip.w, STRIP_ROW_H };
}

static bool inside(ccr_vec2 m, ccr_rect r)
{
    return m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}

void ccr_palette_update(ccr_palette *p, ccr_rect area, ccr_vec2 mouse, float wheel, bool click,
                        float dt)
{
    if (!p->ready)
        return;
    const ccr_rect strip = strip_of(area), fly = fly_of(area);
    if (inside(mouse, area) && wheel != 0.0f)
        p->scroll_to -= wheel * 70.0f;
    if (click && inside(mouse, strip))
        for (int c = 0; c < CCR_TCAT_COUNT; ++c)
            if (inside(mouse, strip_row(strip, c)))
                p->scroll_to = p->heading_y[c] - PAD_TOP;
    const float top = max_scroll(p, area);
    if (p->scroll_to < 0.0f) p->scroll_to = 0.0f;
    if (p->scroll_to > top)  p->scroll_to = top;

    /* glide there (or jump, with no time to glide in) */
    if (dt > 0.0f) {
        float k = dt * 14.0f;
        if (k > 1.0f) k = 1.0f;
        p->scroll += (p->scroll_to - p->scroll) * k;
        if (p->scroll - p->scroll_to < 0.5f && p->scroll_to - p->scroll < 0.5f)
            p->scroll = p->scroll_to;
    } else {
        p->scroll = p->scroll_to;
    }

    ccr_vec2 grab;
    p->hot = inside(mouse, fly) ? ccr_palette_hit(p, area, mouse, &grab) : -1;
}

int ccr_palette_hit(const ccr_palette *p, ccr_rect area, ccr_vec2 mouse, ccr_vec2 *grab)
{
    const ccr_rect fly = fly_of(area);
    if (!p->ready || !inside(mouse, fly))
        return -1;
    const ccr_canvas cv = fly_canvas(p, fly);
    const ccr_vec2 w = ccr_canvas_to_world(&cv, mouse);
    for (uint32_t i = 0; i < p->item_count; ++i) {
        const ccr_palette_item *it = &p->items[i];
        const ccr_vec2 local = { w.x - PAD_X, w.y - it->y };
        if (ccr_blocks_hit(&it->blocks, local) != CCR_NONE) {
            *grab = local;
            return (int)it->t;
        }
    }
    return -1;
}

/* ------------------------------------------------------------ drawing */

void ccr_palette_draw(const ccr_palette *p, ccr_rect area, bool trash)
{
    const ccr_rect strip = strip_of(area), fly = fly_of(area);
    ccr_gui_rect(fly, fly_color());
    ccr_gui_rect(strip, ccr_th(CCR_C_PALETTE));
    ccr_gui_rect((ccr_rect){ strip.x + strip.w - 1.0f, strip.y, 1.0f, strip.h }, ccr_th(CCR_C_PANEL_EDGE));
    if (!p->ready)
        return;

    /* the strip: a dot per category, its name under it */
    const int now = current_cat(p);
    for (int c = 0; c < CCR_TCAT_COUNT; ++c) {
        const ccr_rect row = strip_row(strip, c);
        if (c == now)
            ccr_gui_rect((ccr_rect){ row.x + 4.0f, row.y + 2.0f, row.w - 4.0f, row.h - 4.0f },
                         fly_color());   /* a tab into the list */
        const ccr_theme_cat tc = g_cat_colors[c];
        const ccr_vec2 dot = { row.x + row.w * 0.5f, row.y + 8.0f + DOT_R };
        ccr_gui_circle(dot, DOT_R + 1.5f, ccr_th_cat(tc, CCR_EDGE));
        ccr_gui_circle(dot, DOT_R, ccr_th_cat(tc, CCR_FILL));
        const char *name = ccr_tr(g_cat_names[c]);
        float size = LABEL_SIZE;
        while (size > 8.0f && ccr_gui_text_width(name, size) > row.w - 6.0f)
            size -= 0.5f;
        ccr_gui_text(name, row.x + (row.w - ccr_gui_text_width(name, size)) * 0.5f,
                     dot.y + DOT_R + 5.0f, size, ccr_th(CCR_C_PALETTE_TEXT));
    }

    /* the flyout: headings and real blocks */
    ccr_gui_clip_begin(fly);
    const ccr_canvas cv = fly_canvas(p, fly);
    for (int c = 0; c < CCR_TCAT_COUNT; ++c) {
        const ccr_vec2 at = ccr_canvas_to_screen(&cv, (ccr_vec2){ PAD_X, p->heading_y[c] });
        ccr_gui_text(ccr_tr(g_cat_names[c]), at.x, at.y + 8.0f, HEADING_SIZE,
                     ccr_th(CCR_C_PALETTE_TEXT));
    }
    for (uint32_t i = 0; i < p->item_count; ++i) {
        const ccr_palette_item *it = &p->items[i];
        ccr_blocks_draw(&it->blocks, &cv, fly, (ccr_vec2){ PAD_X, it->y }, 1.0f);
    }
    if (trash) {
        /* everything under a red veil: let go and it is gone */
        ccr_gui_rect(fly, (ccr_th(CCR_C_TRASH) & 0xFFFFFF00u) | 0xE0u);
        const char *msg = ccr_tr(CCR_T_PALETTE_DROP);
        const float tw = ccr_gui_text_width(msg, 20.0f);
        ccr_gui_text(msg, fly.x + (fly.w - tw) * 0.5f, fly.y + fly.h * 0.45f, 20.0f,
                     ccr_th(CCR_C_ERROR));
    }
    ccr_gui_clip_end();

    /* a thin scroll bar, when there is more than fits */
    const float top = max_scroll(p, area);
    if (top > 0.0f) {
        const float view = fly.h / (p->content_h * CCR_PALETTE_ZOOM);
        const float bar_h = fly.h * (view > 1.0f ? 1.0f : view);
        const float bar_y = fly.y + (fly.h - bar_h) * (p->scroll / top);
        ccr_gui_rect_rounded((ccr_rect){ fly.x + fly.w - 6.0f, bar_y + 2.0f, 4.0f, bar_h - 4.0f }, 1.0f,
                             (ccr_th(CCR_C_HINT) & 0xFFFFFF00u) | 0x90u);
    }
}
