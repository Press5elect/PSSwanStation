#!/usr/bin/env bash
# PSSwanStation: build the homebrew title.
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
#   HUI_DIR           PS5_VKHomebrewUI, the interface kit, checked out at the
#                     revision below (default: ../PS5_VKHomebrewUI)
#   LIBSMB2_DIR       libsmb2 (default: ../deps-src/libsmb2)
#   RCHEEVOS_DIR      rcheevos v12 (default: ../deps-src/rcheevos)
#   GLSLANG_DIR       glslang 16.6.0 (default: ../deps-src/glslang)
#   LAPY_HELPER_DIR   a folder holding lapy.elf and lapy-manifest.json: the
#                     Lapy helper for this title, as ps5-native-app-boilerplate's
#                     tools/build-lapy-helper.py PPSA99248 builds it (default:
#                     ../deps-src/lapy). It is what lets the title leave its
#                     sandbox (USB drives, files kept outside the title folder)
# and, optionally:
#   CHTDB_DIR         a folder holding cheats.zip and patches.zip from the
#                     DuckStation chtdb release (default: ../deps-src); without
#                     them the title is staged with no cheat database
#   SLANG_SHADERS_DIR a checkout of github.com/libretro/slang-shaders (default:
#                     ../deps-src/slang-shaders; crt/ is read), from which the
#                     picture tube's shaders are made
#   LIBRETRO_DATABASE_DIR  a checkout of github.com/libretro/libretro-database
#                     (default: ../deps-src/libretro-database; only its
#                     metadat/developer and metadat/redump PlayStation lists and
#                     its cht/Sony - PlayStation cheats are read), from which the
#                     game database and the libretro cheats are made; without it
#                     the title has no descriptions
#   NETWORK_PATH      server/share/folder, written into the staged network.cfg
#                     (a build for one's own console; never in the repository)
#   RELEASE=1         a title folder that may be published: built from
#                     committed source only, with no network.cfg and without
#                     the chtdb files (their entries belong to their authors;
#                     the title fetches them from that project when asked).
#                     ps5/tools/release.py packs it with its source.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
vk=$(cd -- "${PS5_VULKAN_DIR:-$src/../PS5_Vulkan}" && pwd)
export PS5_VULKAN_DIR="$vk"
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
hui=${HUI_DIR:-$src/../PS5_VKHomebrewUI}
# The interface kit's revision: mihawk-99/PS5_VKHomebrewUI (BlackBearReloaded's
# ps5-homebrew-ui with a Vulkan backend), as PS5_VulkanTemplate pins it.
hui_revision=2188642afbcc8d84d36149124f3d9e39dc1b1a05
libsmb2=${LIBSMB2_DIR:-$src/../deps-src/libsmb2}
rcheevos=${RCHEEVOS_DIR:-$src/../deps-src/rcheevos}
lapy=${LAPY_HELPER_DIR:-$src/../deps-src/lapy}
chtdb=${CHTDB_DIR:-$src/../deps-src}
release=${RELEASE:-0}
if [[ $release == 1 ]]; then
    [[ -z ${NETWORK_PATH:-} ]] || { echo "build.sh: a release carries no network.cfg (NETWORK_PATH is set)" >&2; exit 2; }
    [[ -z $(git -C "$src" status --porcelain --untracked-files=no) ]] \
        || { echo "build.sh: a release is built from committed source; commit first" >&2; exit 2; }
fi
# The kit must be the pinned revision: its code and its sounds are in the title.
if [[ $(git -C "$hui" rev-parse HEAD 2>/dev/null) != "$hui_revision" ]]; then
    echo "build.sh: $hui is not PS5_VKHomebrewUI at $hui_revision" >&2
    exit 2
fi
build="$src/build-ps5"
out=${1:-$build/dist}
title=$(sed -n 's/.*"titleId": "\(PPSA[0-9]\{5\}\)".*/\1/p' "$ps5/sce_sys/param.json")
[[ -n $title ]] || { echo "ps5/sce_sys/param.json names no title" >&2; exit 2; }

tool="$vk/build/host/ps5-native-tool"
[[ -x $tool ]] || tool="$vk/build/runtime-shim/ps5-native-tool"
missing=0
for file in "$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
        "$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
        "$vk/runtime/libc.prx" "$tool" "$hui/src/gfx/vk/vk_renderer.cpp" "$libsmb2/lib/libsmb2.c" \
        "$rcheevos/src/rc_client.c" "$lapy/lapy.elf" "$lapy/lapy-manifest.json"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; missing=1; }
done
if (( missing )); then
    echo "Prepare $vk first (tools/setup-native-dependencies.sh, tools/build-radv.sh release," >&2
    echo "tools/rebuild-libc.sh), and check HUI_DIR, LIBSMB2_DIR, RCHEEVOS_DIR and LAPY_HELPER_DIR." >&2
    exit 2
fi
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)

# Configured every time: the build's date is set here.
mkdir -p "$build"
cmake -S "$ps5" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ps5/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DHUI_DIR="$hui" -DLIBSMB2_DIR="$libsmb2" -DRCHEEVOS_DIR="$rcheevos" \
    ${GLSLANG_DIR:+-DGLSLANG_DIR="$GLSLANG_DIR"} > "$build/configure.log" 2>&1 \
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
for dir in games bios covers cheats textures music borders layouts memcards/import memcards/export; do
    mkdir -p "$app/$dir"
done
# The Lapy helper the ELF loader runs when the title has to leave its sandbox
# and no resident Lapy service answers (ps5/src/ps5/elevation). It is built for
# this title alone: its record must name it and the file it describes.
python3 - "$lapy" "$title" <<'CHECK'
import hashlib, json, sys
folder, title = sys.argv[1], sys.argv[2]
record = json.load(open(folder + "/lapy-manifest.json"))
if record.get("target_title") != title:
    sys.exit("build.sh: the Lapy helper in %s was built for %s, not %s" % (folder, record.get("target_title"), title))
if hashlib.sha256(open(folder + "/lapy.elf", "rb").read()).hexdigest() != record.get("elf_sha256"):
    sys.exit("build.sh: lapy.elf is not the file lapy-manifest.json describes")
CHECK
cp -- "$lapy/lapy.elf" "$lapy/lapy-manifest.json" "$app/"
# The cheat and patch database (DuckStation's chtdb), as its release ships it.
staged_db=0
for archive in cheats.zip patches.zip; do
    if [[ $release != 1 && -f $chtdb/$archive ]]; then
        cp -- "$chtdb/$archive" "$app/assets/$archive"
        staged_db=1
    fi
done
if [[ $release == 1 ]]; then
    echo "A release: the cheat database is not staged (the title fetches it when asked)"
else
    (( staged_db )) || echo "No cheat database staged: $chtdb has no cheats.zip or patches.zip" >&2
fi
# The game database: descriptions and the serials of discs by their names.
database=${LIBRETRO_DATABASE_DIR:-$src/../deps-src/libretro-database}
if [[ -f "$database/metadat/developer/Sony - PlayStation.dat" ]]; then
    python3 "$ps5/tools/make-gamedb.py" "$database" "$app/assets/gamedb.zip"
    # The libretro database's cheats (CC BY-SA 4.0), for games chtdb has none for.
    if [[ -d "$database/cht/Sony - PlayStation" ]]; then
        python3 "$ps5/tools/make-libretro-cheats.py" "$database" "$app/assets/libretro-cheats.zip"
    else
        echo "No libretro cheats staged: $database has no cht/Sony - PlayStation" >&2
    fi
else
    echo "No game database staged: $database is not a libretro-database checkout" >&2
fi
# The interface kit's sounds and songs (GPL-3.0-or-later): its two recorded
# sets of the menus' sounds and its three songs. Its fonts are in eboot.bin.
mkdir -p "$app/assets/hui/sfx" "$app/assets/hui/music"
cp -r -- "$hui/assets/audio/sfx/glass" "$hui/assets/audio/sfx/paper" "$app/assets/hui/sfx/"
cp -- "$hui"/assets/audio/music/*.ogg "$app/assets/hui/music/"
# The picture tube: crt-guest-advanced from the libretro slang shaders
# (GPL-2.0-or-later), compiled for the title's chain runner.
slang=${SLANG_SHADERS_DIR:-$src/../deps-src/slang-shaders}
if [[ -f "$slang/crt/crt-guest-advanced.slangp" ]]; then
    python3 "$ps5/tools/make-chains.py" "$slang" "$app/assets/shaders"
else
    echo "No CRT shaders staged: $slang is not a slang-shaders checkout" >&2
fi
if [[ -n ${NETWORK_PATH:-} ]]; then
    {
        echo "# PSSwanStation - games on the network: an SMB share (Windows sharing) or an"
        echo "# FTP server."
        echo "#"
        echo "# One \"path\" line for each folder to scan, the server by its IP address:"
        echo "# server/share/folder for an SMB share, ftp://server/folder for an FTP server"
        echo "# (ftp://user:password@server:port/folder with an account or another port)."
        echo "# The account below: guest with no password for an open share."
        echo
        echo "path = $NETWORK_PATH"
        echo "user = guest"
        echo "password ="
        echo "# domain = WORKGROUP"
    } > "$app/network.cfg"
fi
cp -- "$ps5/README.txt" "$app/README.txt"
cp -- "$ps5/CHANGELOG.txt" "$app/CHANGELOG.txt"
cp -- "$ps5/LEGAL.txt" "$app/LEGAL.txt"
cp -- "$src/LICENSE" "$app/licenses/GPL-3.0.txt"
cp -- "$ps5/licenses/"* "$app/licenses/"
{
    echo "PSSwanStation, build $(sed -n 's/^const int BuildNumber = \([0-9]*\);.*/\1/p' "$ps5/src/main.cpp"), built $(date -u +%Y-%m-%d)"
    echo "SwanStation: $(git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown) plus the ps5 folder"
    echo "PS5_Vulkan:  $(git -C "$vk" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "SDK:         $(cat "$PS5_PAYLOAD_SDK/.ps5-sdk-revision" 2>/dev/null || echo unknown)"
    grep -h -E '^(revision|sdk):' "$vk/.deps/native/radv-release/PROVENANCE.txt" 2>/dev/null | sed 's/^/RADV /'
    echo "UI kit:      $(git -C "$hui" rev-parse HEAD 2>/dev/null || echo unknown) (PS5_VKHomebrewUI)"
    echo "libsmb2:     $(git -C "$libsmb2" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "libnfs:      $(git -C "${LIBNFS_DIR:-$src/../deps-src/libnfs}" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "slang-shaders: $(git -C "$slang" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "rcheevos:    $(git -C "$rcheevos" describe --tags --always 2>/dev/null || echo unknown)"
    echo "glslang:     $(git -C "${GLSLANG_DIR:-$src/../deps-src/glslang}" describe --tags --always 2>/dev/null || echo unknown)"
    echo "Lapy helper: $(sed -n 's/.*"elf_sha256": "\([0-9a-f]*\)".*/\1/p' "$lapy/lapy-manifest.json") (sha256)"
    echo "eboot.bin sha256: $(sha256sum "$app/eboot.bin" | cut -d' ' -f1)"
} > "$app/BUILD.txt"
# Every part as data, with the revision it was built from.
python3 "$ps5/tools/stage-notices.py" "$app" "$src" "$vk" "$PS5_PAYLOAD_SDK" "$hui" "$libsmb2" "$rcheevos" "$lapy" \
    "${LIBRETRO_DATABASE_DIR:-$src/../deps-src/libretro-database}" "${LIBNFS_DIR:-$src/../deps-src/libnfs}" "$slang" \
    "${GLSLANG_DIR:-$src/../deps-src/glslang}"
# Everything is readable and writable over FTP; the program's own files as the
# console wants a title's (and as the updater leaves them).
find "$app" -type d -exec chmod 0777 {} +
find "$app" -type f -exec chmod 0666 {} +
chmod 0777 "$app/eboot.bin" "$app/lapy.elf" "$app/sce_module/libc.prx" "$app/sce_sys/param.json"
printf 'Built %s (eboot.bin %s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
