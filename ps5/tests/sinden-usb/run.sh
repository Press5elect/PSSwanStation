#!/bin/sh
# PSSwanStation - builds and runs the Sinden Lightgun USB decoders' test on a PC
# (src/sinden_usb.cpp), then the MJPEG decoder's (src/sinden_mjpeg.cpp) against
# the host build's FFmpeg (build-host/ffmpeg, or FFMPEG_PREFIX) with pictures
# the PC's ffmpeg makes. The second part is skipped, and says so, without them.
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../src" && pwd)
repo=$(cd "$here/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-sinden.XXXXXX")
trap 'rm -rf "$work"' EXIT
cxx=${CXX:-c++}
flags="-std=c++20 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"

$cxx $flags "$here/test.cpp" "$src/sinden_usb.cpp" -o "$work/sinden_test"
"$work/sinden_test"

# The console's code itself (src/ps5/sinden_ps5.cpp) against a stand-in for
# libSceUsbd with a simulated gun, with AddressSanitizer and UndefinedBehavior-
# Sanitizer, then ThreadSanitizer (the pictures' hand-over between threads).
for sanitizer in address,undefined thread; do
    sflags="-std=c++20 -Wall -Wextra -g -O1 -fsanitize=$sanitizer -fno-sanitize-recover=all"
    $cxx $sflags -I"$here/fake" -include "$here/fake/stat_shim.h" -I"$src" -c "$src/ps5/sinden_ps5.cpp" -o "$work/sinden_ps5.o"
    $cxx $sflags "$here/console_test.cpp" "$src/sinden_usb.cpp" "$work/sinden_ps5.o" -lpthread -o "$work/console_test"
    echo "console rehearsal ($sanitizer):"
    "$work/console_test"
done

ffmpeg_prefix=${FFMPEG_PREFIX:-$repo/build-host/ffmpeg}
if [ ! -f "$ffmpeg_prefix/lib/libavcodec.a" ]; then
    echo "skip MJPEG: no FFmpeg build in $ffmpeg_prefix (build the host build first)"
    exit 0
fi
if ! command -v ffmpeg > /dev/null || ! command -v python3 > /dev/null; then
    echo "skip MJPEG: the PC's ffmpeg and python3 make its pictures"
    exit 0
fi
# A dark picture with a white border round a grey square and a gradient, as
# the gun's camera sees a screen with the lightgun border.
python3 - "$work/expected.gray" <<'EOF'
import sys
w, h = 320, 240
b = bytearray(w * h)
for y in range(h):
    for x in range(w):
        v = 16 + (x * 40) // w
        if 30 <= x < 290 and 20 <= y < 220 and (x < 40 or x >= 280 or y < 30 or y >= 210):
            v = 235
        elif 140 <= x < 180 and 100 <= y < 140:
            v = 128
        b[y * w + x] = v
open(sys.argv[1], "wb").write(b)
EOF
ffmpeg -loglevel error -y -f rawvideo -pix_fmt gray -s 320x240 -i "$work/expected.gray" -frames:v 1 \
    -c:v mjpeg -huffman default -q:v 2 -pix_fmt yuvj422p "$work/picture.jpg"
# The same picture without its Huffman tables (DHT segments), as UVC cameras send MJPEG.
python3 - "$work/picture.jpg" "$work/no-tables.jpg" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
out, i = bytearray(d[:2]), 2
while i + 4 <= len(d) and d[i] == 0xFF:
    marker, length = d[i + 1], (d[i + 2] << 8) | d[i + 3]
    if marker == 0xDA:
        out += d[i:]
        break
    if marker != 0xC4:
        out += d[i:i + 2 + length]
    i += 2 + length
assert b"\xff\xc4" not in out[:out.find(b"\xff\xda")]
open(sys.argv[2], "wb").write(out)
EOF
$cxx $flags -I"$ffmpeg_prefix/include" "$here/mjpeg_test.cpp" "$src/sinden_usb.cpp" "$src/sinden_mjpeg.cpp" \
    "$ffmpeg_prefix/lib/libavcodec.a" "$ffmpeg_prefix/lib/libavutil.a" -lpthread -lm -o "$work/mjpeg_test"
"$work/mjpeg_test" "$work/expected.gray" "$work/picture.jpg" "$work/no-tables.jpg"
