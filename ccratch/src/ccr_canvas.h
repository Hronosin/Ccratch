/*
 * The infinite canvas where blocks will live.
 * For now: a dot grid you can drag around and zoom.
 */
#ifndef CCR_CANVAS_H
#define CCR_CANVAS_H

#include "ccr_gui.h"

#define CCR_ZOOM_MIN    0.25f
#define CCR_ZOOM_MAX    3.0f

typedef struct ccr_canvas {
    ccr_vec2 offset;   /* screen position of canvas origin */
    float    zoom;     /* 1.0 = 100% */
} ccr_canvas;

void     ccr_canvas_init(ccr_canvas *cv);
void     ccr_canvas_update(ccr_canvas *cv, ccr_rect viewport);
void     ccr_canvas_draw_grid(const ccr_canvas *cv, ccr_rect viewport);
ccr_vec2 ccr_canvas_to_screen(const ccr_canvas *cv, ccr_vec2 p);
ccr_vec2 ccr_canvas_to_world(const ccr_canvas *cv, ccr_vec2 p);

#endif /* CCR_CANVAS_H */
