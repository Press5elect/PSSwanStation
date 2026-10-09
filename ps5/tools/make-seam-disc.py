#!/usr/bin/env python3
"""PSSwanStation - a disc that draws 3D-like textured meshes, to find seams.

Polygon seams (the "sparkles" along polygon edges when a game is drawn above
its own resolution) come from two things: a polygon taking texels from beyond
its own part of the texture, and gaps between polygons that meet. This disc
draws meshes of textured polygons that share their corners, in the two kinds
of texture games use (15-bit and 4-bit with a palette), over a cyan screen,
with every texel outside the parts the polygons use in magenta. A picture of
it that has magenta in it took texels it should not have; one with cyan inside
a mesh has gaps. ps5/tools/seam-check.py counts both.

  make-seam-disc.py OUT.cue [--serial SLUS-99990] [--frame N]

The meshes are laid out from a seed (--frame), so several discs give several
arrangements of corners. Needs pycdlib.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import importlib.util
import math
import os
import random
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("testdisc", os.path.join(here, "make-test-disc.py"))
testdisc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(testdisc)

MAGENTA, GREEN, YELLOW = 0x7C1F, 0x1685, 0x173C
T15 = (512, 0)			# the 15-bit texture: texture page 8
T4 = (576, 0)			# the 4-bit texture: texture page 9
CLUT = (512, 256)
PAGE15 = 8 | 2 << 7
PAGE4 = 9
CLUTFIELD = CLUT[0] // 16 | CLUT[1] << 6
# Where each block is in both textures: u from, v from, size 16 x 32.
BLOCKS = ((16, 16), (32, 16))


# The outer edge of each mesh drawn by scene(), for ps5/tools/seam-check.py.
OUTLINES = []


def texel(u, v):
    """0 magenta, 1 green, 2 yellow."""
    for k, (bu, bv) in enumerate(BLOCKS):
        if bu <= u < bu + 16 and bv <= v < bv + 32:
            return 1 + k
    return 0


def upload(x, y, w, h, halfwords):
    words = [0xA0000000, y << 16 | x, h << 16 | w]
    for i in range(0, len(halfwords), 2):
        words.append(halfwords[i] | halfwords[i + 1] << 16)
    return words


def mesh(left, right, top, bottom, page, clut, rng, cells=(8, 6), lod=False):
    nx, ny = cells
    corners = {}
    centre = (left + right) / 2
    for j in range(ny + 1):
        t = j / ny
        y = top + (bottom - top) * t ** 1.25
        half = (right - left) / 2 * (0.45 + 0.55 * t)
        for i in range(nx + 1):
            x = centre - half + 2 * half * i / nx
            if 0 < i < nx and 0 < j < ny:
                x += rng.uniform(-3, 3)
                y2 = y + rng.uniform(-2, 2)
            else:
                y2 = y
            corners[i, j] = (int(round(x)), int(round(y2)))
    outline = [corners[i, 0] for i in range(nx + 1)] + [corners[nx, j] for j in range(1, ny + 1)] \
        + [corners[i, ny] for i in range(nx - 1, -1, -1)] + [corners[0, j] for j in range(ny - 1, 0, -1)]
    OUTLINES.append(outline)

    def line(j, fine):
        """The corners along line j: with fine, one more half way between each two, rounded to a
        whole pixel as a game's own would be, so it lies near the coarse edge but not on it: a
        T-junction where a fine row meets a coarse one."""
        if not fine:
            return [corners[i, j] for i in range(nx + 1)]
        out = []
        for i in range(nx):
            a, b = corners[i, j], corners[i + 1, j]
            out += [a, (int(math.floor((a[0] + b[0]) / 2 + rng.choice((0.25, 0.5, 0.75)))),
                        int(math.floor((a[1] + b[1]) / 2 + rng.choice((0.25, 0.5, 0.75)))))]
        return out + [corners[nx, j]]

    commands = []
    for j in range(ny):
        fine = lod and j % 2 == 1
        upper, lower = line(j, fine), line(j + 1, fine)
        for i in range(len(upper) - 1):
            block = BLOCKS[(i + j) % 2]
            u0, u1 = block[0], block[0] + 16
            v0, v1 = block[1], block[1] + 32
            uv = [(u0, v0), (u1, v0), (u0, v1), (u1, v1)]
            turn = rng.randrange(4)
            if turn & 1:
                uv = [uv[1], uv[0], uv[3], uv[2]]
            if turn & 2:
                uv = [uv[2], uv[3], uv[0], uv[1]]
            # The PS1's maximum texture coordinate is 255; the PS1 samples a
            # polygon's last texel at coordinate - 1, so u1 = 32 is the block's end.
            pts = [upper[i], upper[i + 1], lower[i], lower[i + 1]]
            pos = lambda p: (p[1] & 0x7FF) << 16 | (p[0] & 0x7FF)
            tc = lambda k: uv[k][1] << 8 | uv[k][0]
            if rng.random() < 0.35:
                # Two triangles instead of a quad, split one way or the other.
                order = ((0, 1, 2), (1, 2, 3)) if rng.random() < 0.5 else ((0, 1, 3), (0, 3, 2))
                for a, b, c in order:
                    commands.append([0x25808080, pos(pts[a]), clut << 16 | tc(a), pos(pts[b]), page << 16 | tc(b),
                                     pos(pts[c]), tc(c)])
            else:
                commands.append([0x2D808080, pos(pts[0]), clut << 16 | tc(0), pos(pts[1]), page << 16 | tc(1),
                                 pos(pts[2]), tc(2), pos(pts[3]), tc(3)])
    return commands


def scene(seed):
    rng = random.Random(seed)
    OUTLINES.clear()
    records = []
    for word in (0xE1000400, 0xE2000000, 0xE3000000, 0xE403BD3F, 0xE5000000, 0xE6000000):
        records.append([word])
    records.append([0x02FFFF00, 0x00000000, 0x00F00140])		# the screen cyan
    colours = (MAGENTA, GREEN, YELLOW)
    records.append(upload(T15[0], T15[1], 64, 64, [colours[texel(u, v)] for v in range(64) for u in range(64)]))
    four = []
    for v in range(64):
        for u in range(0, 64, 4):
            four.append(sum((1 + texel(u + k, v)) << (4 * k) for k in range(4)))
    records.append(upload(T4[0], T4[1], 16, 64, four))
    records.append(upload(CLUT[0], CLUT[1], 16, 1, [MAGENTA, MAGENTA, GREEN, YELLOW] + [MAGENTA] * 12))
    # Above: corners shared. Below: rows of twice as many polygons between
    # the others, meeting them at T-junctions, as a game's nearer detail does.
    records += mesh(8, 156, 12, 114, PAGE15, 0, rng, (8, 4))
    records += mesh(164, 312, 12, 114, PAGE4, CLUTFIELD, rng, (8, 4))
    records += mesh(8, 156, 124, 228, PAGE15, 0, rng, (6, 4), lod=True)
    records += mesh(164, 312, 124, 228, PAGE4, CLUTFIELD, rng, (6, 4), lod=True)
    return records


def program(records):
    code = []

    def li(reg, value):
        code.append(0x3C000000 | reg << 16 | (value >> 16) & 0xFFFF)
        code.append(0x34000000 | reg << 21 | reg << 16 | value & 0xFFFF)

    T0, T1, T2, T3, T4R = 8, 9, 10, 11, 12
    code.append(0x3C000000 | T0 << 16 | 0x1F80)		# lui t0, 0x1f80
    for command in (0x00000000, 0x08000001, 0x06C60260, 0x07040010, 0x05000000, 0x03000000):
        li(T1, command)
        code.append(0xAC000000 | T0 << 21 | T1 << 16 | 0x1814)
    data_at = len(code)
    li(T2, 0)										# t2 = the records (patched below)
    rec = len(code)
    code.append(0x8C000000 | T2 << 21 | T3 << 16)		# lw t3, 0(t2): the record's length
    code.append(0)
    done_branch = len(code)
    code.append(0x10000000 | T3 << 21)				# beq t3, zero, done (patched)
    code.append(0x24000000 | T2 << 21 | T2 << 16 | 4)	# addiu t2, t2, 4
    wait = len(code)
    code.append(0x8C000000 | T0 << 21 | T1 << 16 | 0x1814)	# lw t1, GPUSTAT
    code.append(0)
    code.append(0x3C000000 | T4R << 16 | 0x0400)			# lui t4, 0x0400: ready for a command
    code.append(T1 << 21 | T4R << 16 | T1 << 11 | 0x24)	# and t1, t1, t4
    code.append(0x10000000 | T1 << 21 | (wait - len(code) - 1) & 0xFFFF)	# beq t1, zero, wait
    code.append(0)
    word = len(code)
    code.append(0x8C000000 | T2 << 21 | T1 << 16)		# lw t1, 0(t2)
    code.append(0x24000000 | T2 << 21 | T2 << 16 | 4)	# addiu t2, t2, 4
    code.append(0xAC000000 | T0 << 21 | T1 << 16 | 0x1810)	# sw t1, GP0
    code.append(0x2400FFFF | T3 << 21 | T3 << 16)		# addiu t3, t3, -1
    code.append(0x14000000 | T3 << 21 | (word - len(code) - 1) & 0xFFFF)	# bne t3, zero, word
    code.append(0)
    code.append(0x08000000 | ((0x80010000 + rec * 4) >> 2) & 0x03FFFFFF)	# j rec
    code.append(0)
    done = len(code)
    code.append(0x08000000 | ((0x80010000 + done * 4) >> 2) & 0x03FFFFFF)	# j done
    code.append(0)
    code[done_branch] |= (done - done_branch - 1) & 0xFFFF
    data = []
    for r in records:
        data.append(len(r))
        data += r
    data.append(0)
    address = 0x80010000 + len(code) * 4
    code[data_at] = 0x3C000000 | T2 << 16 | address >> 16
    code[data_at + 1] = 0x34000000 | T2 << 21 | T2 << 16 | address & 0xFFFF
    return testdisc.executable(code, data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cue")
    parser.add_argument("--serial", default="SLUS-99990")
    parser.add_argument("--frame", type=int, default=1, help="the seed the meshes are laid out from")
    args = parser.parse_args()
    exe = program(scene(args.frame))
    testdisc.write(args.cue, testdisc.raw(testdisc.iso(args.serial, 0, exe=exe)), args.serial)
    return 0


if __name__ == "__main__":
    sys.exit(main())
