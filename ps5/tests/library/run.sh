#!/bin/sh
# PSSwanStation - builds and runs the library's disc-grouping test on a PC (src/library_names.h).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-library.XXXXXX")
trap 'rm -rf "$work"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$here/test.cpp" -o "$work/library_test"
"$work/library_test"
