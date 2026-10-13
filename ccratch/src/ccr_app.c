#include "ccratch/ccratch.h"
#include "ccr_canvas.h"
#include "ccr_gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette */
#define COL_BG        0x1E2028FFu
#define COL_BAR       0x16181EFFu
#define COL_BAR_TEXT  0x9AA3B5FFu
#define COL_TITLE     0xE8ECF4FFu
#define COL_ACCENT    0xD9464BFFu   /* shrine red */
#define COL_BLOCK     0xE6A23CFFu   /* "events" orange */
#define COL_BLOCK_TXT 0x1E2028FFu

#define TOPBAR_H     40.0f
#define STATUSBAR_H  26.0f

typedef struct ccr_options {
    int         smoke_frames;   /* >0: quit after N frames (CI, headless) */
    const char *screenshot;     /* save the last frame here */
} ccr_options;

const char *ccr_version(void)
{
    return "0.0.1";
}

static void print_usage(const char *argv0)
{
    printf("Ccratch %s - Scratch for raw C\n\n"
           "usage: %s [options]\n\n"
           "  --help                 show this and leave\n"
           "  --version              print version\n"
           "  --smoke-frames N       run N frames and exit (for CI)\n"
           "  --screenshot FILE.png  save the last frame before exiting\n",
           ccr_version(), argv0);
}

/* Returns -1 to keep running, otherwise an exit code. */
static int parse_args(int argc, char **argv, ccr_options *opt)
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("%s\n", ccr_version());
            return 0;
        } else if (strcmp(a, "--smoke-frames") == 0 && i + 1 < argc) {
            opt->smoke_frames = atoi(argv[++i]);
        } else if (strcmp(a, "--screenshot") == 0 && i + 1 < argc) {
            opt->screenshot = argv[++i];
        } else {
            fprintf(stderr, "ccratch: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }
    return -1;
}

/* A placeholder "hat" block so the canvas is not lonely. */
static void draw_hat_block(const ccr_canvas *cv, ccr_vec2 world, const char *label)
{
    const int   font  = (int)(18.0f * cv->zoom);
    const float pad   = 14.0f * cv->zoom;
    const float h     = 44.0f * cv->zoom;
    const float bump  = 18.0f * cv->zoom;
    const float w     = ccr_gui_text_width(label, font) + pad * 2.0f;

    ccr_vec2 p = ccr_canvas_to_screen(cv, world);

    /* the rounded "hat" on top, then the body */
    ccr_gui_rect_rounded((ccr_rect){ p.x, p.y - bump * 0.6f, w * 0.45f, bump * 1.6f }, 0.9f, COL_BLOCK);
    ccr_gui_rect_rounded((ccr_rect){ p.x, p.y, w, h }, 0.25f, COL_BLOCK);
    ccr_gui_text(label, p.x + pad, p.y + (h - (float)font) * 0.5f, font, COL_BLOCK_TXT);

    /* notch where the next block will snap in */
    ccr_gui_rect((ccr_rect){ p.x + 16.0f * cv->zoom, p.y + h - 1.0f, 24.0f * cv->zoom, 6.0f * cv->zoom }, COL_BLOCK);
}

static void draw_topbar(int width)
{
    ccr_gui_rect((ccr_rect){ 0, 0, (float)width, TOPBAR_H }, COL_BAR);
    ccr_gui_circle((ccr_vec2){ 22.0f, TOPBAR_H * 0.5f }, 8.0f, COL_ACCENT);
    ccr_gui_text("Ccratch", 38.0f, 10.0f, 20, COL_TITLE);

    float x = (float)width - 110.0f;
    if (ccr_gui_button((ccr_rect){ x, 7.0f, 100.0f, 26.0f }, "Build"))
        printf("[ccratch] build: nothing to build yet. Go do some shrine work.\n");
}

static void draw_statusbar(int width, int height, const ccr_canvas *cv)
{
    float y = (float)height - STATUSBAR_H;
    ccr_gui_rect((ccr_rect){ 0, y, (float)width, STATUSBAR_H }, COL_BAR);

    char buf[160];
    snprintf(buf, sizeof buf, "No incidents. Donation box: empty.   |   zoom %d%%   |   RMB drag: pan, wheel: zoom",
             (int)(cv->zoom * 100.0f + 0.5f));
    ccr_gui_text(buf, 10.0f, y + 7.0f, 12, COL_BAR_TEXT);
}

int ccr_run(int argc, char **argv)
{
    ccr_options opt = { 0, NULL };
    int early = parse_args(argc, argv, &opt);
    if (early >= 0)
        return early;

    if (!ccr_gui_init(1280, 800, "Ccratch")) {
        fprintf(stderr, "ccratch: could not open a window. Opyat' rabotat'...\n");
        return 1;
    }

    ccr_canvas canvas;
    ccr_canvas_init(&canvas);
    canvas.offset = (ccr_vec2){ 120.0f, 140.0f };

    int frame = 0;
    while (!ccr_gui_should_close()) {
        const int w = ccr_gui_width();
        const int h = ccr_gui_height();
        const ccr_rect viewport = { 0, TOPBAR_H, (float)w, (float)h - TOPBAR_H - STATUSBAR_H };

        ccr_canvas_update(&canvas, viewport);

        ccr_gui_begin_frame(COL_BG);
        ccr_canvas_draw_grid(&canvas, viewport);
        draw_hat_block(&canvas, (ccr_vec2){ 0.0f, 0.0f }, "int main(void)");
        draw_topbar(w);
        draw_statusbar(w, h, &canvas);

        ++frame;
        const bool last = opt.smoke_frames > 0 && frame >= opt.smoke_frames;
        if (last && opt.screenshot && !ccr_gui_screenshot(opt.screenshot))
            fprintf(stderr, "ccratch: failed to save screenshot '%s'\n", opt.screenshot);

        ccr_gui_end_frame();
        if (last)
            break;
    }

    ccr_gui_shutdown();
    return 0;
}
