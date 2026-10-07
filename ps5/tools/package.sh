#!/usr/bin/env bash
# PSSwanStation: the release ZIP of a built title folder.
#
#   ps5/tools/package.sh [OUT.zip] [TITLE_FOLDER]
#
# Archives build-ps5/dist/PPSA99248 (or TITLE_FOLDER) as OUT.zip (default:
# build-ps5/PSSwanStation-PS5-<title>-build<N>.zip) and writes OUT.zip.sha256
# beside it, which the title's updater checks a download against. Every entry
# is stored open to all users (0777): the console only starts an app whose
# files are, and some tools keep a ZIP's permissions when they unpack it (the
# same as ps5-native-app-boilerplate's release ZIPs).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "$ps5/.." && pwd)
title=$(sed -n 's/.*"titleId": "\(PPSA[0-9]\{5\}\)".*/\1/p' "$ps5/sce_sys/param.json")
build=$(sed -n 's/^const int BuildNumber = \([0-9]*\);.*/\1/p' "$ps5/src/main.cpp")
folder=${2:-$src/build-ps5/dist/$title}
out=${1:-$src/build-ps5/PSSwanStation-PS5-$title-build$build.zip}
[[ -f $folder/eboot.bin ]] || { echo "package: $folder holds no eboot.bin (run ps5/tools/build.sh first)" >&2; exit 2; }
python3 - "$folder" "$out" <<'PY'
import os, sys, zipfile
folder, out = os.path.abspath(sys.argv[1]), sys.argv[2]
top = os.path.basename(folder)
entries = []
for base, dirs, files in os.walk(folder):
    dirs.sort()
    relative = os.path.relpath(base, os.path.dirname(folder))
    entries.append((relative + "/", None))
    for name in sorted(files):
        entries.append((relative + "/" + name, os.path.join(base, name)))
with zipfile.ZipFile(out + ".tmp", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for name, path in entries:
        info = zipfile.ZipInfo(name) if path is None else zipfile.ZipInfo.from_file(path, name)
        info.create_system = 3  # Unix: the high half of external_attr is the mode
        if path is None:
            info.external_attr = (0o040777 << 16) | 0x10
            archive.writestr(info, b"")
        else:
            info.external_attr = 0o100777 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(path, "rb") as source:
                archive.writestr(info, source.read(), compresslevel=9)
os.replace(out + ".tmp", out)
with zipfile.ZipFile(out) as check:
    wrong = [i.filename for i in check.infolist() if (i.external_attr >> 16) & 0o777 != 0o777]
    if wrong or check.testzip() is not None:
        raise SystemExit("package: the ZIP did not pass its check: %s" % wrong[:3])
    print("%s: %d entries, all stored as 0777" % (out, len(check.infolist())))
PY
(cd -- "$(dirname -- "$out")" && sha256sum "$(basename -- "$out")" > "$(basename -- "$out").sha256")
echo "package: $out.sha256"
