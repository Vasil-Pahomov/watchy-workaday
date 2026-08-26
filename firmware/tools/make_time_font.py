#!/usr/bin/env python3
"""Generate src/board/font_time.h - the watch's three custom faces.

Adafruit GFX ships nothing larger than 24pt, and its bundled faces are wide
enough that the date and steps lines clip long before they are legible. Every
face here is sized to the widest string it can ever be asked to draw, so no
reachable state clips - which is why the string lists below are in the source
rather than a size someone liked the look of.

Only the glyph ranges actually used are emitted. That is what keeps a 77 px
clock, a 31 px label face and a 23 px UI face to a few kilobytes.

The typeface is Gilroy Regular, vendored at tools/fonts/ so a clean checkout can
regenerate this header without hunting for a system font. It is not a libre face
like the DejaVu it replaces: it is the project owner's licensed copy, and
redistributing this repository publicly is a licence question for them, not a
build question. Nothing else on the watch uses a font - the three faces below are
every glyph the panel can draw.

Run from firmware/:  python tools/make_time_font.py
Requires PIL.
"""
from PIL import Image, ImageDraw, ImageFont

TTF = "tools/fonts/Gilroy-Regular.ttf"
OUT = "src/board/font_time.h"

# The clock. The dash matters: an unset clock draws "--:--", and Adafruit GFX
# silently skips any glyph outside a font's range, so a digits-only face would
# leave a bare colon on an otherwise empty screen.
TIME_FIRST, TIME_LAST = 0x2D, 0x3A
TIME_LINES, TIME_BUDGET = ["88:88"], 188

# The date line, centred, and the step count in the bottom-left corner. The
# corner gets a narrower budget than the full width on purpose: a five-digit day
# has to stay in its own half of the screen rather than run under the clock.
LABEL_FIRST, LABEL_LAST = 0x20, 0x7A
LABEL_LINES = ["Wed 12 Aug", "set time", "Wed 28 Sept"]
LABEL_BUDGET = 190
LABEL_CORNER_LINES = ["88888"]
LABEL_CORNER_BUDGET = 96

# Menus, banners and the mode tag - every screen that is not the watchface.
# drawBanner() starts at x=10, so the budget is the width left of the right edge.
# "yesterday 88888" is the widest of them and sets the size.
SMALL_FIRST, SMALL_LAST = 0x20, 0x7A
SMALL_LINES = ["yesterday 88888", "clock write fail", "sync unfinished",
               "no step data", "sensor stopped", "never synced"]
SMALL_BUDGET = 180


def largest_that_fits(constraints):
    """Largest size at which every (lines, budget) pair still fits."""
    lo, hi, best = 8, 110, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        f = ImageFont.truetype(TTF, mid)
        if all(max(f.getlength(s) for s in lines) <= budget for lines, budget in constraints):
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    return best


def render(font, ch, pad=90):
    img = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(img).text((pad, pad * 2), ch, fill=255, font=font, anchor="ls")
    # 128 would be the obvious threshold and is wrong for this face: Gilroy
    # Regular's stems land near half coverage at the 23 px UI size, so a
    # mid-grey cut drops them and prints a stencil. 100 keeps the stem and costs
    # a fraction of a pixel of weight at the two larger sizes.
    img = img.point(lambda v: 255 if v >= 100 else 0)
    bbox = img.getbbox()
    if bbox is None:
        return b"", 0, 0, 0, 0
    x0, y0, x1, y1 = bbox
    crop = img.crop(bbox).point(lambda v: 1 if v else 0)
    try:
        px = crop.get_flattened_data()
    except AttributeError:
        px = crop.getdata()
    bits = "".join(str(p) for p in px)
    bits += "0" * (-len(bits) % 8)
    data = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
    return data, x1 - x0, y1 - y0, x0 - pad, y0 - pad * 2


def build(name, first, last, size):
    font = ImageFont.truetype(TTF, size)
    asc, desc = font.getmetrics()
    blob, glyphs = bytearray(), []
    for code in range(first, last + 1):
        data, w, h, xo, yo = render(font, chr(code))
        glyphs.append((len(blob), w, h, round(font.getlength(chr(code))), xo, yo, code))
        blob += data
    out = ["const uint8_t %sBitmaps[] PROGMEM = {" % name]
    for i in range(0, len(blob), 12):
        out.append("    " + " ".join("0x%02X," % b for b in blob[i:i + 12]))
    out += ["};", "", "const GFXglyph %sGlyphs[] PROGMEM = {" % name]
    for off, w, h, adv, xo, yo, code in glyphs:
        out.append("    {%5d, %3d, %3d, %3d, %3d, %4d},   // 0x%02X '%s'"
                   % (off, w, h, adv, xo, yo, code, chr(code) if code > 0x20 else " "))
    out += ["};", "",
            "const GFXfont %s PROGMEM = {" % name,
            "    (uint8_t *)%sBitmaps, (GFXglyph *)%sGlyphs," % (name, name),
            "    0x%02X, 0x%02X, %d};" % (first, last, asc + desc), ""]
    return out, len(blob), max((g[2] for g in glyphs), default=0), asc + desc


def main():
    t_size = largest_that_fits([(TIME_LINES, TIME_BUDGET)])
    l_size = largest_that_fits([(LABEL_LINES, LABEL_BUDGET),
                                (LABEL_CORNER_LINES, LABEL_CORNER_BUDGET)])
    s_size = largest_that_fits([(SMALL_LINES, SMALL_BUDGET)])
    t_lines, t_bytes, t_tall, _ = build("WorkadayTime", TIME_FIRST, TIME_LAST, t_size)
    l_lines, l_bytes, _, l_adv = build("WorkadayLabel", LABEL_FIRST, LABEL_LAST, l_size)
    s_lines, s_bytes, _, s_adv = build("WorkadaySmall", SMALL_FIRST, SMALL_LAST, s_size)
    head = [
        "// GENERATED - do not edit by hand. Regenerate with:",
        "//     python tools/make_time_font.py",
        "//",
        "// Three faces, all Gilroy Regular, each sized to the widest string it can ever",
        "// be asked to draw rather than to a size that looked right:",
        "//",
        "//   WorkadayTime  0x2D..0x3A at %d px. Digits, colon and dash; tallest glyph" % t_size,
        "//     %d px against 34 px for GFX's largest bundled face. The dash is not" % t_tall,
        "//     decoration - an unset clock draws \"--:--\", and Adafruit GFX silently",
        "//     skips glyphs outside a font's range, so a digits-only face would leave a",
        "//     bare colon on an otherwise empty screen.",
        "//   WorkadayLabel 0x20..0x7A at %d px, yAdvance %d. The centred date line and" % (l_size, l_adv),
        "//     the step count in the bottom-left corner.",
        "//   WorkadaySmall 0x20..0x7A at %d px, yAdvance %d. Menus, banners and the mode" % (s_size, s_adv),
        "//     tag - everything that is not the watchface. It replaces FreeMonoBold9pt7b,",
        "//     so no bundled GFX face is linked in at all.",
        "//",
        "// %d bytes of bitmap in total." % (t_bytes + l_bytes + s_bytes),
        "#pragma once",
        "",
        "#include <Adafruit_GFX.h>",
        "",
    ]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head + t_lines + l_lines + s_lines))
    print("%s: time=%dpx (%dB)  label=%dpx (%dB)  small=%dpx (%dB)  total=%dB"
          % (OUT, t_size, t_bytes, l_size, l_bytes, s_size, s_bytes,
             t_bytes + l_bytes + s_bytes))


main()
