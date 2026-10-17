#include "ccr_palette.h"
#include "ccr_canvas.h"
#include "ccr_gui.h"
#include "ccr_lang.h"
#include "ccr_theme.h"

#include <stdlib.h>
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
    [CCR_TCAT_CM_PROJECT] = CCR_T_CAT_CM_PROJECT,
    [CCR_TCAT_CM_TARGETS] = CCR_T_CAT_CM_TARGETS,
    [CCR_TCAT_CM_LIBS]    = CCR_T_LIBS_TITLE,
    [CCR_TCAT_CM_VARS]    = CCR_T_CAT_VARIABLES,
    [CCR_TCAT_CM_CONTROL] = CCR_T_CAT_CONTROL,
};

static const ccr_theme_cat g_cat_colors[CCR_TCAT_COUNT] = {
    [CCR_TCAT_CONTROL]   = CCR_CAT_CONTROL,
    [CCR_TCAT_OPERATORS] = CCR_CAT_OPERATOR,
    [CCR_TCAT_VARIABLES] = CCR_CAT_VARIABLE,
    [CCR_TCAT_POINTERS]  = CCR_CAT_POINTER,
    [CCR_TCAT_FUNCTIONS] = CCR_CAT_CALL,
    [CCR_TCAT_TYPES]     = CCR_CAT_TYPE,
    [CCR_TCAT_DECLS]     = CCR_CAT_EVENT,   /* the hat's colour: where code starts */
    [CCR_TCAT_CM_PROJECT] = CCR_CAT_TYPE,
    [CCR_TCAT_CM_TARGETS] = CCR_CAT_PREPROC,
    [CCR_TCAT_CM_LIBS]    = CCR_CAT_LIBRARY,
    [CCR_TCAT_CM_VARS]    = CCR_CAT_VARIABLE,
    [CCR_TCAT_CM_CONTROL] = CCR_CAT_CONTROL,
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

#define ZOOM(p) ((p)->zoom > 0.0f ? (p)->zoom : CCR_PALETTE_ZOOM)

/* The flyout as a little canvas: unscaled units, scrolled. */
static ccr_canvas fly_canvas(const ccr_palette *p, ccr_rect fly)
{
    return (ccr_canvas){ .offset = { fly.x, fly.y - p->scroll * ZOOM(p) },
                         .zoom = ZOOM(p) };
}

static float max_scroll(const ccr_palette *p, ccr_rect area)
{
    /* far enough that the last category's heading can reach the top */
    float m = p->content_h - area.h / ZOOM(p);
    const float last = p->cat_count ? p->cats[p->cat_count - 1].heading_y - PAD_TOP : 0.0f;
    if (m < last)
        m = last;
    return m > 0.0f ? m : 0.0f;
}

/* ------------------------------------------------------------ building */

static void free_items(ccr_palette *p)
{
    for (uint32_t i = 0; i < p->item_count; ++i)
        ccr_blocks_free(&p->items[i].blocks);
    free(p->items);
    p->items = NULL;
    p->item_count = p->item_cap = 0;
    ccr_layout_cache_free(&p->cache);
    ccr_ast_free(&p->ast);
}

/* Does the block say what was typed into the search (its words, or its
 * name: printf, a + b, InitWindow)? */
static bool item_matches(const ccr_palette *p, const ccr_palette_item *it)
{
    char q[96], w[512];
    ccr_fold_case(p->filter, q, sizeof q);
    ccr_fold_case(ccr_template_label(it->t), w, sizeof w);
    if (strstr(w, q))
        return true;
    const ccr_blocks *b = &it->blocks;
    for (uint32_t i = 0; i < b->prim_count; ++i)
        if (b->prims[i].kind == CCR_PRIM_TEXT) {
            ccr_fold_case(b->text + b->prims[i].text, w, sizeof w);
            if (strstr(w, q))
                return true;
        }
    return false;
}

/* Lays out template `t` under the last section's heading, at *y. */
static bool add_item(ccr_palette *p, ccr_template t, float *y)
{
    if (p->item_count == p->item_cap) {
        uint32_t cap = p->item_cap ? p->item_cap * 2 : 128;
        ccr_palette_item *v = realloc(p->items, cap * sizeof *v);
        if (!v)
            return false;
        p->items = v;
        p->item_cap = cap;
    }
    ccr_node_id root = ccr_ast_add(&p->ast, CCR_NODE_BLOCK, CCR_NONE);
    if (root == CCR_NONE)
        return false;
    p->ast.nodes[root].flags |= CCR_FLAG_LOOSE;
    if (ccr_template_is_reporter(t))
        p->ast.nodes[root].flags |= CCR_FLAG_REPORTER;
    if (ccr_template_is_toplevel(t))
        p->ast.nodes[root].flags |= CCR_FLAG_TOPLEVEL;
    ccr_node_id made = ccr_template_build(&p->ast, t, root);
    if (made == CCR_NONE)
        return false;
    if (t == CCR_TEMPLATE_CODE || t == CCR_TEMPLATE_CM_LINE)   /* an empty white bar says nothing: name it */
        p->ast.nodes[made].name = ccr_ast_intern(&p->ast, ccr_tr(CCR_T_TEMPLATE_CODE));

    ccr_palette_item *it = &p->items[p->item_count];
    memset(it, 0, sizeof *it);
    it->t = t;
    it->root = root;
    it->y = *y;
    if (!ccr_layout_cache_reserve(&p->cache, p->ast.node_count) ||
        !ccr_blocks_build_script(&it->blocks, &p->ast, root, &p->cache))
        return false;
    if (p->filter[0] && !item_matches(p, it)) {   /* searching: not this one */
        ccr_blocks_free(&it->blocks);
        return true;
    }
    ++p->item_count;
    /* the block's top may be above 0 (a notch), its bottom below bounds
     * (a tab): bounds covers both */
    *y += it->blocks.bounds.y + it->blocks.bounds.h + GAP;
    return true;
}

static bool pack_shown(const ccr_palette *p, const ccr_lib_pack *pk)
{
    if (!pk->header[0])
        return true;
    for (int i = 0; i < p->include_count; ++i)
        if (ccr_lib_pack_matches(pk, p->includes[i]))
            return true;
    return false;
}

static void heading(ccr_palette *p, const char *name, int color, float *y)
{
    if (p->cat_count == CCR_PALETTE_MAX_CATS)
        return;
    ccr_palette_cat *c = &p->cats[p->cat_count++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->color = color;
    c->heading_y = *y;
    *y += HEADING_H;
}

bool ccr_palette_init(ccr_palette *p)
{
    const float scroll = p->ready ? p->scroll : 0.0f;
    const ccr_lang_kind lang = p->lang;
    char filter[sizeof p->filter];
    memcpy(filter, p->filter, sizeof filter);
    const float zoom = p->zoom;
    /* the includes survive: they say which packs show */
    char (*keep)[128] = malloc(sizeof p->includes);
    const int keep_n = p->include_count;
    if (keep)
        memcpy(keep, p->includes, sizeof p->includes);
    memset(p, 0, sizeof *p);
    if (keep) {
        memcpy(p->includes, keep, sizeof p->includes);
        p->include_count = keep_n;
        free(keep);
    }
    p->hot = -1;
    p->lang = lang;
    memcpy(p->filter, filter, sizeof filter);
    p->zoom = zoom;
    if (!ccr_ast_init(&p->ast))
        return false;
    p->ast.lang = lang;   /* blocks drawn the way the file on screen speaks */

    const bool cmake = lang == CCR_LANG_CMAKE;
    float y = PAD_TOP;
    for (int c = 0; c < CCR_TCAT_COUNT; ++c) {
        if (ccr_tcat_is_cmake((ccr_tcat)c) != cmake)
            continue;
        const float head_y = y;
        const uint32_t had = p->item_count;
        heading(p, ccr_tr(g_cat_names[c]), g_cat_colors[c], &y);
        for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t)
            if (ccr_template_category((ccr_template)t) == (ccr_tcat)c && !add_item(p, (ccr_template)t, &y))
                goto oom;
        /* CMake: every pack that says how to link its library */
        for (int k = 0; c == CCR_TCAT_CM_LIBS && k < ccr_lib_pack_count(); ++k) {
            const ccr_lib_pack *pk = ccr_lib_pack_at(k);
            if (pk->find[0] && !add_item(p, ccr_template_pack_find(k), &y))
                goto oom;
            if (pk->link[0] && !add_item(p, ccr_template_pack_link(k), &y))
                goto oom;
        }
        if (p->filter[0] && p->item_count == had) {   /* searching: nothing here */
            --p->cat_count;
            y = head_y;
            continue;
        }
        y += CAT_GAP;
    }
    /* then a section per block pack the file can use */
    for (int k = 0; k < ccr_lib_pack_count() && !cmake; ++k) {
        const ccr_lib_pack *pk = ccr_lib_pack_at(k);
        /* searching: every pack, so a library's block can be found first */
        if ((!pack_shown(p, pk) && !p->filter[0]) || pk->count == 0 || p->cat_count == CCR_PALETTE_MAX_CATS)
            continue;
        const float head_y = y;
        const uint32_t had = p->item_count;
        heading(p, pk->name, pk->category, &y);
        for (int b = 0; b < pk->count; ++b)
            if (!add_item(p, CCR_TEMPLATE_LIB(pk->first + b), &y))
                goto oom;
        if (p->filter[0] && p->item_count == had) {
            --p->cat_count;
            y = head_y;
            continue;
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

void ccr_palette_set_zoom(ccr_palette *p, float zoom)
{
    if (zoom < CCR_PALETTE_ZOOM_MIN) zoom = CCR_PALETTE_ZOOM_MIN;
    if (zoom > CCR_PALETTE_ZOOM_MAX) zoom = CCR_PALETTE_ZOOM_MAX;
    p->zoom = zoom;   /* drawn at a scale: no layout needed */
}

bool ccr_palette_set_filter(ccr_palette *p, const char *text)
{
    if (strcmp(p->filter, text) == 0)
        return true;
    snprintf(p->filter, sizeof p->filter, "%s", text);
    p->scroll = p->scroll_to = 0.0f;   /* the first matches at the top */
    return !p->ready || ccr_palette_relayout(p);
}

bool ccr_palette_set_includes(ccr_palette *p, const char *const *includes, int count)
{
    if (count > CCR_PALETTE_MAX_INCLUDES)
        count = CCR_PALETTE_MAX_INCLUDES;
    /* would the shown packs change? */
    bool before[CCR_LIB_MAX_PACKS], changed = false;
    for (int k = 0; k < ccr_lib_pack_count() && k < CCR_LIB_MAX_PACKS; ++k)
        before[k] = pack_shown(p, ccr_lib_pack_at(k));
    p->include_count = count;
    for (int i = 0; i < count; ++i)
        snprintf(p->includes[i], sizeof p->includes[i], "%s", includes[i]);
    for (int k = 0; k < ccr_lib_pack_count() && k < CCR_LIB_MAX_PACKS; ++k)
        changed = changed || before[k] != pack_shown(p, ccr_lib_pack_at(k));
    return !changed || !p->ready || ccr_palette_relayout(p);
}

void ccr_palette_free(ccr_palette *p)
{
    free_items(p);
    p->ready = false;
}

bool ccr_palette_set_lang(ccr_palette *p, ccr_lang_kind lang)
{
    if (p->lang == lang)
        return true;
    p->lang = lang;
    p->scroll = p->scroll_to = 0.0f;
    return !p->ready || ccr_palette_relayout(p);
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
    for (int i = 0; i < p->cat_count; ++i)
        if (p->cats[i].heading_y <= p->scroll + PAD_TOP + 1.0f)
            c = i;
    return c;
}

/* Rows get shorter when there are many sections (packs). */
static ccr_rect strip_row(const ccr_palette *p, ccr_rect strip, int c)
{
    float h = STRIP_ROW_H;
    if (p->cat_count > 0 && (strip.h - 12.0f) / (float)p->cat_count < h)
        h = (strip.h - 12.0f) / (float)p->cat_count;
    return (ccr_rect){ strip.x, strip.y + 6.0f + (float)c * h, strip.w, h };
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
        for (int c = 0; c < p->cat_count; ++c)
            if (inside(mouse, strip_row(p, strip, c)))
                p->scroll_to = p->cats[c].heading_y - PAD_TOP;
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
    for (int c = 0; c < p->cat_count; ++c) {
        const ccr_rect row = strip_row(p, strip, c);
        if (c == now)
            ccr_gui_rect((ccr_rect){ row.x + 4.0f, row.y + 2.0f, row.w - 4.0f, row.h - 4.0f },
                         fly_color());   /* a tab into the list */
        const ccr_theme_cat tc = (ccr_theme_cat)p->cats[c].color;
        const float r = row.h < STRIP_ROW_H ? DOT_R * row.h / STRIP_ROW_H : DOT_R;
        const ccr_vec2 dot = { row.x + row.w * 0.5f, row.y + 8.0f * row.h / STRIP_ROW_H + r };
        ccr_gui_circle(dot, r + 1.5f, ccr_th_cat(tc, CCR_EDGE));
        ccr_gui_circle(dot, r, ccr_th_cat(tc, CCR_FILL));
        const char *name = p->cats[c].name;
        float size = LABEL_SIZE;
        while (size > 8.0f && ccr_gui_text_width(name, size) > row.w - 6.0f)
            size -= 0.5f;
        ccr_gui_text(name, row.x + (row.w - ccr_gui_text_width(name, size)) * 0.5f,
                     dot.y + r + 4.0f, size, ccr_th(CCR_C_PALETTE_TEXT));
    }

    /* the flyout: headings and real blocks */
    ccr_gui_clip_begin(fly);
    const ccr_canvas cv = fly_canvas(p, fly);
    for (int c = 0; c < p->cat_count; ++c) {
        const ccr_vec2 at = ccr_canvas_to_screen(&cv, (ccr_vec2){ PAD_X, p->cats[c].heading_y });
        ccr_gui_text(p->cats[c].name, at.x, at.y + 8.0f, HEADING_SIZE, ccr_th(CCR_C_PALETTE_TEXT));
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
        const float view = fly.h / (p->content_h * ZOOM(p));
        const float bar_h = fly.h * (view > 1.0f ? 1.0f : view);
        const float bar_y = fly.y + (fly.h - bar_h) * (p->scroll / top);
        ccr_gui_rect_rounded((ccr_rect){ fly.x + fly.w - 6.0f, bar_y + 2.0f, 4.0f, bar_h - 4.0f }, 1.0f,
                             (ccr_th(CCR_C_HINT) & 0xFFFFFF00u) | 0x90u);
    }
}
