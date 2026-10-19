#!/usr/bin/env python3
"""
Makes Ccratch's fonts in assets/fonts from upstream ones (all OFL, from
github.com/google/fonts):

  nunito-ccratch.ttf     Nunito: Latin with its extensions, Cyrillic,
                         some punctuation. Every language that has these
                         letters needs nothing more.
  noto-jp-ccratch.ttf    Noto Sans JP, Noto Sans SC, Noto Sans KR, Noto
  noto-sc-ccratch.ttf    Sans Armenian: only the characters the shipped
  noto-kr-ccratch.ttf    language packs use (a pack names its font with
  noto-armenian-ccratch.ttf  `@font = noto-jp`), so they stay small.

Run it again after a pack that uses one of the Noto fonts changes: the
test `font_coverage` says when a character is missing.

  mkdir src && cd src
  for f in nunito/Nunito notosansjp/NotoSansJP notosanssc/NotoSansSC \\
           notosanskr/NotoSansKR; do
      curl -LO "https://raw.githubusercontent.com/google/fonts/main/ofl/$f%5Bwght%5D.ttf"
  done
  curl -LO 'https://raw.githubusercontent.com/google/fonts/main/ofl/notosansarmenian/NotoSansArmenian%5Bwdth,wght%5D.ttf'
  cd .. && python3 tools/make_font.py src

These are variable fonts, and raylib's stb_truetype only ever draws a
font's default instance: the weight (and width) are pinned here. Needs
fonttools (pip install fonttools).

Keep NUNITO_RANGES in step with ccratch/src/ccr_app.c: g_font_ranges are
always drawn, g_font_more only for the letters a pack uses.
"""
import glob
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "assets", "fonts")
LANG = os.path.join(ROOT, "lang")

NUNITO_RANGES = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x017F),  # Latin-1, Latin Extended-A
    (0x0180, 0x024F),  # Latin Extended-B (Azerbaijani's schwa, Romanian...)
    (0x0250, 0x02FF),  # IPA, spacing modifiers (Uzbek's turned comma)
    (0x0400, 0x04FF),  # Cyrillic
    (0x0500, 0x052F),  # Cyrillic Supplement
    (0x2010, 0x2027),  # dashes, quotes, bullet, ellipsis
    (0x2030, 0x203A),  # per mille, angle quotes
    (0x2116, 0x2116),  # numero sign
]

# @font name -> (upstream file, axes to pin, characters always kept)
EXTRA = {
    "noto-jp": ("NotoSansJP[wght].ttf", {"wght": 600},
                [(0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF), (0xFF01, 0xFF5E)]),
    "noto-sc": ("NotoSansSC[wght].ttf", {"wght": 600}, [(0x3000, 0x303F), (0xFF01, 0xFF5E)]),
    "noto-kr": ("NotoSansKR[wght].ttf", {"wght": 600}, [(0x3000, 0x303F), (0xFF01, 0xFF5E)]),
    "noto-armenian": ("NotoSansArmenian[wdth,wght].ttf", {"wght": 600, "wdth": 100},
                      [(0x0530, 0x058F)]),
}


def in_nunito(cp):
    return any(a <= cp <= b for a, b in NUNITO_RANGES)


def read_pack(path):
    """(settings, text) of a .lang file: @name/@base/@font, and every string."""
    settings, text = {}, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = (x.strip() for x in line.split("=", 1))
            if key.startswith("@"):
                settings[key] = val
                if key == "@name":   # shown in the list of languages
                    text.append(val)
            else:
                text.append(val)
    return settings, "".join(text)


def pack_fonts():
    """@font name -> every character the packs that use it need."""
    packs = {os.path.basename(p)[:-5]: read_pack(p) for p in glob.glob(os.path.join(LANG, "*.lang"))}
    need = {}
    for code, (settings, text) in packs.items():
        font = settings.get("@font")
        base = settings.get("@base")
        if not font and base in packs:
            font = packs[base][0].get("@font")
        if not font:
            continue
        if font not in EXTRA:
            sys.exit(f"{code}.lang: @font {font} is not one of {', '.join(EXTRA)}")
        need.setdefault(font, set()).update(ord(c) for c in text if ord(c) > 0x7E and not in_nunito(ord(c)))
    return need


def make(src, dst, axes, cps, layout=("kern", "liga"), report=()):
    font = TTFont(src)
    if "fvar" in font:
        font = instancer.instantiateVariableFont(font, axes)
    opts = subset.Options()
    opts.layout_features = list(layout)
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(cps))
    sub.subset(font)
    font.save(dst)
    have = set(TTFont(dst).getBestCmap())
    missing = sorted(c for c in report if c not in have)
    print(f"{os.path.relpath(dst, ROOT)}: {len(have)} characters, {os.path.getsize(dst) // 1024} KiB"
          + (f"; not in {os.path.basename(src)}: {' '.join(chr(c) for c in missing[:40])}" if missing else ""))


def main(srcdir):
    make(os.path.join(srcdir, "Nunito[wght].ttf"), os.path.join(FONTS, "nunito-ccratch.ttf"), {"wght": 700},
         {c for a, b in NUNITO_RANGES for c in range(a, b + 1)})
    need = pack_fonts()
    for name, (src, axes, always) in sorted(EXTRA.items()):
        cps = need.get(name, set()) | {c for a, b in always for c in range(a, b + 1)}
        make(os.path.join(srcdir, src), os.path.join(FONTS, f"{name}-ccratch.ttf"), axes, cps, layout=(),
             report=need.get(name, ()))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
