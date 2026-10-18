#!/usr/bin/env bash
# PSSwanStation - the HD texture block test: gpu_scenes.c drawn by the
# Vulkan renderer three times. First writing out the texture blocks it uses
# (dump/block-<bits>-<hash>.png); then with a pack made from them (each
# picture four times larger, its colours inverted) and without one. Where
# the scenes draw textures the two pictures must differ (the pack's colours),
# and nowhere else.
#
#   ps5/tests/gpu/blocks.sh
#
# Needs what run.sh needs. OUT a folder for the results (default:
# build-host/gpu-blocks).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(cd -- "$here/../../.." && pwd)
guest="$src/tests/guest"
out=${OUT:-$src/build-host/gpu-blocks}
mkdir -p "$out"

flags="--target=mipsel-unknown-none -march=mips1 -mno-abicalls -fno-pic -G0 -O2 -ffreestanding -fno-builtin -nostdlib"
clang $flags -c "$guest/crt0.S" -o "$out/crt0.o" 2>/dev/null
clang $flags -c "$here/gpu_scenes.c" -o "$out/gpu_scenes.o" 2> >(grep -v experimental >&2)
ld.lld -T "$guest/rom.ld" "$out/crt0.o" "$out/gpu_scenes.o" -o "$out/gpu_scenes.elf"
llvm-objcopy -O binary -j .reset -j .magic -j .params -j .exc -j .text -j .rodata "$out/gpu_scenes.elf" "$out/gpu_scenes.rom"

# run NAME SCALE DUMP [PACK]: one run in a root of its own; the screen in NAME.png
run()
{
    local name=$1 scale=$2 dump=$3 pack=${4:-} root="$out/root-$1"
    rm -rf "$root"
    mkdir -p "$root/bios" "$root/data" "$root/textures/GPUTEST"
    [[ -z $pack ]] || cp -- "$pack"/*.png "$root/textures/GPUTEST/"
    python3 "$guest/mkrom.py" "$out/gpu_scenes.rom" "$root/bios/scph5501.bin" 1 0
    cp -- "$src/ps5/CHANGELOG.txt" "$root/"
    printf 'splash = 0\nseen_build = 1000000\nupdate_check = 0\ncovers = 0\n' > "$root/frontend.cfg"
    {
        echo 'swanstation_GPU_Renderer = "Vulkan"'
        echo "swanstation_GPU_ResolutionScale = \"$scale\""
        echo 'swanstation_BIOS = "scph5501.bin"'
        echo 'swanstation_TextureReplacements_EnableTextureBlocks = "true"'
        echo "swanstation_TextureReplacements_DumpTextureBlocks = \"$dump\""
    } > "$root/data/options.cfg"
    printf '200 shot %s\n' "$name" > "$root/script.txt"
    (cd "$src/build-host" && SWANSTATION_BOOT_BIOS=1 SWANSTATION_FRAME_CLOCK=1 SWANSTATION_ROOT="$root" \
        SWANSTATION_TEXTURE_ID=GPUTEST SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=210 \
        SWANSTATION_SIZE=1920x1080 timeout 300 ./swanstation > "$root/run.log" 2>&1) || true
    [[ -f $root/$name.png ]] || { echo "blocks test: $name took no picture"; tail -20 "$root/psswanstation-boot.log"; exit 1; }
    mv "$root/$name.png" "$out/$name.png"
}

run dump 1 true
dumped=("$out/root-dump/textures/GPUTEST/dump"/block-*.png)
[[ -f ${dumped[0]} ]] || { echo "blocks test: no texture block written out"; exit 1; }
echo "written out: ${#dumped[@]} blocks"
rm -rf "$out/pack" && mkdir -p "$out/pack"
python3 -I - "$out/pack" "${dumped[@]}" <<'PY'
import sys
from pathlib import Path
from PIL import Image, ImageOps
dst = Path(sys.argv[1])
for f in sys.argv[2:]:
    img = Image.open(f).convert("RGBA")
    r, g, b, a = img.split()
    rgb = ImageOps.invert(Image.merge("RGB", (r, g, b)))
    big = Image.merge("RGBA", (*rgb.split(), a)).resize((img.width * 4, img.height * 4), Image.NEAREST)
    big.save(dst / Path(f).name)
    print("  %s %dx%d" % (Path(f).name, img.width, img.height))
PY
run base 4 false
run pack 4 false "$out/pack"
python3 -I "$here/compare.py" --blocks "$out"
