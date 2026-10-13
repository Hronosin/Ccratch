#include "ccr_gui.h"

#include <raylib.h>
#include <rlgl.h>

#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

static Color to_rl(ccr_color c)
{
    return (Color){
        (unsigned char)(c >> 24),
        (unsigned char)(c >> 16),
        (unsigned char)(c >> 8),
        (unsigned char)(c)
    };
}

static Rectangle rect_to_rl(ccr_rect r)
{
    return (Rectangle){ r.x, r.y, r.w, r.h };
}

static int button_to_rl(ccr_mouse_button b)
{
    switch (b) {
    case CCR_MOUSE_RIGHT:  return MOUSE_BUTTON_RIGHT;
    case CCR_MOUSE_MIDDLE: return MOUSE_BUTTON_MIDDLE;
    case CCR_MOUSE_LEFT:
    default:               return MOUSE_BUTTON_LEFT;
    }
}

bool ccr_gui_init(int width, int height, const char *title)
{
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(width, height, title);
    if (!IsWindowReady())
        return false;
    SetTargetFPS(60);
    return true;
}

void ccr_gui_shutdown(void)         { CloseWindow(); }
bool ccr_gui_should_close(void)     { return WindowShouldClose(); }
void ccr_gui_end_frame(void)        { EndDrawing(); }
int  ccr_gui_width(void)            { return GetScreenWidth(); }
int  ccr_gui_height(void)           { return GetScreenHeight(); }
double ccr_gui_time(void)           { return GetTime(); }
float ccr_gui_wheel(void)           { return GetMouseWheelMove(); }

void ccr_gui_begin_frame(ccr_color clear)
{
    BeginDrawing();
    ClearBackground(to_rl(clear));
}

bool ccr_gui_screenshot(const char *path)
{
    /* Draw calls are batched; flush them or we capture only the clear color. */
    rlDrawRenderBatchActive();
    Image img = LoadImageFromScreen();
    bool ok = ExportImage(img, path);
    UnloadImage(img);
    return ok;
}

bool ccr_gui_set_icon_png(const unsigned char *png, unsigned int size)
{
    if (!png || size == 0 || size > (unsigned int)INT32_MAX)
        return false;
    Image img = LoadImageFromMemory(".png", png, (int)size);
    if (!IsImageValid(img))
        return false;
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8); /* GLFW wants RGBA */
    SetWindowIcon(img);
    UnloadImage(img);
    return true;
}

ccr_vec2 ccr_gui_mouse(void)
{
    Vector2 v = GetMousePosition();
    return (ccr_vec2){ v.x, v.y };
}

ccr_vec2 ccr_gui_mouse_delta(void)
{
    Vector2 v = GetMouseDelta();
    return (ccr_vec2){ v.x, v.y };
}

bool ccr_gui_mouse_down(ccr_mouse_button button)
{
    return IsMouseButtonDown(button_to_rl(button));
}

void ccr_gui_rect(ccr_rect r, ccr_color c)
{
    DrawRectangleRec(rect_to_rl(r), to_rl(c));
}

void ccr_gui_rect_rounded(ccr_rect r, float roundness, ccr_color c)
{
    DrawRectangleRounded(rect_to_rl(r), roundness, 8, to_rl(c));
}

void ccr_gui_circle(ccr_vec2 center, float radius, ccr_color c)
{
    DrawCircleV((Vector2){ center.x, center.y }, radius, to_rl(c));
}

void ccr_gui_text(const char *text, float x, float y, int size, ccr_color c)
{
    DrawText(text, (int)x, (int)y, size, to_rl(c));
}

float ccr_gui_text_width(const char *text, int size)
{
    return (float)MeasureText(text, size);
}

bool ccr_gui_button(ccr_rect r, const char *label)
{
    return GuiButton(rect_to_rl(r), label) != 0;
}
