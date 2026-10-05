#!/usr/bin/env python3
"""PSSwanStation - makes a disc image for the host test runs.

No game is needed to test the title on a PC: this writes a PlayStation disc
image whose program is thirty instructions of its own (it sets a video mode
and draws two rectangles), under whatever serial number the test asks for, so
the library, the serial detection, the cheat database and the save states
all have something to work on.

  make-test-disc.py OUT.cue [--serial SLUS-00594] [--discs N]

Needs pycdlib.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import io
import os
import struct
import sys

import pycdlib


def program(colour):
    code = []

    def li(reg, value):
        code.append(0x3C000000 | reg << 16 | (value >> 16) & 0xFFFF)		# lui
        code.append(0x34000000 | reg << 21 | reg << 16 | value & 0xFFFF)	# ori

    def sw(reg, base, offset):
        code.append(0xAC000000 | base << 21 | reg << 16 | offset)

    T0, T1 = 8, 9
    code.append(0x3C000000 | T0 << 16 | 0x1F80)		# lui t0, 0x1f80: the I/O ports
    for command in (0x00000000,		# reset the GPU
                    0x08000001,		# 320 x 240, NTSC
                    0x06C60260,		# horizontal display range
                    0x07040010,		# vertical display range
                    0x05000000,		# display area starts at 0, 0
                    0x03000000):	# display on
        li(T1, command)
        sw(T1, T0, 0x1814)
    for word in (0xE1000400,		# drawing to the display area is allowed
                 0xE3000000,		# drawing area: from 0, 0
                 0xE403BD3F,		# to 319, 239
                 0xE5000000,		# no drawing offset
                 0x02000000 | colour, 0x00000000, 0x00F00140,	# fill the screen
                 0x6000A5FF, 0x00500064, 0x00500078):			# an orange rectangle
        li(T1, word)
        sw(T1, T0, 0x1810)
    loop = 0x80010000 + len(code) * 4
    code.append(0x08000000 | (loop >> 2) & 0x03FFFFFF)	# j loop
    code.append(0)										# nop
    text = b"".join(struct.pack("<I", word) for word in code)
    text += b"\0" * (-len(text) % 0x800)
    header = bytearray(0x800)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, 0x80010000)	# pc
    struct.pack_into("<I", header, 0x18, 0x80010000)	# load address
    struct.pack_into("<I", header, 0x1C, len(text))
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)	# stack
    marker = b"Sony Computer Entertainment Inc. for North America area"
    header[0x4C:0x4C + len(marker)] = marker
    return bytes(header) + text


def iso(serial, colour):
    name = serial.replace("-", "_")
    name = name[:8] + "." + name[8:]					# SLUS_005.94
    system = ("BOOT = cdrom:\\%s;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n" % name).encode()
    exe = program(colour)
    disc = pycdlib.PyCdlib()
    disc.new(interchange_level=1, sys_ident="PLAYSTATION", vol_ident="SWANTEST")
    disc.add_fp(io.BytesIO(system), len(system), "/SYSTEM.CNF;1")
    disc.add_fp(io.BytesIO(exe), len(exe), "/%s;1" % name)
    out = io.BytesIO()
    disc.write_fp(out)
    disc.close()
    return out.getvalue()


def raw(image):
    """2048-byte sectors to a raw MODE2/2352 track (form 1; the error
    correction bytes are left zero, emulators do not check them)."""
    out = bytearray()
    for index in range(len(image) // 2048):
        lba = index + 150
        minute, second, frame = lba // 4500, lba // 75 % 60, lba % 75
        bcd = lambda v: (v // 10) << 4 | v % 10
        out += b"\x00" + b"\xff" * 10 + b"\x00"
        out += bytes((bcd(minute), bcd(second), bcd(frame), 2))
        out += bytes((0, 0, 8, 0)) * 2
        out += image[index * 2048:(index + 1) * 2048]
        out += b"\0" * 280
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cue")
    parser.add_argument("--serial", default="SLUS-99901")
    parser.add_argument("--colour", default="804020", help="the screen's colour, as BBGGRR")
    args = parser.parse_args()
    data = raw(iso(args.serial, int(args.colour, 16)))
    # A second of silence after the data, as a pressed disc has.
    data += b"\0" * (2352 * 150)
    base = os.path.splitext(args.cue)[0]
    with open(base + ".bin", "wb") as f:
        f.write(data)
    with open(args.cue, "w") as f:
        f.write('FILE "%s" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % os.path.basename(base + ".bin"))
    print("%s: %s, %d sectors" % (args.cue, args.serial, len(data) // 2352))
    return 0


if __name__ == "__main__":
    sys.exit(main())
