#!/usr/bin/env python3
"""Package the GD-ROM fixture of tools/chd/make_fixture.py as archives.

  make_fixture.py <workdir>

Writes <workdir>/src/{disc.gdi,track01.bin,track02.raw,track03.bin} and:

  stored.zip     every member stored (method 0): a mapped archive hands
                 these out in place, nothing is copied or decoded
  deflate.zip    every member deflated
  subdir.zip     deflated, members under GAME/, plus a decoy .cue so the
                 disc pick has to prefer the .gdi
  solid.7z       one LZMA2 folder holding all members
  single.7z      track03.bin alone, so the member is its folder's whole
                 output and is handed over without a copy
  suffix         stored.zip again under a name with no extension, to be
                 opened as "suffix" + ".zip"
  stored.rar     every member stored, in the RAR 2.9 format, written out
                 by hand here (no free tool packs one; packed ones, made
                 by RAR itself, are in tools/archive/rar)
"""
import os
import shutil
import subprocess
import sys
import zipfile

import py7zr
import struct
import zlib

MEMBERS = ['disc.gdi', 'track01.bin', 'track02.raw', 'track03.bin']


def main():
    work = sys.argv[1]
    src = os.path.join(work, 'src')
    os.makedirs(src, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    subprocess.check_call([sys.executable,
                           os.path.join(here, '..', 'chd', 'make_fixture.py'),
                           os.path.join(src, 'disc'), '--gdi'])

    def zip_of(name, method, prefix='', decoy=False):
        with zipfile.ZipFile(os.path.join(work, name), 'w', method) as z:
            if decoy:
                z.writestr(prefix + 'other.cue', 'FILE "x.bin" BINARY\n')
            for i, m in enumerate(MEMBERS):
                # Extra fields of different lengths on every member, as
                # Info-ZIP and 7-Zip write them (timestamps, unix
                # attributes): the directory walk has to step over them.
                info = zipfile.ZipInfo.from_file(os.path.join(src, m),
                                                 prefix + m)
                info.compress_type = method
                ut = b'\x55\x54\x05\x00\x03' + bytes(4)
                ux = b'\x75\x78\x0b\x00\x01' + bytes(10)
                info.extra = (ut, ut + ux, ux, b'')[i % 4]
                with open(os.path.join(src, m), 'rb') as f:
                    z.writestr(info, f.read())

    zip_of('stored.zip', zipfile.ZIP_STORED)
    zip_of('deflate.zip', zipfile.ZIP_DEFLATED)
    zip_of('subdir.zip', zipfile.ZIP_DEFLATED, 'GAME/', True)
    shutil.copyfile(os.path.join(work, 'stored.zip'),
                    os.path.join(work, 'suffix.zip'))

    with py7zr.SevenZipFile(os.path.join(work, 'solid.7z'), 'w') as z:
        for m in MEMBERS:
            z.write(os.path.join(src, m), m)
    with py7zr.SevenZipFile(os.path.join(work, 'single.7z'), 'w') as z:
        z.write(os.path.join(src, 'track03.bin'), 'track03.bin')

    write_stored_rar(os.path.join(work, 'stored.rar'), src, MEMBERS)


def rar_block(kind, flags, body, data_size=None):
    """A RAR 2.9 block: checksum, kind, flags, size, then the body; the
    checksum is the low half of the CRC-32 of everything after it."""
    size = 7 + len(body)
    head = struct.pack('<BHH', kind, flags, size) + body
    return struct.pack('<H', zlib.crc32(head) & 0xffff) + head


def write_stored_rar(path, src, members):
    with open(path, 'wb') as out:
        out.write(b'Rar!\x1a\x07\x00')
        out.write(rar_block(0x73, 0, bytes(6)))               # archive header
        for m in members:
            with open(os.path.join(src, m), 'rb') as f:
                data = f.read()
            name = m.encode()
            # packed size, size, host (Unix), CRC-32, time, version needed
            # (2.9), method (0x30: stored), name length, attributes
            body = struct.pack('<IIBIIBBHI', len(data), len(data), 3,
                               zlib.crc32(data), 0x3d2a8000, 29, 0x30,
                               len(name), 0o100644) + name
            out.write(rar_block(0x74, 0x8000, body))          # file header
            out.write(data)
        out.write(rar_block(0x7b, 0x4000, b''))               # end of archive


if __name__ == '__main__':
    main()
