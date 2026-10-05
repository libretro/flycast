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
01df02d02b40090000f0008c1000018c862f962fa62fb62fc62fd62fe62f40d1
126040d2292005cb022118713ed2222100e13ed2122210723dd332226c723dd3
32223dd23dd322233dd322233dd212223dd13ed222213ed13ed2122204721222
0472122204721222047212220472122239d13ad2222100e639dd35dc39db4b9e
39da3ad93ad83bd43bd746931ca000e5e92253603fc9036108410c31184137d0
0c3137d021211040fc8f027114a0090034d1136272603820028f10411042f98b
01752b911c366d66536003c90388df8d6362c22dffe1122b922a42282ad12bd2
222104712ad2222101e12ad212222ad200e0022204721222f07228d002221472
122220d2236172603820d18d10421041f98b1cd1ceaf1362ff7f002041080900
44805fa0000080003f7d170050805fa000002000040115000000100020805fa0
28815fa08c805fa0000010a50000000200000080000018a52c805fa000001800
44815fa014805fa000e000ac0100008004e000ac00e1000c0c815fa0000000a5
00a0000080841e0008e000ac0920000100000001e86c5fa0106c5fa000e0000c
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c1000018c862f962fa62fb62fc62fd62fe62f39d1
126039d2292005cb0221187137d2222100e137d21222107236d332226c7236d3
322236d236d3222336d3222336d2122236d137d2222137d137d2222100e637dd
37dc38db4b9e38da38d939d839d43ad746931ca000e5e92253603fc903610841
0c31184135d00c3135d021211040fc8f027114a0090033d1136272603820028f
10411042f98b01752b911c366d66536003c90388df8d6362c22dffe1122b922a
422829d129d22221047129d2222101e128d2122228d200e0022204721222f072
26d00222147212221ed2236172603820d18d10421041f98b1ad1ceaf1362ff7f
0020410844805fa0000080003f7d170050805fa0000020000401150000001000
20805fa028815fa08c805fa0000010a5000000022c805fa00000300044815fa0
0000008014805fa000e000ac0100008004e000ac00e1000c0c815fa0000000a5
00a0000080841e0008e000ac0920000100000001e86c5fa0106c5fa000e0000c
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
