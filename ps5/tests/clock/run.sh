#!/bin/sh
# PSSwanStation - the title's clock from the processor's counter (src/ps5/clock_ps5.c), on the PC.
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each case in a process of its own (the clock starts once in a process): with the address and
# undefined-behaviour sanitizers, and the threads case with the thread sanitizer. From Crux2's
# ps5/tests/clock. Nothing is left in the source tree.
#
#	sh run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../src/ps5"
work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-clock.XXXXXX")
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$here/clock_test.c" "$src/clock_ps5.c" -o "$work/clock_test" -lpthread
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=thread \
    "$here/clock_test.c" "$src/clock_ps5.c" -o "$work/clock_test_tsan" -lpthread
for which in math exact fast slow far-fast far-slow sleep other not-believable; do
    "$work/clock_test" "$which"
done
"$work/clock_test_tsan" threads
echo "clock: all passed"
