#!/usr/bin/env python3
"""Writes a NAOMI cartridge for headless.sh: a flat image, as a dump of a
cartridge's ROM board is, with a program that tries the built-in BIOS's
system menu routine - what the cabinet's TEST switch leads to in a game.
naomi_prog.c is the program, and says what it looks at.

    naomi_cart.py out.bin      writes the cartridge
    naomi_cart.py --verdict    where in main memory the program's verdict is

The header is the part of a real one the BIOS reads (core/reios/reios.cpp,
reios_boot_naomi()): the board's name, the game's, two lists of what to
load - the game's at 0x360 and the test program's at 0x3c0, both this one
program - their entry points at 0x420 and 0x424, and the regions it runs
in. Everything else is 0xff, as in an unprogrammed ROM.

And romsets, for the loader's way with a merged one (core/hw/naomi/
naomi_cart.cpp, naomi_find_named()):

    naomi_cart.py --merged DIR   writes the romsets into DIR
    naomi_cart.py --serial       where in main memory a cartridge's serial is

A merged set is a game and its clones in one archive: the game's ROMs at
the top, each clone's own in a folder of the clone's name. DIR gets one
laid out as Dead or Alive 2's is - doa2m the game, doa2 and doa2a its
clones, which differ from it in their program ROM - with this cartridge
for each of the three program ROMs, told apart by its serial (SERIALS),
and a byte for each of the ROMs they share: the loader goes by names, and
takes a ROM shorter than its place. The BIOS keeps a copy of the header
in main memory, which is where headless.sh reads the serial of the one
that was started. The files:

    doa2m.zip             the merged set
    doa2a.zip             nothing: an empty file named for a clone, which
                          is how a clone is asked for from a merged set
    doa2.zip              the merged set again, under a clone's name
    Dead or Alive 2.zip   and under no set's name: the game's, not a clone's
    folder/doa2m.zip      the game's own ROMs inside a folder of the owner's
                          naming, their names in capitals"""
import os
import shutil
import struct
import sys
import zipfile

LOAD_AT = 0x0c020000          # where naomi_prog.ld links the program
OFFSET  = 0x1000              # where it is in the cartridge
VERDICT = 0x8c300010          # naomi_prog.c's VERDICT

# naomi_prog.c, compiled as it says.
PROGRAM = bytes.fromhex("""
01d02b40136409000c00028c862f962f224f436938d1126838d112600b400900
02e226381e8d036183600188290220311d8f018832d1238d192932d11039408b
31d112600188458f882824892fd112611821488f836002881d8b2dd12dd22221
feaf09002ad12cd22221feaf090008480848117829d11b2825d18221feaf0900
27d110391d8b20d112600288228b1fd112611821278b0200f0cb0e401bd108e2
22218362017214d122211ed112600b40084808481f7819d18b2115d21222feaf
090008480848127814d11b2810d18221feaf090008480848137810d11b280cd1
8221feaf09000848084814780bd18b2107d21222feaf09000000308c0880018c
ffffff1f20f8010c00ff018c30695fa01000308c577e0d601000d0ba0000d0ba
24f8010c4480018c
""")


# Dead or Alive 2's sets, as the core's table has them: the program ROM of
# each, and the ROMs all three share.
PROGRAM_ROMS = (('doa2m', 'doa2verm.ic22'), ('doa2', 'epr-22207.ic22'), ('doa2a', 'epr-22121a.ic22'))
SHARED_ROMS = (['mpr-221%02d.ic%d' % (n, n + 1) for n in range(0, 11)]
               + ['mpr-221%02d.ic%ds' % (n, n + 1) for n in range(11, 21)])
SERIALS = {'doa2m': b'BTS0', 'doa2': b'BTS1', 'doa2a': b'BTS2'}
SERIAL_AT = 0x8c01f400 + 0x134    # the BIOS's copy of the header


def cartridge(serial=b'BTST'):
    cart = bytearray(b'\xff' * 0x10000)
    cart[0x000:0x010] = b'NAOMI'.ljust(16)
    cart[0x010:0x030] = b'FLYCAST TEST'.ljust(32)
    for region in range(8):
        cart[0x030 + region * 32:0x050 + region * 32] = b'SYSTEM MENU TEST'.ljust(32)
    cart[0x130:0x134] = struct.pack('<HBB', 2026, 1, 1)
    cart[0x134:0x138] = serial
    # one piece to load in each list; the next entry, left 0xff, ends it
    piece = struct.pack('<III', OFFSET, LOAD_AT, len(PROGRAM))
    cart[0x360:0x36c] = piece
    cart[0x3c0:0x3cc] = piece
    cart[0x420:0x428] = struct.pack('<II', LOAD_AT, LOAD_AT)
    cart[0x428] = 0xff        # every region
    cart[OFFSET:OFFSET + len(PROGRAM)] = PROGRAM
    return bytes(cart)


def write_merged(out):
    os.makedirs(os.path.join(out, 'folder'), exist_ok=True)
    merged = os.path.join(out, 'doa2m.zip')
    with zipfile.ZipFile(merged, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in SHARED_ROMS:
            z.writestr(name, b'\0')
        for game, rom in PROGRAM_ROMS:
            z.writestr(rom if game == 'doa2m' else game + '/' + rom, cartridge(SERIALS[game]))
    shutil.copyfile(merged, os.path.join(out, 'doa2.zip'))
    shutil.copyfile(merged, os.path.join(out, 'Dead or Alive 2.zip'))
    open(os.path.join(out, 'doa2a.zip'), 'wb').close()
    with zipfile.ZipFile(os.path.join(out, 'folder', 'doa2m.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for name in SHARED_ROMS:
            z.writestr('My Dead or Alive 2/' + name.upper(), b'\0')
        z.writestr('My Dead or Alive 2/' + PROGRAM_ROMS[0][1].upper(), cartridge(SERIALS['doa2m']))


def main():
    if sys.argv[1:] == ['--verdict']:
        print('0x%08x' % VERDICT)
        return 0
    if sys.argv[1:] == ['--serial']:
        print('0x%08x' % SERIAL_AT)
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == '--merged':
        write_merged(sys.argv[2])
        return 0
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(sys.argv[1], 'wb') as f:
        f.write(cartridge())
    return 0


if __name__ == '__main__':
    sys.exit(main())
