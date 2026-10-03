#!/usr/bin/env python3
"""Write a synthetic two-track CD image (CUE + BIN) for the CHD tests.

  Track 1  MODE1/2352  data sectors: sync, BCD MSF header, mode 1, then a
                       payload of slow-moving noise -- compressible enough
                       that every CD codec stores real compressed hunks
                       rather than falling back to uncompressed ones
  Track 2  AUDIO       16-bit stereo, a sweeping triangle with noise

--variant N rewrites a few scattered data sectors with different content.
A CHD built from the variant against the base image as its parent then
holds hunks of its own alongside hunks that reference the parent, which is
what the parent chain has to resolve.

--gdi writes a GD-ROM layout instead: a GDI sheet over three track files,
a data track and an audio track in the low-density area and a data track
at LBA 45000 in the high-density area, so chdman records GD-ROM track
metadata rather than CD track metadata.

Usage: make_fixture.py out_basename [--variant N | --gdi]
"""
import os
import struct
import sys

SECTOR      = 2352
DATA_FRAMES = 1500
AUDIO_FRAMES = 900
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def msf(lba):
    lba += 150
    return bytes((bcd(lba // 4500), bcd((lba // 75) % 60), bcd(lba % 75)))


class Lcg:
    def __init__(self, seed):
        self.s = seed & 0xffffffff

    def next(self):
        self.s = (self.s * 1103515245 + 12345) & 0xffffffff
        return self.s >> 16


def data_sector(lba, rng, salt):
    out = bytearray(SECTOR)
    out[0:12] = SYNC
    out[12:15] = msf(lba)
    out[15] = 1
    level = (lba * 7 + salt) & 0xff
    for i in range(16, SECTOR):
        if (i & 15) == 0:
            level = (level + (rng.next() & 3)) & 0xff
        out[i] = level ^ (rng.next() & 1)
    return bytes(out)


def audio_frames(rng):
    out = bytearray()
    phase = 0
    for _ in range(AUDIO_FRAMES):
        for _ in range(588):
            phase = (phase + 37) & 0xffff
            tri = phase if phase < 0x8000 else 0xffff - phase
            l = tri - 0x4000 + (rng.next() & 7)
            r = (0x4000 - tri) + (rng.next() & 7)
            out += struct.pack('<hh', l, r)
    return bytes(out)


def write_gdi(base):
    rng = Lcg(0x6d1)
    d = os.path.dirname(base) or '.'
    tracks = [
        # (number, lba, ctrl, file)
        (1, 0, 4, 'track01.bin'),
        (2, 450, 0, 'track02.raw'),
        (3, 45000, 4, 'track03.bin'),
    ]
    with open(os.path.join(d, 'track01.bin'), 'wb') as f:
        for lba in range(300):
            f.write(data_sector(lba, rng, 1))
    with open(os.path.join(d, 'track02.raw'), 'wb') as f:
        f.write(audio_frames(Lcg(0xa0d11)))
    with open(os.path.join(d, 'track03.bin'), 'wb') as f:
        for i in range(DATA_FRAMES):
            f.write(data_sector(45000 + i, rng, 3))
    with open(base + '.gdi', 'w') as f:
        f.write('%d\n' % len(tracks))
        for n, lba, ctrl, name in tracks:
            f.write('%d %d %d 2352 %s 0\n' % (n, lba, ctrl, name))
    return 0


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    base = sys.argv[1]
    variant = 0
    if len(sys.argv) >= 4 and sys.argv[2] == '--variant':
        variant = int(sys.argv[3])
    if len(sys.argv) >= 3 and sys.argv[2] == '--gdi':
        return write_gdi(base)

    rng = Lcg(0x5eed)
    data = bytearray()
    for lba in range(DATA_FRAMES):
        data += data_sector(lba, rng, 0)

    if variant:
        vr = Lcg(0x1000 + variant)
        for k in range(8):
            lba = (k * 181 + variant * 13) % DATA_FRAMES
            data[lba * SECTOR:(lba + 1) * SECTOR] = data_sector(lba, vr,
                                                                variant)

    pcm = audio_frames(Lcg(0xa0d10))

    name = os.path.basename(base)
    with open(base + '.bin', 'wb') as f:
        f.write(data)
        f.write(pcm)
    with open(base + '.cue', 'w') as f:
        f.write('FILE "%s.bin" BINARY\n' % name)
        f.write('  TRACK 01 MODE1/2352\n')
        f.write('    INDEX 01 00:00:00\n')
        f.write('  TRACK 02 AUDIO\n')
        f.write('    INDEX 01 %02d:%02d:%02d\n'
                % (DATA_FRAMES // 4500, (DATA_FRAMES // 75) % 60,
                   DATA_FRAMES % 75))
    return 0


if __name__ == '__main__':
    sys.exit(main())
