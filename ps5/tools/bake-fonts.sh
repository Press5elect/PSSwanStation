#!/usr/bin/env bash
# PSSwanStation: bake the interface's fonts into ps5/assets/hui/fonts, as the
# interface kit draws them (signed distance fields, .huifont). Its six faces
# from PS5_VKHomebrewUI's third_party/fonts, with accented Latin and Cyrillic
# (game names are not all ASCII), and Font Awesome's icons that ps5/src/ui.h
# names, as a seventh. The results are kept in the repository, so a build
# needs no baking; run this again after adding an icon to ui.h.
#
#   ps5/tools/bake-fonts.sh [PS5_VKHomebrewUI checkout]
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit=$(cd -- "${1:-${HUI_DIR:-$ps5/../../PS5_VKHomebrewUI}}" && pwd)
out="$ps5/assets/hui/fonts"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
"${HOST_CXX:-clang++}" -std=c++20 -O2 -w -I"$kit/third_party/stb" -I"$kit/src" \
    "$ps5/tools/font-baker/bake_font.cpp" -o "$work/bake_font"
while read -r source output size range atlas glyphs; do
    "$work/bake_font" "$kit/third_party/fonts/$source" "$out/$output.huifont" "$size" "$range" "$atlas" "$glyphs" \
        2>/dev/null
done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8 2048 european
Inter-SemiBold.ttf inter-semibold 56 8 2048 european
Montserrat-Medium.ttf montserrat-medium 56 8 2048 european
DejaVuSansMono.ttf dejavu-sans-mono 52 8 2048 european
PressStart2P-Regular.ttf press-start-2p 32 4 1024 european
PatrickHand-Regular.ttf patrick-hand 56 8 2048 european
FONTS
# The icons: every "// fXXX" ui.h gives a constant in namespace icon.
sed -n '/^namespace icon/,/^}/p' "$ps5/src/ui.h" | grep -o '// f[0-9a-f]\{3\}' | cut -c4- | sort -u > "$work/icons.txt"
"$work/bake_font" "$ps5/assets/fa-solid-900.ttf" "$out/icons.huifont" 56 8 1024 "list=$work/icons.txt"
cp "$kit"/third_party/fonts/*-LICENSE.txt "$out/"
