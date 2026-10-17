/*
 * The palette: every template is on it, drawn as the block it makes; a
 * press on one says which, and where it was held; categories scroll.
 *
 * Needs a window (text is measured): run under xvfb.
 */
#include "ccr_gui.h"
#include "ccr_palette.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

/* How many built-in blocks a palette for `cmake` files shows. */
static uint32_t expected(bool cmake)
{
    uint32_t n = 0;
    for (int t = 0; t < CCR_TEMPLATE_COUNT; ++t)
        n += ccr_tcat_is_cmake(ccr_template_category((ccr_template)t)) == cmake;
    return n;
}

/* Every block is hit where it is drawn, held where it was pressed. */
static void check_hits(ccr_palette *pp)
{
    const ccr_palette *const p = pp;
    const ccr_rect area = { 0, 40, CCR_PALETTE_W, 100000 };   /* tall: no scrolling */
    for (uint32_t i = 0; i < p->item_count; ++i) {
        const ccr_palette_item *it = &p->items[i];
        const ccr_prim *first = &it->blocks.prims[0];
        ccr_vec2 local = { first->rect.x + 4.0f, first->rect.y + first->rect.h * 0.5f };
        ccr_vec2 m = { area.x + CCR_PALETTE_STRIP_W + (14.0f + local.x) * CCR_PALETTE_ZOOM,
                       area.y + (it->y + local.y) * CCR_PALETTE_ZOOM };
        ccr_vec2 grab;
        int hit = ccr_palette_hit(p, area, m, &grab);
        CHECK(hit == (int)it->t, "%s: a press on it gave %d", ccr_template_label(it->t), hit);
        if (hit == (int)it->t)
            CHECK(grab.x > local.x - 1.0f && grab.x < local.x + 1.0f &&
                  grab.y > local.y - 1.0f && grab.y < local.y + 1.0f,
                  "%s: held at %.1f,%.1f, pressed at %.1f,%.1f", ccr_template_label(it->t),
                  grab.x, grab.y, local.x, local.y);
        if (i > 0)
            CHECK(it->y > p->items[i - 1].y, "%s overlaps the block above", ccr_template_label(it->t));
    }
}

int main(void)
{
    if (!ccr_gui_init(1280, 800, "ccratch test")) {
        fprintf(stderr, "no window (run under xvfb-run)\n");
        return 2;
    }
    static ccr_palette p;
    CHECK(ccr_palette_init(&p), "palette init failed");
    CHECK(p.item_count == expected(false), "%u blocks on the palette, %u C templates",
          p.item_count, expected(false));
    check_hits(&p);
    const ccr_rect area = { 0, 40, CCR_PALETTE_W, 100000 };

    /* the strip is not a block */
    ccr_vec2 grab;
    CHECK(ccr_palette_hit(&p, area, (ccr_vec2){ 20.0f, 100.0f }, &grab) < 0, "the strip gave a block");

    /* a click on a category scrolls its heading to the top (a short window) */
    const ccr_rect small = { 0, 40, CCR_PALETTE_W, 500 };
    ccr_palette_update(&p, small, (ccr_vec2){ 40.0f, 40.0f + 6.0f + 3.5f * 60.0f }, 0.0f, true, 0.0f);
    CHECK(p.scroll > p.cats[CCR_TCAT_POINTERS].heading_y - 20.0f && p.scroll < p.cats[CCR_TCAT_POINTERS].heading_y,
          "Pointers: scrolled to %.0f, heading at %.0f", p.scroll, p.cats[CCR_TCAT_POINTERS].heading_y);
    /* the wheel goes back up, not past the top */
    for (int i = 0; i < 200; ++i)
        ccr_palette_update(&p, small, (ccr_vec2){ 200.0f, 200.0f }, 3.0f, false, 0.0f);
    CHECK(p.scroll == 0.0f, "scrolled past the top: %.0f", p.scroll);

    CHECK(ccr_palette_relayout(&p), "relayout failed");
    CHECK(p.item_count == expected(false), "relayout lost blocks");

    /* a CMake file on screen: CMake's blocks, and only those */
    CHECK(ccr_palette_set_lang(&p, CCR_LANG_CMAKE), "set_lang failed");
    CHECK(p.item_count == expected(true) && expected(true) > 20, "%u blocks for CMake, %u templates",
          p.item_count, expected(true));
    for (uint32_t i = 0; i < p.item_count; ++i)
        CHECK(ccr_tcat_is_cmake(ccr_template_category(p.items[i].t)), "%s on CMake's palette",
              ccr_template_label(p.items[i].t));
    check_hits(&p);
    CHECK(ccr_palette_set_lang(&p, CCR_LANG_C) && p.item_count == expected(false), "back to C");
    ccr_palette_free(&p);
    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("palette: all checks passed\n");
    return g_failures ? 1 : 0;
}
