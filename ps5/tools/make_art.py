#!/usr/bin/env python3
# Mupen64Plus PS5: draws the home-screen art (app/sce_sys/): icon0.png (512x512), background-source.png and
# pic0.dds / pic1.dds (3840x2160, BC7_UNORM in a DX10 DDS without mipmaps, what the PS5 home screen reads).
#
# The BC7 encoder is a small one: every 4x4 block in mode 6 (two RGBA endpoints, 4-bit indices), endpoints
# from the block's colour bounding box. Plenty for a smooth background.
#
#   make_art.py <out-dir> [font.ttf bold.ttf]     (needs Pillow and numpy)
#
# SPDX-License-Identifier: MIT
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ACCENT = (255, 196, 64)
BLUE = (64, 140, 255)
RED = (235, 64, 72)
GREEN = (60, 200, 110)


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def gradient(w, h, top, bottom):
    t = np.linspace(0.0, 1.0, h)[:, None, None]
    img = (np.array(top)[None, None, :] * (1 - t) + np.array(bottom)[None, None, :] * t)
    return Image.fromarray(np.repeat(img, w, axis=1).astype(np.uint8), "RGB")


def glow(img, center, radius, color, strength):
    w, h = img.size
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - center[0]) ** 2 + (yy - center[1]) ** 2) / radius
    a = np.clip(1.0 - d, 0, 1) ** 2 * strength
    base = np.asarray(img).astype(np.float32)
    base += a[:, :, None] * np.array(color, np.float32)[None, None, :]
    return Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB")


def text_center(draw, cx, y, text, fnt, fill):
    l, t, r, b = draw.textbbox((0, 0), text, font=fnt)
    draw.text((cx - (r - l) / 2 - l, y - t), text, font=fnt, fill=fill)
    return b - t


def make_icon(black, bold):
    s = 512
    img = gradient(s, s, (26, 20, 64), (10, 10, 26))
    img = glow(img, (256, 210), 330, (90, 70, 200), 0.9)
    d = ImageDraw.Draw(img)
    # four coloured bars, a nod to the console's four-colour palette (not its logo)
    for k, c in enumerate([RED, ACCENT, GREEN, BLUE]):
        x0 = 96 + k * 84
        d.rounded_rectangle((x0, 384, x0 + 68, 400), radius=8, fill=c)
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ds = ImageDraw.Draw(shadow)
    text_center(ds, 262, 108, "64", font(black, 250), (0, 0, 0, 200))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    img.paste(shadow, (0, 0), shadow)
    text_center(d, 256, 100, "64", font(black, 250), (255, 255, 255))
    text_center(d, 256, 44, "MUPEN", font(bold, 54), ACCENT)
    text_center(d, 256, 426, "PS5", font(bold, 48), (220, 220, 240))
    return img


def make_background(black, bold):
    w, h = 3840, 2160
    img = gradient(w, h, (16, 12, 40), (6, 6, 16))
    img = glow(img, (2700, 900), 1700, (70, 50, 170), 0.8)
    img = glow(img, (600, 2000), 1200, (20, 60, 140), 0.5)
    d = ImageDraw.Draw(img)
    # a huge faint "64" behind the title
    ghost = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(ghost).text((1900, 120), "64", font=font(black, 1700), fill=(255, 255, 255, 18))
    img.paste(ghost, (0, 0), ghost)
    d.text((2050, 760), "Mupen64Plus", font=font(black, 230), fill=(255, 255, 255))
    d.text((2060, 1030), "Nintendo 64 emulator for PS5", font=font(bold, 96), fill=(200, 200, 225))
    for k, c in enumerate([RED, ACCENT, GREEN, BLUE]):
        x0 = 2060 + k * 230
        d.rounded_rectangle((x0, 1200, x0 + 190, 1228), radius=14, fill=c)
    return img


# ---- BC7 mode 6 ---------------------------------------------------------------------------------------------
def bc7_encode(img):
    a = np.asarray(img.convert("RGBA")).astype(np.int32)
    h, w, _ = a.shape
    blocks = a.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    n = blocks.shape[0]
    lo = blocks.min(axis=1)
    hi = blocks.max(axis=1)
    # 7-bit endpoints with the p-bit set: value = q * 2 + 1
    q0 = np.clip((lo - 1) // 2, 0, 127)
    q1 = np.clip((hi - 1) // 2, 0, 127)
    e0 = (q0 * 2 + 1).astype(np.float32)
    e1 = (q1 * 2 + 1).astype(np.float32)
    axis = e1 - e0
    den = (axis * axis).sum(axis=1)
    den[den == 0] = 1
    t = ((blocks - e0[:, None, :]) * axis[:, None, :]).sum(axis=2) / den[:, None]
    idx = np.clip(np.rint(t * 15), 0, 15).astype(np.int64)
    # the anchor (pixel 0) index must have its top bit clear: swap the endpoints when it doesn't
    swap = idx[:, 0] >= 8
    q0s, q1s = q0.copy(), q1.copy()
    q0s[swap], q1s[swap] = q1[swap], q0[swap]
    idx[swap] = 15 - idx[swap]

    bits = np.zeros((n, 128), np.uint8)
    pos = 0

    def put(val, width):
        nonlocal pos
        for k in range(width):
            bits[:, pos + k] = (val >> k) & 1
        pos += width

    put(np.full(n, 1 << 6), 7)  # mode 6
    for c in range(4):  # R0 R1 G0 G1 B0 B1 A0 A1
        put(q0s[:, c], 7)
        put(q1s[:, c], 7)
    put(np.ones(n, np.int64), 1)  # P0
    put(np.ones(n, np.int64), 1)  # P1
    put(idx[:, 0], 3)
    for p in range(1, 16):
        put(idx[:, p], 4)
    assert pos == 128
    return np.packbits(bits, axis=1, bitorder="little").tobytes()


def dds_bc7(width, height, data):
    header = bytearray(148)
    struct.pack_into("<4sIIIIIII", header, 0, b"DDS ", 124, 0x000A1007, height, width, len(data), 1, 1)
    struct.pack_into("<II4s", header, 76, 32, 0x4, b"DX10")
    struct.pack_into("<I", header, 108, 0x1000)  # DDSCAPS_TEXTURE
    struct.pack_into("<IIIII", header, 128, 98, 3, 0, 1, 1)  # BC7_UNORM, TEXTURE2D, array size 1
    return bytes(header) + data


def main():
    out = sys.argv[1]
    black = sys.argv[2] if len(sys.argv) > 2 else "C:/Windows/Fonts/seguibl.ttf"
    bold = sys.argv[3] if len(sys.argv) > 3 else "C:/Windows/Fonts/segoeuib.ttf"
    os.makedirs(out, exist_ok=True)
    make_icon(black, bold).save(os.path.join(out, "icon0.png"), optimize=True)
    bg = make_background(black, bold)
    bg.save(os.path.join(out, "background-source.png"), optimize=True)
    dds = dds_bc7(bg.width, bg.height, bc7_encode(bg))
    for name in ("pic0.dds", "pic1.dds"):
        with open(os.path.join(out, name), "wb") as f:
            f.write(dds)
    print("art written to %s (%d-byte DDS)" % (out, len(dds)))


if __name__ == "__main__":
    main()
