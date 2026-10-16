#!/usr/bin/env python3
"""
mkartwork.py - build the pico-bootLoader menu tiles of pico-1943.

    tools/capture_footage.sh ~/roms/arcade /tmp/1943_footage
    tools/mkartwork.py --footage /tmp/1943_footage --bootloader ../pico-bootLoader/emu/assets

writes DIR/themes/0/1943.png (640x480) and DIR/themes/1/1943.png (320x240).
Run the bootloader's tools/png2raw.py on the two PNGs afterwards to build
their .444/.555 caches. The screensaver image built into the firmware,
DefaultSS*.c, is made by tools/mkdefaultss.py.

The ROM sets are not part of this project, so neither are pictures of the
games: they are taken from your own ROM sets by the host harness
(tools/capture_footage.sh), then laid out here. The footage folder holds five
320x240 PPM frames with Tate mode Off (the game area is x 48-271):

    title.ppm      theme 0: the 1943 logo, top of the badge
    kai-title.ppm  theme 0: the 1943 Kai logo, bottom of the badge
    left.ppm       theme 1: left panel
    middle.ppm     theme 1: middle panel
    right.ppm      theme 1: right panel

The tiles follow the two themes of pico-bootLoader, with the geometry of its
tools/make_category_art.py, as pico-dragonslair's tools/mkartwork.py does:

    theme 0   640x480: flat saturated ground, a huge ghosted silhouette (a
              P-38 Lightning, the player's plane), a boxed badge on the left,
              the wordmark and a rule
    theme 1   320x240: three slanted panels of game footage, white wordmark
              across the middle

Requires numpy and ffmpeg (with librsvg). Derived from pico-dragonslair's
tools/mkartwork.py.
"""

import argparse
import base64
import os
import subprocess
import sys
import tempfile

import numpy as np


def load_footage(footage, name):
    path = os.path.join(footage, name + ".ppm")
    if not os.path.exists(path):
        sys.exit("%s not found - see tools/capture_footage.sh" % path)
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    if (w, h) != (320, 240):
        sys.exit("%s: expected a 320x240 picture" % path)
    return np.frombuffer(parts[3][:w * h * 3], np.uint8).reshape(h, w, 3)


def write_png(path, img):
    h, w, _ = img.shape
    with tempfile.NamedTemporaryFile(suffix=".ppm", delete=False) as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(img.tobytes())
        ppm = f.name
    try:
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", ppm, path], check=True)
    finally:
        os.unlink(ppm)


def png_data_uri(img):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "a.png")
        write_png(path, img)
        with open(path, "rb") as f:
            return "data:image/png;base64," + base64.b64encode(f.read()).decode("ascii")


def rasterise(svg, out_png):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "tile.svg")
        with open(path, "w") as f:
            f.write(svg)
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", path, "-frames:v", "1", "-update", "1",
                        "-pix_fmt", "rgb24", "-compression_level", "9", out_png], check=True)


def decode_png(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         check=True, capture_output=True).stdout
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height", "-of", "csv=p=0", path],
                           check=True, capture_output=True, text=True).stdout.strip().split(",")
    w, h = int(probe[0]), int(probe[1])
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


# ---------------------------------------------------------------------------
# Theme 0, geometry from pico-bootLoader's tools/make_category_art.py
# ---------------------------------------------------------------------------
T0_W, T0_H = 640, 480
T0_GROUND, T0_TINT = "#1a7f99", "#2f95ae"
BADGE_X, BADGE_Y, BADGE_W, BADGE_H = 2, 186, 90, 108
BADGE_FRAME = 8
RULE_Y, RULE_TH = 229, 2
DASH_X0, DASH_X1 = 94, 108
TEXT_X, TEXT_BASELINE, TEXT_SIZE = 114, 246, 45
WORDMARK = "1943"

# The ghosted silhouette: a P-38 Lightning seen from above, nose up, in a
# 100x100 box. `knock` is punched back out of it in the ground colour.
PLANE_HULL = (
    '<path d="M1 44 L99 44 L97 50 L56 53 L44 53 L3 50 Z"/>'     # wing, tapered
    '<ellipse cx="28" cy="40" rx="5" ry="18"/>'                 # engines
    '<ellipse cx="72" cy="40" rx="5" ry="18"/>'
    '<rect x="25.5" y="44" width="5" height="40" rx="2.5"/>'    # booms
    '<rect x="69.5" y="44" width="5" height="40" rx="2.5"/>'
    '<path d="M50 24 Q56 30 56 44 L55 60 Q50 63 45 60 L44 44 Q44 30 50 24 Z"/>'  # gondola
    '<rect x="22" y="80" width="56" height="5" rx="2"/>'        # tailplane
    '<rect x="17" y="21" width="22" height="1.6" rx="0.8"/>'    # propellers
    '<rect x="61" y="21" width="22" height="1.6" rx="0.8"/>'
)
PLANE_KNOCK = (
    '<ellipse cx="50" cy="38" rx="2.4" ry="5.5"/>'              # canopy
)
WATERMARK = (515, 345, 240, -14)  # centre x, centre y, box size, rotation

# The badge: the two logos, one above the other, on the sea of the title
# screens. The rows of the 320x240 frames the logos are taken from, the game
# area's columns, and the badge picture's size: the aspect of the badge's
# inside, 74x92.
LOGO_ROWS = {"title": (26, 94), "kai-title": (26, 100)}
GAME_X0, GAME_W = 48, 224
BADGE_ART_W, BADGE_ART_H = 224, 278
LOGO_GAP = 10


def badge_art(footage):
    crops = []
    for name, (y0, y1) in LOGO_ROWS.items():
        crops.append(load_footage(footage, name)[y0:y1, GAME_X0:GAME_X0 + GAME_W])
    # the sea: the pixels that are clearly blue, averaged
    px = np.concatenate([c.reshape(-1, 3) for c in crops]).astype(np.int32)
    sea = px[(px[:, 2] > px[:, 0] + 60) & (px[:, 2] > px[:, 1] + 20)].mean(axis=0)
    art = np.empty((BADGE_ART_H, BADGE_ART_W, 3), np.uint8)
    art[:] = np.round(sea).astype(np.uint8)
    y = (BADGE_ART_H - sum(c.shape[0] for c in crops) - LOGO_GAP) // 2
    for c in crops:
        art[y:y + c.shape[0]] = c
        y += c.shape[0] + LOGO_GAP
    return art, "#%02x%02x%02x" % tuple(int(round(v)) for v in sea)


def theme0_svg(rule_x0, badge=None):
    cx, cy, size, rot = WATERMARK
    s = size / 100.0
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{T0_W}" height="{T0_H}">',
        f'<rect width="{T0_W}" height="{T0_H}" fill="{T0_GROUND}"/>',
        f'<g transform="rotate({rot} {cx} {cy}) translate({cx - size / 2},{cy - size / 2}) scale({s:.5f})">'
        f'<g fill="{T0_TINT}">{PLANE_HULL}</g><g fill="{T0_GROUND}">{PLANE_KNOCK}</g></g>',
    ]
    if rule_x0 is None:
        # measuring pass: the wordmark alone
        parts.append(
            f'<rect width="{T0_W}" height="{T0_H}" fill="#000"/>'
            f'<text x="{TEXT_X}" y="{TEXT_BASELINE}" font-family="Liberation Sans" font-weight="bold" '
            f'font-size="{TEXT_SIZE}" letter-spacing="2" fill="#ffffff">{WORDMARK}</text></svg>')
        return "".join(parts)

    art, sea = badge
    ix, iy = BADGE_X + BADGE_FRAME, BADGE_Y + BADGE_FRAME
    iw, ih = BADGE_W - 2 * BADGE_FRAME, BADGE_H - 2 * BADGE_FRAME
    parts.append(
        f'<rect x="{BADGE_X}" y="{BADGE_Y}" width="{BADGE_W}" height="{BADGE_H}" fill="#ffffff"/>'
        f'<rect x="{ix}" y="{iy}" width="{iw}" height="{ih}" fill="{sea}"/>'
        f'<svg x="{ix}" y="{iy}" width="{iw}" height="{ih}" viewBox="0 0 {BADGE_ART_W} {BADGE_ART_H}" '
        f'preserveAspectRatio="xMidYMid slice"><image xlink:href="{png_data_uri(art)}" '
        f'width="{BADGE_ART_W}" height="{BADGE_ART_H}"/></svg>')
    parts.append(
        f'<rect x="{DASH_X0}" y="{RULE_Y}" width="{DASH_X1 - DASH_X0}" height="{RULE_TH}" fill="#ffffff"/>'
        f'<text x="{TEXT_X}" y="{TEXT_BASELINE}" font-family="Liberation Sans" font-weight="bold" '
        f'font-size="{TEXT_SIZE}" letter-spacing="2" fill="#ffffff">{WORDMARK}</text>'
        f'<rect x="{rule_x0}" y="{RULE_Y}" width="{637 - rule_x0}" height="{RULE_TH}" fill="#ffffff"/>')
    parts.append("</svg>")
    return "".join(parts)


def theme0_tile(out_png, footage):
    # measure the wordmark's ink, then start the rule 12px after it
    with tempfile.TemporaryDirectory() as d:
        probe = os.path.join(d, "m.png")
        rasterise(theme0_svg(None), probe)
        ink = (decode_png(probe) > 128).any(axis=2)
        cols = np.where(ink.any(axis=0))[0]
        rule_x0 = int(cols.max()) + 12
    rasterise(theme0_svg(rule_x0, badge_art(footage)), out_png)


# ---------------------------------------------------------------------------
# Theme 1: panels 8.5 degrees off vertical, 2px black seams
# ---------------------------------------------------------------------------
T1_W, T1_H = 320, 240
PANEL_CLIP = {
    "L": "0,0 120,0 84,240 0,240",
    "M": "120,0 236,0 200,240 84,240",
    "R": "236,0 320,0 320,240 200,240",
}
# x offset of each 320x240 picture in its panel, to centre the panel on what
# the picture shows: the formation, the carrier, the island
PANEL_X = {"L": -60, "M": 30, "R": 80}
T1_TEXT_Y, T1_TEXT_SIZE, T1_STROKE = 114, 33, "#101820"


def theme1_tile(out_png, footage):
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{T1_W}" height="{T1_H}"><defs>',
    ]
    for slot, pts in PANEL_CLIP.items():
        parts.append(f'<clipPath id="p{slot}"><polygon points="{pts}"/></clipPath>')
    parts.append(f'</defs><rect width="{T1_W}" height="{T1_H}" fill="#000"/>')
    for slot, name in zip("LMR", ("left", "middle", "right")):
        img = load_footage(footage, name)
        parts.append(f'<g clip-path="url(#p{slot})"><image xlink:href="{png_data_uri(img)}" x="{PANEL_X[slot]}" '
                     f'y="0" width="{T1_W}" height="{T1_H}"/></g>')
    parts.append('<g stroke="#000" stroke-width="2" fill="none"><path d="M120 0L84 240M236 0L200 240"/></g>')
    parts.append(
        f'<text x="{T1_W // 2}" y="{T1_TEXT_Y}" text-anchor="middle" font-family="Liberation Sans Narrow" '
        f'font-weight="bold" font-style="italic" font-size="{T1_TEXT_SIZE}" letter-spacing="1" '
        f'fill="#ffffff" stroke="{T1_STROKE}" stroke-width="3" paint-order="stroke fill">{WORDMARK}</text>')
    parts.append("</svg>")
    rasterise("".join(parts), out_png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--footage", metavar="DIR", required=True, help="frames from tools/capture_footage.sh")
    ap.add_argument("--bootloader", metavar="DIR", required=True, help="pico-bootLoader emu/assets directory")
    args = ap.parse_args()

    for theme, make in (("0", theme0_tile), ("1", theme1_tile)):
        path = os.path.join(args.bootloader, "themes", theme, "1943.png")
        make(path, args.footage)
        print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
