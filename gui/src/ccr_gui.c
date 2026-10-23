#include "ccr_gui.h"

#include <raylib.h>
#include <rlgl.h>
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Texture2D g_shapes_tex;
static float     g_scale = 1.0f;  /* interface size: everything is drawn in units this many pixels big */

/* The mouse, in interface units. */
static Vector2 mouse_units(void)
{
    const Vector2 v = GetMousePosition();
    return (Vector2){ v.x / g_scale, v.y / g_scale };
}

/* A clip rectangle given in units, in the framebuffer's pixels. */
static void scissor_units(float x, float y, float w, float h)
{
    BeginScissorMode((int)(x * g_scale), (int)(y * g_scale), (int)(w * g_scale + 0.999f), (int)(h * g_scale + 0.999f));
}
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
     * calls OpenGL that was never loaded: a segfault. Ask GLFW first, and
     * stay connected: raylib's own glfwInit then just reuses it, so the
     * display cannot go away in between (seen with a slow Xvfb). */
    if (!glfwInit())
        return false;

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
     * texture; the mipmaps above would smear it, so shapes get their own. */
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
void ccr_gui_set_clipboard(const char *text) { SetClipboardText(text ? text : ""); }
const char *ccr_gui_clipboard(void)          { return GetClipboardText(); }

void ccr_gui_set_cursor(ccr_cursor c)
{
    static int now = -1;   /* only when it changes: X11 is not free */
    if ((int)c == now)
        return;
    now = (int)c;
    SetMouseCursor(c == CCR_CURSOR_EW ? MOUSE_CURSOR_RESIZE_EW
                   : c == CCR_CURSOR_NS ? MOUSE_CURSOR_RESIZE_NS
                   : c == CCR_CURSOR_HAND ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
}

void ccr_gui_cancel_close(void)
{
    GLFWwindow *w = glfwGetCurrentContext();
    if (w)
        glfwSetWindowShouldClose(w, GLFW_FALSE);
}
void ccr_gui_end_frame(void)
{
    rlDrawRenderBatchActive();
    rlPopMatrix();
    EndDrawing();
}
int  ccr_gui_width(void)            { return (int)((float)GetScreenWidth() / g_scale); }
int  ccr_gui_height(void)           { return (int)((float)GetScreenHeight() / g_scale); }

void ccr_gui_window_fit(int w, int h)
{
    const int mon = GetCurrentMonitor();
    const int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
    int pw = (int)((float)w * g_scale), ph = (int)((float)h * g_scale);
    if (mw > 0 && pw > mw * 92 / 100)
        pw = mw * 92 / 100;
    if (mh > 0 && ph > mh * 88 / 100)
        ph = mh * 88 / 100;
    if (pw == GetScreenWidth() && ph == GetScreenHeight())
        return;
    SetWindowSize(pw, ph);
    if (mw > 0 && mh > 0) {
        const Vector2 at = GetMonitorPosition(mon);
        SetWindowPosition((int)at.x + (mw - pw) / 2, (int)at.y + (mh - ph) / 2);
    }
}

void ccr_gui_set_scale(float s)
{
    g_scale = s < 0.5f ? 0.5f : s > 4.0f ? 4.0f : s;
    if (IsWindowReady()) {   /* never so small that the panels pile up: 640x400 units */
        const int mon = GetCurrentMonitor();
        int mw = (int)(640.0f * g_scale), mh = (int)(400.0f * g_scale);
        if (GetMonitorWidth(mon) > 0 && mw > GetMonitorWidth(mon) * 9 / 10)
            mw = GetMonitorWidth(mon) * 9 / 10;
        if (GetMonitorHeight(mon) > 0 && mh > GetMonitorHeight(mon) * 9 / 10)
            mh = GetMonitorHeight(mon) * 9 / 10;
        SetWindowMinSize(mw, mh);
    }
}
float ccr_gui_scale(void)           { return g_scale; }

float ccr_gui_auto_scale(void)
{
    /* the system's own setting (GNOME/KDE scaling, Windows' %), if any */
    const Vector2 dpi = GetWindowScaleDPI();
    if (dpi.x > 1.05f)
        return dpi.x;
    /* else from the screen: Ccratch was drawn for a 27" 2560x1440 one,
     * about 109 pixels an inch; denser screens get bigger units */
    const int mon = GetCurrentMonitor();
    const int px = GetMonitorWidth(mon), mm = GetMonitorPhysicalWidth(mon), py = GetMonitorHeight(mon);
    float s = 1.0f;
    if (px > 0 && mm > 100 && mm < 2000) {   /* sizes some screens report are nonsense */
        const float ppi = (float)px / ((float)mm / 25.4f);
        s = ppi / 109.0f;
    }
    /* and never so big that the window cannot hold the panels */
    if (py > 0 && (float)py / s < 720.0f)
        s = (float)py / 720.0f;
    s = (float)(int)(s * 4.0f + 0.5f) / 4.0f;   /* in quarters: 100%, 125%, 150%... */
    s = s < 0.75f ? 0.75f : s > 3.0f ? 3.0f : s;
    if (getenv("CCRATCH_UI_SCALE_WHY"))
        fprintf(stderr, "ccratch: screen %d: %dx%d pixels, %dx%d mm, system scale %.2f -> interface %.0f%%\n", mon, px,
                py, mm, GetMonitorPhysicalHeight(mon), (double)dpi.x, (double)(s * 100.0f));
    return s;
}
double ccr_gui_time(void)           { return GetTime(); }
float ccr_gui_wheel(void)           { return GetMouseWheelMove(); }

void ccr_gui_begin_frame(ccr_color clear)
{
    BeginDrawing();
    ClearBackground(to_rl(clear));
    rlPushMatrix();
    rlScalef(g_scale, g_scale, 1.0f);
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
    const ccr_font_part part = { ttf, size, codepoints, count };
    return ccr_gui_load_fonts(&part, 1, base_size);
}

bool ccr_gui_load_fonts(const ccr_font_part *parts, int n, float base_size)
{
    int total = 0;
    for (int i = 0; i < n; ++i) {
        if (!parts[i].ttf || parts[i].size == 0 || parts[i].size > (unsigned int)INT32_MAX || parts[i].count < 0)
            return false;
        total += parts[i].count;
    }
    if (total <= 0)
        return false;
    /* each part's glyphs, then one atlas for all of them */
    GlyphInfo *glyphs = RL_CALLOC((size_t)total, sizeof *glyphs);
    if (!glyphs)
        return false;
    int got = 0;
    for (int i = 0; i < n; ++i) {
        if (parts[i].count == 0)
            continue;
        GlyphInfo *g = LoadFontData(parts[i].ttf, (int)parts[i].size, (int)base_size, (int *)parts[i].codepoints,
                                    parts[i].count, FONT_DEFAULT);
        if (!g)
            continue;   /* that font could not be read: its letters show as boxes */
        memcpy(glyphs + got, g, (size_t)parts[i].count * sizeof *g);
        got += parts[i].count;
        RL_FREE(g);
    }
    if (got == 0) {
        RL_FREE(glyphs);
        return false;
    }
    Font f = { 0 };
    f.baseSize = (int)base_size;
    f.glyphCount = got;
    f.glyphPadding = 4;
    f.glyphs = glyphs;
    Image atlas = GenImageFontAtlas(f.glyphs, &f.recs, f.glyphCount, f.baseSize, f.glyphPadding, 0);
    f.texture = LoadTextureFromImage(atlas);
    for (int i = 0; i < f.glyphCount; ++i) {
        UnloadImage(f.glyphs[i].image);
        f.glyphs[i].image = ImageFromImage(atlas, f.recs[i]);
    }
    UnloadImage(atlas);
    if (f.texture.id == 0) {
        UnloadFont(f);
        return false;
    }
    /* drawn at many sizes (the canvas zooms): mipmaps keep small text smooth */
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    if (g_font_loaded)
        UnloadFont(g_font);
    g_font = f;
    g_font_loaded = true;
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
    Vector2 v = mouse_units();
    return (ccr_vec2){ v.x, v.y };
}

ccr_vec2 ccr_gui_mouse_delta(void)
{
    Vector2 v = GetMouseDelta();
    return (ccr_vec2){ v.x / g_scale, v.y / g_scale };
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

void ccr_gui_rect_corners(ccr_rect r, float radius, unsigned corners, ccr_color c)
{
    if (r.w <= 0.0f || r.h <= 0.0f)
        return;
    float rad = radius;
    if (rad > r.w * 0.5f) rad = r.w * 0.5f;
    if (rad > r.h * 0.5f) rad = r.h * 0.5f;
    if (rad < 0.0f) rad = 0.0f;
    const Color col = to_rl(c);
    /* middle column, then the two side strips between the corners */
    DrawRectangleRec((Rectangle){ r.x + rad, r.y, r.w - rad * 2.0f, r.h }, col);
    DrawRectangleRec((Rectangle){ r.x, r.y + rad, rad, r.h - rad * 2.0f }, col);
    DrawRectangleRec((Rectangle){ r.x + r.w - rad, r.y + rad, rad, r.h - rad * 2.0f }, col);
    if (rad <= 0.0f)
        return;
    /* each corner: a quarter circle, or the square it would sit in */
    const struct { unsigned bit; float cx, cy, from; } k[4] = {
        { CCR_CORNER_TL, r.x + rad,       r.y + rad,       180.0f },
        { CCR_CORNER_TR, r.x + r.w - rad, r.y + rad,       270.0f },
        { CCR_CORNER_BR, r.x + r.w - rad, r.y + r.h - rad,   0.0f },
        { CCR_CORNER_BL, r.x + rad,       r.y + r.h - rad,  90.0f },
    };
    for (int i = 0; i < 4; ++i) {
        if (corners & k[i].bit) {
            DrawCircleSector((Vector2){ k[i].cx, k[i].cy }, rad, k[i].from, k[i].from + 90.0f, 8, col);
        } else {
            float sx = (k[i].bit == CCR_CORNER_TL || k[i].bit == CCR_CORNER_BL) ? r.x : r.x + r.w - rad;
            float sy = (k[i].bit == CCR_CORNER_TL || k[i].bit == CCR_CORNER_TR) ? r.y : r.y + r.h - rad;
            DrawRectangleRec((Rectangle){ sx, sy, rad, rad }, col);
        }
    }
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

void ccr_gui_line(ccr_vec2 a, ccr_vec2 b, float thick, ccr_color c)
{
    DrawLineEx((Vector2){ a.x, a.y }, (Vector2){ b.x, b.y }, thick, to_rl(c));
}

void ccr_gui_triangle(ccr_vec2 a, ccr_vec2 b, ccr_vec2 c, ccr_color col)
{
    /* raylib draws only counter-clockwise triangles (y down) */
    float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0.0f) {
        ccr_vec2 t = b;
        b = c;
        c = t;
    }
    DrawTriangle((Vector2){ a.x, a.y }, (Vector2){ b.x, b.y }, (Vector2){ c.x, c.y }, to_rl(col));
}

void ccr_gui_clip_begin(ccr_rect r)
{
    scissor_units(r.x, r.y, r.w, r.h);
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
    case CCR_KEY_P:      return IsKeyPressed(KEY_P);
    case CCR_KEY_W:      return IsKeyPressed(KEY_W);
    case CCR_KEY_TAB:    return IsKeyPressed(KEY_TAB);
    case CCR_KEY_B:      return IsKeyPressed(KEY_B);
    case CCR_KEY_R:      return IsKeyPressed(KEY_R);
    case CCR_KEY_F:      return IsKeyPressed(KEY_F);
    case CCR_KEY_F1:     return IsKeyPressed(KEY_F1);
    case CCR_KEY_F3:     return IsKeyPressed(KEY_F3);
    case CCR_KEY_ENTER:  return IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    case CCR_KEY_EQUAL:  return IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD);
    case CCR_KEY_MINUS:  return IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT);
    case CCR_KEY_ZERO:   return IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0);
    case CCR_KEY_C:      return IsKeyPressed(KEY_C);
    case CCR_KEY_V:      return IsKeyPressed(KEY_V);
    case CCR_KEY_A:      return IsKeyPressed(KEY_A);
    case CCR_KEY_M:      return IsKeyPressed(KEY_M);
    case CCR_KEY_UP:     return IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP);
    case CCR_KEY_DOWN:   return IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN);
    case CCR_KEY_F2:     return IsKeyPressed(KEY_F2);
    case CCR_KEY_F12:    return IsKeyPressed(KEY_F12);
    case CCR_KEY_LEFT:   return IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
    case CCR_KEY_RIGHT:  return IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);
    case CCR_KEY_F8:     return IsKeyPressed(KEY_F8);
    case CCR_KEY_F9:     return IsKeyPressed(KEY_F9);
    case CCR_KEY_F10:    return IsKeyPressed(KEY_F10);
    case CCR_KEY_F11:    return IsKeyPressed(KEY_F11);
    case CCR_KEY_G:      return IsKeyPressed(KEY_G);
    case CCR_KEY_COMMA:  return IsKeyPressed(KEY_COMMA);
    case CCR_KEY_K:      return IsKeyPressed(KEY_K);
    }
    return false;
}

bool ccr_gui_ctrl_down(void)
{
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

bool ccr_gui_alt_down(void)
{
    return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
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

bool ccr_gui_button(ccr_rect r, const char *label, const ccr_button_style *st)
{
    /* a click: pressed and let go over the button */
    static Vector2 pressed_at = { -1e9f, -1e9f };
    const Vector2 m = mouse_units();
    const Rectangle rr = rect_to_rl(r);
    const bool over = CheckCollisionPointRec(m, rr);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        pressed_at = m;
    const bool held = IsMouseButtonDown(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(pressed_at, rr);
    const bool clicked = over && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
                         CheckCollisionPointRec(pressed_at, rr);

    ccr_color fill = held && over ? st->press : over ? st->hover : st->fill;
    const float round = r.h > 0.0f ? 8.0f / r.h : 0.3f;
    DrawRectangleRounded(rr, round, 8, to_rl(st->edge));
    DrawRectangleRounded((Rectangle){ r.x + 1.5f, r.y + 1.5f, r.w - 3.0f, r.h - 3.0f }, round, 8,
                         to_rl(fill));
    const float size = r.h * 0.6f > 18.0f ? 18.0f : r.h * 0.6f;
    const float tw = ccr_gui_text_width(label, size);
    ccr_gui_text(label, r.x + (r.w - tw) * 0.5f, r.y + (r.h - size) * 0.5f, size, st->text);
    return clicked;
}

/* ---- text box ----
 * Not raygui's: it reads one character per frame, and raylib empties its
 * character queue every frame, so two keys landing in one frame lost one. */

static int g_tb_cursor = -1;   /* byte offset; -1 = put it at the end */
static bool g_tb_all;          /* all of it selected: what is typed replaces it */
static int  g_tb_held;         /* a character taken off raylib's queue to look at */

/* Is a character waiting? (It stays waiting: the loop below reads it.) */
static bool GetCharPressedPeek(void)
{
    if (!g_tb_held)
        g_tb_held = GetCharPressed();
    return g_tb_held > 0;
}

static int next_char(void)
{
    if (g_tb_held) {
        const int c = g_tb_held;
        g_tb_held = 0;
        return c;
    }
    return GetCharPressed();
}

void ccr_gui_textbox_reset(void)
{
    g_tb_cursor = -1;
    g_tb_all = false;
}

void ccr_gui_textbox_select_all(void)
{
    g_tb_cursor = -1;
    g_tb_all = true;
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

static ccr_color g_tb_edge = 0x2E8EB8FFu, g_tb_fill = 0xF5F5F5FFu, g_tb_text = 0x303440FFu;

void ccr_gui_textbox_colors(ccr_color edge, ccr_color fill, ccr_color text)
{
    g_tb_edge = edge;
    g_tb_fill = fill;
    g_tb_text = text;
}

bool ccr_gui_textbox(ccr_rect r, char *buf, int cap, float font_size)
{
    int len = (int)strlen(buf);
    if (g_tb_cursor < 0 || g_tb_cursor > len)
        g_tb_cursor = len;
    int cur = g_tb_cursor;

    /* all of it selected: typing, pasting or erasing replaces it; a
     * move of the cursor just lets it go */
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_A))
        g_tb_all = true;
    if (g_tb_all) {
        const bool erase = key_hit(KEY_BACKSPACE) || key_hit(KEY_DELETE);
        const bool replace = GetCharPressedPeek() || (ctrl && IsKeyPressed(KEY_V)) || erase;
        if (replace) {
            buf[0] = '\0';
            len = cur = 0;
            g_tb_all = false;
            if (erase) {
                g_tb_cursor = 0;
                goto drawn;   /* erased: nothing else this frame */
            }
        } else if (key_hit(KEY_LEFT) || key_hit(KEY_RIGHT) || IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_END)) {
            g_tb_all = false;
        }
    }

    /* everything typed this frame */
    for (int cp = next_char(); cp > 0; cp = next_char()) {
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
drawn:
    cur = g_tb_cursor;

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

    DrawRectangleRec(rect_to_rl(r), to_rl(g_tb_edge));
    DrawRectangleRec((Rectangle){ r.x + 2, r.y + 2, r.w - 4, r.h - 4 }, to_rl(g_tb_fill));
    scissor_units(r.x + 2, r.y + 2, r.w - 4, r.h - 4);
    const float ty = r.y + (r.h - font_size) * 0.5f;
    if (g_tb_all && buf[0]) {   /* selected: on a band of the edge's colour, faint */
        const float tw = MeasureTextEx(font, buf, font_size, spacing).x;
        Color band = to_rl(g_tb_edge);
        band.a = 70;
        DrawRectangleRec((Rectangle){ r.x + pad - scroll - 1.0f, ty - 1.0f, tw + 2.0f, font_size + 2.0f }, band);
    }
    DrawTextEx(font, buf, (Vector2){ r.x + pad - scroll, ty }, font_size, spacing,
               to_rl(g_tb_text));
    if (((int)(GetTime() * 2.0)) % 2 == 0)   /* blinking cursor */
        DrawRectangleRec((Rectangle){ r.x + pad - scroll + cursor_x, ty, 2.0f, font_size },
                         to_rl(g_tb_text));
    EndScissorMode();

    Vector2 m = mouse_units();
    bool outside = !CheckCollisionPointRec(m, rect_to_rl(r));
    bool done = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && outside);
    if (done)
        g_tb_cursor = -1;
    return done;
}
