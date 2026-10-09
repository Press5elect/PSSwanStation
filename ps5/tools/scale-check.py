#!/usr/bin/env python3
"""PSSwanStation - how a scaling filter enlarges.

  scale-check.py REF.png K OUT.png...

REF is a screenshot of the game's own picture shown with square pixels at K
times its size (whole); each OUT the same moment through a scaling filter.
For each, on a part of the picture with detail: its sharpness against bilinear
and Lanczos enlargements of the game's own picture, the shift that fits best
(in quarter pixels), PSNR against Lanczos, and overshoot (brighter or darker
than every game pixel around it). Needs numpy, scipy and Pillow.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys, numpy as np
from PIL import Image
def load(p): return np.asarray(Image.open(p).convert("RGB")).astype(np.float64)
def bbox(a):
    m = a.sum(2) > 24; ys = np.where(m.any(1))[0]; xs = np.where(m.any(0))[0]
    return xs.min(), ys.min(), xs.max()+1, ys.max()+1
G = np.array([0.299, 0.587, 0.114])
ref = load(sys.argv[1]); k = int(sys.argv[2])
x0,y0,x1,y1 = bbox(ref); native = ref[y0+k//2:y1:k, x0+k//2:x1:k].astype(np.uint8)
def grad(a): return np.abs(np.diff(a, axis=1)).mean() + np.abs(np.diff(a, axis=0)).mean()
for p in sys.argv[3:]:
    out = load(p); a,b,c,d = bbox(out); full = out[b:d, a:c] @ G; H, W = full.shape
    cy, cx, h, w = int(H*0.30), int(W*0.25), int(H*0.2)//2*2, int(W*0.16)//2*2
    o = full[cy:cy+h, cx:cx+w]
    ref_of = {}
    for name, f in (("bilinear", Image.BILINEAR), ("lanczos", Image.LANCZOS)):
        ref_of[name] = (np.asarray(Image.fromarray(native).resize((W, H), f)).astype(np.float64) @ G)[cy:cy+h, cx:cx+w]
    lz = ref_of["lanczos"]
    R = np.fft.fft2(lz); fy = np.fft.fftfreq(h)[:,None]; fx = np.fft.fftfreq(w)[None,:]
    best = None
    for dx in np.arange(-2, 2.01, 0.25):
        for dy in np.arange(-2, 2.01, 0.25):
            s = np.real(np.fft.ifft2(R * np.exp(-2j*np.pi*(fx*dx + fy*dy))))
            e = ((s - o)[8:-8, 8:-8]**2).mean()
            if best is None or e < best[0]: best = (e, dx, dy)
    e, dx, dy = best
    # Overshoot: brighter or darker than every native pixel around it by more than 24.
    nat = native.astype(np.float64) @ G
    from scipy.ndimage import maximum_filter, minimum_filter
    hi = np.asarray(Image.fromarray(maximum_filter(nat, 3).astype(np.uint8)).resize((W, H), Image.NEAREST)).astype(float)[cy:cy+h, cx:cx+w]
    lo = np.asarray(Image.fromarray(minimum_filter(nat, 3).astype(np.uint8)).resize((W, H), Image.NEAREST)).astype(float)[cy:cy+h, cx:cx+w]
    over = ((o > hi + 24) | (o < lo - 24)).mean() * 100
    print("%-28s sharpness %.2f x bilinear, %.2f x Lanczos  shift %+.2f,%+.2f  PSNR to Lanczos %.1f dB  overshoot %.2f%%"
          % (p.split('/')[-2].replace('root-', ''), grad(o)/grad(ref_of["bilinear"]), grad(o)/grad(lz), dx, dy,
             10*np.log10(255**2/e), over))
