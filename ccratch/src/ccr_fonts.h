/*
 * ccr_fonts - which characters come from which of Ccratch's fonts.
 *
 * Nunito (assets/fonts/nunito-ccratch.ttf) is drawn for CCR_FONT_ALWAYS
 * whatever the language, and for CCR_FONT_MORE when the language uses
 * them. Everything else comes from the font a pack names with @font
 * (noto-jp...), cut down by tools/make_font.py to what the shipped packs
 * use. Keep in step with NUNITO_RANGES there.
 */
#ifndef CCR_FONTS_H
#define CCR_FONTS_H

#include <stdbool.h>
#include <stddef.h>

static const int CCR_FONT_ALWAYS[][2] = {
    { 0x0020, 0x007E }, { 0x00A0, 0x017F }, { 0x0400, 0x04FF },
    { 0x2010, 0x2027 }, { 0x2030, 0x203A }, { 0x2116, 0x2116 },
};
static const int CCR_FONT_MORE[][2] = { { 0x0180, 0x02FF }, { 0x0500, 0x052F } };

#define CCR_FONT_N_ALWAYS (sizeof CCR_FONT_ALWAYS / sizeof CCR_FONT_ALWAYS[0])
#define CCR_FONT_N_MORE   (sizeof CCR_FONT_MORE / sizeof CCR_FONT_MORE[0])

static inline bool ccr_font_in(int c, const int (*r)[2], size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (c >= r[i][0] && c <= r[i][1])
            return true;
    return false;
}

/* Does Nunito draw `c` (when the language uses it)? */
static inline bool ccr_font_nunito(int c)
{
    return ccr_font_in(c, CCR_FONT_ALWAYS, CCR_FONT_N_ALWAYS) || ccr_font_in(c, CCR_FONT_MORE, CCR_FONT_N_MORE);
}

#endif /* CCR_FONTS_H */
