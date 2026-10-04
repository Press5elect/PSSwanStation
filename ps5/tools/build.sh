#!/usr/bin/env bash
# SwanStation for PS5: build the homebrew title.
#
#   ps5/tools/build.sh [OUT_DIR]
#
# Builds SwanStation's core and the frontend in ps5/ with ps5/toolchain.cmake,
# links them into a signed eboot.bin (ps5/tools/link.sh), and stages the title
# folder OUT_DIR/PPSA99248 (default: build-ps5/dist), ready to copy to
# /data/homebrew/PPSA99248 on the console.
#
# What it needs:
#   PS5_VULKAN_DIR    a PS5_Vulkan checkout (default: ../PS5_Vulkan beside this
#                     repository) prepared with tools/setup-native-dependencies.sh,
#                     tools/build-radv.sh release and tools/rebuild-libc.sh
#   PS5_PAYLOAD_SDK   the payload SDK fork (default: PS5_Vulkan's)
#   IMGUI_DIR         Dear ImGui v1.91 (default: ../imgui)
#   LIBSMB2_DIR       libsmb2 (default: ../deps-src/libsmb2)
# and, optionally:
#   CHTDB_DIR         a folder holding cheats.zip and patches.zip from the
#                     DuckStation chtdb release (default: ../deps-src); without
#                     them the title is staged with no cheat database
#   LIBRETRO_DATABASE_DIR  a checkout of github.com/libretro/libretro-database
#                     (default: ../deps-src/libretro-database; only its
#                     metadat/developer and metadat/redump PlayStation lists are
#                     read), from which the game database is made; without it
#                     the title has no descriptions
#   NETWORK_PATH      server/share/folder, written into the staged network.cfg
#                     (a build for one's own console; never in the repository)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
vk=$(cd -- "${PS5_VULKAN_DIR:-$src/../PS5_Vulkan}" && pwd)
export PS5_VULKAN_DIR="$vk"
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
imgui=${IMGUI_DIR:-$src/../imgui}
libsmb2=${LIBSMB2_DIR:-$src/../deps-src/libsmb2}
chtdb=${CHTDB_DIR:-$src/../deps-src}
build="$src/build-ps5"
out=${1:-$build/dist}
title=$(sed -n 's/.*"titleId": "\(PPSA[0-9]\{5\}\)".*/\1/p' "$ps5/sce_sys/param.json")
[[ -n $title ]] || { echo "ps5/sce_sys/param.json names no title" >&2; exit 2; }

tool="$vk/build/host/ps5-native-tool"
[[ -x $tool ]] || tool="$vk/build/runtime-shim/ps5-native-tool"
missing=0
for file in "$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
        "$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
        "$vk/runtime/libc.prx" "$tool" "$imgui/imgui.cpp" "$libsmb2/lib/libsmb2.c"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; missing=1; }
done
if (( missing )); then
    echo "Prepare $vk first (tools/setup-native-dependencies.sh, tools/build-radv.sh release," >&2
    echo "tools/rebuild-libc.sh), and check IMGUI_DIR and LIBSMB2_DIR." >&2
    exit 2
fi
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

# Configured every time: the build's date is set here.
mkdir -p "$build"
cmake -S "$ps5" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ps5/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DIMGUI_DIR="$imgui" -DLIBSMB2_DIR="$libsmb2" > "$build/configure.log" 2>&1 \
    || { cat "$build/configure.log" >&2; exit 2; }
cmake --build "$build" --target swanstation --parallel "${JOBS:-$(nproc)}"

app="$out/$title"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/assets" "$app/licenses"
cp -- "$build/swanstation" "$app/eboot.bin"
cp -- "$ps5/sce_sys/param.json" "$ps5/sce_sys/icon0.png" "$ps5/sce_sys/pic0.dds" "$ps5/sce_sys/pic1.dds" \
    "$app/sce_sys/"
cp -- "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
# The folders the title keeps its files in (README.txt says what goes where).
for dir in games bios covers cheats; do
    mkdir -p "$app/$dir"
done
# The helper elfldr runs when "USB drives" is on (ps5/src/ps5/elevation, from
# ps5-native-app-boilerplate's sandbox-elevation example).
make -s -C "$ps5/src/ps5/elevation/payload" PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" \
    OUTPUT="$build/sandbox-elevator.elf"
python3 "$ps5/src/ps5/elevation/validate-elevation-helper.py" "$build/sandbox-elevator.elf"
cp -- "$build/sandbox-elevator.elf" "$app/sandbox-elevator.elf"
# The cheat and patch database (DuckStation's chtdb), as its release ships it.
staged_db=0
for archive in cheats.zip patches.zip; do
    if [[ -f $chtdb/$archive ]]; then
        cp -- "$chtdb/$archive" "$app/assets/$archive"
        staged_db=1
    fi
done
(( staged_db )) || echo "No cheat database staged: $chtdb has no cheats.zip or patches.zip" >&2
# The game database: descriptions and the serials of discs by their names.
database=${LIBRETRO_DATABASE_DIR:-$src/../deps-src/libretro-database}
if [[ -f "$database/metadat/developer/Sony - PlayStation.dat" ]]; then
    python3 "$ps5/tools/make-gamedb.py" "$database" "$app/assets/gamedb.zip"
else
    echo "No game database staged: $database is not a libretro-database checkout" >&2
fi
if [[ -n ${NETWORK_PATH:-} ]]; then
    {
        echo "# SwanStation - games on a network share (SMB / Windows sharing)."
        echo "#"
        echo "# One \"path\" line for each folder to scan: server/share/folder, the server by"
        echo "# its IP address. The account: guest with no password for an open share."
        echo
        echo "path = $NETWORK_PATH"
        echo "user = guest"
        echo "password ="
        echo "# domain = WORKGROUP"
    } > "$app/network.cfg"
fi
cp -- "$ps5/README.txt" "$app/README.txt"
cp -- "$src/LICENSE" "$app/licenses/GPL-3.0.txt"
cp -- "$ps5/licenses/"* "$app/licenses/"
{
    echo "SwanStation for PS5, build $(sed -n 's/^const int BuildNumber = \([0-9]*\);.*/\1/p' "$ps5/src/main.cpp"), built $(date -u +%Y-%m-%d)"
    echo "SwanStation: $(git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown) plus the ps5 folder"
    echo "PS5_Vulkan:  $(git -C "$vk" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "SDK:         $(cat "$PS5_PAYLOAD_SDK/.ps5-sdk-revision" 2>/dev/null || echo unknown)"
    grep -h -E '^(revision|sdk):' "$vk/.deps/native/radv-release/PROVENANCE.txt" 2>/dev/null | sed 's/^/RADV /'
    echo "Dear ImGui:  $(git -C "$imgui" describe --tags --always 2>/dev/null || echo unknown)"
    echo "libsmb2:     $(git -C "$libsmb2" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "eboot.bin sha256: $(sha256sum "$app/eboot.bin" | cut -d' ' -f1)"
} > "$app/BUILD.txt"
# Everything is readable and writable over FTP.
find "$app" -type d -exec chmod 0777 {} +
find "$app" -type f -exec chmod 0666 {} +
printf 'Built %s (eboot.bin %s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
