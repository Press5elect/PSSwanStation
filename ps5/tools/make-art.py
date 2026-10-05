#!/usr/bin/env python3
"""PSSwanStation - draws the title's pictures.

  ps5/assets/logo-box.png  the mark's box, without the swan, for the interface (512 x 512)
  ps5/sce_sys/icon0.png    the home screen's icon (512 x 512)
  ps5/sce_sys/pic0.dds     the picture behind the title on the home screen
  ps5/sce_sys/pic1.dds     and while it loads (3840 x 2160, BC7)

Everything is drawn here, from shapes: the swan is this port's own mark.
Needs Pillow and numpy.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PS5 = os.path.dirname(HERE)
ASSETS = os.path.join(PS5, "assets")
SCE_SYS = os.path.join(PS5, "sce_sys")

NAVY_TOP = (22, 34, 66)
NAVY_BOTTOM = (8, 11, 24)
BLUE = (74, 163, 255)
VIOLET = (120, 90, 255)


def bezier(p0, p1, p2, p3, steps):
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        yield (u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
               u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1], t)


def swan(size, colour=(255, 255, 255, 255), shade=(190, 214, 245, 255), beak=(255, 170, 60, 255)):
    """The swan alone, on a transparent square of `size` pixels."""
    k = 4
    s = size * k
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(image)

    def P(x, y):
        return (x * s, y * s)

    def ellipse(cx, cy, rx, ry, fill):
        d.ellipse([P(cx - rx, cy - ry), P(cx + rx, cy + ry)], fill=fill)

    # The body, with a raised tail at the right.
    ellipse(0.585, 0.640, 0.250, 0.135, colour)
    d.polygon([P(0.70, 0.56), P(0.905, 0.435), P(0.835, 0.66), P(0.74, 0.74)], fill=colour)
    ellipse(0.80, 0.585, 0.075, 0.075, colour)
    # The neck: an S from the chest up to the head, thinner towards the head.
    for x, y, t in bezier((0.415, 0.640), (0.200, 0.560), (0.520, 0.360), (0.345, 0.235), 160):
        r = 0.062 - 0.020 * t
        ellipse(x, y, r, r, colour)
    # The head and the beak.
    ellipse(0.335, 0.232, 0.052, 0.047, colour)
    d.polygon([P(0.300, 0.212), P(0.195, 0.268), P(0.300, 0.268)], fill=beak)
    ellipse(0.327, 0.222, 0.0095, 0.0095, (20, 30, 56, 255))
    # The wing: a shaded feather line over the body.
    wing = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    w = ImageDraw.Draw(wing)
    w.ellipse([P(0.47, 0.545), P(0.86, 0.745)], fill=shade)
    w.ellipse([P(0.44, 0.500), P(0.87, 0.705)], fill=(0, 0, 0, 0))
    image.alpha_composite(wing)
    return image.resize((size, size), Image.LANCZOS)


def glow(size, centre, radius, colour, strength):
    """A soft light as an RGBA layer."""
    w, h = size
    ys, xs = np.mgrid[0:h, 0:w]
    dist = np.sqrt((xs - centre[0]) ** 2 + (ys - centre[1]) ** 2) / radius
    alpha = np.clip(1 - dist, 0, 1) ** 2 * strength
    layer = np.zeros((h, w, 4), np.uint8)
    layer[..., 0], layer[..., 1], layer[..., 2] = colour
    layer[..., 3] = (alpha * 255).astype(np.uint8)
    return Image.fromarray(layer, "RGBA")


def gradient(size, top, bottom):
    w, h = size
    t = np.linspace(0, 1, h)[:, None, None]
    rows = (np.array(top)[None, None, :] * (1 - t) + np.array(bottom)[None, None, :] * t)
    return Image.fromarray(np.broadcast_to(rows, (h, w, 3)).astype(np.uint8), "RGB").convert("RGBA")


def water(image, y, amplitude, colour, width):
    """A line of small waves across the picture."""
    w, h = image.size
    d = ImageDraw.Draw(image)
    points = [(x, y + math.sin(x / w * math.pi * 9) * amplitude) for x in range(0, w + 8, 8)]
    d.line(points, fill=colour, width=width, joint="curve")


def icon(size, bird_too=True):
    """The mark: the swan on its water, in a rounded box. Without the bird it
    is the box the interface draws its own, moving swan into."""
    k = 2
    s = size * k
    base = gradient((s, s), (30, 62, 130), (10, 16, 40))
    base.alpha_composite(glow((s, s), (s * 0.30, s * 0.20), s * 0.85, BLUE, 0.55))
    base.alpha_composite(glow((s, s), (s * 0.85, s * 0.95), s * 0.70, VIOLET, 0.40))
    water(base, s * 0.800, s * 0.010, (150, 200, 255, 150), max(s // 110, 2))
    water(base, s * 0.865, s * 0.008, (150, 200, 255, 90), max(s // 140, 2))
    if bird_too:
        # A soft shadow under the swan lifts it off the background.
        bird = swan(int(s * 0.92))
        shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        shadow.paste((0, 0, 0, 110), (int(s * 0.04), int(s * 0.055)), bird.split()[3])
        base.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s * 0.02)))
        base.alpha_composite(bird, (int(s * 0.04), int(s * 0.04)))
    # Rounded corners.
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.19), fill=255)
    base.putalpha(mask)
    return base.resize((size, size), Image.LANCZOS)


def backdrop(width, height, loading):
    image = gradient((width, height), NAVY_TOP, NAVY_BOTTOM)
    image.alpha_composite(glow((width, height), (width * 0.20, height * 0.10), height * 0.95, BLUE, 0.34))
    image.alpha_composite(glow((width, height), (width * 0.88, height * 0.95), height * 0.95, VIOLET, 0.26))
    for i, (y, a) in enumerate(((0.80, 70), (0.85, 48), (0.90, 30))):
        water(image, height * y, height * 0.006, (150, 200, 255, a), max(height // 360, 2))
    # The mark and the name, right of the middle: the home screen puts its own
    # things at the left.
    side = int(height * 0.36)
    cx = width * (0.50 if loading else 0.64)
    top = int(height * 0.22)
    mark = icon(side)
    image.alpha_composite(mark, (int(cx - side / 2), top))
    d = ImageDraw.Draw(image)
    bold = ImageFont.truetype(os.path.join(ASSETS, "Roboto-Bold.ttf"), int(height * 0.085))
    medium = ImageFont.truetype(os.path.join(ASSETS, "Roboto-Medium.ttf"), int(height * 0.030))
    # The name: "PS" in the mark's blue, the emulator's name after it.
    name = "PSSwanStation"
    w = d.textlength(name, font=bold)
    d.text((cx - w / 2, top + side + height * 0.035), name[:2], font=bold, fill=BLUE + (255,))
    d.text((cx - w / 2 + d.textlength(name[:2], font=bold), top + side + height * 0.035), name[2:], font=bold,
           fill=(238, 242, 250, 255))
    line = "for PS5"
    w = d.textlength(line, font=medium)
    d.text((cx - w / 2, top + side + height * 0.145), line, font=medium, fill=(160, 170, 192, 255))
    return image.convert("RGB")


# ----------------------------------------------------------------- BC7

WEIGHTS4 = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int64)


def bc7_mode6(rgb):
    """BC7, every block in mode 6 (one subset, 7.7.7.7 endpoints with a shared
    low bit, 4-bit indices): plenty for smooth art. `rgb` is H x W x 3, with H
    and W multiples of four."""
    h, w, _ = rgb.shape
    out = bytearray()
    band = 64		# rows of blocks at a time
    for y0 in range(0, h // 4, band):
        rows = rgb[y0 * 4:(y0 + band) * 4]
        bh = rows.shape[0] // 4
        bw = w // 4
        blocks = rows.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3).astype(np.float64)
        n = blocks.shape[0]
        mean = blocks.mean(axis=1, keepdims=True)
        centred = blocks - mean
        # The principal axis, by power iteration.
        cov = np.einsum("nij,nik->njk", centred, centred)
        axis = np.ones((n, 3)) / math.sqrt(3)
        for _ in range(8):
            axis = np.einsum("njk,nk->nj", cov, axis)
            norm = np.linalg.norm(axis, axis=1, keepdims=True)
            axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), 1 / math.sqrt(3))
        proj = np.einsum("nij,nj->ni", centred, axis)
        lo = mean[:, 0, :] + axis * proj.min(axis=1, keepdims=True)
        hi = mean[:, 0, :] + axis * proj.max(axis=1, keepdims=True)
        # Endpoints: 7 bits and the low bit set (alpha is 255, which needs it).
        e0 = np.clip(np.rint((lo - 1) / 2), 0, 127).astype(np.int64)
        e1 = np.clip(np.rint((hi - 1) / 2), 0, 127).astype(np.int64)
        c0 = (e0 << 1 | 1).astype(np.float64)
        c1 = (e1 << 1 | 1).astype(np.float64)
        span = c1 - c0
        length = np.maximum((span * span).sum(axis=1, keepdims=True), 1e-9)
        t = np.einsum("nij,nj->ni", blocks - c0[:, None, :], span) / length
        # The nearest of the sixteen weights.
        index = np.abs(np.clip(t, 0, 1)[:, :, None] * 64 - WEIGHTS4[None, None, :]).argmin(axis=2).astype(np.int64)
        # The first index must have its top bit clear: swap the endpoints.
        swap = index[:, 0] >= 8
        index[swap] = 15 - index[swap]
        e0s = np.where(swap[:, None], e1, e0)
        e1s = np.where(swap[:, None], e0, e1)

        lowbits = np.zeros(n, np.uint64)
        highbits = np.zeros(n, np.uint64)

        def put(value, position, bits):
            nonlocal lowbits, highbits
            value = value.astype(np.uint64)
            if position < 64:
                lowbits |= (value << np.uint64(position)) & np.uint64(0xFFFFFFFFFFFFFFFF)
                if position + bits > 64:
                    highbits |= value >> np.uint64(64 - position)
            else:
                highbits |= value << np.uint64(position - 64)

        put(np.full(n, 0x40), 0, 7)
        position = 7
        for channel in range(3):
            put(e0s[:, channel], position, 7)
            put(e1s[:, channel], position + 7, 7)
            position += 14
        put(np.full(n, 127), position, 7)
        put(np.full(n, 127), position + 7, 7)
        position += 14
        put(np.full(n, 1), position, 1)
        put(np.full(n, 1), position + 1, 1)
        position += 2
        put(index[:, 0], position, 3)
        position += 3
        for i in range(1, 16):
            put(index[:, i], position, 4)
            position += 4
        assert position == 128
        packed = np.empty((n, 2), "<u8")
        packed[:, 0] = lowbits
        packed[:, 1] = highbits
        out += packed.tobytes()
    return bytes(out)


def bc7_decode_mode6(data, width, height):
    """For the check below: decodes what bc7_mode6 wrote."""
    blocks = np.frombuffer(data, "<u8").reshape(-1, 2)
    lo, hi = blocks[:, 0], blocks[:, 1]

    def get(position, bits):
        if position + bits <= 64:
            return (lo >> np.uint64(position)) & np.uint64((1 << bits) - 1)
        if position >= 64:
            return (hi >> np.uint64(position - 64)) & np.uint64((1 << bits) - 1)
        low = lo >> np.uint64(position)
        return (low | (hi << np.uint64(64 - position))) & np.uint64((1 << bits) - 1)

    ends = [[None, None] for _ in range(3)]
    position = 7
    for channel in range(3):
        ends[channel][0] = (get(position, 7).astype(np.int64) << 1) | 1
        ends[channel][1] = (get(position + 7, 7).astype(np.int64) << 1) | 1
        position += 14
    position += 16
    index = [get(position, 3).astype(np.int64)]
    position += 3
    for _ in range(15):
        index.append(get(position, 4).astype(np.int64))
        position += 4
    index = np.stack(index, axis=1)
    weight = WEIGHTS4[index]
    pixels = np.stack([((64 - weight) * ends[c][0][:, None] + weight * ends[c][1][:, None] + 32) >> 6
                       for c in range(3)], axis=2)
    return pixels.reshape(height // 4, width // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(height, width, 3)


def write_dds(path, image):
    rgb = np.asarray(image.convert("RGB"))
    height, width, _ = rgb.shape
    data = bc7_mode6(rgb)
    error = np.abs(bc7_decode_mode6(data, width, height).astype(np.int64) - rgb.astype(np.int64))
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0xA1007, height, width, len(data), 1, 1)
    header += struct.pack("<2I4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<4I4x", 0x1000, 0, 0, 0)
    header += struct.pack("<5I", 98, 3, 0, 1, 1)		# BC7_UNORM, a 2D texture
    assert len(header) == 148
    with open(path, "wb") as f:
        f.write(header + data)
    print("%s: %d x %d, mean error %.2f, largest %d" % (os.path.basename(path), width, height, error.mean(), error.max()))


def main():
    os.makedirs(ASSETS, exist_ok=True)
    os.makedirs(SCE_SYS, exist_ok=True)
    # The interface draws the swan itself (ps5/src/swan.cpp), so that it can
    # move: its picture is only the box.
    icon(512, bird_too=False).save(os.path.join(ASSETS, "logo-box.png"), optimize=True)
    icon(512).convert("RGBA").save(os.path.join(SCE_SYS, "icon0.png"), optimize=True)
    for name, loading in (("pic0", False), ("pic1", True)):
        picture = backdrop(3840, 2160, loading)
        write_dds(os.path.join(SCE_SYS, name + ".dds"), picture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
