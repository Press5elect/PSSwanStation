#!/bin/sh
# PSSwanStation - the updater's test: builds update.cpp alone (with the test's
# stand-ins for the frontend's helpers and the project's miniz) and runs it
# against a local stand-in for GitHub's releases.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   run.sh [plain] [asan] [tsan]      which builds to run; all three by default
#
#   UPDATE_TEST_WORK   the folder to work in (default: a new temporary one,
#                      removed afterwards)
#   UPDATE_TEST_ZIP    a real release ZIP of the title to go through as well
#   UPDATE_TEST_ONLY   the name of one test
#   CXX, CC            the compilers (default: clang++, clang)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
miniz="$root/dep/libchdr/deps/miniz-3.1.1"
CXX=${CXX:-clang++}
CC=${CC:-clang}
real=${UPDATE_TEST_ZIP:-$root/../out/PSSwanStation-PS5-PPSA99248-build9.zip}
builds=${*:-plain asan tsan}

if [ -n "${UPDATE_TEST_WORK:-}" ]; then
	work=$UPDATE_TEST_WORK
	mkdir -p "$work"
	keep=1
else
	work=$(mktemp -d "${TMPDIR:-/tmp}/psswan-update-test.XXXXXX")
	keep=0
fi

server=
finish()
{
	[ -n "$server" ] && kill "$server" 2>/dev/null
	[ "$keep" = 0 ] && rm -rf "$work"
	return 0
}
trap finish EXIT

# The releases: built and served by the stand-in. It writes its port when it
# listens (the first run also makes a ZIP that unpacks to over 1 GiB).
rm -f "$work/port"
python3 -I "$here/mock_github.py" "$work" "$real" &
server=$!
waited=0
while [ ! -s "$work/port" ]; do
	kill -0 "$server" 2>/dev/null || { echo "the stand-in server did not start"; exit 2; }
	[ "$waited" -lt 1800 ] || { echo "the stand-in server took too long"; exit 2; }
	sleep 0.1
	waited=$((waited + 1))
done
port=$(cat "$work/port")
echo "releases served on 127.0.0.1:$port, working in $work"

# The switches the title's own build gives miniz: no stdio, no time, the
# archive APIs kept (SWANSTATION_STANDALONE).
common="-DSWANSTATION_STANDALONE=1 -DPSSWAN_UPDATE_TEST=1 -I$root/ps5/src -isystem $miniz -g -pthread"
failed=0
for build in $builds; do
	case $build in
		plain) flags="-O1" ;;
		asan) flags="-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" ;;
		tsan) flags="-O1 -fsanitize=thread" ;;
		*) echo "unknown build: $build"; exit 2 ;;
	esac
	echo "== $build"
	$CC $common $flags -w -c "$miniz/miniz.c" -o "$work/miniz-$build.o"
	$CXX -std=c++20 -Wall -Wextra $common $flags \
		"$here/test.cpp" "$here/stubs.cpp" "$root/ps5/src/update.cpp" "$work/miniz-$build.o" -o "$work/test-$build"
	if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 \
		"$work/test-$build" "$work" "$port" ${UPDATE_TEST_ONLY:-}; then
		echo "== $build: passed"
	else
		echo "== $build: FAILED"
		failed=1
	fi
done
exit $failed
