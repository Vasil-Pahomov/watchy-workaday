#!/usr/bin/env python3
"""Generate src/board/font_time.h - the watchface's two custom faces.

Adafruit GFX ships nothing larger than 24pt, and its bundled faces are wide
enough that the date and steps lines clip long before they are legible. Both
faces here are sized to the widest string they can ever be asked to draw, so no
reachable state clips - which is why the string lists below are in the source
rather than a size someone liked the look of.

Only the glyph ranges actually used are emitted. That is what keeps a 59 px clock
and a 26 px label face to a few kilobytes.

Run from Firmware/:  python tools/make_time_font.py
Requires PIL. DejaVu is used because its licence permits embedding the rendered
glyphs in a firmware image, which Arial and Segoe UI do not.
"""
from PIL import Image, ImageDraw, ImageFont

TTF = r"C:\Windows\Fonts\DejaVuSans-Bold.ttf"
OUT = "src/board/font_time.h"

# The clock. The dash matters: an unset clock draws "--:--", and Adafruit GFX
# silently skips any glyph outside a font's range, so a digits-only face would
# leave a bare colon on an otherwise empty screen.
TIME_FIRST, TIME_LAST = 0x2D, 0x3A
TIME_WIDEST, TIME_BUDGET = "88:88", 188

# The date and steps lines. Every string either can hold.
LABEL_FIRST, LABEL_LAST = 0x20, 0x7A
LABEL_LINES = ["Wed 12 Aug", "set time", "88888 steps", "no step data", "Wed 28 Sept"]
LABEL_BUDGET = 190


def largest_that_fits(lines, budget):
    lo, hi, best = 8, 90, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        f = ImageFont.truetype(TTF, mid)
        if max(f.getlength(s) for s in lines) <= budget:
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    return best


def render(font, ch, pad=90):
    img = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(img).text((pad, pad * 2), ch, fill=255, font=font, anchor="ls")
    bbox = img.point(lambda v: 255 if v >= 128 else 0).getbbox()
    if bbox is None:
        return b"", 0, 0, 0, 0
    x0, y0, x1, y1 = bbox
    crop = img.crop(bbox).point(lambda v: 1 if v >= 128 else 0)
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
    t_size = largest_that_fits([TIME_WIDEST], TIME_BUDGET)
    l_size = largest_that_fits(LABEL_LINES, LABEL_BUDGET)
    t_lines, t_bytes, t_tall, _ = build("WorkadayTime", TIME_FIRST, TIME_LAST, t_size)
    l_lines, l_bytes, _, l_adv = build("WorkadayLabel", LABEL_FIRST, LABEL_LAST, l_size)
    head = [
        "// GENERATED - do not edit by hand. Regenerate with:",
        "//     python tools/make_time_font.py",
        "//",
        "// Two faces, both DejaVu Sans Bold, each sized to the widest string it can ever",
        "// be asked to draw rather than to a size that looked right:",
        "//",
        "//   WorkadayTime  0x2D..0x3A at %d px. Digits, colon and dash; tallest glyph" % t_size,
        "//     %d px against 34 px for GFX's largest bundled face. The dash is not" % t_tall,
        "//     decoration - an unset clock draws \"--:--\", and Adafruit GFX silently",
        "//     skips glyphs outside a font's range, so a digits-only face would leave a",
        "//     bare colon on an otherwise empty screen.",
        "//   WorkadayLabel 0x20..0x7A at %d px. The date and steps lines; yAdvance %d" % (l_size, l_adv),
        "//     against 18 for FreeMonoBold9pt7b, sized so \"no step data\" still fits.",
        "//",
        "// %d bytes of bitmap in total." % (t_bytes + l_bytes),
        "#pragma once",
        "",
        "#include <Adafruit_GFX.h>",
        "",
    ]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head + t_lines + l_lines))
    print("%s: time=%dpx (%dB)  label=%dpx (%dB)  total=%dB"
          % (OUT, t_size, t_bytes, l_size, l_bytes, t_bytes + l_bytes))


main()
