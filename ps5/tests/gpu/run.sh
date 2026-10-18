#!/usr/bin/env bash
# PSSwanStation - the renderer tests: gpu_scenes.c drawn by the software
# renderer and by the hardware renderer (Vulkan, the PC's driver), video
# memory compared cell by cell, and the background counted where the scenes
# cover it.
#
#   ps5/tests/gpu/run.sh [SCALE...]        (default: 1 4)
#
# Needs the PC build (build-host, ps5/tools/host-test.sh makes it), clang,
# ld.lld and llvm-objcopy for the MIPS program, python3 with Pillow.
# Environment: MODE 1 draws the 3D scenes (gpu_scenes.c) in the first nine
# cells, GPU_OPTIONS adds lines to data/options.cfg for the hardware
# runs (one option a line), OUT a folder for the results (default:
# build-host/gpu-test).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(cd -- "$here/../../.." && pwd)
guest="$src/tests/guest"
out=${OUT:-$src/build-host/gpu-test}
scales=("$@")
[[ ${#scales[@]} -gt 0 ]] || scales=(1 4)
mkdir -p "$out"

flags="--target=mipsel-unknown-none -march=mips1 -mno-abicalls -fno-pic -G0 -O2 -ffreestanding -fno-builtin -nostdlib"
clang $flags -c "$guest/crt0.S" -o "$out/crt0.o" 2>/dev/null
clang $flags -c "$here/gpu_scenes.c" -o "$out/gpu_scenes.o" 2> >(grep -v experimental >&2)
ld.lld -T "$guest/rom.ld" "$out/crt0.o" "$out/gpu_scenes.o" -o "$out/gpu_scenes.elf"
llvm-objcopy -O binary -j .reset -j .magic -j .params -j .exc -j .text -j .rodata "$out/gpu_scenes.elf" "$out/gpu_scenes.rom"

# run NAME RENDERER SCALE: one run in a root of its own; video memory in NAME.vram, the screen in NAME.png
run()
{
    local name=$1 renderer=$2 scale=$3 root="$out/root-$1"
    rm -rf "$root"
    mkdir -p "$root/bios" "$root/data"
    python3 "$guest/mkrom.py" "$out/gpu_scenes.rom" "$root/bios/scph5501.bin" 1 "${MODE:-0}"
    cp -- "$src/ps5/CHANGELOG.txt" "$root/"
    {
        echo "splash = 0"
        echo "seen_build = 1000000"
        echo "update_check = 0"
        echo "covers = 0"
    } > "$root/frontend.cfg"
    {
        echo "swanstation_GPU_Renderer = \"$renderer\""
        echo "swanstation_GPU_ResolutionScale = \"$scale\""
        echo "swanstation_BIOS = \"scph5501.bin\""
        [[ $renderer == Software ]] || printf '%s\n' "${GPU_OPTIONS:-}"
    } > "$root/data/options.cfg"
    printf '200 shot %s\n' "$name" > "$root/script.txt"
    (cd "$src/build-host" && SWANSTATION_BOOT_BIOS=1 SWANSTATION_FRAME_CLOCK=1 SWANSTATION_ROOT="$root" \
        SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=210 SWANSTATION_SIZE=1920x1080 \
        SWANSTATION_VRAM_DUMP="120:$out/$name.vram" timeout 300 ./swanstation > "$root/run.log" 2>&1) || true
    [[ -f $out/$name.vram ]] || { echo "gpu test: $name wrote no video memory"; tail -20 "$root/psswanstation-boot.log"; exit 1; }
    [[ -f $root/$name.png ]] && mv "$root/$name.png" "$out/$name.png"
}

run sw Software 1
for scale in "${scales[@]}"; do
    run "hw$scale" Vulkan "$scale"
done
python3 -I "$here/compare.py" "$out" "${scales[@]}"
