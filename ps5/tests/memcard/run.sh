#!/bin/sh
# PSSwanStation - builds and runs the memory card test on a PC.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# memcard.cpp with the test and its stand-ins for the frontend, under the
# address and undefined-behaviour sanitizers: the test feeds it damaged cards,
# and a read outside a card must fail the run. Nothing is left in the source
# tree: the program and the files it writes go to a temporary folder.
#
#	sh run.sh            build and run
#	CXX=clang++-18 sh run.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src"
work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-memcard.XXXXXX")
trap 'rm -rf "$work"' EXIT

"${CXX:-clang++}" -std=c++20 -Wall -Wextra -g -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$src" "$here/test.cpp" "$here/stubs.cpp" "$src/memcard.cpp" -o "$work/memcard-test"

ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 \
	"$work/memcard-test" "$work"
