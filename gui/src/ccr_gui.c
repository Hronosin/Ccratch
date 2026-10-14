#include "ccr_gui.h"

#include <raylib.h>
#include <rlgl.h>

#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

static Texture2D g_shapes_tex;

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
    SetExitKey(KEY_NULL);   /* raylib quits on Esc by default: not with unsaved work */

    /* The default font is a 10px pixel font. Scaled down by the canvas zoom
     * it drops whole pixel rows; mipmaps + trilinear keep it readable. */
    Font font = GetFontDefault();
    GenTextureMipmaps(&font.texture);
    SetTextureFilter(font.texture, TEXTURE_FILTER_TRILINEAR);

    /* raylib draws shapes with a white pixel borrowed from the font
     * texture; the mipmaps above would smear it, so shapes get their own.
     * raygui re-borrows the font pixel when it loads its style, so load
     * that style first and override after. */
    GuiLoadStyleDefault();
    Image white = GenImageColor(1, 1, WHITE);
    g_shapes_tex = LoadTextureFromImage(white);
    UnloadImage(white);
    SetShapesTexture(g_shapes_tex, (Rectangle){ 0, 0, 1, 1 });
    return true;
}

void ccr_gui_shutdown(void)
{
    if (g_shapes_tex.id != 0)
        UnloadTexture(g_shapes_tex);
    g_shapes_tex = (Texture2D){ 0 };
    CloseWindow();
}

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

bool ccr_gui_mouse_pressed(ccr_mouse_button button)
{
    return IsMouseButtonPressed(button_to_rl(button));
}

bool ccr_gui_mouse_released(ccr_mouse_button button)
{
    return IsMouseButtonReleased(button_to_rl(button));
}

void ccr_gui_rect(ccr_rect r, ccr_color c)
{
    DrawRectangleRec(rect_to_rl(r), to_rl(c));
}

void ccr_gui_rect_rounded(ccr_rect r, float roundness, ccr_color c)
{
    DrawRectangleRounded(rect_to_rl(r), roundness, 8, to_rl(c));
}

void ccr_gui_hex(ccr_rect r, ccr_color c)
{
    const float half = r.h * 0.5f;
    const float cy = r.y + half;
    const Color col = to_rl(c);

    if (r.w <= r.h) {          /* degenerate: just a diamond-ish box */
        DrawRectangleRec(rect_to_rl(r), col);
        return;
    }
    DrawRectangleRec((Rectangle){ r.x + half, r.y, r.w - r.h, r.h }, col);
    /* raylib wants counter-clockwise vertices */
    DrawTriangle((Vector2){ r.x + half, r.y }, (Vector2){ r.x, cy },
                 (Vector2){ r.x + half, r.y + r.h }, col);
    DrawTriangle((Vector2){ r.x + r.w - half, r.y }, (Vector2){ r.x + r.w - half, r.y + r.h },
                 (Vector2){ r.x + r.w, cy }, col);
}

void ccr_gui_circle(ccr_vec2 center, float radius, ccr_color c)
{
    DrawCircleV((Vector2){ center.x, center.y }, radius, to_rl(c));
}

/* The default raylib font is a 10px pixel font; spacing scales with size. */
void ccr_gui_text(const char *text, float x, float y, float size, ccr_color c)
{
    DrawTextEx(GetFontDefault(), text, (Vector2){ x, y }, size, size / 10.0f, to_rl(c));
}

float ccr_gui_text_width(const char *text, float size)
{
    return MeasureTextEx(GetFontDefault(), text, size, size / 10.0f).x;
}

bool ccr_gui_key_pressed(ccr_key key)
{
    switch (key) {
    case CCR_KEY_HOME: return IsKeyPressed(KEY_HOME);
    case CCR_KEY_S:    return IsKeyPressed(KEY_S);
    case CCR_KEY_F5:   return IsKeyPressed(KEY_F5);
    }
    return false;
}

bool ccr_gui_ctrl_down(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

bool ccr_gui_file_dropped(char *out, unsigned int cap)
{
    if (!IsFileDropped() || cap == 0)
        return false;
    FilePathList files = LoadDroppedFiles();
    bool ok = files.count > 0;
    if (ok) {
        const char *p = files.paths[0];
        unsigned int i = 0;
        for (; p[i] && i + 1 < cap; ++i)
            out[i] = p[i];
        out[i] = '\0';
    }
    UnloadDroppedFiles(files);
    return ok;
}

bool ccr_gui_button(ccr_rect r, const char *label)
{
    return GuiButton(rect_to_rl(r), label) != 0;
}
