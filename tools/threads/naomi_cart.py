#!/usr/bin/env python3
"""Writes a NAOMI cartridge for headless.sh: a flat image, as a dump of a
cartridge's ROM board is, with a program that tries the built-in BIOS -
how it starts a program, and its system routines, among them the system
menu's, which is what the cabinet's TEST switch leads to in a game.
naomi_prog.c is the program, and says what it looks at.

    naomi_cart.py out.bin      writes the cartridge
    naomi_cart.py --verdict    where in main memory the program's verdict is

The header is the part of a real one the BIOS reads (core/reios/reios.cpp,
reios_boot_naomi()): the board's name, the game's, how an M2 board's ROMs
lie (0x138), the settings the game wants in place of the BIOS's defaults
(0x1e0, the same for every region: SETTINGS), two lists of what to load -
the game's at 0x360 and the test program's at 0x3c0, both this one program
- their entry points at 0x420 and 0x424, and the regions it runs in.
Everything else is 0xff, as in an unprogrammed ROM.

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
02d0736593662b40136409006401028c224f00e05a400df004d002600b400900
1df05a00264f0b00090009000080018c862f224f829143d00b40183f42d11030
6d8f00e27b97fc37ffe311e12360084036071041fa8f01723cd112606f98fc38
00e700e600e50b408364826009885b8b81500188588b82500188558b84500b88
528b855002884f8b865001884c8b87500388498b8d500188468b8e500188438b
8f577827408b4b91fc31105002883b8f736300e640e23361084118e5fc355c31
62211042f78f017355e27b6118717360240f1041fb8f01773194fc3405e11114
1bd1126000e7f366f3650b401875f06118211e8ff3600c8408201a8ff3601070
00840c605588148ff360107004840c6055880e8b12977c3f264f0b00f6680dd1
0dd22221feaf09000ad10cd22221feaf090008d10ad22221feaf09005c011801
580109001000028c0000803f2c80018c2080018c1000308c1800d0ba1900d0ba
1a00d0ba862f962fa62fb62f224f4369536a636b50d1126850d1126000e700e6
00e50b4000e405e22638388d0361836001882902048829033b222031348f2822
47d13a8d192947d11039398b46d11261017220313d8f8828048da36043d11261
18213f8b0788468b7491103b4c8f8828538d8360058855890200f0cb0e403bd1
08e222218361017133d2122201e11638508f836003884a8935d14ca0090035d1
35d22221feaf090008480848117833d11b2830d18221feaf090031d1c5af1039
0848084812782dd11b282ad18221feaf090008480848137828d11b2825d18221
feaf090008480848147824d18b2121d21222feaf09000848084815781fd18b21
1cd21222feaf09000848084816781bd18b2118d21222feaf09001ad10b410900
aaaf090013d118d22221feaf090017d101a0090016d1126000e700e600e50b40
00e4084808481f780cd18b2109d21222feaf0900002009000000308c0880018c
ffffff1f24f8010c00ff018c30695fa04080018c1000308c1000d0ba0000d0ba
20f8010c3000028c577e0d604880018c4480018c
""")


# Dead or Alive 2's sets, as the core's table has them: the program ROM of
# each, and the ROMs all three share.
PROGRAM_ROMS = (('doa2m', 'doa2verm.ic22'), ('doa2', 'epr-22207.ic22'), ('doa2a', 'epr-22121a.ic22'))
SHARED_ROMS = (['mpr-221%02d.ic%d' % (n, n + 1) for n in range(0, 11)]
               + ['mpr-221%02d.ic%ds' % (n, n + 1) for n in range(11, 21)])
SERIALS = {'doa2m': b'BTS0', 'doa2': b'BTS1', 'doa2a': b'BTS2'}
SERIAL_AT = 0x8c01f400 + 0x134    # the BIOS's copy of the header

# The settings the cartridge asks for, which naomi_prog.c looks for in what
# the BIOS hands out: there are some; the monitor as it is and sound in the
# attract mode; a coin chute to each player; coin setting 12 (two coins to
# a credit); the four numbers of the "manual" coin setting, which this is
# not; and credits for each of eight things - two to start, three for the
# third.
SETTINGS = bytes([1, 0, 1, 12, 0, 0, 0, 0, 2, 1, 3, 1, 1, 1, 1, 1])


def cartridge(serial=b'BTST'):
    cart = bytearray(b'\xff' * 0x10000)
    cart[0x000:0x010] = b'NAOMI'.ljust(16)
    cart[0x010:0x030] = b'FLYCAST TEST'.ljust(32)
    for region in range(8):
        cart[0x030 + region * 32:0x050 + region * 32] = b'SYSTEM MENU TEST'.ljust(32)
    cart[0x130:0x134] = struct.pack('<HBB', 2026, 1, 1)
    cart[0x134:0x138] = serial
    cart[0x138:0x13c] = struct.pack('<HH', 1, 0)   # ROMs every 8 MB; the bus's timing as the BIOS has it
    for region in range(8):
        cart[0x1e0 + region * 16:0x1f0 + region * 16] = SETTINGS
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
