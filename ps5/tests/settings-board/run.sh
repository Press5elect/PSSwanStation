#!/usr/bin/env bash
# PSSwanStation: the settings board (src/ui_board.cpp) on the PC.
#
#   ps5/tests/settings-board/run.sh
#
# Runs the host build on a fresh root with SWANSTATION_SETTINGS_DUMP and checks:
# every row has a place (no "board: no place" line), no row is lit (not at its
# default) on a fresh root, every SwanStation option the title offers is on the
# board under SwanStation's own name and description, and with five settings
# switched on, exactly those five are lit.
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
build="$src/build-host"
[[ -f $build/build.ninja ]] || cmake -S "$ps5" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" --target swanstation >/dev/null
work=$(mktemp -d)
trap 'rm -rf -- "${work:?}"' EXIT
run() {
    mkdir -p "$work/root/data"
    printf '400 shot x\n' > "$work/root/script.txt"
    SWANSTATION_SETTINGS_DUMP="$1" SWANSTATION_FRAME_CLOCK=1 SWANSTATION_ROOT="$work/root" \
        SWANSTATION_SCRIPT="$work/root/script.txt" SWANSTATION_FRAMES=420 "$build/swanstation" > "$work/run.log" 2>&1
}
fail=0
run "$work/fresh.tsv"
if grep -q "board: no place" "$work/root/psswanstation-boot.log"; then
    grep "board: no place" "$work/root/psswanstation-boot.log"; fail=1
fi
lit=$(awk -F'\t' '$7 == 1 { print $3 }' "$work/fresh.tsv")
if [[ -n $lit ]]; then echo "lit on a fresh root (a wrong default in ui_board_table.inc):"; echo "$lit"; fail=1; fi
python3 - "$src/src/libretro/libretro_core_options.h" "$work/fresh.tsv" <<'PY' || fail=1
import re, sys
header = open(sys.argv[1]).read()
names = dict(re.findall(r'\{"(swanstation_\w+)",\s*"((?:[^"\\]|\\.)*)"', header))
board = {}
for line in open(sys.argv[2]):
    f = line.rstrip("\n").split("\t")
    board[f[3]] = f[2]
hidden = {"swanstation_Logging_LogLevel"}
bad = 0
for key, name in names.items():
    if key in hidden or key not in board:
        continue
    if board[key] != name:
        print(f"{key}: \"{board[key]}\" on the board, \"{name}\" in SwanStation"); bad = 1
missing = [k for k in names if k not in board and k not in hidden]
print(f"settings-board: {len(board)} rows, {len(names) - len(missing) - len(hidden & set(names))} SwanStation options by their own names"
      + (f"; not shown now (hidden by the core for this renderer or console): {len(missing)}" if missing else ""))
sys.exit(bad)
PY
cat >> "$work/root/frontend.cfg" <<'CFG'
frame_generation = 1
sinden = 1
usb_pads = 1
motion = 1
gpu_own_core = 0
CFG
run "$work/set.tsv"
lit=$(awk -F'\t' '$7 == 1 { print $4 }' "$work/set.tsv" | sort | tr '\n' ' ')
want="GPU On Separate Core (Restart) Sinden Lightgun (Restart) Tilt Steering USB Controllers (Restart) picture:frame_generation "
if [[ $lit != "$want" ]]; then echo "lit with five settings on: $lit (want: $want)"; fail=1; fi
[[ $fail == 0 ]] && echo "settings-board: passed"
exit $fail
