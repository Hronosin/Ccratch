#include "ccr_pie.h"

#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265f

static const char *const g_labels[CCR_ACTION_COUNT] = {
    [CCR_ACTION_SAVE]      = "Save",
    [CCR_ACTION_COMMENT]   = "Comment out",
    [CCR_ACTION_REDO]      = "Redo",
    [CCR_ACTION_EDIT_TEXT] = "Edit text",
    [CCR_ACTION_DELETE]    = "Delete",
    [CCR_ACTION_FIT_VIEW]  = "Fit view",
    [CCR_ACTION_UNDO]      = "Undo",
    [CCR_ACTION_DUPLICATE] = "Duplicate",
};

static const char *const g_shortcuts[CCR_ACTION_COUNT] = {
    [CCR_ACTION_SAVE]      = "Ctrl+S",
    [CCR_ACTION_COMMENT]   = "Ctrl+/",
    [CCR_ACTION_REDO]      = "Ctrl+Y",
    [CCR_ACTION_EDIT_TEXT] = "click field",
    [CCR_ACTION_DELETE]    = "Del",
    [CCR_ACTION_FIT_VIEW]  = "Home",
    [CCR_ACTION_UNDO]      = "Ctrl+Z",
    [CCR_ACTION_DUPLICATE] = "Ctrl+D",
};

const char *ccr_action_label(ccr_action a, bool uncomment)
{
    if (a == CCR_ACTION_COMMENT && uncomment)
        return "Uncomment";
    return (a > CCR_ACTION_NONE && a < CCR_ACTION_COUNT) ? g_labels[a] : "";
}

const char *ccr_action_shortcut(ccr_action a)
{
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
    pie->hot = CCR_ACTION_NONE;
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

#define LABEL_SIZE    18.0f
#define HINT_SIZE     11.0f
#define BOX_H         30.0f
#define BOX_PAD       10.0f

#define COL_BOX       0x2A2E37F2u
#define COL_BOX_EDGE  0x454B57FFu
#define COL_HOT       0x4C7BD9FFu
#define COL_HOT_OFF   0x3A3F4BFFu
#define COL_TEXT      0xF2F2F2FFu
#define COL_HINT      0x9AA0ABFFu
#define COL_OFF       0x666C78FFu
#define COL_RING      0x1E2128D0u

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

static ccr_rect box_of(const ccr_pie *pie, int dir, float w)
{
    float a = (float)dir * 45.0f * PI_F / 180.0f;
    float sx = sinf(a), sy = -cosf(a);
    float r = radius_of(dir);
    float px = pie->center.x + sx * r, py = pie->center.y + sy * r;
    ccr_rect b = { px - w * 0.5f, py - BOX_H * 0.5f, w, BOX_H };
    if (sx > 0.3f) b.x = px;              /* right side: grows away */
    else if (sx < -0.3f) b.x = px - w;    /* left side */
    if (dir == CCR_PIE_N) b.y = py - BOX_H;
    if (dir == CCR_PIE_S) b.y = py;
    return b;
}

void ccr_pie_draw(const ccr_pie *pie)
{
    if (!pie->open)
        return;
    const ccr_vec2 c = pie->center;

    /* the dial in the middle: a ring with the aimed-at slice lit */
    ccr_gui_ring(c, CCR_PIE_DEAD_ZONE - 8.0f, CCR_PIE_DEAD_ZONE, 0.0f, 360.0f, COL_RING);
    if (pie->hot != CCR_ACTION_NONE) {
        float mid = (float)pie->hot * 45.0f - 90.0f;
        ccr_color col = pie->enabled[pie->hot] ? COL_HOT : COL_HOT_OFF;
        ccr_gui_ring(c, CCR_PIE_DEAD_ZONE - 9.0f, CCR_PIE_DEAD_ZONE + 1.0f,
                     mid - 22.5f, mid + 22.5f, col);
    }

    for (int d = 0; d < CCR_PIE_DIRS; ++d) {
        const ccr_action a = (ccr_action)d;
        const char *label = ccr_action_label(a, pie->uncomment);
        const char *hint = ccr_action_shortcut(a);
        float lw = ccr_gui_text_width(label, LABEL_SIZE);
        float hw = ccr_gui_text_width(hint, HINT_SIZE);
        ccr_rect b = box_of(pie, d, lw + hw + BOX_PAD * 3.0f);

        const bool on = pie->enabled[a], hot = pie->hot == a;
        ccr_gui_rect_rounded((ccr_rect){ b.x - 1.0f, b.y - 1.0f, b.w + 2.0f, b.h + 2.0f }, 0.45f,
                             hot && on ? COL_HOT : COL_BOX_EDGE);
        ccr_gui_rect_rounded(b, 0.45f, hot && on ? COL_HOT : COL_BOX);
        ccr_gui_text(label, b.x + BOX_PAD, b.y + (BOX_H - LABEL_SIZE) * 0.5f, LABEL_SIZE,
                     on ? COL_TEXT : COL_OFF);
        ccr_gui_text(hint, b.x + b.w - BOX_PAD - hw, b.y + (BOX_H - HINT_SIZE) * 0.5f + 1.0f,
                     HINT_SIZE, on ? (hot ? COL_TEXT : COL_HINT) : COL_OFF);
    }
}
