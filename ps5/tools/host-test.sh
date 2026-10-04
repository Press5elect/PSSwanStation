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
disc "Choice Test (Europe)" SLES-01208 205030
disc "Patch Test (Europe)" SCES-00568 804020
disc "Two Discs (USA) (Disc 1)" SLUS-99901 602060
disc "Two Discs (USA) (Disc 2)" SLUS-99902 206060
chtdb=${CHTDB_DIR:-$src/../deps-src}
for archive in cheats.zip patches.zip; do
    [[ -f $chtdb/$archive ]] && cp -- "$chtdb/$archive" "$root/assets/"
done
database=${LIBRETRO_DATABASE_DIR:-$src/../deps-src/libretro-database}
[[ -f "$database/metadat/developer/Sony - PlayStation.dat" && ! -f $root/assets/gamedb.zip ]] \
    && python3 "$ps5/tools/make-gamedb.py" "$database" "$root/assets/gamedb.zip"
# The software rasteriser of a PC draws 1x quickly enough to script against.
[[ -f $root/data/options.cfg ]] || printf 'swanstation_GPU_ResolutionScale = "1"\n' > "$root/data/options.cfg"
rm -f "$root/data/history.txt"

# Frames, with the animations' clock counting them (SWANSTATION_FRAME_CLOCK):
# the splash is on the screen until frame 150, the swan lands in the header by 240,
# and a game's start takes the 63 frames of its flight at the screen.
cat > "$root/script.txt" <<'SCRIPT'
90 shot 01-splash
200 shot 02-splash-flight
270 shot 03-library
275 right
285 triangle
300 shot 04-details
305 right
313 right
321 cross
335 shot 05-details-options
340 circle
348 right
356 cross
370 shot 06-details-cheats
375 cross
390 shot 07-cheat-on
395 circle
403 left
411 left
419 left
497 cross
810 shot 08-game
815 options
830 shot 09-menu
835 down
845 cross
855 cross
910 shot 10-state-saved
915 options
922 up
930 cross
938 cross
970 shot 11-library-shelf
SCRIPT
SWANSTATION_FRAME_CLOCK=1 SWANSTATION_ROOT="$root" SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=990 \
    SWANSTATION_SIZE=${SWANSTATION_SIZE:-1920x1080} "$build/swanstation" > "$root/run.log" 2>&1
grep -E "game:|cheats:|state:|shot|CRASH" "$root/swanstation-boot.log"
if grep -q CRASH "$root/swanstation-boot.log"; then
    echo "host-test: the run crashed (see $root/swanstation-boot.log)" >&2
    exit 1
fi
echo "host-test: screenshots are in $root"
