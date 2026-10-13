#include "ccr_canvas.h"

#include <math.h>

#define CCR_GRID_STEP   24.0f
#define CCR_ZOOM_MIN    0.25f
#define CCR_ZOOM_MAX    3.0f

void ccr_canvas_init(ccr_canvas *cv)
{
    cv->offset = (ccr_vec2){ 0.0f, 0.0f };
    cv->zoom   = 1.0f;
}

ccr_vec2 ccr_canvas_to_screen(const ccr_canvas *cv, ccr_vec2 p)
{
    return (ccr_vec2){ p.x * cv->zoom + cv->offset.x,
                       p.y * cv->zoom + cv->offset.y };
}

ccr_vec2 ccr_canvas_to_world(const ccr_canvas *cv, ccr_vec2 p)
{
    return (ccr_vec2){ (p.x - cv->offset.x) / cv->zoom,
                       (p.y - cv->offset.y) / cv->zoom };
}

static bool point_in_rect(ccr_vec2 p, ccr_rect r)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

void ccr_canvas_update(ccr_canvas *cv, ccr_rect viewport)
{
    ccr_vec2 mouse = ccr_gui_mouse();
    if (!point_in_rect(mouse, viewport))
        return;

    /* Pan: right or middle mouse drag */
    if (ccr_gui_mouse_down(CCR_MOUSE_RIGHT) || ccr_gui_mouse_down(CCR_MOUSE_MIDDLE)) {
        ccr_vec2 d = ccr_gui_mouse_delta();
        cv->offset.x += d.x;
        cv->offset.y += d.y;
    }

    /* Zoom around the cursor so the point under it stays put */
    float wheel = ccr_gui_wheel();
    if (wheel != 0.0f) {
        ccr_vec2 before = ccr_canvas_to_world(cv, mouse);
        float zoom = cv->zoom * powf(1.1f, wheel);
        if (zoom < CCR_ZOOM_MIN) zoom = CCR_ZOOM_MIN;
        if (zoom > CCR_ZOOM_MAX) zoom = CCR_ZOOM_MAX;
        cv->zoom = zoom;
        cv->offset.x = mouse.x - before.x * zoom;
        cv->offset.y = mouse.y - before.y * zoom;
    }
}

void ccr_canvas_draw_grid(const ccr_canvas *cv, ccr_rect viewport)
{
    float step = CCR_GRID_STEP * cv->zoom;
    if (step < 8.0f)
        return; /* too dense to be useful */

    float start_x = viewport.x + fmodf(cv->offset.x - viewport.x, step);
    float start_y = viewport.y + fmodf(cv->offset.y - viewport.y, step);
    if (start_x < viewport.x) start_x += step;
    if (start_y < viewport.y) start_y += step;

    const ccr_color dot = 0x3A3F4BFF;
    for (float y = start_y; y < viewport.y + viewport.h; y += step)
        for (float x = start_x; x < viewport.x + viewport.w; x += step)
            ccr_gui_rect((ccr_rect){ x - 1.0f, y - 1.0f, 2.0f, 2.0f }, dot);
}
