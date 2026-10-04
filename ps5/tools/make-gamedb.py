#!/usr/bin/env python3
"""SwanStation for PS5 - makes the title's game database.

  make-gamedb.py LIBRETRO_DATABASE_DIR OUT.zip

From a checkout of github.com/libretro/libretro-database (CC BY-SA 4.0), two
of its PlayStation lists:

  metadat/developer/Sony - PlayStation.dat   for each serial number: a
        description, the developer and publisher, the release date, the
        genre and the number of players
  metadat/redump/Sony - PlayStation.dat      the Redump names of the discs
        with their serial numbers

and writes them as two tab-separated files in one ZIP archive, which the
title reads at start (ps5/src/gamedb.cpp):

  games.tsv   serial, name, developer, publisher, year, month, players, genre, description
  names.tsv   a disc's name, reduced to its letters and digits, and its serial

The second is how a game in the library is known by its file name before it
has ever run: its details, its cheats and its own settings are all kept by
serial number.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import sys
import zipfile

FIELD = re.compile(r'^\t(\w+) "(.*)"\s*$')
ROM_SERIAL = re.compile(r'serial "([^"]+)"')


def games(path):
    """The dat's games, each a dict of its top-level fields plus 'serials'."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for block in text.split("\ngame (")[1:]:
        game = {"serials": []}
        for line in block.split("\n"):
            match = FIELD.match(line)
            if match:
                game.setdefault(match.group(1), match.group(2))
            elif line.lstrip().startswith("rom (") or line.lstrip().startswith("serial "):
                game["serials"] += ROM_SERIAL.findall(line)
        if "serial" in game:
            game["serials"].insert(0, game["serial"])
        yield game


def reduce(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def clean(value):
    return value.replace("\\\"", "\"").replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def serial_form(serial):
    """SLUS-00594, as the emulator writes a disc's ID."""
    serial = serial.strip().upper().replace("_", "-").replace(".", "")
    return serial if re.fullmatch(r"[A-Z]{4}-\d{5}", serial) else ""


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    database, out = sys.argv[1], sys.argv[2]
    details = os.path.join(database, "metadat", "developer", "Sony - PlayStation.dat")
    redump = os.path.join(database, "metadat", "redump", "Sony - PlayStation.dat")

    rows = {}
    names = {}
    # Redump's names are the ones disc images usually carry. A name it lists
    # under several serials (the disc's own, then variants of it) keeps the
    # first, which is the one on the disc.
    for game in games(redump):
        serial = serial_form(game.get("serial", ""))
        if serial and game.get("name"):
            names.setdefault(reduce(game["name"]), serial)
    for game in games(details):
        for serial in dict.fromkeys(serial_form(s) for s in game["serials"]):
            if not serial or serial in rows:
                continue
            rows[serial] = "\t".join([
                serial, clean(game.get("name", "")), clean(game.get("developer", "")),
                clean(game.get("publisher", "")), clean(game.get("releaseyear", "")),
                clean(game.get("releasemonth", "")), clean(game.get("users", "")),
                clean(game.get("genre", "")), clean(game.get("description", ""))])
        serial = serial_form(game.get("serial", ""))
        if serial and game.get("name"):
            names.setdefault(reduce(game["name"]), serial)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr("games.tsv", "\n".join(rows[s] for s in sorted(rows)) + "\n")
        archive.writestr("names.tsv", "\n".join("%s\t%s" % (n, names[n]) for n in sorted(names) if n) + "\n")
    print("%s: %d games, %d names, %d KiB" % (os.path.basename(out), len(rows), len(names),
                                             os.path.getsize(out) >> 10))
    return 0


if __name__ == "__main__":
    sys.exit(main())
