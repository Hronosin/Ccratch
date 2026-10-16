#include "ccr_pie.h"
#include "ccr_lang.h"
#include "ccr_theme.h"

#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265f

static const ccr_text g_labels[CCR_ACTION_COUNT] = {
    [CCR_ACTION_SAVE]      = CCR_T_PIE_SAVE,
    [CCR_ACTION_COMMENT]   = CCR_T_PIE_COMMENT,
    [CCR_ACTION_REDO]      = CCR_T_PIE_REDO,
    [CCR_ACTION_EDIT_TEXT] = CCR_T_PIE_EDIT_TEXT,
    [CCR_ACTION_DELETE]    = CCR_T_PIE_DELETE,
    [CCR_ACTION_FIT_VIEW]  = CCR_T_PIE_FIT_VIEW,
    [CCR_ACTION_UNDO]      = CCR_T_PIE_UNDO,
    [CCR_ACTION_DUPLICATE] = CCR_T_PIE_DUPLICATE,
};

static const char *const g_shortcuts[CCR_ACTION_COUNT] = {
    [CCR_ACTION_SAVE]      = "Ctrl+S",
    [CCR_ACTION_COMMENT]   = "Ctrl+/",
    [CCR_ACTION_REDO]      = "Ctrl+Y",
    [CCR_ACTION_EDIT_TEXT] = NULL,   /* translated */
    [CCR_ACTION_DELETE]    = NULL,
    [CCR_ACTION_FIT_VIEW]  = "Home",
    [CCR_ACTION_UNDO]      = "Ctrl+Z",
    [CCR_ACTION_DUPLICATE] = "Ctrl+D",
};

const char *ccr_action_label(ccr_action a, bool uncomment)
{
    if (a == CCR_ACTION_COMMENT && uncomment)
        return ccr_tr(CCR_T_PIE_UNCOMMENT);
    return (a > CCR_ACTION_NONE && a < CCR_ACTION_COUNT) ? ccr_tr(g_labels[a]) : "";
}

const char *ccr_action_shortcut(ccr_action a)
{
    if (a == CCR_ACTION_EDIT_TEXT)
        return ccr_tr(CCR_T_PIE_CLICK_FIELD);
    if (a == CCR_ACTION_DELETE)
        return ccr_tr(CCR_T_PIE_KEY_DELETE);
    return (a > CCR_ACTION_NONE && a < CCR_ACTION_COUNT) ? g_shortcuts[a] : "";
}

void ccr_pie_open(ccr_pie *pie, ccr_vec2 center, const bool enabled[CCR_ACTION_COUNT],
                  bool uncomment)
{
    pie->open = true;
    pie->center = center;
    for (int i = 0; i < CCR_ACTION_COUNT; ++i)
        pie->enabled[i] = enabled[i];
    pie->uncomment = uncomment;
    pie->fold = 0;
    pie->hot = CCR_ACTION_NONE;
    pie->opened_at = ccr_gui_time();
}

void ccr_pie_close(ccr_pie *pie)
{
    pie->open = false;
    pie->hot = CCR_ACTION_NONE;
}

ccr_action ccr_pie_aim(const ccr_pie *pie, ccr_vec2 mouse)
{
    float dx = mouse.x - pie->center.x, dy = mouse.y - pie->center.y;
    if (dx * dx + dy * dy < CCR_PIE_DEAD_ZONE * CCR_PIE_DEAD_ZONE)
        return CCR_ACTION_NONE;
    /* compass angle: 0 up, clockwise */
    float deg = atan2f(dx, -dy) * 180.0f / PI_F;
    if (deg < 0.0f)
        deg += 360.0f;
    return (ccr_action)((int)((deg + 22.5f) / 45.0f) % CCR_PIE_DIRS);
}

ccr_action ccr_pie_update(ccr_pie *pie, ccr_vec2 mouse, bool click, bool cancel)
{
    if (!pie->open)
        return CCR_ACTION_NONE;
    pie->hot = ccr_pie_aim(pie, mouse);
    if (cancel) {
        ccr_pie_close(pie);
        return CCR_ACTION_NONE;
    }
    if (!click)
        return CCR_ACTION_NONE;
    ccr_action picked = pie->hot;
    if (picked != CCR_ACTION_NONE && !pie->enabled[picked])
        picked = CCR_ACTION_NONE;
    ccr_pie_close(pie);
    return picked;
}

/* ---------------------------------------------------------- drawing */

#define LABEL_SIZE    20.0f
#define HINT_SIZE     14.0f
#define BOX_H         30.0f
#define BOX_PAD       10.0f

#define COL_BOX       ccr_th(CCR_C_PANEL)
#define COL_BOX_EDGE  ccr_th(CCR_C_PANEL_EDGE)
#define COL_HOT       ccr_th(CCR_C_HOT)
#define COL_HOT_OFF   ccr_th(CCR_C_PANEL_EDGE)
#define COL_TEXT      ccr_th(CCR_C_PANEL_TEXT)
#define COL_HINT      ccr_th(CCR_C_PANEL_HINT)
#define COL_OFF       ccr_th(CCR_C_DISABLED)
#define COL_RING      ((ccr_th(CCR_C_PANEL) & 0xFFFFFF00u) | 0xD0u)

/* How far out each slice's box sits: further at the sides, so the boxes
 * clear each other, like Blender's. */
static float radius_of(int dir)
{
    switch (dir) {
    case CCR_PIE_N: case CCR_PIE_S: return 78.0f;
    case CCR_PIE_E: case CCR_PIE_W: return 100.0f;
    default:                        return 96.0f;
    }
}

#define OPEN_TIME     0.16f   /* seconds to pop out */
#define STAGGER       0.012f  /* each slice a little after the one before */

static ccr_rect box_of(const ccr_pie *pie, int dir, float w, float out)
{
    float a = (float)dir * 45.0f * PI_F / 180.0f;
    float sx = sinf(a), sy = -cosf(a);
    float r = radius_of(dir) * out;
    float px = pie->center.x + sx * r, py = pie->center.y + sy * r;
    ccr_rect b = { px - w * 0.5f, py - BOX_H * 0.5f, w, BOX_H };
    if (sx > 0.3f) b.x = px;              /* right side: grows away */
    else if (sx < -0.3f) b.x = px - w;    /* left side */
    if (dir == CCR_PIE_N) b.y = py - BOX_H;
    if (dir == CCR_PIE_S) b.y = py;
    return b;
}

/* 0..1 with a little overshoot past 1 on the way: things pop. */
static float ease_out_back(float k)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float u = k - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float clamp01(float k) { return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k; }

/* `c` with its alpha scaled by k */
static ccr_color faded(ccr_color c, float k)
{
    return (c & 0xFFFFFF00u) | (ccr_color)((float)(c & 0xFFu) * clamp01(k));
}

void ccr_pie_draw(const ccr_pie *pie)
{
    if (!pie->open)
        return;
    const ccr_vec2 c = pie->center;
    const float since = pie->animate ? (float)(ccr_gui_time() - pie->opened_at) : 1.0f;

    /* the dial in the middle: a ring with the aimed-at slice lit; it
     * opens like an iris */
    const float iris = ease_out_back(clamp01(since / OPEN_TIME));
    const float fade_in = clamp01(since / (OPEN_TIME * 0.6f));
    const float ro = CCR_PIE_DEAD_ZONE * iris;
    ccr_gui_ring(c, ro - 8.0f > 0.0f ? ro - 8.0f : 0.0f, ro, 0.0f, 360.0f * clamp01(since / OPEN_TIME),
                 faded(COL_RING, fade_in));
    if (pie->hot != CCR_ACTION_NONE) {
        float mid = (float)pie->hot * 45.0f - 90.0f;
        ccr_color col = pie->enabled[pie->hot] ? COL_HOT : COL_HOT_OFF;
        ccr_gui_ring(c, ro - 9.0f > 0.0f ? ro - 9.0f : 0.0f, ro + 1.0f,
                     mid - 22.5f, mid + 22.5f, faded(col, fade_in));
    }

    for (int d = 0; d < CCR_PIE_DIRS; ++d) {
        const ccr_action a = (ccr_action)d;
        /* slices fly out of the centre one after another, clockwise */
        const float k = clamp01((since - STAGGER * (float)d) / OPEN_TIME);
        if (k <= 0.0f)
            continue;
        const float out = 0.25f + 0.75f * ease_out_back(k);
        const float alpha = clamp01(k * 2.0f);

        const char *label = ccr_action_label(a, pie->uncomment);
        const char *hint = ccr_action_shortcut(a);
        if (a == CCR_ACTION_FIT_VIEW && pie->fold) {   /* over a block that folds */
            label = ccr_tr(pie->fold == 1 ? CCR_T_PIE_FOLD : CCR_T_PIE_UNFOLD);
            hint = ccr_tr(CCR_T_PIE_KEY_DOUBLE);
        }
        float lw = ccr_gui_text_width(label, LABEL_SIZE);
        float hw = ccr_gui_text_width(hint, HINT_SIZE);
        ccr_rect b = box_of(pie, d, lw + hw + BOX_PAD * 3.0f, out);

        const bool on = pie->enabled[a], hot = pie->hot == a;
        ccr_gui_rect_rounded((ccr_rect){ b.x - 1.0f, b.y - 1.0f, b.w + 2.0f, b.h + 2.0f }, 0.45f,
                             faded(hot && on ? COL_HOT : COL_BOX_EDGE, alpha));
        ccr_gui_rect_rounded(b, 0.45f, faded(hot && on ? COL_HOT : COL_BOX, alpha));
        ccr_gui_text(label, b.x + BOX_PAD, b.y + (BOX_H - LABEL_SIZE) * 0.5f, LABEL_SIZE,
                     faded(on ? (hot ? 0xFFFFFFFFu : COL_TEXT) : COL_OFF, alpha));
        ccr_gui_text(hint, b.x + b.w - BOX_PAD - hw, b.y + (BOX_H - HINT_SIZE) * 0.5f + 1.0f,
                     HINT_SIZE, faded(on ? (hot ? 0xFFFFFFFFu : COL_HINT) : COL_OFF, alpha));
    }
}
