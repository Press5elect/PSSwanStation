#!/usr/bin/env bash
# PSSwanStation - the USB disc drive on a PC: the three ways to play a disc.
#
#   ps5/tests/discdrive/run.sh [WORK_DIR]
#
# A disc image stands in for the drive (SWANSTATION_FAKE_DRIVE; see
# discdrive.cpp), made by make-fake-disc.py: a data track that boots, Form 2
# sectors, a CD audio track with a pregap, four LibCrypt sectors. The PC build
# (build-host, made by tools/host-test.sh) is run with a script that picks the
# disc in the library and each way in turn:
#
#   copy    to the games folder: the .bin must be the disc's, byte for byte;
#           the .cue its tracks with INDEX 00; the .sbi its four sectors; the
#           copy then starts
#   ram     into memory, then started from there
#   stream  started from the drive
#
# and three times more with a weaker drive: "raw" (whole sectors, no sub-channel:
# the copy is the same, no .sbi, no INDEX 00) and "data" (2048 bytes a sector:
# Form 2 sectors and the audio are lost, the game still starts), and one that
# refuses Form 2 sectors to such reads altogether ("data-strict": the copy is
# still made).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$(cd -- "$here/../../.." && pwd)
build="$src/build-host"
work=${1:-$build/discdrive-test}
[[ -x $build/swanstation ]] || { echo "build the PC version first (ps5/tools/host-test.sh)" >&2; exit 2; }
rm -rf "$work"
mkdir -p "$work"
python3 "$here/make-fake-disc.py" "$work/disc/Fake Disc.cue" --serial SLUS-99903
disc="$work/disc/Fake Disc.cue"

failed=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAILED: $1"; failed=$((failed + 1)); fi; }

# One run: a fresh root, the disc in the "drive", the script's presses.
run() {
    local name=$1 mode=$2 presses=$3
    local root="$work/$name"
    mkdir -p "$root/games" "$root/assets" "$root/data"
    printf 'disc_drive = 1\n' > "$root/frontend.cfg"
    printf 'swanstation_GPU_ResolutionScale = "1"\n' > "$root/data/options.cfg"
    {
        echo "455 cross"		# the "What's new" of a fresh folder
        echo "462 shot 01-library"
        echo "470 cross"
        echo "490 shot 02-choice"
        echo "$presses"
        echo "1100 shot 04-game"
    } > "$root/script.txt"
    SWANSTATION_FAKE_DRIVE="$disc" SWANSTATION_FAKE_DRIVE_MODE="$mode" SWANSTATION_FRAME_CLOCK=1 \
        SWANSTATION_ROOT="$root" SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=1120 \
        SWANSTATION_SIZE=1280x720 "$build/swanstation" > "$root/run.log" 2>&1 || true
    grep -E "disc drive:|library: Disc|game: (starting|running|the emulator)|CRASH" "$root/psswanstation-boot.log" | sed 's/^/    /'
}

copy_presses=$'495 cross\n505 shot 03-places\n510 cross'
ram_presses=$'495 down\n503 cross'
stream_presses=$'495 down\n503 down\n511 cross'

echo "== copy, whole sectors with sub-channel Q"
run copy "" "$copy_presses"
r="$work/copy/games"
check "the boot log names the read mode" "grep -q 'read as whole sectors with their sub-channel Q' '$work/copy/psswanstation-boot.log'"
check "the .bin is the disc's, byte for byte" "cmp -s '$work/disc/Fake Disc.bin' '$r/PlayStation disc SLUS-99903.bin'"
check "the .cue has INDEX 00 for track 2" "grep -q 'INDEX 00 00:02:57' '$r/PlayStation disc SLUS-99903.cue' && grep -q 'INDEX 01 00:04:57' '$r/PlayStation disc SLUS-99903.cue'"
check "the .sbi is the disc's" "cmp -s '$work/disc/Fake Disc.sbi' '$r/PlayStation disc SLUS-99903.sbi'"
check "the copy started" "grep -q 'game: running, serial SLUS-99903' '$work/copy/psswanstation-boot.log' && grep -q 'game: starting .*games/PlayStation disc SLUS-99903.cue' '$work/copy/psswanstation-boot.log'"

echo "== into memory"
run ram "" "$ram_presses"
check "read into memory" "grep -q 'read into memory' '$work/ram/psswanstation-boot.log'"
check "started from memory" "grep -q 'game: starting driveram://cd1/' '$work/ram/psswanstation-boot.log' && grep -q 'game: running, serial SLUS-99903' '$work/ram/psswanstation-boot.log'"

echo "== from the drive"
run stream "" "$stream_presses"
check "started from the drive" "grep -q 'game: starting drive://cd1/' '$work/stream/psswanstation-boot.log' && grep -q 'game: running, serial SLUS-99903' '$work/stream/psswanstation-boot.log'"

echo "== copy, a drive with no sub-channel"
run copy-raw raw "$copy_presses"
r="$work/copy-raw/games"
check "the read mode says LibCrypt is lost" "grep -q 'no sub-channel (LibCrypt' '$work/copy-raw/psswanstation-boot.log'"
check "the .bin is the disc's, byte for byte" "cmp -s '$work/disc/Fake Disc.bin' '$r/PlayStation disc SLUS-99903.bin'"
check "no .sbi" "[[ ! -e '$r/PlayStation disc SLUS-99903.sbi' ]]"
check "the .cue adds no pregap of its own" "grep -q 'PREGAP 00:00:00' '$r/PlayStation disc SLUS-99903.cue' && ! grep -q 'INDEX 00' '$r/PlayStation disc SLUS-99903.cue'"
check "the copy started" "grep -q 'game: running, serial SLUS-99903' '$work/copy-raw/psswanstation-boot.log'"

echo "== copy, a drive that gives 2048 bytes a sector"
run copy-data data "$copy_presses"
r="$work/copy-data/games"
check "the read mode says what is lost" "grep -q '2048 bytes a sector' '$work/copy-data/psswanstation-boot.log'"
check "the data track's 2048 bytes a sector are the disc's" "python3 -c '
import sys
a = open(sys.argv[1], \"rb\").read(); b = open(sys.argv[2], \"rb\").read()
sys.exit(any(a[i * 2352 + 24:i * 2352 + 2072] != b[i * 2352 + 24:i * 2352 + 2072] for i in range(177)))
' '$work/disc/Fake Disc.bin' '$r/PlayStation disc SLUS-99903.bin'"
check "the audio track is silence" "python3 -c '
import sys
b = open(sys.argv[1], \"rb\").read()
sys.exit(any(b[357 * 2352:]))
' '$r/PlayStation disc SLUS-99903.bin'"
check "the Form 2 sectors are not" "! cmp -s -i $((177 * 2352)) -n $((30 * 2352)) '$work/disc/Fake Disc.bin' '$r/PlayStation disc SLUS-99903.bin'"
check "the copy started" "grep -q 'game: running, serial SLUS-99903' '$work/copy-data/psswanstation-boot.log'"

echo "== copy, a drive that refuses Form 2 sectors to 2048-byte reads"
run copy-strict data-strict "$copy_presses"
r="$work/copy-strict/games"
check "the copy was made" "[[ -s '$r/PlayStation disc SLUS-99903.cue' ]]"
check "the data track's Form 1 sectors are the disc's" "python3 -c '
import sys
a = open(sys.argv[1], \"rb\").read(); b = open(sys.argv[2], \"rb\").read()
sys.exit(any(a[i * 2352 + 24:i * 2352 + 2072] != b[i * 2352 + 24:i * 2352 + 2072] for i in range(177)))
' '$work/disc/Fake Disc.bin' '$r/PlayStation disc SLUS-99903.bin'"
check "the copy started" "grep -q 'game: running, serial SLUS-99903' '$work/copy-strict/psswanstation-boot.log'"

echo "== an empty drive, seen with USB drives on and the disc drive setting off"
root="$work/empty"
mkdir -p "$root/games" "$root/assets" "$root/data"
printf 'usb = 1\n' > "$root/frontend.cfg"
{ echo "455 cross"; echo "470 cross"; echo "490 shot 01-empty"; } > "$root/script.txt"
SWANSTATION_FAKE_DRIVE="$disc" SWANSTATION_FAKE_DRIVE_MODE=empty SWANSTATION_USB="$root/usb" SWANSTATION_FRAME_CLOCK=1 \
    SWANSTATION_ROOT="$root" SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=520 SWANSTATION_SIZE=1280x720 \
    "$build/swanstation" > "$root/run.log" 2>&1 || true
grep -E "disc drive:|library: Disc" "$root/psswanstation-boot.log" | sed 's/^/    /'
check "the empty drive is listed" "grep -q 'no disc in it' '$root/psswanstation-boot.log' && grep -q 'library: Disc: 1 disc' '$root/psswanstation-boot.log'"

echo "== the drive unplugged"
root="$work/unplug"
mkdir -p "$root/games" "$root/assets" "$root/data" "$work/unplug-disc"
cp "$work/disc/"* "$work/unplug-disc/"
printf 'disc_drive = 1\n' > "$root/frontend.cfg"
echo "455 cross" > "$root/script.txt"
SWANSTATION_FAKE_DRIVE="$work/unplug-disc/Fake Disc.cue" SWANSTATION_FRAME_CLOCK=1 SWANSTATION_ROOT="$root" \
    SWANSTATION_SCRIPT="$root/script.txt" SWANSTATION_FRAMES=900 SWANSTATION_SIZE=1280x720 \
    "$build/swanstation" > "$root/run.log" 2>&1 &
pid=$!
for _ in $(seq 60); do grep -q "went in" "$root/psswanstation-boot.log" 2>/dev/null && break; sleep 0.5; done
rm -f "$work/unplug-disc/Fake Disc.cue"
wait $pid || true
grep -E "disc drive:|library: Disc" "$root/psswanstation-boot.log" | sed 's/^/    /'
check "the disc left with its drive" "grep -q 'was unplugged' '$root/psswanstation-boot.log' && grep -q 'library: Disc: 0 disc' '$root/psswanstation-boot.log'"

if (( failed )); then echo "discdrive: $failed check(s) failed"; exit 1; fi
echo "discdrive: all passed (screenshots in $work)"
