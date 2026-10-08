#!/usr/bin/env python3
"""PSSwanStation - the libretro database's PlayStation cheats, by serial number.

  make-libretro-cheats.py LIBRETRO_DATABASE_DIR OUT.zip

From a checkout of github.com/libretro/libretro-database (CC BY-SA 4.0): its
cht/Sony - PlayStation folder, RetroArch's cheat files, one per game and code
device, named by the game's title and regions ("<title> (USA) (GameShark).cht").
The title knows a game by its disc's serial number, so each file is matched to
serials through the database's own Redump list (metadat/redump), by its whole
name where that is a disc's name, else by its title within the regions the file names. Each serial gets
the codes, in the cheat format the title reads (ps5/src/cheats.cpp), as
<serial>.cht in OUT.zip; the title uses them only for a game the chtdb
collection has no codes for.

Left out: Xploder files (another code format), and codes with a value to be
filled in ("????"), whose choices a RetroArch file does not name.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import sys
import zipfile

DEVICES = re.compile(r"\s*\((GameShark|Game Buster|Action Replay|Pro Action Replay|Xploder|Code Breaker|CodeBreaker)[^)]*\)\s*$",
                     re.IGNORECASE)
PAIR = re.compile(r"^[0-9A-Fa-f]{8}[ +:]?[0-9A-Fa-f]{4}$")


def reduce(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def serial_form(serial):
    serial = serial.strip().upper().replace("_", "-").replace(".", "")
    return serial if re.fullmatch(r"[A-Z]{4}-\d{5}", serial) else ""


def redump_serials(path):
    """By whole name and by title alone: the serials of the discs so named."""
    by_name, by_title = {}, {}
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for block in text.split("\ngame (")[1:]:
        name = re.search(r'\bname "([^"]+)"', block)
        serials = {s for s in (serial_form(x) for x in re.findall(r'serial "([^"]+)"', block)) if s}
        if not name or not serials:
            continue
        by_name.setdefault(reduce(name.group(1)), set()).update(serials)
        by_title.setdefault(reduce(re.sub(r"\s*\(.*$", "", name.group(1))), set()).update(serials)
    return by_name, by_title


REGION_PREFIXES = {
    "usa": ("SLUS", "SCUS", "PAPX", "LSP"), "canada": ("SLUS", "SCUS"),
    "europe": ("SLES", "SCES", "SCED", "SLED"), "uk": ("SLES", "SCES"), "germany": ("SLES", "SCES"),
    "france": ("SLES", "SCES"), "italy": ("SLES", "SCES"), "spain": ("SLES", "SCES"), "australia": ("SLES", "SCES"),
    "japan": ("SLPS", "SCPS", "SLPM", "SCPM", "SIPS", "PCPX"), "asia": ("SLPS", "SCPS", "SLPM", "SCPM"),
    "korea": ("SLKA", "SCKA"),
}


def same_regions(serials, title_regions):
    """A title-only match keeps the discs of the regions the cheat file names."""
    m = re.search(r"\(([^)]*)\)", title_regions)
    if not m:
        return serials
    prefixes = set()
    for region in m.group(1).split(","):
        prefixes.update(REGION_PREFIXES.get(region.strip().lower(), ()))
    if not prefixes:
        return serials
    return {s for s in serials if s[:4] in prefixes or s[:3] in prefixes}


def parse_cht(text):
    """RetroArch's cheatN_desc / cheatN_code pairs, in order."""
    values = {}
    for line in text.splitlines():
        m = re.match(r'\s*(cheat\d+_(?:desc|code))\s*=\s*"(.*)"\s*$', line)
        if m:
            values[m.group(1)] = m.group(2)
    entries = []
    i = 0
    while f"cheat{i}_code" in values or f"cheat{i}_desc" in values:
        entries.append((values.get(f"cheat{i}_desc", "").strip(), values.get(f"cheat{i}_code", "").strip()))
        i += 1
    return entries


def convert(desc, code):
    """The code as the title's lines (address and value, one pair a line), or None."""
    if "?" in code or not code:
        return None
    # RetroArch writes "ADDRESS+VALUE+ADDRESS+VALUE..." (sometimes "ADDRESS VALUE" or
    # "ADDRESSVALUE" as one part); encrypted codes ("$...") are another format.
    tokens = [t.strip() for t in re.split(r"[+\n]", code) if t.strip()]
    lines = []
    i = 0
    while i < len(tokens):
        part = tokens[i]
        if PAIR.match(part):
            digits = re.sub(r"[ +:]", "", part)
            i += 1
        elif (re.fullmatch(r"[0-9A-Fa-f]{8}", part) and i + 1 < len(tokens)
              and re.fullmatch(r"[0-9A-Fa-f]{4}", tokens[i + 1])):
            digits = part + tokens[i + 1]
            i += 2
        else:
            return None
        lines.append(digits[:8].upper() + " " + digits[8:].upper())
    return lines or None


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    database, out = sys.argv[1], sys.argv[2]
    by_name, by_title = redump_serials(os.path.join(database, "metadat", "redump", "Sony - PlayStation.dat"))
    folder = os.path.join(database, "cht", "Sony - PlayStation")
    per_serial = {}
    files = matched = 0
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith(".cht"):
            continue
        files += 1
        stem = name[:-4]
        device = DEVICES.search(stem)
        if device and device.group(1).lower() == "xploder":
            continue
        title_regions = DEVICES.sub("", stem)
        serials = by_name.get(reduce(title_regions))
        how = "by its name"
        if not serials:
            serials = same_regions(by_title.get(reduce(re.sub(r"\s*\(.*$", "", title_regions)), set()), title_regions)
            how = "by its title and region"
        if not serials:
            continue
        with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as f:
            entries = parse_cht(f.read())
        sections = []
        for desc, code in entries:
            lines = convert(desc, code)
            if lines is None:
                continue
            title = re.sub(r"[\[\]\\\r\n]", " ", desc).strip() or "Code"
            sections.append(f"[{title}]\nType = Gameshark\nActivation = EndFrame\n"
                            f"Description = From the libretro database ({stem}), matched to this disc {how}: it may not "
                            f"fit this version of the game.\n" + "\n".join(lines) + "\n")
        if not sections:
            continue
        matched += 1
        for serial in sorted(serials):
            per_serial.setdefault(serial, []).extend(sections)
    with zipfile.ZipFile(out + ".tmp", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for serial in sorted(per_serial):
            z.writestr(serial + ".cht", "\n".join(per_serial[serial]))
    os.replace(out + ".tmp", out)
    print(f"libretro-cheats.zip: {matched} of {files} cheat files used, {len(per_serial)} serials, "
          f"{os.path.getsize(out) // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
