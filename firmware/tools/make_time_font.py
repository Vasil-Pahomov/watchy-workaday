#!/usr/bin/env python3
"""Generate src/board/font_time.h - the watch's three custom faces.

Adafruit GFX ships nothing larger than 24pt, and its bundled faces are wide
enough that the date and steps lines clip long before they are legible. Every
face here is sized to the widest string it can ever be asked to draw, so no
reachable state clips - which is why the string lists below are in the source
rather than a size someone liked the look of.

Only the glyph ranges actually used are emitted. That is what keeps a 76 px
clock, a 30 px label face and a 21 px UI face to a few kilobytes.

The typeface is Gilroy ExtraBold, vendored at tools/fonts/ so a clean checkout can
regenerate this header without hunting for a system font. It is not a libre face
like the DejaVu it replaces: it is the project owner's licensed copy, and
redistributing this repository publicly is a licence question for them, not a
build question. Nothing else on the watch uses a font - the three faces below are
every glyph the panel can draw.

Run from firmware/:  python tools/make_time_font.py
Requires PIL.
"""
from PIL import Image, ImageDraw, ImageFont

TTF = "tools/fonts/Gilroy-ExtraBold.ttf"
OUT = "src/board/font_time.h"


def widest_digit():
    """The widest digit in this face - which is not '8', and is not a given.

    Gilroy ExtraBold's figures are proportional: '0' is five pixels wider than
    '8' at the clock size. Every placeholder below that stands for "any digit"
    is built from this rather than typed, so changing the typeface cannot
    quietly leave a face sized against a narrower digit than it will be drawn.
    """
    font = ImageFont.truetype(TTF, 64)
    return max("0123456789", key=font.getlength)


DIGIT = widest_digit()


def clock_strings():
    """Every string drawTimeLarge() can be handed, so the clock is sized against
    its real maximum rather than against a guess at it.

    "88:88" stood in for that maximum until the face changed, and it is exactly
    the kind of guess proportional figures break: a clock sized against 88:88
    fits every hour except the ones containing a zero, so it would look right
    all day and run off both edges at 20:00. core::formatTime() drops the hour's
    leading zero and keeps the minutes', in both 12h and 24h mode - 12h's hours
    1..12 are a subset of 24h's 0..23, so one enumeration covers both.
    """
    out = {"--:--"}
    for hour in range(24):
        for minute in range(60):
            out.add("%d:%02d" % (hour, minute))
    return sorted(out)


# The clock. The dash matters: an unset clock draws "--:--", and Adafruit GFX
# silently skips any glyph outside a font's range, so a digits-only face would
# leave a bare colon on an otherwise empty screen.
TIME_FIRST, TIME_LAST = 0x2D, 0x3A
# The clock is measured differently from the two faces below, and the difference
# is the whole point of the number: the other two must not clip, so an advance
# width - which includes the side bearings - is the safe over-estimate. The clock
# must *touch* both edges at its widest, so what has to equal the panel is the
# inked span, and side bearings would leave exactly the gap this is sized to
# close. largest_ink_that_fits() below measures what drawCentred() will actually
# paint. 200 is the panel, not a margin inside it.
TIME_LINES, TIME_BUDGET = clock_strings(), 200

# The date line, centred, and the step count in the bottom-left corner. The
# corner gets a narrower budget than the full width on purpose: a five-digit day
# has to stay in its own half of the screen rather than run under the clock. It
# starts at column 0 now rather than at an 8 px margin, so those 96 pixels are
# measured from the panel's left edge.
LABEL_FIRST, LABEL_LAST = 0x20, 0x7A
LABEL_LINES = ["Wed 12 Aug", "set time", "Wed 28 Sept"]
LABEL_BUDGET = 190
LABEL_CORNER_LINES = [DIGIT * 5]
LABEL_CORNER_BUDGET = 96

# Menus, banners and the mode tag - every screen that is not the watchface.
# drawBanner() starts at x=10, so the budget is the width left of the right edge.
# "yesterday 00000" is the widest of them and sets the size.
SMALL_FIRST, SMALL_LAST = 0x20, 0x7A
SMALL_LINES = ["yesterday " + DIGIT * 5, "clock write fail", "sync unfinished",
               "no step data", "sensor stopped", "never synced",
               # The theme item's two labels, drawn by drawMenu() rather than
               # drawBanner(). That starts at x=12, so its true budget is 188 and
               # 180 is 8 px conservative for these two (10 px for the banners
               # above). They are listed anyway because this list is meant to be
               # every string the face can be asked to draw, not only the ones
               # that set the size - and neither is: at the 21 px this search
               # settles on they measure 150 and 143.
               "White on black", "Black on white",
               # The Find phone screen (PROTOCOL.md section 4.1): its menu label,
               # every status and outcome it can show, and the widest reachable
               # progress line - two minutes, and an attempt counter saturated at
               # 255. None of them sets the size either; the widest, "no phone
               # found", is 156 px. "stopped on phone" was the first wording for
               # the dismissed case and measured 182 - two pixels over - which
               # would have shrunk every screen's UI face by a pixel to fit one
               # message. Hence "phone found", which is also the plainer thing to
               # say.
               "Find phone", "searching", "connected", "phone ringing",
               "phone vibrating",
               "phone found", "no phone found", "battery too low", "radio failed",
               "not available", "interrupted", "stopped",
               "2:00  try 255",
               # The Sync screen (PROTOCOL.md section 5.1), which narrates the
               # window the wearer just opened rather than reporting the previous
               # one. It shares "searching", "connected", "no phone found" and
               # "radio failed" with the list above - the same words for the same
               # thing, which is the point of them being shared - and adds its own
               # endings plus section 3.2's result codes. None sets the size; the
               # widest, "clock write fail", is already listed at the top.
               "Sync", "synchronized", "watch busy", "sync failed",
               "last sync ok", "bad message", "app mismatch", "bad time sent"]
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


def glyph_metrics(size, chars):
    """{char: (width, advance, xOffset)} at `size`, from the rendered bitmaps."""
    font = ImageFont.truetype(TTF, size)
    out = {}
    for ch in set(chars):
        _, w, _, xo, _ = render(font, ch)
        out[ch] = (w, round(font.getlength(ch)), xo)
    return out


def ink_width(metrics, text):
    """Width of the inked span of `text`, in the pixels the panel will light.

    Adafruit GFX's getTextBounds() arithmetic, run on the glyphs this file is
    about to emit: advances between characters, and the outermost glyphs' own
    bearings at the two ends. It measures the bitmaps rather than the outline
    they came from, because the bitmaps are what the panel gets.
    """
    x, lo, hi = 0, None, None
    for ch in text:
        w, adv, xo = metrics[ch]
        if w:
            lo = x + xo if lo is None else min(lo, x + xo)
            hi = x + xo + w - 1 if hi is None else max(hi, x + xo + w - 1)
        x += adv
    return 0 if lo is None else hi - lo + 1


def widest(size, lines):
    """(width, line) of whichever of `lines` inks widest at `size`."""
    metrics = glyph_metrics(size, "".join(lines))
    return max((ink_width(metrics, s), s) for s in lines)


def largest_ink_that_fits(lines, budget):
    """Largest size at which every line's inked span still fits `budget`."""
    lo, hi, best = 8, 110, 8
    while lo <= hi:
        mid = (lo + hi) // 2
        if widest(mid, lines)[0] <= budget:
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    return best


def render(font, ch, pad=90):
    img = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(img).text((pad, pad * 2), ch, fill=255, font=font, anchor="ls")
    # A plain mid-grey cut. The 100 that used to be here was propping up Gilroy
    # Regular, whose stems landed near half coverage at the 23 px UI size and
    # came out as a stencil; ExtraBold has no stem thin enough to be at risk, and
    # a low threshold on it only smears the counters shut.
    img = img.point(lambda v: 255 if v >= 128 else 0)
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
    t_size = largest_ink_that_fits(TIME_LINES, TIME_BUDGET)
    l_size = largest_that_fits([(LABEL_LINES, LABEL_BUDGET),
                                (LABEL_CORNER_LINES, LABEL_CORNER_BUDGET)])
    s_size = largest_that_fits([(SMALL_LINES, SMALL_BUDGET)])
    t_wide, t_widest = widest(t_size, TIME_LINES)
    t_lines, t_bytes, t_tall, _ = build("WorkadayTime", TIME_FIRST, TIME_LAST, t_size)
    l_lines, l_bytes, _, l_adv = build("WorkadayLabel", LABEL_FIRST, LABEL_LAST, l_size)
    s_lines, s_bytes, _, s_adv = build("WorkadaySmall", SMALL_FIRST, SMALL_LAST, s_size)
    head = [
        "// GENERATED - do not edit by hand. Regenerate with:",
        "//     python tools/make_time_font.py",
        "//",
        "// Three faces, all Gilroy ExtraBold, each sized to the widest string it can",
        "// ever be asked to draw rather than to a size that looked right:",
        "//",
        "//   WorkadayTime  0x2D..0x3A at %d px. Digits, colon and dash; tallest glyph" % t_size,
        "//     %d px against 34 px for GFX's largest bundled face. The widest clock" % t_tall,
        "//     it can be asked to draw is \"%s\", which inks %d of the panel's 200" % (t_widest, t_wide),
        "//     columns: the clock is sized to reach both edges, so that is the",
        "//     constraint rather than a by-product. The dash is not",
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
