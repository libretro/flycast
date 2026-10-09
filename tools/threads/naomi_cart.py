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
in. Everything else is 0xff, as in an unprogrammed ROM."""
import struct
import sys

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


def main():
    if sys.argv[1:] == ['--verdict']:
        print('0x%08x' % VERDICT)
        return 0
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    cart = bytearray(b'\xff' * 0x10000)
    cart[0x000:0x010] = b'NAOMI'.ljust(16)
    cart[0x010:0x030] = b'FLYCAST TEST'.ljust(32)
    for region in range(8):
        cart[0x030 + region * 32:0x050 + region * 32] = b'SYSTEM MENU TEST'.ljust(32)
    cart[0x130:0x134] = struct.pack('<HBB', 2026, 1, 1)
    cart[0x134:0x138] = b'BTST'
    # one piece to load in each list; the next entry, left 0xff, ends it
    piece = struct.pack('<III', OFFSET, LOAD_AT, len(PROGRAM))
    cart[0x360:0x36c] = piece
    cart[0x3c0:0x3cc] = piece
    cart[0x420:0x428] = struct.pack('<II', LOAD_AT, LOAD_AT)
    cart[0x428] = 0xff        # every region
    cart[OFFSET:OFFSET + len(PROGRAM)] = PROGRAM
    with open(sys.argv[1], 'wb') as f:
        f.write(cart)
    return 0


if __name__ == '__main__':
    sys.exit(main())
