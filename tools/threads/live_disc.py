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
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224f65d11260
65d2292005cb0221187164d2222100e163d21222107263d332226c7262d33222
9c7262d33222047261d33222107261d3322261d261d3222361d3222361d21222
61d162d2222162d162d2222162d263d30ce132221041fc8f047200e15ed21222
047212225ed15fd222215fd15fd2222100eb5fde59dd5fdc5fd9849872a000ea
3c88208f03c97f945cd10b41090033a009007a9459d10b4109007793b923a360
3fc9036108410c31184155d22c3155d231211042fc8f027121a009004fd10b41
1fe419a0090003880d8f3fe1163a0e8da360e2af090033d20ca0090031d23ba0
09000388d9893fe1163a058fa36001c8f18945d245d12221d22effe1122c44d1
44d22221047144d22221047143d22221047143d2222101e142d2122242d200e3
322204721222f07240d3322214721222926189213ed333629266136763618921
7231028b10431042f68b3fe1163a058fa36001c8c2892cd211d12221017a1691
1c3bbd6ba3602d88a88d2de1163a878d0f888e8d1e88b48f03c909941fd10b41
0900b9af0900ff03e07f007cff7f4108e003090044805fa0000080003f7d1700
50805fa0000020000401150000007f020000df0177df27000000100020805fa0
28815fa08c805fa088805fa017b7d138000010a500000002000018a500000080
180018a54c0e00802c805fa00000180044815fa014805fa00c815fa01000018c
000000a500a000000000400060805fa000e000ac0100008000e1000c09200001
00000001e86c5fa0106c5fa000e0000c80841e00
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c2c00018c436128411c3404d220e142221041fc8f
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224f5dd11260
5dd2292005cb022118715cd2222100e15bd2122210725bd332226c725ad33222
9c725ad33222047259d33222107259d3322259d259d3222359d3222359d21222
59d15ad222215ad15ad222215ad15bd2222100eb5ade5bdd5bdc5cd9849872a0
00ea3c88208f03c97f9459d10b41090033a009007a9456d10b4109007793b923
a3603fc9036108410c31184151d22c3151d231211042fc8f027121a009004cd1
0b411fe419a0090003880d8f3fe1163a0e8da360e2af090032d20ca0090031d2
3ba009000388d9893fe1163a058fa36001c8f18941d242d12221d22effe1122c
40d141d22221047140d22221047140d2222104713fd2222101e13fd212223fd2
00e3322204721222f0723dd3322214721222926189213bd33362926613676361
89217231028b10431042f68b3fe1163a058fa36001c8c28928d211d12221017a
16911c3bbd6ba3602d88a88d2de1163a878d0f888e8d1e88b48f03c909941cd1
0b410900b9af0900ff03e07f007cff7f4108e00344805fa0000080003f7d1700
50805fa0000020000401150000007f020000df0177df27000000100020805fa0
28815fa08c805fa088805fa017b7d138000010a5000000022c805fa000003000
44815fa00000008014805fa00c815fa01000018c000000a500a0000000004000
60805fa000e000ac0100008000e1000c0920000100000001e86c5fa0106c5fa0
00e0000c80841e00
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
