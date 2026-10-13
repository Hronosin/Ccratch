/*
 * ccr_gui - the only place in Ccratch that knows raylib exists.
 *
 * The IDE talks to this API and nothing else, so the backend can be
 * swapped (Nuklear, cimgui, hand-rolled Vulkan out of spite) without
 * touching the rest of the code.
 */
#ifndef CCR_GUI_H
#define CCR_GUI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Colors are 0xRRGGBBAA. */
typedef uint32_t ccr_color;

typedef struct ccr_vec2 { float x, y; } ccr_vec2;
typedef struct ccr_rect { float x, y, w, h; } ccr_rect;

typedef enum ccr_mouse_button {
    CCR_MOUSE_LEFT,
    CCR_MOUSE_RIGHT,
    CCR_MOUSE_MIDDLE
} ccr_mouse_button;

/* Window and frame lifecycle */
bool  ccr_gui_init(int width, int height, const char *title);
void  ccr_gui_shutdown(void);
bool  ccr_gui_should_close(void);
void  ccr_gui_begin_frame(ccr_color clear);
void  ccr_gui_end_frame(void);
bool  ccr_gui_screenshot(const char *path);

/* Window icon from an in-memory PNG. Returns false if it could not decode. */
bool  ccr_gui_set_icon_png(const unsigned char *png, unsigned int size);

/* Queries */
int      ccr_gui_width(void);
int      ccr_gui_height(void);
double   ccr_gui_time(void);
ccr_vec2 ccr_gui_mouse(void);
ccr_vec2 ccr_gui_mouse_delta(void);
float    ccr_gui_wheel(void);
bool     ccr_gui_mouse_down(ccr_mouse_button button);

/* Drawing */
void  ccr_gui_rect(ccr_rect r, ccr_color c);
void  ccr_gui_rect_rounded(ccr_rect r, float roundness, ccr_color c);
void  ccr_gui_circle(ccr_vec2 center, float radius, ccr_color c);
void  ccr_gui_text(const char *text, float x, float y, int size, ccr_color c);
float ccr_gui_text_width(const char *text, int size);

/* Widgets */
bool  ccr_gui_button(ccr_rect r, const char *label);

#ifdef __cplusplus
}
#endif

#endif /* CCR_GUI_H */
