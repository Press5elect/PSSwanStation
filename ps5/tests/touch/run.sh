#!/usr/bin/env bash
# PSSwanStation: the touch pad's halves (src/touch_click.h), with the sanitizers.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=$(mktemp -d)
trap 'rm -rf -- "${out:?}"' EXIT
${CXX:-g++} -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../src" "$here/test.cpp" -o "$out/test"
"$out/test"
