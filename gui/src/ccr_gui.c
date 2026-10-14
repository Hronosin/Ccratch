#include "ccr_gui.h"

#include <raylib.h>
#include <rlgl.h>
#include <GLFW/glfw3.h>

#include <string.h>

#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

static Texture2D g_shapes_tex;
static Font      g_font;          /* loaded TTF; .texture.id == 0: raylib's pixel font */
static bool      g_font_loaded;

/* The font text is drawn with, and the gap between letters it wants. */
static Font text_font(void) { return g_font_loaded ? g_font : GetFontDefault(); }
static float text_spacing(float size) { return g_font_loaded ? 0.0f : size / 10.0f; }

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

    /* raylib 5.5 carries on after GLFW fails to reach a display and then
     * calls OpenGL that was never loaded: a segfault. Ask GLFW first. */
    if (!glfwInit())
        return false;
    glfwTerminate();

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
    if (g_font_loaded)
        UnloadFont(g_font);
    g_font_loaded = false;
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

bool ccr_gui_load_font(const unsigned char *ttf, unsigned int size, float base_size,
                       const int *codepoints, int count)
{
    if (!ttf || size == 0 || size > (unsigned int)INT32_MAX || count <= 0)
        return false;
    Font f = LoadFontFromMemory(".ttf", ttf, (int)size, (int)base_size, (int *)codepoints, count);
    if (f.texture.id == 0 || f.glyphCount <= 0 || f.texture.id == GetFontDefault().texture.id)
        return false;
    /* drawn at many sizes (the canvas zooms): mipmaps keep small text smooth */
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    if (g_font_loaded)
        UnloadFont(g_font);
    g_font = f;
    g_font_loaded = true;
    GuiSetFont(g_font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    /* raygui may have borrowed the font's white pixel for shapes again */
    SetShapesTexture(g_shapes_tex, (Rectangle){ 0, 0, 1, 1 });
    return true;
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

void ccr_gui_ring(ccr_vec2 center, float inner, float outer,
                  float from_deg, float to_deg, ccr_color c)
{
    int segments = (int)((to_deg - from_deg) / 4.0f) + 2;
    DrawRing((Vector2){ center.x, center.y }, inner, outer, from_deg, to_deg, segments, to_rl(c));
}

void ccr_gui_clip_begin(ccr_rect r)
{
    BeginScissorMode((int)r.x, (int)r.y, (int)r.w, (int)r.h);
}

void ccr_gui_clip_end(void)
{
    EndScissorMode();
}

/* The default raylib font is a 10px pixel font; spacing scales with size. */
void ccr_gui_text(const char *text, float x, float y, float size, ccr_color c)
{
    DrawTextEx(text_font(), text, (Vector2){ x, y }, size, text_spacing(size), to_rl(c));
}

float ccr_gui_text_width(const char *text, float size)
{
    return MeasureTextEx(text_font(), text, size, text_spacing(size)).x;
}

bool ccr_gui_key_pressed(ccr_key key)
{
    switch (key) {
    case CCR_KEY_HOME: return IsKeyPressed(KEY_HOME);
    case CCR_KEY_S:    return IsKeyPressed(KEY_S);
    case CCR_KEY_ESCAPE: return IsKeyPressed(KEY_ESCAPE);
    case CCR_KEY_Y:    return IsKeyPressed(KEY_Y) || IsKeyPressedRepeat(KEY_Y);
    case CCR_KEY_Z:    return IsKeyPressed(KEY_Z) || IsKeyPressedRepeat(KEY_Z);
    case CCR_KEY_F5:   return IsKeyPressed(KEY_F5);
    case CCR_KEY_D:    return IsKeyPressed(KEY_D);
    case CCR_KEY_DELETE: return IsKeyPressed(KEY_DELETE);
    case CCR_KEY_SLASH:  return IsKeyPressed(KEY_SLASH);
    }
    return false;
}

bool ccr_gui_ctrl_down(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

bool ccr_gui_shift_down(void)
{
    return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
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

/* ---- text box ----
 * Not raygui's: it reads one character per frame, and raylib empties its
 * character queue every frame, so two keys landing in one frame lost one. */

static int g_tb_cursor = -1;   /* byte offset; -1 = put it at the end */

void ccr_gui_textbox_reset(void)
{
    g_tb_cursor = -1;
}

static bool key_hit(int key)
{
    return IsKeyPressed(key) || IsKeyPressedRepeat(key);
}

/* start of the UTF-8 codepoint before / after byte `i` */
static int utf8_prev(const char *s, int i)
{
    if (i <= 0)
        return 0;
    --i;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
        --i;
    return i;
}

static int utf8_next(const char *s, int i, int len)
{
    if (i >= len)
        return len;
    ++i;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
        ++i;
    return i;
}

bool ccr_gui_textbox(ccr_rect r, char *buf, int cap, float font_size)
{
    int len = (int)strlen(buf);
    if (g_tb_cursor < 0 || g_tb_cursor > len)
        g_tb_cursor = len;
    int cur = g_tb_cursor;

    /* everything typed this frame */
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) {
        if (cp < 32)
            continue;
        int n = 0;
        const char *enc = CodepointToUTF8(cp, &n);
        if (len + n >= cap)
            continue;
        memmove(buf + cur + n, buf + cur, (size_t)(len - cur + 1));
        memcpy(buf + cur, enc, (size_t)n);
        cur += n;
        len += n;
    }
    /* Ctrl+V: paste, on one line */
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        for (const char *c = clip; c && *c && len + 1 < cap; ++c) {
            char ch = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
            memmove(buf + cur + 1, buf + cur, (size_t)(len - cur + 1));
            buf[cur++] = ch;
            ++len;
        }
    }
    if (key_hit(KEY_BACKSPACE) && cur > 0) {
        int p = utf8_prev(buf, cur);
        memmove(buf + p, buf + cur, (size_t)(len - cur + 1));
        len -= cur - p;
        cur = p;
    }
    if (key_hit(KEY_DELETE) && cur < len) {
        int nx = utf8_next(buf, cur, len);
        memmove(buf + cur, buf + nx, (size_t)(len - nx + 1));
        len -= nx - cur;
    }
    if (key_hit(KEY_LEFT))  cur = utf8_prev(buf, cur);
    if (key_hit(KEY_RIGHT)) cur = utf8_next(buf, cur, len);
    if (IsKeyPressed(KEY_HOME)) cur = 0;
    if (IsKeyPressed(KEY_END))  cur = len;
    g_tb_cursor = cur;

    /* draw: white field, accent border, text scrolled so the cursor shows */
    Font font = text_font();
    const float spacing = text_spacing(font_size);
    char saved = buf[cur];
    buf[cur] = '\0';
    float cursor_x = MeasureTextEx(font, buf, font_size, spacing).x;
    buf[cur] = saved;
    const float pad = 8.0f;
    float scroll = cursor_x - (r.w - pad * 2.0f);
    if (scroll < 0.0f)
        scroll = 0.0f;

    DrawRectangleRec(rect_to_rl(r), (Color){ 0x2E, 0x8E, 0xB8, 0xFF });
    DrawRectangleRec((Rectangle){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 }, RAYWHITE);
    BeginScissorMode((int)r.x + 2, (int)r.y + 2, (int)r.w - 4, (int)r.h - 4);
    const float ty = r.y + (r.h - font_size) * 0.5f;
    DrawTextEx(font, buf, (Vector2){ r.x + pad - scroll, ty }, font_size, spacing,
               (Color){ 0x30, 0x34, 0x40, 0xFF });
    if (((int)(GetTime() * 2.0)) % 2 == 0)   /* blinking cursor */
        DrawRectangleRec((Rectangle){ r.x + pad - scroll + cursor_x, ty, 2.0f, font_size },
                         (Color){ 0x30, 0x34, 0x40, 0xFF });
    EndScissorMode();

    Vector2 m = GetMousePosition();
    bool outside = !CheckCollisionPointRec(m, rect_to_rl(r));
    bool done = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && outside);
    if (done)
        g_tb_cursor = -1;
    return done;
}
