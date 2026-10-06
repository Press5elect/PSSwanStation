#!/bin/bash
# PSSwanStation - netplay's tests: builds test.cpp (one side of a session with
# a made-up emulator, see its top) and runs sessions between two of it on
# 127.0.0.1, comparing what the two sides print.
#
#   run.sh            everything (about two minutes)
#   run.sh quick      without the scenarios that wait for a time limit
#
# Environment:
#   NETPLAY_TEST_PORT   the first of the ports used (default 21500; about 45
#                       above it are used). Below the ports the system gives
#                       to outgoing connections, which a host cannot open
#                       while one of those has it.
#   NETPLAY_TEST_OUT    where the programs and what they printed are kept
#                       (default: a new directory under $TMPDIR, removed when
#                       everything passed)
#   CXX                 the compiler (default clang++)
#
# What is covered:
#    1  a 3000-frame session: equal checksums, both sides ran the same inputs,
#       no resync; a third player who knocks meanwhile is refused
#    2  another game, another BIOS: both sides Failed, each saying what differs
#    3  one side's memory changed in the middle: a resync on both, then equal
#    4  one side killed: the other Ended at once; one side frozen: the other
#       shows the silence and is Ended after 20 seconds
#    5  a menu open on one side for 2 seconds: the other waits, then both go on
#    6  joining where nobody hosts: Failed at once; where nothing answers:
#       Failed after the 10 seconds a connect is given
#    7  host, stop, host again, in one process: two sessions, with a host that
#       was stopped while it waited before each
#    8  one side's refreshes up to 30 ms apart at random, delay 1, 2 and 5
#    9  strangers on the port: text, another version, a cut-off message, noise
#   10  sessions under the address and undefined-behaviour sanitizers
#   11  sessions under the thread sanitizer
#   12  sessions built as for a system whose sockets cannot be told not to wait
#   13  the packing of the state (codec.cpp)
#   14  what a step() call costs while playing, at a steady refresh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/../../src
mode=${1:-all}
cxx=${CXX:-clang++}
port=${NETPLAY_TEST_PORT:-21500}
if [ -n "${NETPLAY_TEST_OUT:-}" ]; then
	out=$NETPLAY_TEST_OUT
	mkdir -p "$out"
	keep=1
else
	out=$(mktemp -d "${TMPDIR:-/tmp}/netplay-test.XXXXXX")
	keep=0
fi
export NETPLAY_TEST_LOG=1
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=67
export UBSAN_OPTIONS=print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1:exitcode=66:second_deadlock_stack=1

passed=0
failed=0
results=$out/results.txt
: >"$results"

pass() { echo "ok    $1" | tee -a "$results"; }
fail() { echo "FAIL  $1" | tee -a "$results"; }

# verdict "what it is": passes when the command before it succeeded.
verdict()
{
	if [ $? = 0 ]; then pass "$1"; else fail "$1"; fi
}

# value FILE LINE KEY: the value of KEY= on the last line of FILE that starts
# with LINE.
value()
{
	grep "^$2 " "$1" 2>/dev/null | tail -1 | tr ' ' '\n' | grep "^$3=" | head -1 | cut -d= -f2-
}

# has FILE TEXT: FILE has a line with TEXT in it.
has()
{
	grep -qF -- "$2" "$1" 2>/dev/null
}

# wait_for FILE TEXT: until FILE has TEXT in it, for 10 seconds at most.
wait_for()
{
	local tries=0
	until has "$1" "$2" || [ $tries -ge 200 ]; do
		sleep 0.05
		tries=$((tries + 1))
	done
}

less_than() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 < b + 0) }'; }
at_least() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 >= b + 0) }'; }

build()
{
	local name=$1
	shift
	echo "building $name"
	if ! "$cxx" -std=c++20 -Wall -Wextra -Werror -g -pthread -fno-exceptions "$@" \
			"$here/test.cpp" "$here/stubs.cpp" "$src/netplay.cpp" -o "$out/$name" 2>"$out/$name.build.log"; then
		cat "$out/$name.build.log"
		echo "FAIL  building $name" | tee -a "$results"
		return 1
	fi
}

# pair NAME PROGRAM "HOST OPTIONS" "JOIN OPTIONS": a session between two. What
# they print is in NAME.host.txt and NAME.join.txt, their exit codes in
# hostCode and joinCode. The join side tries for 5 seconds until the host is
# there.
pair()
{
	local name=$1 program=$2
	# shellcheck disable=SC2086
	NETPLAY_TEST_NAME=host "$out/$program" host $3 >"$out/$name.host.txt" 2>"$out/$name.host.log" &
	local hostPid=$!
	# shellcheck disable=SC2086
	NETPLAY_TEST_NAME=join "$out/$program" join --retry 5 $4 >"$out/$name.join.txt" 2>"$out/$name.join.log" &
	local joinPid=$!
	wait $hostPid
	hostCode=$?
	wait $joinPid
	joinCode=$?
}

# in_step NAME [SESSION]: both sides ended well and printed the same checksum
# and the same sums of both players' inputs.
in_step()
{
	local name=$1
	local a b key
	[ "$hostCode" = 0 ] && [ "$joinCode" = 0 ] || { echo "      exit codes: host $hostCode, join $joinCode"; return 1; }
	for key in frames checksum in0 in1; do
		a=$(value "$out/$name.host.txt" RESULT $key)
		b=$(value "$out/$name.join.txt" RESULT $key)
		[ -n "$a" ] && [ "$a" = "$b" ] || { echo "      $key: host '$a', join '$b'"; return 1; }
	done
	! has "$out/$name.host.txt" VIOLATION && ! has "$out/$name.join.txt" VIOLATION
}

show()
{
	local name=$1 side
	for side in host join; do
		[ -f "$out/$name.$side.txt" ] && sed "s/^/      $side: /" "$out/$name.$side.txt"
		[ -f "$out/$name.$side.log" ] && tail -n 25 "$out/$name.$side.log" | sed "s/^/      $side! /"
	done
}

# session "what it is" NAME PROGRAM "HOST OPTIONS" "JOIN OPTIONS": a session
# that must end with the two in step.
session()
{
	local what=$1 name=$2
	pair "$name" "$3" "$4" "$5"
	if in_step "$name"; then pass "$what"; else fail "$what"; show "$name"; fi
}

# ------------------------------------------------------------------ building
build test -O2 || exit 1
build test-asan -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer || exit 1
build test-tsan -O1 -fsanitize=thread || exit 1
build test-plain -O2 -DNETPLAY_PLAIN_SOCKETS || exit 1
echo "building codec"
"$cxx" -std=c++20 -Wall -Wextra -Werror -g -O1 -pthread -fno-exceptions -fsanitize=address,undefined \
	-fno-sanitize-recover=undefined "$here/codec.cpp" "$here/stubs.cpp" -o "$out/codec" || exit 1

# ------------------------------------------ the scenarios that wait for a limit
# These mostly sleep, so they run beside the others; their results are read at
# the end.
slow_frozen()
{
	# 4b: the join side is frozen (not killed: its sockets stay open and say
	# nothing). The host shows the silence, then ends after 20 seconds.
	local p=$((port + 40))
	NETPLAY_TEST_NAME=host "$out/test" host --port $p --frames 100000000 --timeout 40 >"$out/frozen.host.txt" 2>"$out/frozen.host.log" &
	local hostPid=$!
	NETPLAY_TEST_NAME=join "$out/test" join --retry 5 --port $p --frames 100000000 --timeout 40 >"$out/frozen.join.txt" 2>"$out/frozen.join.log" &
	local joinPid=$!
	sleep 2
	kill -STOP $joinPid
	wait $hostPid
	echo "code=$?" >"$out/frozen.code"
	kill -KILL $joinPid 2>/dev/null
	wait $joinPid 2>/dev/null
}

slow_nobody()
{
	# 6b: an address nothing answers from. Whether the network says so at once
	# or never, the join side must be Failed within the connect's 10 seconds.
	local program=$1 name=$2
	NETPLAY_TEST_NAME=join "$out/$program" join --addr 10.255.255.1 --port $((port + 41)) --timeout 30 >"$out/$name.join.txt" 2>"$out/$name.join.log"
	echo "code=$?" >"$out/$name.code"
}

if [ "$mode" != quick ]; then
	slow_frozen &
	frozenJob=$!
	slow_nobody test nobody &
	nobodyJob=$!
	slow_nobody test-plain nobody-plain &
	nobodyPlainJob=$!
fi

# -------------------------------------------------------------------- 1 normal
p=$((port + 1))
NETPLAY_TEST_NAME=host "$out/test" host --port $p --frames 3000 --extra 30000 >"$out/normal.host.txt" 2>"$out/normal.host.log" &
hostPid=$!
NETPLAY_TEST_NAME=join "$out/test" join --retry 5 --port $p --frames 3000 >"$out/normal.join.txt" 2>"$out/normal.join.log" &
joinPid=$!
# A third player, while the two play: they reach frame 3000 and go on for
# ten times as long.
wait_for "$out/normal.host.txt" "RESULT role=host"
NETPLAY_TEST_NAME=third "$out/test" join --port $p --frames 3000 --timeout 15 >"$out/normal.third.txt" 2>"$out/normal.third.log"
thirdCode=$?
kill -0 $hostPid 2>/dev/null
stillPlaying=$?
wait $hostPid; hostCode=$?
wait $joinPid; joinCode=$?
in_step normal
verdict "1  a 3000-frame session ends with the two in step"
[ "$(value "$out/normal.host.txt" RESULT resyncs)$(value "$out/normal.join.txt" RESULT resyncs)" = 00 ]
verdict "1  ... with no resync"
[ "$(value "$out/normal.host.txt" FINAL saves)$(value "$out/normal.join.txt" FINAL loads)" = 11 ]
verdict "1  ... the state travelled once (one save, one load)"
has "$out/normal.join.txt" 'state=Ended error="The other player left."'
verdict "1  ... the join side is told the host left"
[ "$thirdCode" = 2 ] && has "$out/normal.third.txt" 'state=Failed error="Nobody is hosting at'
verdict "1  a third player is refused while the two play"
[ $stillPlaying = 0 ] && at_least "$(value "$out/normal.host.txt" FINAL runs)" 33000
verdict "1  ... and the two were playing then, and went on"
grep -h "^STEP" "$out/normal.host.txt" "$out/normal.join.txt" | sed 's/^/      /'

# ------------------------------------------------------- 2 not the same game
pair game test "--port $((port + 2)) --game SLUS-00594" "--port $((port + 2)) --game SCES-01237"
has "$out/game.host.txt" 'state=Failed error="The other player has a different game: SLUS-00594 here, SCES-01237 there."'
verdict "2  another game: the host is Failed and says which"
has "$out/game.join.txt" 'state=Failed error="The other player has a different game: SCES-01237 here, SLUS-00594 there."'
verdict "2  ... and the join side, in its own words"
[ "$hostCode$joinCode$(value "$out/game.host.txt" FINAL runs)$(value "$out/game.join.txt" FINAL runs)" = 2200 ]
verdict "2  ... neither ran a frame"
pair bios test "--port $((port + 3)) --bios scph5501" "--port $((port + 3)) --bios scph7502"
has "$out/bios.host.txt" 'state=Failed error="The other player has a different BIOS: scph5501 here, scph7502 there."' && has "$out/bios.join.txt" 'state=Failed error="The other player has a different BIOS: scph7502 here, scph5501 there."'
verdict "2  another BIOS: both Failed, each saying which"

# ------------------------------------------------------------------ 3 resync
session "3  the host's memory changed at frame 500: back in step" corrupt-host test \
	"--port $((port + 4)) --frames 2000 --corrupt 500" "--port $((port + 4)) --frames 2000"
at_least "$(value "$out/corrupt-host.host.txt" FINAL resyncs)" 1 && at_least "$(value "$out/corrupt-host.join.txt" FINAL resyncs)" 1
verdict "3  ... after a resync on both sides"
[ "$(value "$out/corrupt-host.host.txt" FINAL saves)$(value "$out/corrupt-host.join.txt" FINAL loads)" = 22 ]
verdict "3  ... the state travelled twice"
session "3  the join side's memory changed at frame 700: back in step" corrupt-join test \
	"--port $((port + 5)) --frames 2000" "--port $((port + 5)) --frames 2000 --corrupt 700"
at_least "$(value "$out/corrupt-join.host.txt" FINAL resyncs)" 1 && at_least "$(value "$out/corrupt-join.join.txt" FINAL resyncs)" 1
verdict "3  ... after a resync on both sides"
session "3  both changed, with a delay of 6 and uneven refreshes" corrupt-both test \
	"--port $((port + 6)) --frames 900 --delay 6 --corrupt 130 --jitter 3" "--port $((port + 6)) --frames 900 --delay 6 --corrupt 400 --jitter 5"
at_least "$(value "$out/corrupt-both.host.txt" FINAL resyncs)" 2 && at_least "$(value "$out/corrupt-both.join.txt" FINAL resyncs)" 2
verdict "3  ... after two resyncs or more on both sides"

# ------------------------------------------------------------------ 4 killed
killed()
{
	# killed NAME PORT VICTIM [PROGRAM]: a session one side of which is killed
	# after a second and a half.
	local name=$1 p=$2 program=${4:-test}
	NETPLAY_TEST_NAME=host "$out/$program" host --port $p --frames 100000000 --timeout 20 >"$out/$name.host.txt" 2>"$out/$name.host.log" &
	local hostPid=$!
	NETPLAY_TEST_NAME=join "$out/$program" join --retry 5 --port $p --frames 100000000 --timeout 20 >"$out/$name.join.txt" 2>"$out/$name.join.log" &
	local joinPid=$!
	sleep 1.5
	if [ "$3" = join ]; then kill -KILL $joinPid; else kill -KILL $hostPid; fi
	wait $hostPid 2>/dev/null
	wait $joinPid 2>/dev/null
}
killed kill-join $((port + 7)) join
has "$out/kill-join.host.txt" 'state=Ended error="The other player left."'
verdict "4  the join side killed: the host is Ended, told the other left"
less_than "$(value "$out/kill-join.host.txt" FINAL elapsed)" 2.5
verdict "4  ... within a second"
at_least "$(value "$out/kill-join.host.txt" FINAL runs)" 1000
verdict "4  ... having played until then"
killed kill-host $((port + 8)) host
has "$out/kill-host.join.txt" 'state=Ended error="The other player left."' && less_than "$(value "$out/kill-host.join.txt" FINAL elapsed)" 2.5
verdict "4  the host killed: the join side is Ended, told the other left"

# ------------------------------------------------------------------- 5 pause
session "5  a menu on the host for 2 seconds: in step afterwards" pause-host test \
	"--port $((port + 9)) --frames 3000 --pause-at 1000" "--port $((port + 9)) --frames 3000"
[ "$(value "$out/pause-host.join.txt" FINAL remotepause)" = 1 ] && at_least "$(value "$out/pause-host.join.txt" FINAL gapms)" 1900
verdict "5  ... the join side knew and ran nothing for 2 seconds"
at_least "$(value "$out/pause-host.host.txt" FINAL gapms)" 1900
verdict "5  ... nor did the host"
session "5  a menu on the join side for 2 seconds: in step afterwards" pause-join test \
	"--port $((port + 10)) --frames 3000 --delay 4" "--port $((port + 10)) --frames 3000 --delay 4 --pause-at 1500"
[ "$(value "$out/pause-join.host.txt" FINAL remotepause)" = 1 ] && at_least "$(value "$out/pause-join.host.txt" FINAL gapms)" 1900
verdict "5  ... the host knew and ran nothing for 2 seconds"

# ------------------------------------------------------- 6 nobody is hosting
NETPLAY_TEST_NAME=join "$out/test" join --port $((port + 11)) >"$out/early.join.txt" 2>"$out/early.join.log"
joinCode=$?
[ $joinCode = 2 ] && has "$out/early.join.txt" 'state=Failed error="Nobody is hosting at 127.0.0.1:'
verdict "6  joining before anyone hosts: Failed, saying nobody is hosting"
less_than "$(value "$out/early.join.txt" FINAL elapsed)" 1
verdict "6  ... at once"
NETPLAY_TEST_NAME=join "$out/test" join --addr not.an.address --port $((port + 11)) >"$out/name.join.txt" 2>"$out/name.join.log"
has "$out/name.join.txt" 'is not an address'
verdict "6  joining a name: Failed, asking for an address"

# -------------------------------------------------- 7 host, stop, host again
p=$((port + 12))
NETPLAY_TEST_NAME=host "$out/test" host --port $p --frames 1500 --sessions 2 >"$out/again.host.txt" 2>"$out/again.host.log" &
hostPid=$!
# Each join side comes when the host it is for is there: the one before it
# was stopped while it waited.
wait_for "$out/again.host.txt" "LISTENING session=1"
NETPLAY_TEST_NAME=join "$out/test" join --retry 5 --port $p --frames 1500 >"$out/again.join1.txt" 2>"$out/again.join1.log"
join1Code=$?
wait_for "$out/again.host.txt" "LISTENING session=2"
NETPLAY_TEST_NAME=join "$out/test" join --retry 5 --port $p --frames 1500 --seed 5 >"$out/again.join2.txt" 2>"$out/again.join2.log"
join2Code=$?
wait $hostPid; hostCode=$?
again()
{
	local n key a b
	[ "$hostCode$join1Code$join2Code" = 000 ] || { echo "      exit codes: host $hostCode, join $join1Code and $join2Code"; return 1; }
	for n in 1 2; do
		for key in checksum in0 in1; do
			a=$(grep "^RESULT role=host session=$n " "$out/again.host.txt" | tr ' ' '\n' | grep "^$key=")
			b=$(grep "^RESULT " "$out/again.join$n.txt" | tr ' ' '\n' | grep "^$key=")
			[ -n "$a" ] && [ "$a" = "$b" ] || { echo "      session $n $key: host '$a', join '$b'"; return 1; }
		done
	done
	! has "$out/again.host.txt" VIOLATION
}
again
verdict "7  host, stop, host, a session, stop, host, stop, host, a session: both in step"
[ "$(grep -c '^AFTER .*threads=1 (before 1) files=5 (before 3)' "$out/again.host.txt")" = 2 ]
verdict "7  ... with the threads and the files of before after each stop()"
has "$out/results.txt" "FAIL  7" && sed 's/^/      host: /' "$out/again.host.txt"

# ------------------------------------------------------- 8 uneven refreshes
for delay in 1 2 5; do
	session "8  the join side's refreshes up to 30 ms apart, delay $delay: in step" "jitter$delay" test \
		"--port $((port + 13 + delay)) --frames 500 --delay $delay" "--port $((port + 13 + delay)) --frames 500 --delay $delay --jitter 30"
	[ "$(value "$out/jitter$delay.host.txt" FINAL resyncs)" = 0 ]
	verdict "8  ... with no resync"
done
session "8  the host's refreshes up to 30 ms apart, the join side's up to 10, delay 3: in step" jitter-both test \
	"--port $((port + 19)) --frames 400 --delay 3 --jitter 30" "--port $((port + 19)) --frames 400 --delay 3 --jitter 10"

# ------------------------------------------------------------- 9 strangers
stranger()
{
	# stranger NAME PROGRAM PORT SEND [SEED]: a host, and somebody on its port
	# who is not a player.
	local name=$1 program=$2 p=$3
	NETPLAY_TEST_NAME=host "$out/$program" host --port $p --timeout 15 >"$out/$name.host.txt" 2>"$out/$name.host.log" &
	local hostPid=$!
	wait_for "$out/$name.host.txt" LISTENING
	"$out/test" raw --port $p --send "$4" --seed "${5:-1}" >"$out/$name.raw.txt" 2>&1
	wait $hostPid
	hostCode=$?
}
stranger text test $((port + 20)) garbage
[ "$hostCode" = 2 ] && has "$out/text.host.txt" 'state=Failed error="The other side sent something that is not PSSwanStation netplay."'
verdict "9  text on the port: Failed, as not netplay"
stranger version test $((port + 21)) version
has "$out/version.host.txt" 'state=Failed error="The other player has a different version of PSSwanStation: build 9 here, build 42 there."'
verdict "9  another version: Failed, naming both builds"
stranger short test $((port + 22)) short
has "$out/short.host.txt" 'state=Ended error="The other player left."'
verdict "9  a message cut off: Ended, the other left"
noise=0
for seed in 1 2 3 4 5 6 7 8; do
	stranger "noise$seed" test-asan $((port + 22 + seed)) noise $seed
	[ "$hostCode" = 2 ] && has "$out/noise$seed.host.txt" "FINAL role=host" && noise=$((noise + 1))
done
[ $noise = 8 ]
verdict "9  noise after a proper greeting, eight kinds: the session ends, nothing breaks ($noise of 8)"

# ------------------------------------------------------------ 10 sanitizers
session "10 address and undefined-behaviour sanitizers: a session" asan-normal test-asan \
	"--port $((port + 31)) --frames 2000" "--port $((port + 31)) --frames 2000"
session "10 ... a resync and a menu" asan-resync test-asan \
	"--port $((port + 32)) --frames 1500 --corrupt 300 --pause-at 900 --pause-ms 500" "--port $((port + 32)) --frames 1500 --jitter 2"
at_least "$(value "$out/asan-resync.host.txt" FINAL resyncs)" 1 && at_least "$(value "$out/asan-resync.join.txt" FINAL resyncs)" 1
verdict "10 ... with a resync on both sides"
! grep -qE "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error" "$out"/asan-*.log "$out"/noise*.log
verdict "10 ... and nothing reported"

# ----------------------------------------------------------- 11 thread sanitizer
session "11 thread sanitizer: a session" tsan-normal test-tsan \
	"--port $((port + 33)) --frames 2000" "--port $((port + 33)) --frames 2000"
session "11 ... a resync and a menu" tsan-resync test-tsan \
	"--port $((port + 34)) --frames 1500 --pause-at 900 --pause-ms 500" "--port $((port + 34)) --frames 1500 --corrupt 300 --jitter 2"
at_least "$(value "$out/tsan-resync.host.txt" FINAL resyncs)" 1 && at_least "$(value "$out/tsan-resync.join.txt" FINAL resyncs)" 1
verdict "11 ... with a resync on both sides"
p=$((port + 35))
NETPLAY_TEST_NAME=host "$out/test-tsan" host --port $p --frames 800 --sessions 2 >"$out/tsan-again.host.txt" 2>"$out/tsan-again.host.log" &
hostPid=$!
wait_for "$out/tsan-again.host.txt" "LISTENING session=1"
NETPLAY_TEST_NAME=join "$out/test-tsan" join --retry 5 --port $p --frames 800 >"$out/tsan-again.join1.txt" 2>"$out/tsan-again.join1.log"
wait_for "$out/tsan-again.host.txt" "LISTENING session=2"
NETPLAY_TEST_NAME=join "$out/test-tsan" join --retry 5 --port $p --frames 800 >"$out/tsan-again.join2.txt" 2>"$out/tsan-again.join2.log"
wait $hostPid
hostCode=$?
[ $hostCode = 0 ] && [ "$(grep -c '^RESULT' "$out/tsan-again.host.txt")" = 2 ]
verdict "11 ... host, stop, host again, twice"
! grep -q "WARNING: ThreadSanitizer" "$out"/tsan-*.log
verdict "11 ... and nothing reported"

# ---------------------------------------------- 12 sockets that always wait
session "12 sockets that cannot be told not to wait: a session" plain-normal test-plain \
	"--port $((port + 36)) --frames 3000" "--port $((port + 36)) --frames 3000"
session "12 ... a resync" plain-resync test-plain \
	"--port $((port + 37)) --frames 1500 --corrupt 300" "--port $((port + 37)) --frames 1500"
killed plain-kill $((port + 38)) join test-plain
has "$out/plain-kill.host.txt" 'state=Ended error="The other player left."'
verdict "12 ... the join side killed: the host is Ended"

# ----------------------------------------------------- 14 what step() costs
# Both sides call step() every 2 ms, as a display would every 16.7, so that
# the threads are not in each other's way; nothing else runs meanwhile.
if [ "$mode" != quick ]; then
	wait $frozenJob $nobodyJob $nobodyPlainJob
fi
session "14 a session at a refresh of 2 ms: in step" cost test \
	"--port $((port + 39)) --frames 2500 --refresh-us 2000" "--port $((port + 39)) --frames 2500 --refresh-us 2000"
for side in host join; do
	less_than "$(value "$out/cost.$side.txt" STEP p50us)" 100 && less_than "$(value "$out/cost.$side.txt" STEP p99us)" 1000
	verdict "14 ... step() on the $side side: half the calls under 0.1 ms, 99 in 100 under 1 ms"
done
grep -h "^STEP" "$out/cost.host.txt" "$out/cost.join.txt" | sed 's/^/      /'

# ------------------------------------------------------------------ 13 codec
"$out/codec"
verdict "13 the packing of the state"

# ------------------------------------------------- the ones that took their time
if [ "$mode" != quick ]; then
	has "$out/frozen.host.txt" 'state=Ended error="The other player stopped answering."'
	verdict "4  the join side frozen: the host is Ended, told the other stopped answering"
	[ "$(value "$out/frozen.host.txt" FINAL silence)" = 1 ]
	verdict "4  ... having shown the silence first"
	elapsed=$(value "$out/frozen.host.txt" FINAL elapsed)
	at_least "$elapsed" 21 && less_than "$elapsed" 25
	verdict "4  ... 20 seconds after the last word (${elapsed} s from the start, 2 of them playing)"
	for name in nobody nobody-plain; do
		elapsed=$(value "$out/$name.join.txt" FINAL elapsed)
		has "$out/$name.join.txt" "state=Failed" && less_than "$elapsed" 11.5
		verdict "6  joining an address nothing answers from ($name): Failed within the connect's limit (${elapsed} s)"
		has "$out/$name.join.txt" "threads=1 (before 1)"
		verdict "6  ... with the threads of before"
		sed 's/^/      /' "$out/$name.join.txt" | grep FINAL
	done
fi

passed=$(grep -c "^ok" "$results")
failed=$(grep -c "^FAIL" "$results")
echo
echo "$passed passed, $failed failed"
if [ "$failed" = 0 ] && [ "$keep" = 0 ]; then
	rm -rf "$out"
else
	echo "what the programs printed is in $out"
fi
[ "$failed" = 0 ]
