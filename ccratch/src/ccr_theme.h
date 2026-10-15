/*
 * ccr_theme - what everything looks like.
 *
 * Every colour the window uses has a key. The dark Shrine theme is built
 * in; other themes are plain files, themes/NAME.theme, found at run time
 * like language packs. A theme only needs the keys it changes:
 *
 *     @name    = Super Serious
 *     @name.ru = Суровая
 *     bg       = #F8E8EF
 *     control  = #FFC9A9
 *
 * Colours are #RRGGBB or #RRGGBBAA.
 */
#ifndef CCR_THEME_H
#define CCR_THEME_H

#include "ccr_gui.h"

#include <stdbool.h>
#include <stdio.h>

/* X(ID, "key", default) - the window */
#define CCR_THEME_COLORS(X)                                                 \
    X(BG,           "bg",            0x1E2028FFu)  /* canvas */             \
    X(GRID,         "grid",          0x3A3F4BFFu)  /* canvas dots */        \
    X(BAR,          "bar",           0x16181EFFu)  /* top and status bars */\
    X(BAR_TEXT,     "bar_text",      0x9AA3B5FFu)                           \
    X(TITLE,        "title",         0xE8ECF4FFu)  /* "Ccratch", headings */\
    X(HINT,         "hint",          0x5A6170FFu)                           \
    X(ACCENT,       "accent",        0xD9464BFFu)  /* shrine red */         \
    X(OK,           "ok",            0x59C059FFu)                           \
    X(ERROR,        "error",         0xD9464BFFu)                           \
    X(GLOW,         "glow",          0xFFE066FFu)  /* drop here, look here */\
    X(PALETTE,      "palette",       0x1A1C23FFu)  /* the block palette */  \
    X(PALETTE_TEXT, "palette_text",  0x9AA3B5FFu)                           \
    X(TRASH,        "trash",         0x2A1E22FFu)  /* palette, block over it */\
    X(PANEL,        "panel",         0x2A2E37F2u)  /* pie menu, agreement */\
    X(PANEL_EDGE,   "panel_edge",    0x454B57FFu)                           \
    X(PANEL_TEXT,   "panel_text",    0xF2F2F2FFu)                           \
    X(PANEL_HINT,   "panel_hint",    0x9AA0ABFFu)                           \
    X(DISABLED,     "disabled",      0x666C78FFu)                           \
    X(HOT,          "hot",           0x4C7BD9FFu)  /* chosen pie slice */   \
    X(BACKDROP,     "backdrop",      0x000000B0u)  /* behind a dialog */    \
    X(BUTTON,       "button",        0x2A2E37FFu)                           \
    X(BUTTON_HOVER, "button_hover",  0x363B46FFu)                           \
    X(BUTTON_PRESS, "button_press",  0x4C7BD9FFu)                           \
    X(BUTTON_EDGE,  "button_edge",   0x454B57FFu)                           \
    X(BUTTON_TEXT,  "button_text",   0xE8ECF4FFu)                           \
    X(FIELD,        "field",         0xF5F5F5FFu)  /* text box */           \
    X(FIELD_EDGE,   "field_edge",    0x2E8EB8FFu)                           \
    X(FIELD_TEXT,   "field_text",    0x303440FFu)                           \
    X(SHADOW,       "shadow",        0x00000060u)  /* under a lifted block */\
    X(DIM,          "dim",           0x4A4F5AFFu)  /* commented-out blocks */\
    X(DIM_TEXT,     "dim_text",      0xAEB4C0FFu)

/* Block categories, in the order ccr_blocks uses. Each has three keys:
 * NAME (fill), NAME_edge and NAME_text. */
#define CCR_THEME_CATEGORIES(X)                                          \
    X(CONTROL,  "control",  0xFFAB19FFu, 0xCF8B17FFu, 0xFFFFFFFFu)       \
    X(OPERATOR, "operator", 0x59C059FFu, 0x389438FFu, 0xFFFFFFFFu)       \
    X(VARIABLE, "variable", 0xFF8C1AFFu, 0xDB6E00FFu, 0xFFFFFFFFu)       \
    X(CALL,     "call",     0xFF6680FFu, 0xFF3355FFu, 0xFFFFFFFFu)       \
    X(EVENT,    "event",    0xFFBF00FFu, 0xCC9900FFu, 0xFFFFFFFFu)       \
    X(TYPE,     "type",     0x5CB1D6FFu, 0x2E8EB8FFu, 0xFFFFFFFFu)       \
    X(PREPROC,  "preproc",  0x9966FFFFu, 0x774DCBFFu, 0xFFFFFFFFu)       \
    X(POINTER,  "pointer",  0xE6544DFFu, 0xB8372FFFu, 0xFFFFFFFFu)       \
    X(LITERAL,  "literal",  0xFFFFFFFFu, 0xC8CCD6FFu, 0x575E75FFu)       \
    X(DRAGON,   "dragon",   0x8A8F9CFFu, 0x5F6470FFu, 0xFFFFFFFFu)

typedef enum ccr_theme_color {
#define X(id, key, def) CCR_C_##id,
    CCR_THEME_COLORS(X)
#undef X
    CCR_C_COUNT
} ccr_theme_color;

typedef enum ccr_theme_cat {
#define X(id, key, f, e, t) CCR_CAT_##id,
    CCR_THEME_CATEGORIES(X)
#undef X
    CCR_CAT_COUNT
} ccr_theme_cat;

typedef enum ccr_cat_role { CCR_FILL, CCR_EDGE, CCR_TEXT } ccr_cat_role;

/* A colour of the theme in use. */
ccr_color ccr_th(ccr_theme_color c);
ccr_color ccr_th_cat(ccr_theme_cat cat, ccr_cat_role role);

typedef struct ccr_theme_info {
    char code[32];          /* file name without .theme; "shrine" is built in */
    char name[64];          /* in the interface language, if the file says */
    char path[1024];
} ccr_theme_info;

/* Built-in Shrine first, then themes found in $CCRATCH_THEME_DIR,
 * ~/.local/share/ccratch/themes and share/ccratch/themes. */
int  ccr_theme_scan(ccr_theme_info *out, int cap);

/* Switches theme; false (and no change) if there is no such theme. */
bool ccr_theme_use(const char *code, FILE *log);
const char *ccr_theme_code(void);
const char *ccr_theme_name(void);

/* --theme, else the one saved in the config file, else Shrine. */
void ccr_theme_pick(const char *cli, FILE *log);
bool ccr_theme_save_choice(const char *code);

/* --check-theme: problems found (unknown keys, bad colours). With
 * `complete`, keys the file leaves out count too. */
int  ccr_theme_check(const char *path, bool complete, FILE *out);

#endif /* CCR_THEME_H */
