#!/usr/bin/env python3
"""
Makes assets/fonts/nunito-ccratch.ttf from upstream Nunito.

Nunito ships as one variable font whose default weight is ExtraLight (200),
and raylib's stb_truetype only ever draws a font's default. So this pins
the weight, and keeps only the characters Ccratch can show: Latin with its
European extensions, Cyrillic, and some punctuation. Needs fonttools.

  curl -LO 'https://raw.githubusercontent.com/google/fonts/main/ofl/nunito/Nunito%5Bwght%5D.ttf'
  python3 tools/make_font.py 'Nunito[wght].ttf' assets/fonts/nunito-ccratch.ttf

Keep RANGES in step with g_font_ranges in ccratch/src/ccr_app.c.
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

WEIGHT = 700
RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x017F),  # Latin-1, Latin Extended-A
    (0x0400, 0x04FF),  # Cyrillic
    (0x2010, 0x2027),  # dashes, quotes, bullet, ellipsis
    (0x2030, 0x203A),  # per mille, angle quotes
    (0x2116, 0x2116),  # numero sign
    (0x2190, 0x2193),  # arrows
]


def main(src, dst):
    font = TTFont(src)
    font = instancer.instantiateVariableFont(font, {"wght": WEIGHT})
    opts = subset.Options()
    opts.layout_features = ["kern", "liga"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=[c for a, b in RANGES for c in range(a, b + 1)])
    sub.subset(font)
    font.save(dst)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
