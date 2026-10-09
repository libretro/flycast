#!/usr/bin/env python3
"""Writes the program that rewrites its own code (smc_prog.c) as the ELF
file a Dreamcast program is built as, for headless.sh: a core runs it as
content, with the built-in BIOS.

    smc_elf.py out.elf      writes the program
    smc_elf.py --verdict    where in main memory its verdict is

smc_prog.c says what it does and what the verdict means."""
import struct
import sys

LOAD_AT = 0x8c010000          # where live_prog.ld links a program
VERDICT = 0x8c00f800          # smc_prog.c's VERDICT

# smc_prog.c, compiled as it says.
PROGRAM = bytes.fromhex("""
01df02d02b40090000f0008c7000018c224ffc7f422ff2600b400900047f264f
0b00090010d14c3105e22d415d6008344c640ed22b2441210c600f922b201181
0d9012810c9013810b9014810a9015810be0168109e017810b00090000700170
ff700270fe7009000800610400e00000862f962fa62fb62fc62fd62f224f47d1
01e2222146d183922121027109e221210be144d2112202727b93312242d27993
312202727793312240d2112240d17392212100e83fd900e50b49836401788360
4088f98f00e535d102e2222100e8629c39db35da39d98361cb21112b0b49a364
8030258b017883600a88f58f83612bd103e2222114e84c9c31db29da2fd98361
cb21112b0b49a3648030178b017883601e88f58f836122d428d00b4009001d88
138f04e21dd1222122dc24db1ca000e924d18b2119d21222feaf0900ec7822d1
1b2816d18221feaf090014d11fd22221feaf09001ed19b2110d21222feaf0900
93604088388d05e2936a05e11d4a19d00c3a00e883650b4c93640b4ba3640039
e88f017883600a88f58f8365e8af017906a000e07ea000e11360090000f8008c
0000208c1000208c000f208c0010208c0210208c2400018c020f208c1000018c
1200208c0002d0ba0003d0ba0a03d0ba0004d0ba0001208c48d1222100e8899d
47dc48db48da49d983607fc90361db21112c0b4ab364036183607fc90031448f
8c6101789038f08f83603cd106e222213fd43dd00b4009001d883c8f07e237d1
222100e838d9836405e11d4439d00b490c348030348b017883604088f38f08e2
2ed1222134d135d2222100e82ed9836405e11d442fd00b490c348030268b0178
83604088f48f836424d109e2222124d14192212123d424d00b40090055881b89
1ed127d22221feaf090026d22b211bd21222feaf090019d123d22221feaf0900
22d18b2115d21222feaf090020d18b2112d21222feaf090015d413d00b400900
1d8804890dd11bd22221feaf09001ad1129221210ed40cd00b4009003188048b
06d116d22221feaf090004d114d22221feaf090000e155e131e0090000f8008c
020f208c000f208c1000018c409c00000000208c0001208c0018208c78563412
0009d0ba0005d0ba0006d0ba0007d0ba0008d0ba0109d0ba1200208cc05a0d60
0209d0ba
""")


def main():
    if sys.argv[1:] == ['--verdict']:
        print('0x%08x' % VERDICT)
        return 0
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    # one loadable segment, where the program also starts, with a page of
    # zeroes asked for after it
    offset = 0x100
    header = struct.pack('<4s5B7xHHIIIIIHHHHHH',
                         b'\x7fELF', 1, 1, 1, 0, 0,   # 32-bit, little-endian
                         2, 42, 1,                    # executable, SuperH
                         LOAD_AT, 52, 0, 0,           # entry, program headers
                         52, 32, 1, 40, 0, 0)
    segment = struct.pack('<8I', 1, offset, LOAD_AT, LOAD_AT,
                          len(PROGRAM), len(PROGRAM) + 4096, 5, 4)
    with open(sys.argv[1], 'wb') as f:
        f.write((header + segment).ljust(offset, b'\0') + PROGRAM)
    return 0


if __name__ == '__main__':
    sys.exit(main())
