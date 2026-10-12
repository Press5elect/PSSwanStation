#!/usr/bin/env python3
"""PSSwanStation - a disc image for testing the USB disc drive on a PC.

Stands in for a PlayStation disc in a drive (SWANSTATION_FAKE_DRIVE): a data
track from make-test-disc.py (a program that boots), 30 Mode 2 Form 2 sectors
after it (what a game's videos and XA sound are), a CD audio track with a
two-second pregap (INDEX 00), and an .sbi with four sectors whose sub-channel
Q is altered as LibCrypt's are.

  make-fake-disc.py OUT.cue --serial SLUS-99903

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import random
import struct
import subprocess
import sys

FORM2 = 30
AUDIO_PREGAP = 150
AUDIO = 300
SBI_LBAS = (100, 101, 102, 103)


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def msf(lba):
    return lba // 4500, (lba // 75) % 60, lba % 75


def form2_sector(lba, rng):
    s = bytearray(2352)
    s[0] = 0
    s[1:11] = b"\xff" * 10
    s[11] = 0
    m, sec, f = msf(lba + 150)
    s[12:16] = bytes((bcd(m), bcd(sec), bcd(f), 2))
    sub = bytes((1, 1, 0x64, 0))			# file 1, channel 1, form 2 + real-time + audio
    s[16:20] = sub
    s[20:24] = sub
    s[24:24 + 2324] = bytes(rng.getrandbits(8) for _ in range(2324))
    return bytes(s)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("cue")
    p.add_argument("--serial", default="SLUS-99903")
    a = p.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.cue)), exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    maker = os.path.join(here, "..", "..", "tools", "make-test-disc.py")
    subprocess.check_call([sys.executable, maker, a.cue, "--serial", a.serial, "--colour", "306020"],
                          stdout=subprocess.DEVNULL)
    base = os.path.splitext(a.cue)[0]
    binpath = base + ".bin"
    rng = random.Random(1999)
    with open(binpath, "rb") as f:
        data = f.read()
    data_sectors = len(data) // 2352
    out = bytearray(data)
    for i in range(FORM2):
        out += form2_sector(data_sectors + i, rng)
    track2_pregap = data_sectors + FORM2
    for _ in range(AUDIO_PREGAP + AUDIO):
        out += bytes(rng.getrandbits(8) for _ in range(2352))
    with open(binpath, "wb") as f:
        f.write(out)
    m0 = msf(track2_pregap)
    m1 = msf(track2_pregap + AUDIO_PREGAP)
    with open(a.cue, "w") as f:
        f.write('FILE "%s" BINARY\n' % os.path.basename(binpath))
        f.write("  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n")
        f.write("  TRACK 02 AUDIO\n    INDEX 00 %02d:%02d:%02d\n    INDEX 01 %02d:%02d:%02d\n" % (m0 + m1))
    # LibCrypt-like Q: what the disc would say, the relative frame changed.
    with open(base + ".sbi", "wb") as f:
        f.write(b"SBI\0")
        for lba in SBI_LBAS:
            am, as_, af = msf(lba + 150)
            rm, rs, rf = msf(lba)
            q = bytes((0x41, 0x01, 0x01, bcd(rm), bcd(rs), bcd(rf) ^ 0x01, 0, bcd(am), bcd(as_), bcd(af)))
            f.write(bytes((bcd(am), bcd(as_), bcd(af), 1)) + q)
    print("%s: %d data sectors, %d Form 2, audio track at %d (pregap from %d), %d LibCrypt sectors"
          % (a.cue, data_sectors, FORM2, track2_pregap + AUDIO_PREGAP, track2_pregap, len(SBI_LBAS)))


main()
