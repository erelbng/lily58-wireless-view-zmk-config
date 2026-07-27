#!/usr/bin/env python3
"""
Generate 1-bit LVGL art for the OLED fireplace widget (SSD1306 128x32,
mounted portrait on the Lily58: the viewer sees 32 wide x 128 tall).

Outputs (checked into the repo, regenerate after tweaking):
    config/src/fire_art.h        frame count + extern declarations
    config/src/fire_frames.c     campfire animation frames (44x32)
    config/src/mug_art.c         cute tea mug for the pause reminder
    assets/fire-preview.gif      preview of the animation (4x scale)
    assets/reminder-preview.png  preview of the mug art
    assets/display-mock.png      mock of the whole 128x32 screen
    assets/display-preview.gif   animated mock

Tweak knobs below (FRAMES, sizes, cooling, logs, mug) and re-run:
    python3 scripts/gen_display_art.py

The panel scans landscape (128x32) but is mounted rotated 90° on the board,
so all art is drawn in viewer space (portrait, flames up) and rotated 90°
clockwise into framebuffer space here. Canvas-drawn text in the C code
rotates at runtime instead. Lit pixels are index 1.
"""

import random
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "config" / "src"
ASSETS = REPO / "assets"

# ----- knobs -----------------------------------------------------------------
FRAMES = 16          # frames in the loop
FIRE_W, FIRE_H = 32, 64   # viewer space: full width, bottom of the screen
SEED = 1337          # change for a different (deterministic) flame
WARMUP = 60          # sim steps before capturing
CROSSFADE = 4        # frames blended onto the loop start for a seamless loop
LOG_TOP = 52         # y where the logs begin (flames sit on top)

BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def rotate_viewer_to_fb(img):
    """viewer space -> framebuffer space = rotate 90° clockwise.

    fb(x, y) = viewer(y, H-1-x). Verified on the actual board: fire drawn
    at the framebuffer left appears at the viewer bottom, flames pointing
    left, i.e. viewer = 90° CCW of framebuffer.
    """
    w, h = img.size
    out = Image.new("1", (h, w))
    for y in range(w):
        for x in range(h):
            out.putpixel((x, y), img.getpixel((y, h - 1 - x)))
    return out


# ----- fire simulation (classic heat propagation) ----------------------------

def simulate_fire():
    rng = random.Random(SEED)
    w, h = FIRE_W, LOG_TOP + 2  # heat grid reaches just into the log area
    heat = [[0.0] * w for _ in range(h)]

    def step():
        # stoke the source row: hot in the middle, flickering
        for x in range(w):
            centered = 1.0 - abs(x - w / 2) / (w / 2)   # 0 at edges, 1 middle
            base = 300 * (centered ** 1.2) if 5 <= x <= w - 5 else 0
            heat[h - 1][x] = max(0.0, min(255.0, base + rng.uniform(-110, 55)))
        # propagate upward with cooling; cool more at the sides -> flame shape
        for y in range(h - 2, -1, -1):
            for x in range(w):
                # turbulence: sample from below with a random sideways drift
                drift = rng.choice((-1, -1, 0, 0, 0, 1, 1))
                sx = max(0, min(w - 1, x + drift))
                below = (
                    heat[y + 1][sx] * 2
                    + heat[y + 1][max(0, sx - 1)]
                    + heat[y + 1][min(w - 1, sx + 1)]
                ) / 4
                edge = abs(x - w / 2) / (w / 2)
                cooling = rng.uniform(1.0, 8.0) + 12 * edge ** 2
                heat[y][x] = max(0.0, below - cooling)

    def snapshot():
        return [row[:] for row in heat]

    for _ in range(WARMUP):
        step()
    frames = []
    for _ in range(FRAMES + CROSSFADE):
        step()
        frames.append(snapshot())
    # blend the tail onto the head so the loop doesn't jump
    for i in range(CROSSFADE):
        a = frames[FRAMES + i]
        b = frames[i]
        t = (i + 1) / (CROSSFADE + 1)
        frames[i] = [
            [av * (1 - t) + bv * t for av, bv in zip(ar, br)]
            for ar, br in zip(a, b)
        ]
    return frames[:FRAMES]


def _outlined_log(draw, p1, p2, width=4):
    """Diagonal log: lit line with a dark core = outline on black."""
    draw.line([p1, p2], fill=1, width=width)
    draw.line([p1, p2], fill=0, width=width - 2)
    for (x, y) in (p1, p2):
        r = width // 2
        draw.ellipse([x - r, y - r, x + r, y + r], outline=1, fill=0)


def draw_logs(draw: ImageDraw.ImageDraw):
    """Campfire: two small crossed logs over a few stones."""
    draw_w = FIRE_W
    _outlined_log(draw, (3, LOG_TOP + 7), (draw_w - 7, LOG_TOP))
    _outlined_log(draw, (6, LOG_TOP), (draw_w - 4, LOG_TOP + 7))
    # stones along the bottom edge
    for x in range(0, draw_w - 3, 7):
        draw.arc([x, FIRE_H - 3, x + 6, FIRE_H + 2], 180, 360, fill=1)


def fire_frame_image(heat) -> Image.Image:
    """Dither one heat snapshot into a 1-bit frame (lit flames on dark)."""
    img = Image.new("1", (FIRE_W, FIRE_H), 0)
    for y in range(len(heat)):
        for x in range(FIRE_W):
            v = heat[y][x]
            if v < 28:  # below ember glow: stay dark
                continue
            threshold = (BAYER4[y % 4][x % 4] + 0.5) / 16 * 235 + 20
            if v >= threshold:
                img.putpixel((x, y), 1)
    d = ImageDraw.Draw(img)
    draw_logs(d)
    return img


# ----- pause reminder mug ----------------------------------------------------

MUG_W, MUG_H = 26, 30


def mug_image() -> Image.Image:
    """Steaming tea mug with a little face, lit-on-dark for the OLED."""
    img = Image.new("1", (MUG_W, MUG_H), 0)
    d = ImageDraw.Draw(img)
    # steam: three wavy strands
    for cx, phase in ((8, 0), (13, 2), (18, 1)):
        pts = []
        for t in range(0, 9):
            off = [0, 1, 1, 0, 0, -1, -1, 0][(t + phase) % 8]
            pts.append((cx + off, 9 - t))
        d.line(pts, fill=1, width=1)
    # mug body
    d.rounded_rectangle([2, 11, 19, 27], radius=3, outline=1, width=1)
    # tea surface
    d.line([4, 14, 17, 14], fill=1)
    # handle
    d.arc([18, 15, 25, 25], 280, 80, width=1, fill=1)
    # face
    d.point([(7, 19), (8, 19), (13, 19), (14, 19)], fill=1)
    d.arc([8, 20, 13, 24], 20, 160, fill=1)  # smile
    return img


# ----- README preview mocks ---------------------------------------------------

def _mock_screen(fire: Image.Image, reminder=False) -> Image.Image:
    """Rough PIL imitation of the screen in viewer space (32x128 portrait)."""
    scr = Image.new("1", (32, 128), 0)
    d = ImageDraw.Draw(scr)
    # battery, top
    d.rectangle([1, 3, 26, 13], outline=1)
    d.rectangle([3, 5, 18, 11], fill=1)
    d.rectangle([27, 6, 29, 10], fill=1)
    if reminder:
        d.text((4, 40), "0:42", fill=1)
        scr.paste(mug_image(), (3, 64))
    else:
        d.text((6, 16), ")1", fill=1)
        d.text((3, 36), "Base", fill=1)
        d.rectangle([2, 56, 29, 59], outline=1)
        d.rectangle([3, 57, 14, 58], fill=1)
        scr.paste(fire, (0, 128 - FIRE_H))
    return scr


def emit_display_mocks(frames, scale=4):
    mocks = [
        _mock_screen(f).resize((32 * scale, 128 * scale), Image.NEAREST).convert("L")
        for f in frames
    ]
    rem = _mock_screen(frames[0], reminder=True)
    rem = rem.resize((32 * scale, 128 * scale), Image.NEAREST).convert("L")

    strip = Image.new("L", (32 * scale * 2 + 12, 128 * scale), 40)
    strip.paste(mocks[0], (0, 0))
    strip.paste(rem, (32 * scale + 12, 0))
    strip.save(ASSETS / "display-mock.png")
    mocks[0].convert("P").save(
        ASSETS / "display-preview.gif",
        save_all=True, append_images=[m.convert("P") for m in mocks[1:]],
        duration=150, loop=0,
    )


# ----- C emission -------------------------------------------------------------

def pack_1bit(img: Image.Image) -> bytes:
    w, h = img.size
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if img.getpixel((x, y)):
                out[y * stride + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


PALETTE = """\
        0x00, 0x00, 0x00, 0xff, /*Color of index 0*/
        0xff, 0xff, 0xff, 0xff, /*Color of index 1*/
"""


def emit_image(name: str, img: Image.Image) -> str:
    data = pack_1bit(img)
    w, h = img.size
    hexes = ", ".join(f"0x{b:02x}" for b in data)
    lines = []
    line = "        "
    for part in hexes.split(", "):
        if len(line) + len(part) + 2 > 100:
            lines.append(line.rstrip())
            line = "        "
        line += part + ", "
    lines.append(line.rstrip().rstrip(","))
    body = "\n".join(lines)
    return f"""\
static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{
{PALETTE}
{body},
}};

const lv_img_dsc_t {name} = {{
    .header.cf = LV_IMG_CF_INDEXED_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {w},
    .header.h = {h},
    .data_size = {len(data) + 8},
    .data = {name}_map,
}};
"""


HEADER_COMMENT = """\
/*
 * Generated by scripts/gen_display_art.py — edit that script, not this file.
 * SPDX-License-Identifier: MIT
 */
"""


def main():
    SRC.mkdir(parents=True, exist_ok=True)
    ASSETS.mkdir(parents=True, exist_ok=True)

    print("simulating fire...")
    heats = simulate_fire()
    frames = [fire_frame_image(h) for h in heats]
    fb_frames = [rotate_viewer_to_fb(f) for f in frames]

    parts = [HEADER_COMMENT, "#include <lvgl.h>\n"]
    for i, f in enumerate(fb_frames):
        parts.append(emit_image(f"fire_frame_{i:02d}", f))
    names = ", ".join(f"&fire_frame_{i:02d}" for i in range(len(fb_frames)))
    parts.append(
        f"const lv_img_dsc_t *fire_frames[{len(frames)}] = {{{names}}};\n"
    )
    (SRC / "fire_frames.c").write_text("\n".join(parts))

    mug = mug_image()
    (SRC / "mug_art.c").write_text(
        "\n".join([HEADER_COMMENT, "#include <lvgl.h>\n", emit_image("mug_art", mug)])
    )

    (SRC / "fire_art.h").write_text(f"""\
{HEADER_COMMENT}
#pragma once

#include <lvgl.h>

#define FIRE_FRAME_COUNT {len(frames)}
#define FIRE_FRAME_W {FIRE_H}  /* framebuffer-space: pre-rotated 90 deg */
#define FIRE_FRAME_H {FIRE_W}

extern const lv_img_dsc_t *fire_frames[FIRE_FRAME_COUNT];
extern const lv_img_dsc_t mug_art;
""")

    # previews
    scale = 4
    gif = [
        f.convert("P").resize((FIRE_W * scale, FIRE_H * scale), Image.NEAREST)
        for f in frames
    ]
    gif[0].save(
        ASSETS / "fire-preview.gif",
        save_all=True, append_images=gif[1:], duration=150, loop=0,
    )
    mug.resize((MUG_W * scale, MUG_H * scale), Image.NEAREST).save(
        ASSETS / "reminder-preview.png"
    )
    emit_display_mocks(frames)
    total = sum((len(pack_1bit(f)) + 8) for f in frames)
    print(f"{len(frames)} frames, {total} bytes flash, previews in assets/")


if __name__ == "__main__":
    main()
