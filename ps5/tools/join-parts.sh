#!/usr/bin/env bash
# join-parts.sh - joins the source archives attached to a PSSwanStation
# release in parts (NAME.tar.gz.part1of4 ... part4of4 -> NAME.tar.gz), checks
# each joined archive against the SHA-256 in SOURCES.txt, and deletes the
# parts once the joined archive is right. A joined archive whose SHA-256 does
# not match is deleted instead, and its parts are kept.
#
#   bash join-parts.sh [FOLDER]      (default: the current folder)
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "${1:-.}"
shopt -s nullglob
failed=0
joined=0
for first in *.part1of*; do
    whole=${first%.part1of*}
    count=${first##*.part1of}
    parts=()
    for ((i = 1; i <= count; i++)); do
        part="$whole.part${i}of$count"
        [[ -f $part ]] || { echo "missing: $part (parts of $whole kept)" >&2; failed=1; continue 2; }
        parts+=("$part")
    done
    cat -- "${parts[@]}" > "$whole.joining"
    want=""
    [[ -f SOURCES.txt ]] && want=$(awk -v n="$whole" '$2 == n && length($1) == 64 { print $1; exit }' SOURCES.txt)
    if [[ -n $want ]]; then
        got=$(sha256sum -- "$whole.joining" | cut -d' ' -f1)
        if [[ $got != "$want" ]]; then
            rm -f -- "$whole.joining"
            echo "BAD:     $whole: SHA-256 $got, SOURCES.txt says $want (parts kept)" >&2
            failed=1
            continue
        fi
        note="SHA-256 matches"
    else
        note="no SHA-256 in SOURCES.txt to check it against"
    fi
    mv -- "$whole.joining" "$whole"
    rm -f -- "${parts[@]}"
    joined=$((joined + 1))
    echo "ok:      $whole ($count parts joined and deleted; $note)"
done
(( joined > 0 || failed > 0 )) || { echo "no *.part1of* files here" >&2; exit 1; }
exit $failed
