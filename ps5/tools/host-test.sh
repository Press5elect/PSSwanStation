#!/usr/bin/env bash
# SwanStation for PS5: the PC test run.
#
#   ps5/tools/host-test.sh [ROOT_DIR]
#
# Builds the title's frontend and the emulator for this PC (build-host), makes
# a small library of generated test discs (ps5/tools/make-test-disc.py, no
# game is needed), and runs the title without a window or a pad: a script
# presses the buttons and saves screenshots into ROOT_DIR (default:
# build-host/test-root). The Vulkan driver is the PC's (lavapipe will do), with
# VK_EXT_headless_surface.
#
#   SWANSTATION_SIZE=3840x2160   the picture's size (default 1920x1080)
#   SWANSTATION_GPU=name         which Vulkan device, by a part of its name
#
# Needs python3 with pycdlib (the discs).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
build="$src/build-host"
root=${1:-$build/test-root}
if [[ ! -f $build/build.ninja ]]; then
    cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        ${IMGUI_DIR:+-DIMGUI_DIR="$IMGUI_DIR"} ${LIBSMB2_DIR:+-DLIBSMB2_DIR="$LIBSMB2_DIR"}
fi
cmake --build "$build" --target swanstation --parallel "${JOBS:-$(nproc)}"

mkdir -p "$root/games" "$root/assets" "$root/data"
disc() { [[ -f "$root/games/$1.cue" ]] || python3 "$ps5/tools/make-test-disc.py" "$root/games/$1.cue" --serial "$2" --colour "$3"; }
# Serials the cheat database knows, so its entries can be seen and switched.
disc "Patch Test (Europe)" SCES-00568 804020
disc "Choice Test (Europe)" SLES-01208 205030
disc "Two Discs (USA) (Disc 1)" SLUS-99901 602060
disc "Two Discs (USA) (Disc 2)" SLUS-99902 206060
chtdb=${CHTDB_DIR:-$src/../deps-src}
for archive in cheats.zip patches.zip; do
    [[ -f $chtdb/$archive ]] && cp -- "$chtdb/$archive" "$root/assets/"
done
# The software rasteriser of a PC draws 1x quickly enough to script against.
[[ -f $root/data/options.cfg ]] || printf 'swanstation_GPU_ResolutionScale = "1"\n' > "$root/data/options.cfg"

cat > "$root/script.txt" <<'SCRIPT'
40 shot 01-library
45 right
60 cross
400 shot 02-game
405 options
420 shot 03-menu
425 down
432 down
439 down
446 cross
460 shot 04-cheats
465 cross
480 shot 05-cheat-on
485 circle
495 up
502 up
509 cross
520 cross
600 shot 06-state-saved
605 options
612 up
620 cross
628 cross
650 shot 07-library-again
SCRIPT
SWANSTATION_ROOT="$root" SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=660 \
    SWANSTATION_SIZE=${SWANSTATION_SIZE:-1920x1080} "$build/swanstation" > "$root/run.log" 2>&1
grep -E "game:|cheats:|state:|shot|CRASH" "$root/swanstation-boot.log"
if grep -q CRASH "$root/swanstation-boot.log"; then
    echo "host-test: the run crashed (see $root/swanstation-boot.log)" >&2
    exit 1
fi
echo "host-test: screenshots are in $root"
