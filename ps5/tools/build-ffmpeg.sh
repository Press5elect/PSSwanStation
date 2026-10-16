#!/usr/bin/env bash
# PSSwanStation: build the part of FFmpeg the title plays ES-DE themes'
# videos with: libavformat (the MP4 container), libavcodec (the H.264, AAC
# and MP3 decoders and their parsers) and libavutil, as static libraries,
# LGPL-2.1-or-later (no --enable-gpl, no --enable-nonfree).
#
#   ps5/tools/build-ffmpeg.sh ps5|host OUT_DIR
#
# OUT_DIR gets include/ and lib/ (libavformat.a, libavcodec.a, libavutil.a).
# The console refuses the title its own player (libSceAvPlayer), so the title
# decodes the videos itself, on the processor.
#
# What it needs:
#   FFMPEG_DIR        FFmpeg's source at the revision below (default:
#                     ../deps-src/ffmpeg-n9.0.2 beside this repository)
#   PS5_PAYLOAD_SDK   for ps5: the payload SDK (prospero-clang)
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
target=${1:?ps5 or host}
out=${2:?an output folder}
ffmpeg=$(cd -- "${FFMPEG_DIR:-$src/../deps-src/ffmpeg-n9.0.2}" && pwd)
# FFmpeg n9.0.2 (github.com/FFmpeg/FFmpeg, the tag's commit).
revision=946fcce07b6dcd0331c8cc609192aeff5e1924f8
if [[ -d $ffmpeg/.git && $(git -C "$ffmpeg" rev-parse HEAD) != "$revision" ]]; then
    echo "build-ffmpeg.sh: $ffmpeg is not FFmpeg at $revision" >&2
    exit 2
fi

mkdir -p "$out"
out=$(cd -- "$out" && pwd)
stamp="$out/.built"
want="$revision $target $(sha256sum "${BASH_SOURCE[0]}" | cut -c1-16)"
if [[ -f $stamp && $(cat "$stamp") == "$want" && -f $out/lib/libavformat.a ]]; then
    exit 0
fi
work="$out/build"
rm -rf -- "$work" "$out/include" "$out/lib"
mkdir -p "$work"

common=(
    --prefix="$out" --enable-static --disable-shared --enable-pic
    --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample
    --disable-everything --disable-debug
    --enable-pthreads
    --enable-decoder=h264,aac,aac_fixed,mp3,mp3float
    --enable-parser=h264,aac,mpegaudio
    --enable-demuxer=mov
)
# The build machine's paths are kept out of the objects (a published file
# names no folder of the machine it was built on).
prefix_map="-ffile-prefix-map=$ffmpeg/= -ffile-prefix-map=$work/="

case $target in
ps5)
    sdk=${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK is not set}
    flags=(
        --enable-cross-compile --target-os=freebsd --arch=x86_64 --cpu=znver2
        --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++"
        --ar="$sdk/bin/prospero-ar" --ranlib="$sdk/bin/prospero-ranlib" --nm="$sdk/bin/prospero-nm"
        --strip="$sdk/bin/prospero-strip" --pkg-config=false
        --extra-cflags="-ffp-contract=off -ffunction-sections -fdata-sections $prefix_map"
        --extra-ldflags="-lkernel_web -lSceLibcInternal"
    )
    ;;
host)
    flags=(--extra-cflags="-ffunction-sections -fdata-sections $prefix_map")
    ;;
*)
    echo "build-ffmpeg.sh: ps5 or host, not $target" >&2
    exit 2
    ;;
esac

(cd "$work" && "$ffmpeg/configure" "${common[@]}" "${flags[@]}" > configure.log 2>&1) \
    || { tail -40 "$work/ffbuild/config.log" >&2; exit 2; }
make -C "$work" -j"${JOBS:-$(nproc)}" > "$work/make.log" 2>&1 || { tail -40 "$work/make.log" >&2; exit 2; }
make -C "$work" install > /dev/null
echo "$want" > "$stamp"
echo "FFmpeg $(cat "$ffmpeg/RELEASE" 2>/dev/null) for $target in $out"
