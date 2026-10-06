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
"""
import os
import shutil
import subprocess
import sys
import zipfile

import py7zr

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


if __name__ == '__main__':
    main()
