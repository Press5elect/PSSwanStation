#!/usr/bin/env bash
# PSSwanStation: the achievements module's test.
#
#   ps5/tests/achievements/run.sh [plain|asan|tsan|slow]...   (default: plain asan tsan)
#
# Builds ps5/src/achievements.cpp with rcheevos' sources and the test in this
# folder, makes three test discs with ps5/tools/make-test-disc.py, starts
# mock_server.py on a free local port and runs the test against it: plainly,
# under the address and undefined-behaviour sanitizers, and under the thread
# sanitizer. "slow" is the plain build with the minute it takes to see a
# game's load tried again and a ping. Nothing is sent to retroachievements.org: the module is pointed at the
# mock server, and the test ends itself on a request to any other.
#
# What it needs: clang, python3 with pycdlib (for the discs), curl, and
#   RCHEEVOS_DIR   an rcheevos checkout (default: ../deps-src/rcheevos beside
#                  this repository), at the version the title is built with
# Everything it writes goes to WORK_DIR (default: a temporary folder, removed
# at the end; KEEP=1 keeps it).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd -- "$here/../.." && pwd)
rc=${RCHEEVOS_DIR:-$ps5/../../deps-src/rcheevos}
[[ -f $rc/include/rc_client.h ]] || { echo "no rcheevos in $rc (set RCHEEVOS_DIR)" >&2; exit 2; }
cc=${CC:-clang}
cxx=${CXX:-clang++}

work=${WORK_DIR:-$(mktemp -d)}
mkdir -p "$work"
work=$(cd -- "$work" && pwd)

# The part of rcheevos the title compiles (the same list and definitions go
# into ps5/CMakeLists.txt): rc_client, the runtime, the server calls, and the
# disc hashing without the cartridge, ZIP and encrypted formats.
rc_sources=(
    rc_client.c rc_compat.c rc_util.c rc_version.c
    rapi/rc_api_common.c rapi/rc_api_info.c rapi/rc_api_runtime.c rapi/rc_api_user.c
    rcheevos/alloc.c rcheevos/condition.c rcheevos/condset.c rcheevos/consoleinfo.c rcheevos/format.c
    rcheevos/lboard.c rcheevos/memref.c rcheevos/operand.c rcheevos/richpresence.c rcheevos/runtime.c
    rcheevos/runtime_progress.c rcheevos/trigger.c rcheevos/value.c
    rhash/md5.c rhash/hash.c rhash/hash_disc.c rhash/cdreader.c
)
rc_definitions=(-DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP -DRC_HASH_NO_ENCRYPTED)

# rcheevos' own files get the address and thread sanitizers but not the
# undefined-behaviour one: its parser measures what it will allocate by
# offsetting a null pointer (RC_ALLOC with no buffer), and adds 0 to a null
# array where a game has no achievements. Both are reported, neither is a
# fault of the module, which is built with every check, as the test is.
build() {   # NAME "FLAGS FOR RCHEEVOS" "FLAGS FOR THE MODULE AND THE TEST"
    local name=$1 dir=$work/build-$1 objects=()
    local -a for_rc=($2) for_module=($3)
    mkdir -p "$dir"
    for source in "${rc_sources[@]}"; do
        local object=$dir/${source//\//_}.o
        "$cc" -c -O1 -g "${for_rc[@]}" "${rc_definitions[@]}" -I"$rc/include" "$rc/src/$source" -o "$object"
        objects+=("$object")
    done
    "$cxx" -std=c++20 -Wall -Wextra -O1 -g -pthread "${for_module[@]}" "${rc_definitions[@]}" -I"$rc/include" \
        -I"$ps5/src" -I"$here" "$ps5/src/achievements.cpp" "$here/test.cpp" "$here/stubs.cpp" "${objects[@]}" \
        -o "$dir/test"
}

server=
stop_server() {
    if [[ -n $server ]]; then
        kill "$server" 2>/dev/null || true
        wait "$server" 2>/dev/null || true
        server=
    fi
}
cleanup() {
    stop_server
    [[ -n ${WORK_DIR:-}${KEEP:-} ]] || rm -rf -- "$work"
}
trap cleanup EXIT

run() {     # NAME: a server of its own, so each run starts from nothing earned
    local name=$1
    rm -rf -- "$work/cache" "$work/cache2" "$work/port"
    python3 "$here/mock_server.py" --port-file "$work/port" > "$work/server-$name.log" 2>&1 &
    server=$!
    for _ in $(seq 100); do
        [[ -f $work/port ]] && break
        sleep 0.05
    done
    [[ -f $work/port ]] || { echo "the mock server did not start" >&2; exit 1; }
    echo "$name:"
    SWANSTATION_RA_HOST="http://127.0.0.1:$(<"$work/port")" TEST_DISC_HASH=$disc_hash \
        "$work/build-${name%-slow}/test" "$work"
    stop_server
}

python3 "$ps5/tools/make-test-disc.py" "$work/disc.cue" --serial SLUS-99901 > /dev/null
python3 "$ps5/tools/make-test-disc.py" "$work/other.cue" --serial SLUS-99902 > /dev/null
python3 "$ps5/tools/make-test-disc.py" "$work/disc2.cue" --serial SLUS-99903 > /dev/null
disc_hash=$(python3 "$here/mock_server.py" --hash "$work/disc.cue")

modes=("$@")
(( ${#modes[@]} )) || modes=(plain asan tsan)
for mode in "${modes[@]}"; do
    case $mode in
    plain)
        build plain "" ""
        run plain
        ;;
    asan)
        build asan "-fsanitize=address -fno-omit-frame-pointer" \
            "-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 run asan
        ;;
    tsan)
        build tsan "-fsanitize=thread" "-fsanitize=thread"
        TSAN_OPTIONS=halt_on_error=1 run tsan
        ;;
    slow)
        build plain "" ""
        TEST_SLOW=1 run plain-slow
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
    esac
done
echo "achievements test: passed (${modes[*]})"
