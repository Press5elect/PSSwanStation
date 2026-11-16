#!/usr/bin/env python3
# PSSwanStation: the manual's settings reference, from the settings board itself.
#
#   python3 ps5/tools/settings-reference.py DUMP.tsv [DUMP.tsv ...] > reference.md
#
# A DUMP is what SWANSTATION_SETTINGS_DUMP=<file> writes from the host build (one
# line a setting: area, section, name, id, ...). Several dumps are merged (one on
# a fresh root, one with the settings that show more rows switched on), in the
# board's order; a title row that ui_board_table.inc names but no dump showed (one
# only a console shows, as HDR's) goes after the row before it in the table.
# The About section's facts are left out. ps5/MANUAL.md's
# "Settings reference" is this output; regenerate it when the board changes
# (ps5/docs/dev/testing.md).
#
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import re
import sys
from collections import OrderedDict

table = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "ui_board_table.inc")
AREAS = ["Console", "Graphics", "Display", "Audio", "Controllers", "Library", "Saving", "Interface", "Network", "System"]
known = []  # (area, section, name) of the title's rows, in the table's order
for m in re.finditer(r'\{ "[^"]*", (\w+), "([^"]*)", "([^"]*)",', open(table, encoding="utf-8").read()):
    if m.group(1) in AREAS:
        known.append((m.group(1), m.group(2), m.group(3)))

areas = OrderedDict()
for path in sys.argv[1:]:
    order = []
    for line in open(path, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 9:
            continue
        area, section, name = f[0], f[1], f[2]
        if section in ("About", "Licences", "Other"):
            continue
        sections = areas.setdefault(area, OrderedDict())
        names = sections.setdefault(section, [])
        if name not in names:
            # A row only the second dump has goes after the row before it.
            before = order[-1] if order and order[-1][0] == (area, section) else None
            if before is not None and before[1] in names:
                names.insert(names.index(before[1]) + 1, name)
            else:
                names.append(name)
        order.append(((area, section), name))

for i, (area, section, name) in enumerate(known):
    names = areas.get(area, {}).get(section)
    if names is None or name in names:
        continue
    before = [n for (a, s, n) in known[:i] if a == area and s == section and n in names]
    names.insert(names.index(before[-1]) + 1 if before else len(names), name)

print("| Area | Section | Settings |")
print("| --- | --- | --- |")
for area, sections in areas.items():
    first = True
    for section, names in sections.items():
        shown = ", ".join(n.replace("|", "\\|") for n in names)
        print(f"| {area if first else ''} | {section} | {shown} |")
        first = False
