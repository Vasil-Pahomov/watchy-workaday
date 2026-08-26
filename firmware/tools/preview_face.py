#!/usr/bin/env python3
"""Render the watch screens to a PNG, exactly as the panel will show them.

This is not a mock-up. It reads the generated faces out of src/board/font_time.h
and the layout constants out of src/board/display.cpp, then walks Adafruit GFX's
own glyph blitting and getTextBounds arithmetic, so what comes out is the
framebuffer the firmware would push - a layout can be looked at before it costs
a flash cycle and a wrist.

Two things keep it honest rather than merely close:

  * Every constant it needs is read by name from display.cpp. Rename or delete
    one and this exits with an error instead of quietly drawing the old face.
  * It clips at the panel edge like the hardware does, and says so. A string
    that runs off the 200th column is a font-budget bug, and it should surface
    here rather than on the watch.

Two directories, because the renders have two different jobs:

  * docs/preview/ is checked in. README.md shows those two PNGs so a reader can
    see the face without owning the hardware. They are generated, not drawn -
    regenerate them with --all rather than editing them, and do it in the same
    commit as any layout change, or the repository shows a watch that no longer
    exists.
  * preview/ is scratch, git-ignored, and where an ad-hoc render lands while you
    are pushing a baseline around.

Run from firmware/:
    python tools/preview_face.py --all                 # rebuild docs/preview/
    python tools/preview_face.py --hero                # the face, framed
    python tools/preview_face.py --sheet               # every screen and state
    python tools/preview_face.py --battery 8 --steps 412 --time 9:05
Requires PIL.
"""
import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

OUT_DIR = "docs/preview"   # tracked; what README.md shows
SCRATCH = "preview"        # git-ignored; ad-hoc renders
# Thirteen panels at 4x would be a 3300 px wide file for a README to scale back
# down. Half that keeps every pixel on an integer boundary and the file at 50 kB.
kSheetScale = 2
# The same face the watch wears, for the captions around it. Vendored beside the
# generator, so this works from a clean checkout like everything else here.
GILROY = "tools/fonts/Gilroy-Regular.ttf"

FONT_HEADER = "src/board/font_time.h"
DISPLAY_CPP = "src/board/display.cpp"
DISPLAY_H = "src/board/display.h"
BOARD_H = "src/board/board_v20.h"

# The constants this tool needs from the firmware, and where each one lives.
# Missing names are a hard error - see the module docstring.
NEEDED = {
    BOARD_H: ["kDisplayWidth", "kDisplayHeight"],
    DISPLAY_H: ["kBatteryTrackPixels"],
    DISPLAY_CPP: ["kMargin", "kStatusBaseline", "kDateBaseline", "kTimeBaseline",
                  "kStepsBaseline", "kGaugeHeight", "kGaugeBorder", "kGaugeBodyWidth",
                  "kGaugeCapWidth", "kGaugeCapHeight", "kGaugeTop", "kGaugeLeft"],
}

INK, PAPER = 1, 0


# ── reading the firmware ────────────────────────────────────────────────────

def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def constants():
    """Every constexpr integer the layout is made of, evaluated in order."""
    out = {}
    for path, names in NEEDED.items():
        text = read(path)
        for name in names:
            m = re.search(r"constexpr\s+\w+\s+%s\s*=\s*([^;]+);" % name, text)
            if not m:
                sys.exit("%s: %s is gone. The preview cannot mirror a layout it "
                         "cannot find - update tools/preview_face.py." % (path, name))
            # C++ integer division truncates; Python's / does not.
            expr = m.group(1).strip().replace("/", "//")
            out[name] = int(eval(expr, {"__builtins__": {}}, dict(out)))
    return out


def fonts():
    """{name: {first, last, yAdvance, glyphs: [(off,w,h,adv,xo,yo)], bitmaps}}."""
    text = read(FONT_HEADER)
    out = {}
    for name in re.findall(r"const GFXfont (\w+) PROGMEM", text):
        blob = re.search(r"const uint8_t %sBitmaps\[\] PROGMEM = \{(.*?)\};" % name,
                         text, re.S).group(1)
        bitmaps = [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", blob)]
        table = re.search(r"const GFXglyph %sGlyphs\[\] PROGMEM = \{(.*?)\};" % name,
                          text, re.S).group(1)
        glyphs = [tuple(int(v) for v in row)
                  for row in re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*"
                                        r"(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", table)]
        first, last, yadv = re.search(
            r"const GFXfont %s PROGMEM = \{.*?0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*(\d+)\}"
            % name, text, re.S).groups()
        out[name] = {"first": int(first, 16), "last": int(last, 16),
                     "yAdvance": int(yadv), "glyphs": glyphs, "bitmaps": bitmaps}
    return out


# ── Adafruit GFX, reimplemented ─────────────────────────────────────────────

def glyph_of(font, ch):
    code = ord(ch)
    if code < font["first"] or code > font["last"]:
        return None  # GFX skips it silently, and so must the preview.
    return font["glyphs"][code - font["first"]]


def text_bounds(font, text):
    """getTextBounds() at the origin: (x1, y1, w, h) of the inked span."""
    minx, miny, maxx, maxy = 1 << 15, 1 << 15, -(1 << 15), -(1 << 15)
    x = 0
    for ch in text:
        g = glyph_of(font, ch)
        if g is None:
            continue
        _, gw, gh, adv, xo, yo = g
        if gw and gh:
            minx, miny = min(minx, x + xo), min(miny, yo)
            maxx, maxy = max(maxx, x + xo + gw - 1), max(maxy, yo + gh - 1)
        x += adv
    if maxx < minx:
        return 0, 0, 0, 0
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def draw_text(fb, font, text, x, y, colour=INK, label=""):
    """drawChar() per glyph. `y` is the baseline, as it is on the device."""
    w, h = len(fb[0]), len(fb)
    for ch in text:
        g = glyph_of(font, ch)
        if g is None:
            continue
        off, gw, gh, adv, xo, yo = g
        bit = 0
        bits = 0
        for yy in range(gh):
            for xx in range(gw):
                if not (bit & 7):
                    bits = font["bitmaps"][off + (bit >> 3)]
                bit += 1
                if bits & (0x80 >> ((bit - 1) & 7)):
                    px, py = x + xo + xx, y + yo + yy
                    if 0 <= px < w and 0 <= py < h:
                        fb[py][px] = colour
                    else:
                        clipped.add(label or text)
        x += adv
    return x


def draw_centred(fb, font, text, baseline, width, label=""):
    x1, _, w, _ = text_bounds(font, text)
    # Truncating division, like C++.
    x = int((width - w) / 2) - x1
    draw_text(fb, font, text, x, baseline, label=label)


def fill_rect(fb, x, y, w, h, colour):
    fh, fw = len(fb), len(fb[0])
    for py in range(max(0, y), min(fh, y + h)):
        for px in range(max(0, x), min(fw, x + w)):
            fb[py][px] = colour


# ── the screens, mirroring app/screens.cpp draw() ───────────────────────────

clipped = set()


def blank(k):
    return [[PAPER] * k["kDisplayWidth"] for _ in range(k["kDisplayHeight"])]


def gauge_fill_pixels(percent, track):
    """core::gaugeFillPixels(), the one piece of logic the host tests cover."""
    if track == 0 or percent == 0:
        return 0
    if percent >= 100:
        return track
    filled = (percent * track + 50) // 100
    return 1 if filled == 0 else filled


def draw_gauge(fb, k, percent):
    fill_rect(fb, k["kGaugeLeft"], k["kGaugeTop"], k["kGaugeBodyWidth"], k["kGaugeHeight"], INK)
    fill_rect(fb, k["kGaugeLeft"] + k["kGaugeBorder"], k["kGaugeTop"] + k["kGaugeBorder"],
              k["kBatteryTrackPixels"], k["kGaugeHeight"] - 2 * k["kGaugeBorder"], PAPER)
    fill_rect(fb, k["kGaugeLeft"] + k["kGaugeBodyWidth"],
              k["kGaugeTop"] + (k["kGaugeHeight"] - k["kGaugeCapHeight"]) // 2,
              k["kGaugeCapWidth"], k["kGaugeCapHeight"], INK)
    fill = gauge_fill_pixels(percent, k["kBatteryTrackPixels"])
    if fill:
        fill_rect(fb, k["kGaugeLeft"] + k["kGaugeBorder"], k["kGaugeTop"] + k["kGaugeBorder"],
                  fill, k["kGaugeHeight"] - 2 * k["kGaugeBorder"], INK)


def clock(text):
    """core::formatTime()'s padding rule, so a typed "09:41" previews as the watch
    would actually draw it. The hour drops its leading zero, the minutes keep
    theirs."""
    hour, _, minute = text.partition(":")
    if len(hour) == 2 and hour.startswith("0"):
        hour = hour[1:]
    return hour + ":" + minute if minute else text


def face(k, f, time="14:32", date="Wed 12 Aug", steps="8432", battery=76, mode=""):
    time = clock(time)
    fb = blank(k)
    if mode:
        draw_text(fb, f["WorkadaySmall"], mode, k["kMargin"], k["kStatusBaseline"], label="mode")
    draw_gauge(fb, k, battery)
    draw_centred(fb, f["WorkadayTime"], time, k["kTimeBaseline"], k["kDisplayWidth"], "time")
    draw_centred(fb, f["WorkadayLabel"], date, k["kDateBaseline"], k["kDisplayWidth"], "date")
    if steps:
        draw_text(fb, f["WorkadayLabel"], steps, k["kMargin"], k["kStepsBaseline"], label="steps")
    return fb


def menu(k, f, items=("Steps", "Sync"), selected=0):
    fb = blank(k)
    for i, item in enumerate(items):
        y = 20 + i * 30
        colour = INK
        if i == selected:
            fill_rect(fb, 0, y, k["kDisplayWidth"], 30, INK)
            colour = PAPER
        draw_text(fb, f["WorkadaySmall"], item, 12, y + 21, colour, label="menu:" + item)
    return fb


def banner(k, f, line1, line2, battery=76):
    fb = blank(k)
    draw_gauge(fb, k, battery)
    draw_text(fb, f["WorkadaySmall"], line1, 10, 95, label="banner:" + line1)
    draw_text(fb, f["WorkadaySmall"], line2, 10, 120, label="banner:" + line2)
    return fb


# ── output ──────────────────────────────────────────────────────────────────

BACKDROP, BEZEL, CAPTION, DIM = (24, 24, 27), (58, 58, 64), (238, 238, 242), (150, 150, 158)


def to_image(fb, scale):
    h, w = len(fb), len(fb[0])
    img = Image.new("L", (w, h), 0)
    img.putdata([255 if v else 0 for row in fb for v in row])
    return img.resize((w * scale, h * scale), Image.NEAREST)


def caption_font(size):
    try:
        return ImageFont.truetype(GILROY, size)
    except OSError:
        sys.exit("%s is missing - it is what the watch and these captions are set "
                 "in. Restore it or point GILROY somewhere else." % GILROY)


def save(img, path):
    """Palette-quantised and optimised: these live in the repository, so their
    size is a cost every clone pays."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    if img.mode == "L" and img.getextrema() == (0, 255):
        img = img.convert("1")
    elif img.mode == "RGB":
        img = img.quantize(colors=32, method=Image.MEDIANCUT)
    img.save(path, optimize=True)
    print("%s  (%dx%d, %d bytes)" % (path, img.width, img.height, os.path.getsize(path)))


def sheet(shots, scale):
    """One PNG of every screen, captioned, on the grey of a desk."""
    font = caption_font(9 * scale)
    caption_h = 13 * scale
    cols = min(4, len(shots))
    rows = (len(shots) + cols - 1) // cols
    w = h = 200 * scale
    pad = 14 * scale // 2
    out = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad + caption_h) + pad), BACKDROP)
    pen = ImageDraw.Draw(out)
    for i, (title, fb) in enumerate(shots):
        x = pad + (i % cols) * (w + pad)
        y = pad + (i // cols) * (h + pad + caption_h)
        out.paste(to_image(fb, scale).convert("RGB"), (x, y))
        pen.text((x + w // 2, y + h + caption_h // 2 + 2), title, font=font, fill=DIM,
                 anchor="mm")
    return out


def gauge_crop(k, percent, scale):
    """Just the corner the gauge lives in, blown up."""
    fb = blank(k)
    draw_gauge(fb, k, percent)
    pad = 3
    x0, y0 = k["kGaugeLeft"] - pad, k["kGaugeTop"] - pad
    x1 = k["kGaugeLeft"] + k["kGaugeBodyWidth"] + k["kGaugeCapWidth"] + pad
    y1 = k["kGaugeTop"] + k["kGaugeHeight"] + pad
    return to_image([row[x0:x1] for row in fb[y0:y1]], scale).convert("RGB")


def hero(k, f, scale=3, levels=(100, 75, 50, 25, 5, 0)):
    """The face in something that reads as a watch, over the gauge full to flat.
    This is the one README.md opens with."""
    face_img = to_image(face(k, f, "9:03", "Wed 9 Sep", "8432", 76), scale).convert("RGB")
    gauges = [(p, gauge_crop(k, p, scale)) for p in levels]
    gw, gh = gauges[0][1].size
    side = 200 * scale
    strip_w = len(gauges) * gw + (len(gauges) - 1) * 22
    bezel = 34
    width = max(side + 2 * 90, strip_w + 2 * 60)
    height = 84 + side + 2 * bezel + 74 + gh + 40 + 46

    out = Image.new("RGB", (width, height), BACKDROP)
    pen = ImageDraw.Draw(out)
    pen.text((width // 2, 40), "Workaday \u00b7 Watchy 2.0 \u00b7 200\u00d7200 e-paper",
             font=caption_font(30), fill=CAPTION, anchor="mm")

    bx, by = (width - side - 2 * bezel) // 2, 84
    pen.rounded_rectangle([bx, by, bx + side + 2 * bezel, by + side + 2 * bezel],
                          radius=46, fill=BEZEL)
    out.paste(face_img, (bx + bezel, by + bezel))

    sy = by + side + 2 * bezel + 60
    pen.text((width // 2, sy - 16), "battery gauge, top-right corner",
             font=caption_font(24), fill=DIM, anchor="mm")
    sx = (width - strip_w) // 2
    for percent, img in gauges:
        out.paste(img, (sx, sy + 14))
        pen.text((sx + gw // 2, sy + 14 + gh + 24), "%d%%" % percent,
                 font=caption_font(24), fill=CAPTION, anchor="mm")
        sx += gw + 22
    return out


def every_screen(k, f):
    """Every state the firmware can put on the panel, including the ones that are
    awkward to reach on a wrist: a flat cell, an unset clock, a dead step sensor,
    Recovery mode. Reaching them here is the point - nobody drains a battery to
    check that 0 % draws."""
    return [
        ("watchface", face(k, f, "14:32", "Wed 12 Aug", "8432", 76)),
        ("single-digit hour and day", face(k, f, "9:03", "Wed 9 Sep", "77", 88)),
        ("battery 100%", face(k, f, "9:41", "Mon 3 Nov", "142", 100)),
        ("battery 50%", face(k, f, "12:00", "Tue 4 Nov", "6015", 50)),
        ("battery 4%, critical", face(k, f, "23:58", "Wed 5 Nov", "11207", 4)),
        ("battery 0%, flat", face(k, f, "6:30", "Thu 6 Nov", "0", 0)),
        ("clock never set", face(k, f, "--:--", "set time", "--", 61)),
        ("step sensor stopped", face(k, f, "17:20", "Fri 7 Nov", "--", 61)),
        ("recovery mode", face(k, f, "8:15", "Sat 8 Nov", "930", 33, "RECOV")),
        ("menu", menu(k, f)),
        ("menu, Sync selected", menu(k, f, selected=1)),
        ("Steps screen", banner(k, f, "8432 steps", "yesterday 11207")),
        ("Sync screen", banner(k, f, "Sync", "last sync ok")),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default="",
                    help="default: docs/preview/ for --hero and --sheet, preview/ otherwise")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--time", default="14:32")
    ap.add_argument("--date", default="Wed 12 Aug")
    ap.add_argument("--steps", default="8432")
    ap.add_argument("--battery", type=int, default=76)
    ap.add_argument("--mode", default="", help='"SAFE" or "RECOV"; empty is Normal')
    ap.add_argument("--sheet", action="store_true", help="every screen and state")
    ap.add_argument("--hero", action="store_true", help="the face framed, over the gauge sweep")
    ap.add_argument("--all", action="store_true",
                    help="rebuild every PNG README.md shows, into " + OUT_DIR)
    args = ap.parse_args()

    k, f = constants(), fonts()

    if args.all:
        save(hero(k, f), os.path.join(OUT_DIR, "hero.png"))
        save(sheet(every_screen(k, f), kSheetScale), os.path.join(OUT_DIR, "screens.png"))
    elif args.hero:
        save(hero(k, f), args.out or os.path.join(OUT_DIR, "hero.png"))
    elif args.sheet:
        save(sheet(every_screen(k, f), max(1, args.scale // 2)),
             args.out or os.path.join(OUT_DIR, "screens.png"))
    else:
        save(to_image(face(k, f, args.time, args.date, args.steps, args.battery, args.mode),
                      args.scale),
             args.out or os.path.join(SCRATCH, "watchface.png"))

    if clipped:
        print("CLIPPED at the panel edge, which the watch will do too: %s"
              % ", ".join(sorted(clipped)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
