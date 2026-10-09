#!/usr/bin/env python3
"""PSSwanStation - counts the seams in a picture of the seam disc.

  seam-check.py PICTURE.png SEED

PICTURE is a screenshot of ps5/tools/make-seam-disc.py's disc made with that
seed. Inside each mesh (its outline, a little inside the edge), a magenta
pixel is a texel taken from outside the polygon's part of the texture, a cyan
one a gap between polygons. Prints both counts for each mesh.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import importlib.util
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("seamdisc", os.path.join(here, "make-seam-disc.py"))
seamdisc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seamdisc)


def main():
    picture = Image.open(sys.argv[1]).convert("RGB")
    seamdisc.scene(int(sys.argv[2]))
    # The game's picture is the part that is not black: 320 x 240 of it.
    box = picture.convert("L").point(lambda v: 255 if v > 8 else 0).getbbox()
    sx, sy = (box[2] - box[0]) / 320, (box[3] - box[1]) / 240
    px = picture.load()
    total = [0, 0]
    names = ("15-bit", "4-bit", "15-bit T-junction", "4-bit T-junction")
    for name, outline in zip(names, seamdisc.OUTLINES):
        mask = Image.new("L", picture.size, 0)
        ImageDraw.Draw(mask).polygon([(box[0] + x * sx, box[1] + y * sy) for x, y in outline], fill=255)
        mask = mask.filter(ImageFilter.MinFilter(2 * int(max(sx, sy)) + 1))
        m = mask.load()
        magenta = cyan = 0
        x0, y0, x1, y1 = mask.getbbox()
        for y in range(y0, y1):
            for x in range(x0, x1):
                if not m[x, y]:
                    continue
                r, g, b = px[x, y]
                if r - g > 50 and b - g > 50:
                    magenta += 1
                elif b - r > 50 and b > 90:
                    cyan += 1
        total[0] += magenta
        total[1] += cyan
        print("%s mesh: %d magenta (texels from outside), %d cyan (gaps)" % (name, magenta, cyan))
    print("total: %d magenta, %d cyan" % tuple(total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
