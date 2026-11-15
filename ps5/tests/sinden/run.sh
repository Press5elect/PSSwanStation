#!/bin/sh
# PSSwanStation - builds and runs the Sinden Lightgun tracker's test on a PC
# (src/sinden_track.cpp), against camera frames it draws with a known aim.
#
#   run.sh          the quick run under AddressSanitizer and UndefinedBehaviorSanitizer,
#                   then the full run, optimised, that measures (about two minutes)
#   run.sh quick    the quick run only, optimised
#   run.sh full     the full run only
#   run.sh asan     the quick run under the sanitizers only
#
# SINDEN_OUT=<folder> writes some of the frames there (PGM, and PPM with what
# was found drawn on them).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-sinden.XXXXXX")
trap 'rm -rf "$work"' EXIT
mode=${1:-all}
cxx=${CXX:-c++}
if [ "$mode" = all ] || [ "$mode" = asan ]; then
	"$cxx" -std=c++17 -Wall -Wextra -g -O1 -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -DSINDEN_SANITIZED \
		"$here/test.cpp" "$here/../../src/sinden_track.cpp" -o "$work/sinden_asan"
	"$work/sinden_asan" quick
fi
if [ "$mode" != asan ]; then
	"$cxx" -std=c++17 -Wall -Wextra -O2 -pthread "$here/test.cpp" "$here/../../src/sinden_track.cpp" -o "$work/sinden_test"
	if [ "$mode" = quick ]; then
		"$work/sinden_test" quick
	else
		"$work/sinden_test"
	fi
fi
