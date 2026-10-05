#!/usr/bin/env python3
"""Write a bootable GD-ROM image (GDI + three track files) around the
bare-metal test program in live_prog.c, for tools/threads/live.sh.

Usage: live_disc.py out.gdi [--no-region-array]

The program is carried here already compiled, so the test needs no SH-4
compiler; live_prog.c says how PROGRAM was produced. --no-region-array
takes the build of it that renders without ever writing a region array.
The image is the least the core's HLE BIOS needs to boot it: an IP.BIN
whose bootstrap jumps to the boot file, and an ISO 9660 volume holding
1ST_READ.BIN.
"""
import os
import struct
import sys

PROGRAM = bytes.fromhex("""
01df02d02b40090000f0008c2c00018c436128411c3404d220e142221041fc8f
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224ff07f95d1
126195d226652666226794d2266322625b216b217b213b212b21131f90d11060
fec90021ffe28fd322230473222300e204732123106001cb00218bd112608bd3
392005cb0221187189d33221f4712221107188d332216c7187d332219c7187d3
3221047186d33221107186d3322186d186d3122386d3122386d12221fc7186d2
222186d186d2222186d287d30ce132221041fc8f047200e182d2122204721222
82d183d2222183d183d2222100e0021fffe1111f00e2222f80de7bdd80dc81d9
be98a6a000ea288f03c9ba947ed10b4109005ca00900b5947bd10b410900a363
08433c330843ac3318430943ac33aa911923a3603fc9036108410c31184173d2
2c3173d231211042fc8f027142a009006dd10b411fe43aa0090003882e8f3fe1
163a2f8da360dbafa36352d22da009008a91103a188ff252f151183285911632
0989f3511821038980945fd10b41090048d15aa0090075945bd10b410900f7af
090044d151a009003fe1163a508f017aff7a47a0a3600388b28da3633fe1163a
058fa36001c8d08952d253d12221d22effe1122c51d152d22221047151d22221
047151d22221047150d2222101e150d2122250d200e3322204721222f0724ed3
322214721222926189214cd3336292661367636189217231028b10431042f68b
47d1126ba3629c7231911632a08df261b831f1521632008b111ff25012300089
121fa36001c8ac8932d13ed21222017ab22fa3602d88028f2de179af0900163a
018b50af3c880f88028f1e8853af0900a18f03c90c9424d10b410900a7af0900
ff03e07f007cff7ffa00c8001f7c9500e0030900846c5fa0f46c5fa0c0785fa0
0400d8ff0800d8ff44805fa0000080003f7d1700000020000401150000007f02
0000df0177df27000000100020805fa028815fa08c805fa017b7d138000010a5
00000002000018a500000080180018a54c0e00802c805fa00000180044815fa0
14805fa00c815fa01000018c000000a500a000000000400060805fa000e000ac
0100008000e1000c0920000100000001e86c5fa0106c5fa000e0000c80841e00
0c00d8ff50805fa0
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c2c00018c436128411c3404d220e142221041fc8f
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224ff07f8dd1
12618dd22665266622678cd2266322625b216b217b213b212b21131f88d11060
fec90021ffe287d322230473222300e204732123106001cb002183d1126083d3
392005cb0221187181d33221f4712221107180d332216c717fd332219c717fd3
322104717ed3322110717ed332217ed17ed312237ed312237ed12221fc717ed2
22217ed17ed222217ed17fd2222100e0021fffe1111f00e2222f7cde7cdd7ddc
7dd9be98a6a000ea288f03c9ba947bd10b4109005ca00900b59478d10b410900
a36308433c330843ac3318430943ac33aa911923a3603fc9036108410c311841
6fd22c316fd231211042fc8f027142a009006ad10b411fe43aa0090003882e8f
3fe1163a2f8da360dbafa36351d22da009008a91103a188ff252f15118328591
16320989f3511821038980945bd10b41090048d15aa00900759458d10b410900
f7af090043d151a009003fe1163a508f017aff7a47a0a3600388b28da3633fe1
163a058fa36001c8d0894fd24fd12221d22effe1122c4ed14ed2222104714ed2
222104714dd2222104714dd2222101e14cd212224cd200e3322204721222f072
4ad33222147212229261892148d3336292661367636189217231028b10431042
f68b44d1126ba3629c7231911632a08df261b831f1521632008b111ff2501230
0089121fa36001c8ac892fd13ad21222017ab22fa3602d88028f2de179af0900
163a018b50af3c880f88028f1e8853af0900a18f03c90c9420d10b410900a7af
0900ff03e07f007cff7ffa00c8001f7c9500e003846c5fa0f46c5fa0c0785fa0
0400d8ff0800d8ff44805fa0000080003f7d1700000020000401150000007f02
0000df0177df27000000100020805fa028815fa08c805fa017b7d138000010a5
000000022c805fa00000300044815fa00000008014805fa00c815fa01000018c
000000a500a000000000400060805fa000e000ac0100008000e1000c09200001
00000001e86c5fa0106c5fa000e0000c80841e000c00d8ff50805fa0
""")

SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
BASE = 45000            # LBA of the high-density data track
ROOT = BASE + 20        # root directory
FILE = BASE + 21        # 1ST_READ.BIN


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def raw_sector(lba, data):
    """One MODE1/2352 sector; the core does not check EDC/ECC."""
    fad = lba + 150
    hdr = bytes((bcd(fad // 4500), bcd((fad // 75) % 60), bcd(fad % 75), 1))
    return SYNC + hdr + data.ljust(2048, b'\0') + bytes(2352 - 16 - 2048)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dirent(name, lba, size, flags):
    ln = 33 + len(name) + ((len(name) + 1) & 1)
    rec = (bytes((ln, 0)) + both32(lba) + both32(size) + bytes(7)
           + bytes((flags, 0, 0)) + struct.pack('<H', 1) + struct.pack('>H', 1)
           + bytes((len(name),)) + name)
    return rec.ljust(ln, b'\0')


def main():
    if len(sys.argv) not in (2, 3) or sys.argv[2:] not in ([], ['--no-region-array']):
        sys.stderr.write(__doc__)
        return 2
    out = sys.argv[1]
    program = PROGRAM_NO_REGION_ARRAY if len(sys.argv) == 3 else PROGRAM
    d = os.path.dirname(out) or '.'

    ip = bytearray(16 * 2048)
    meta = (b'SEGA SEGAKATANA ' + b'SEGA ENTERPRISES' + b'0000 ' + b'GD-ROM'
            + b'1/1  ' + b'JUE     ' + b'E000' + b'F' + b'1' + b'0' + b' '
            + b'T-00000   ' + b'V1.000' + b'20260101' + b'        '
            + b'1ST_READ.BIN    ' + b'FLYCAST TEST    '
            + b'THREADS TEST'.ljust(128))
    ip[0:len(meta)] = meta
    # Bootstrap at 0x300: mov.l @(4,pc),r0; jmp @r0; nop; nop; .long 0x8c010000
    ip[0x300:0x30c] = (bytes((0x01, 0xd0, 0x2b, 0x40, 0x09, 0x00, 0x09, 0x00))
                       + struct.pack('<I', 0x8c010000))

    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[156:156 + 34] = dirent(b'\0', ROOT, 2048, 2)
    root = (dirent(b'\0', ROOT, 2048, 2) + dirent(b'\x01', ROOT, 2048, 2)
            + dirent(b'1ST_READ.BIN;1', FILE, len(program), 0))

    sectors = {}
    for i in range(16):
        sectors[BASE + i] = bytes(ip[i * 2048:(i + 1) * 2048])
    sectors[BASE + 16] = bytes(pvd)
    sectors[ROOT] = root
    for i in range((len(program) + 2047) // 2048):
        sectors[FILE + i] = program[i * 2048:(i + 1) * 2048]

    with open(os.path.join(d, 'track01.bin'), 'wb') as f:
        for lba in range(300):
            f.write(raw_sector(lba, b''))
    with open(os.path.join(d, 'track02.raw'), 'wb') as f:
        f.write(bytes(2352 * 300))
    with open(os.path.join(d, 'track03.bin'), 'wb') as f:
        for lba in range(BASE, BASE + 600):
            f.write(raw_sector(lba, sectors.get(lba, b'')))
    with open(out, 'w') as f:
        f.write('3\n1 0 4 2352 track01.bin 0\n2 450 0 2352 track02.raw 0\n'
                '3 %d 4 2352 track03.bin 0\n' % BASE)
    return 0


if __name__ == '__main__':
    sys.exit(main())
