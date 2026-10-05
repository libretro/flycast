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
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224ff47f87d1
1060fec90021ffe285d322230473222300e204732123106001cb002181d11260
81d3392005cb0221187180d33221f471222110717ed332216c717ed332219c71
7dd3322104717dd3322110717cd332217cd17dd312237dd312237dd12221fc71
7cd222217cd17dd222217dd27dd30ce132221041fc8f047200e179d212220472
122279d179d2222179d17ad2222100e0021fffe1111f00e2222f77de71dd77dc
77d9b29844a000ea3c88288f03c9ad9474d10b41090068a00900a89471d10b41
0900a36308433c330843ac3318430943ac339d911923a3603fc9036108410c31
184169d22c3169d231211042fc8f02714ea0090063d10b411fe446a009000388
3a8f3fe1163a3b8da360dbafa36348d239a009007d91103a218ff252f1511832
78911632158942d159d21222017ab22fa3602d88de8d2de1163ab58d0f88bc8d
1e88168f03c966944ed10b4109001ca009005c944bd10b410900e4af09003fe1
163ae48f017aff7a4aa0a36030d1dbaf09000388a68da3633fe1163a058fa360
01c8c48943d244d12221d22effe1122c42d143d22221047142d22221047142d2
2221047141d2222101e141d2122241d200e3322204721222f0723fd332221472
1222926189213dd3336292661367636189217231028b10431042f68b38d1126b
a3629c7218911632948df261b831f1521632008b111ff25012300089121fa360
01c8b38923d18faf0900ff03e07f007cff7ffa00c800e003950009000400d8ff
0800d8ff44805fa0000080003f7d1700000020000401150000007f020000df01
77df27000000100020805fa028815fa08c805fa017b7d138000010a500000002
000018a500000080180018a54c0e00802c805fa00000180044815fa014805fa0
0c815fa01000018c000000a500a0000050805fa00000400060805fa000e000ac
0100008000e1000c0920000100000001e86c5fa0106c5fa000e0000c80841e00
0c00d8ff
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c2c00018c436128411c3404d220e142221041fc8f
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224ff47f7fd1
1060fec90021ffe27dd322230473222300e204732123106001cb002179d11260
79d3392005cb0221187178d33221f4712221107176d332216c7176d332219c71
75d33221047175d33221107174d3322174d175d3122375d3122375d12221fc71
74d2222174d175d2222175d175d2222100e0021fffe1111f00e2222f72de73dd
73dc74d9b29844a000ea3c88288f03c9ad9471d10b41090068a00900a8946ed1
0b410900a36308433c330843ac3318430943ac339d911923a3603fc903610841
0c31184165d22c3165d231211042fc8f02714ea0090060d10b411fe446a00900
03883a8f3fe1163a3b8da360dbafa36347d239a009007d91103a218ff252f151
183278911632158941d156d21222017ab22fa3602d88de8d2de1163ab58d0f88
bc8d1e88168f03c966944bd10b4109001ca009005c9448d10b410900e4af0900
3fe1163ae48f017aff7a4aa0a36030d1dbaf09000388a68da3633fe1163a058f
a36001c8c48940d240d12221d22effe1122c3fd13fd2222104713fd222210471
3ed2222104713ed2222101e13dd212223dd200e3322204721222f0723bd33222
147212229261892139d3336292661367636189217231028b10431042f68b35d1
126ba3629c7218911632948df261b831f1521632008b111ff25012300089121f
a36001c8b38920d18faf0900ff03e07f007cff7ffa00c800e00395000400d8ff
0800d8ff44805fa0000080003f7d1700000020000401150000007f020000df01
77df27000000100020805fa028815fa08c805fa017b7d138000010a500000002
2c805fa00000300044815fa00000008014805fa00c815fa01000018c000000a5
00a0000050805fa00000400060805fa000e000ac0100008000e1000c09200001
00000001e86c5fa0106c5fa000e0000c80841e000c00d8ff
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
