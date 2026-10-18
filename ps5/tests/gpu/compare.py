#!/usr/bin/env python3
"""PSSwanStation - the renderer tests' comparison (run.sh).

    compare.py OUT SCALE...

Reads OUT/sw.vram and OUT/hw<SCALE>.vram (1024 x 512 little-endian 16-bit
pixels, as the console keeps video memory) and, for each of the sixteen
80x60 cells of gpu_scenes.c, says how many pixels differ from the software
renderer's: exactly, by more than one step of a colour channel, and in the
mask bit. From the screenshots (OUT/sw.png, OUT/hw<SCALE>.png) it counts the
magenta background showing through in each cell, beyond what the software
renderer shows: the gaps between polygons. Writes OUT/diff<SCALE>.png.

SPDX-FileCopyrightText: 2026 Press5elect
SPDX-License-Identifier: GPL-3.0-or-later
"""
import struct
import sys
from pathlib import Path

from PIL import Image

NAMES = ["grid tex", "grid shaded", "fan 15-bit", "T-junction", "blend 0", "blend 1", "blend 2", "blend 3",
         "tex blend 0", "tex blend 1", "tex blend 2", "tex blend 3", "mask", "dither", "lines", "rects"]


def MAGENTA(r, g, b):
    return r > 180 and g < 80 and b > 180


def RED(r, g, b):
    return r > 180 and g < 70 and b < 70


def vram(path):
    data = path.read_bytes()
    return struct.unpack("<%dH" % (len(data) // 2), data)


def channels(p):
    return (p & 31, (p >> 5) & 31, (p >> 10) & 31, p >> 15)


def share(png, cell, test):
    """The share of a cell's area whose colour passes test, on the screen."""
    img = Image.open(png).convert("RGB")
    w, h = img.size
    # The 320x240 picture is shown at 4:3, as large as fits, centred.
    ph = h
    pw = ph * 4 // 3
    if pw > w:
        pw = w
        ph = pw * 3 // 4
    ox, oy = (w - pw) // 2, (h - ph) // 2
    cx, cy = (cell % 4) * 80, (cell // 4) * 60
    x0, x1 = ox + cx * pw // 320, ox + (cx + 80) * pw // 320
    y0, y1 = oy + cy * ph // 240, oy + (cy + 60) * ph // 240
    px = img.load()
    total = magenta = 0
    for y in range(y0 + 1, y1 - 1):
        for x in range(x0 + 1, x1 - 1):
            r, g, b = px[x, y]
            total += 1
            if test(r, g, b):
                magenta += 1
    return magenta / max(total, 1)


def cell_box(img, cell):
    """A cell's pixels on the screen (as share() places them)."""
    w, h = img.size
    ph = h
    pw = ph * 4 // 3
    if pw > w:
        pw = w
        ph = pw * 3 // 4
    ox, oy = (w - pw) // 2, (h - ph) // 2
    cx, cy = (cell % 4) * 80, (cell // 4) * 60
    return (ox + cx * pw // 320 + 1, oy + cy * ph // 240 + 1, ox + (cx + 80) * pw // 320 - 1,
            oy + (cy + 60) * ph // 240 - 1)


def blocks(out):
    """blocks.sh: the share of each cell that the HD pack changed."""
    base = Image.open(out / "base.png").convert("RGB")
    pack = Image.open(out / "pack.png").convert("RGB")
    bp, pp = base.load(), pack.load()
    untextured = {4, 5, 6, 7, 14}
    bad = 0
    print("cell                changed (share of the cell)")
    for cell in range(16):
        x0, y0, x1, y1 = cell_box(base, cell)
        total = changed = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                total += 1
                changed += bp[x, y] != pp[x, y]
        share = changed / max(total, 1)
        note = ""
        if cell in untextured and share > 0.001:
            note, bad = "  << untextured, should not change", bad + 1
        if cell in (0, 2) and share < 0.5:
            note, bad = "  << textured, should change", bad + 1
        print("  %2d %-14s %8.4f%s" % (cell, NAMES[cell], share, note))
    return 1 if bad else 0


def main():
    if sys.argv[1] == "--blocks":
        return blocks(Path(sys.argv[2]))
    out = Path(sys.argv[1])
    sw = vram(out / "sw.vram")
    worst = 0
    for scale in sys.argv[2:]:
        hw = vram(out / ("hw%s.vram" % scale))
        print("scale %sx                exact   >1 step   mask       gaps    seams (share of the cell)" % scale)
        diff = Image.new("RGB", (320 * 3, 240))
        dp = diff.load()
        for cell in range(16):
            cx, cy = (cell % 4) * 80, (cell // 4) * 60
            exact = step = mask = 0
            for y in range(cy, cy + 60):
                for x in range(cx, cx + 80):
                    a, b = sw[y * 1024 + x], hw[y * 1024 + x]
                    ca, cb = channels(a), channels(b)
                    for i, p in enumerate((a, b)):
                        r, g, bl, _ = channels(p)
                        dp[x + i * 320, y] = (r * 8, g * 8, bl * 8)
                    if a != b:
                        exact += 1
                        far = max(abs(ca[i] - cb[i]) for i in range(3)) > 1
                        step += far
                        mask += ca[3] != cb[3]
                        dp[x + 640, y] = (255, 0, 0) if far else (255, 200, 0)
            gaps = bleed = ""
            if (out / "sw.png").exists() and (out / ("hw%s.png" % scale)).exists():
                hwp, swp = out / ("hw%s.png" % scale), out / "sw.png"
                gaps = "%+.4f" % (share(hwp, cell, MAGENTA) - share(swp, cell, MAGENTA))
                if cell == 0:
                    bleed = "%+.4f" % (share(hwp, cell, RED) - share(swp, cell, RED))
            print("  %2d %-14s %7d %9d %6d   %8s %8s" % (cell, NAMES[cell], exact, step, mask, gaps, bleed))
            worst = max(worst, step)
        diff.save(out / ("diff%s.png" % scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
