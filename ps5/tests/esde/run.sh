#!/usr/bin/env bash
# PSSwanStation: the ES-DE theme reader's test (esde.cpp) on the PC, with
# AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   ps5/tests/esde/run.sh
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ps5=$(cd "$here/../.." && pwd)
out=${BUILD_DIR:-$ps5/../build-host}/esde-test
mkdir -p "$out"
c++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$ps5/src" -I"$ps5/third_party/pugixml" \
    "$here/test.cpp" "$ps5/src/esde.cpp" "$ps5/third_party/pugixml/pugixml.cpp" -o "$out/esde-test"
c++ -std=c++20 -O1 -g -I"$ps5/src" -I"$ps5/third_party/pugixml" \
    "$here/dump.cpp" "$ps5/src/esde.cpp" "$ps5/third_party/pugixml/pugixml.cpp" -o "$out/esde-dump"
cd "$here"
"$out/esde-test" "$here/theme"
echo "esde test: passed (esde-dump THEME_FOLDER prints any theme's views: $out/esde-dump)"
