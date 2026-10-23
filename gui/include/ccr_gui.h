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

typedef enum ccr_key {
    CCR_KEY_HOME,
    CCR_KEY_S,
    CCR_KEY_Y,
    CCR_KEY_Z,
    CCR_KEY_ESCAPE,
    CCR_KEY_F5,
    CCR_KEY_D,
    CCR_KEY_DELETE,
    CCR_KEY_SLASH,
    CCR_KEY_P,
    CCR_KEY_W,
    CCR_KEY_TAB,
    CCR_KEY_B,
    CCR_KEY_R,
    CCR_KEY_F,
    CCR_KEY_F1,
    CCR_KEY_F3,
    CCR_KEY_ENTER,
    CCR_KEY_EQUAL,     /* = and the keypad's + */
    CCR_KEY_MINUS,     /* - and the keypad's - */
    CCR_KEY_ZERO,
    CCR_KEY_C,
    CCR_KEY_V,
    CCR_KEY_A,
    CCR_KEY_M,
    CCR_KEY_UP,        /* held: repeats */
    CCR_KEY_DOWN,
    CCR_KEY_F2,
    CCR_KEY_F12,
    CCR_KEY_LEFT,      /* held: repeats */
    CCR_KEY_RIGHT,
    CCR_KEY_F8,
    CCR_KEY_F9,
    CCR_KEY_F10,
    CCR_KEY_F11,
    CCR_KEY_G,
    CCR_KEY_K,
    CCR_KEY_COMMA
} ccr_key;

/* Window and frame lifecycle */
bool  ccr_gui_init(int width, int height, const char *title);
void  ccr_gui_shutdown(void);
bool  ccr_gui_should_close(void);
void  ccr_gui_begin_frame(ccr_color clear);
void  ccr_gui_end_frame(void);
bool  ccr_gui_screenshot(const char *path);

/* Text from now on in this TrueType font (UTF-8), rasterized at base_size
 * for the given characters. Until then: raylib's ASCII pixel font. */
bool  ccr_gui_load_font(const unsigned char *ttf, unsigned int size, float base_size,
                        const int *codepoints, int count);

/* Some characters from one font file, some from another: Latin from one,
 * Japanese from the next... drawn as one font. */
typedef struct ccr_font_part {
    const unsigned char *ttf;
    unsigned int         size;
    const int           *codepoints;
    int                  count;
} ccr_font_part;
bool  ccr_gui_load_fonts(const ccr_font_part *parts, int n, float base_size);

/* Window icon from an in-memory PNG. Returns false if it could not decode. */
bool  ccr_gui_set_icon_png(const unsigned char *png, unsigned int size);

/* Interface size. Everything (sizes, the mouse, ccr_gui_width) is in units
 * of `scale` pixels: 1.5 draws all half as big again, for dense screens.
 * ccr_gui_auto_scale: the system's scaling, else from the screen's pixels
 * an inch (Ccratch was drawn for 27" 2560x1440), in quarters. */
void  ccr_gui_set_scale(float scale);
float ccr_gui_scale(void);
float ccr_gui_auto_scale(void);
/* The window w x h units big (so bigger with the scale), but never over
 * most of the screen; centred on it. */
void  ccr_gui_window_fit(int w, int h);

/* Queries */
int      ccr_gui_width(void);
int      ccr_gui_height(void);
double   ccr_gui_time(void);
ccr_vec2 ccr_gui_mouse(void);
ccr_vec2 ccr_gui_mouse_delta(void);
float    ccr_gui_wheel(void);
bool     ccr_gui_mouse_down(ccr_mouse_button button);
bool     ccr_gui_mouse_pressed(ccr_mouse_button button);   /* went down this frame */
bool     ccr_gui_mouse_released(ccr_mouse_button button);  /* went up this frame */
bool     ccr_gui_key_pressed(ccr_key key);
bool     ccr_gui_ctrl_down(void);
bool     ccr_gui_shift_down(void);
bool     ccr_gui_alt_down(void);

/* If a file was dropped onto the window this frame, copies its path into
 * `out` and returns true. Only the first file of a drop is reported. */
bool     ccr_gui_file_dropped(char *out, unsigned int cap);
/* The system clipboard, as text (NULL / "" if it holds none). */
void        ccr_gui_set_clipboard(const char *text);
const char *ccr_gui_clipboard(void);
/* The mouse pointer's shape, for edges that can be dragged. */
typedef enum ccr_cursor { CCR_CURSOR_DEFAULT, CCR_CURSOR_EW, CCR_CURSOR_NS, CCR_CURSOR_HAND } ccr_cursor;
void     ccr_gui_set_cursor(ccr_cursor c);
/* The window's close button was pressed but there is unsaved work: keep
 * the window open (ccr_gui_should_close is false again next frame). */
void     ccr_gui_cancel_close(void);

/* Drawing. Text sizes are floats so the canvas can zoom smoothly. */
void  ccr_gui_rect(ccr_rect r, ccr_color c);
void  ccr_gui_rect_rounded(ccr_rect r, float roundness, ccr_color c);
/* A rectangle with only some corners rounded (radius in pixels), built
 * from pieces that do not overlap: safe to draw see-through. */
#define CCR_CORNER_TL 1u
#define CCR_CORNER_TR 2u
#define CCR_CORNER_BR 4u
#define CCR_CORNER_BL 8u
void  ccr_gui_rect_corners(ccr_rect r, float radius, unsigned corners, ccr_color c);
void  ccr_gui_hex(ccr_rect r, ccr_color c);   /* Scratch-style boolean shape */
void  ccr_gui_circle(ccr_vec2 center, float radius, ccr_color c);
void  ccr_gui_line(ccr_vec2 a, ccr_vec2 b, float thick, ccr_color c);
void  ccr_gui_triangle(ccr_vec2 a, ccr_vec2 b, ccr_vec2 c, ccr_color col);   /* any winding */
/* Part of a ring; angles in degrees, 0 = right, clockwise (y is down). */
void  ccr_gui_ring(ccr_vec2 center, float inner, float outer,
                   float from_deg, float to_deg, ccr_color c);
void  ccr_gui_text(const char *text, float x, float y, float size, ccr_color c);
/* Drawing outside `r` is cut off until ccr_gui_clip_end. */
void  ccr_gui_clip_begin(ccr_rect r);
void  ccr_gui_clip_end(void);
float ccr_gui_text_width(const char *text, float size);

/* Widgets */
typedef struct ccr_button_style {
    ccr_color fill, hover, press, edge, text;
} ccr_button_style;

/* True on the frame a click (press and release over it) lands. */
bool  ccr_gui_button(ccr_rect r, const char *label, const ccr_button_style *st);

/* A text box being edited. Returns true when editing ends (Enter, or a
 * click outside it); `buf` holds the text. Every character typed during a
 * frame is taken, so fast typing never loses letters. Call
 * ccr_gui_textbox_reset() when a new edit starts (cursor goes to the end). */
bool  ccr_gui_textbox(ccr_rect r, char *buf, int cap, float font_size);
/* Colours of the text box from now on. */
void  ccr_gui_textbox_colors(ccr_color edge, ccr_color fill, ccr_color text);
void  ccr_gui_textbox_reset(void);
/* The next textbox starts with all of its text selected: typing replaces it. */
void  ccr_gui_textbox_select_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CCR_GUI_H */
