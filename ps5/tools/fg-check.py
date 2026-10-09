#!/usr/bin/env python3
"""PSSwanStation - how true frame generation's pictures are.

  fg-check.py ROOT FIRST COUNT RATE

ROOT holds screenshots fFIRST.png ... of a run of make-test-disc.py's moving
disc (--moving N), one for each display frame, the picture fitted to the
screen (4:3) with square pixels. Each is compared with the true picture at
that moment (the square 8 pixels a picture rightwards, the posts 2 leftwards).
RATE is the game's pictures per display frame (0.5 for a game of 30 pictures
a second at 60 Hz). Prints the square's place in each frame, the steps
between them (even steps: smooth movement), and the share of pixels that
differ, edges within two game pixels of their true place not counted.
Needs numpy, scipy and Pillow.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys, statistics
import numpy as np
from PIL import Image
root, first, count, rate = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
frames = [np.asarray(Image.open(f"{root}/f{f}.png").convert("RGB")).astype(int) for f in range(first, first + count)]
H, W = frames[0].shape[:2]
# The game's area: 4:3 fitted.
gh = H; gw = H * 4 // 3; ox = (W - gw) // 2
sx = gw / 320; sy = gh / 240
BG = (0x20, 0x40, 0x80); ORANGE = (0xFF, 0xA5, 0x00)
POSTS = [(0x40, 0x30, 0x20), (0x68, 0x48, 0x30)]
def square_x(img):
    g = img[int(110*sy)+5:int(158*sy)-5, ox:ox+gw]
    m = (abs(g[...,0]-255) < 40) & (abs(g[...,1]-165) < 40) & (g[...,2] < 60)
    cols = np.where(m.sum(0) > m.shape[0]*0.5)[0]
    return cols.min()/sx if len(cols) else None
def truth(s):
    img = np.zeros((240*4, 320*4, 3), np.uint8); img[:] = BG
    def rect(x, y, w, h, c):
        x0 = int(round(x*4)); img[y*4:(y+h)*4, max(0,x0):max(0,x0+w*4)] = c
    for k, c in enumerate(POSTS):
        px = (200 + k*90 - 2*s) % 256
        rect(px, 60, 24, 120, c)
    rect((8*s) % 256, 110, 48, 48, ORANGE)
    rect(16, 12, 120, 10, (255,255,255)); rect(20, 14, 40, 6, (0xE0, 0x20, 0x20))
    return np.asarray(Image.fromarray(img).resize((gw, gh), Image.BOX)).astype(int)
def post_x(img):
    g = img[int(70*sy)+2:int(100*sy), ox:ox+gw]
    m = (abs(g[...,0]-0x40) < 12) & (abs(g[...,1]-0x30) < 12) & (abs(g[...,2]-0x20) < 12)
    cols = np.where(m.sum(0) > m.shape[0]*0.5)[0]
    if not len(cols): return None
    # the post's left edge (it may wrap round the 256-pixel line)
    c = cols.min()/sx
    return c
xs = [square_x(f) for f in frames]
# The game's picture number at frame 0, from the dark post (2 pixels a picture,
# leftwards, from 200): modulo 128, with the square's place to say which.
est = []
for i, f in enumerate(frames):
    p = post_x(f)
    if p is not None:
        est.append(((200 - p) % 256) / 2 - rate * i)
s0 = statistics.median(est)
worst = 0; total = 0; line = []; per = []
for i, f in enumerate(frames):
    t = truth(s0 + rate*i)
    # Edges of the true picture, widened by a game pixel and a half: a half-pixel
    # place cannot be drawn on the game's own pixels, so those are not counted.
    tg = t.sum(2)
    edge = np.zeros(tg.shape, bool)
    edge[:, 1:] |= np.abs(np.diff(tg, axis=1)) > 30
    edge[1:, :] |= np.abs(np.diff(tg, axis=0)) > 30
    from scipy.ndimage import binary_dilation
    edge = binary_dilation(edge, iterations=int(2.2 * sx) + 1)
    d = ((np.abs(f[:, ox:ox+gw] - t).sum(2) > 60) & ~edge).mean() * 100
    total += d; worst = max(worst, d); per.append("%.1f" % d)
    line.append("%.1f" % (xs[i] if xs[i] is not None else -1))
steps = [round(b - a, 2) for a, b in zip(xs, xs[1:]) if a is not None and b is not None]
print("square x:", " ".join(line))
print("steps:", [float(x) for x in steps])
print("per frame %:", " ".join(per))
print("differs from the true picture: mean %.2f%%, worst %.2f%%" % (total/len(frames), worst))
