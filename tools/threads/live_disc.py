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
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224f68d11260
68d2292005cb0221187167d2222100e166d21222107266d332226c7265d33222
9c7265d33222047264d33222107264d3322264d264d3222364d3222364d21222
64d165d2222165d165d2222165d266d30ce132221041fc8f047200e161d21222
0472122261d162d2222162d162d2222100eb62de5cdd62dc62d98a9843a000ea
3c88208f03c985945fd10b41090059a0090080945cd10b4109007d93b923a360
3fc9036108410c31184158d22c3158d231211042fc8f027147a0090052d10b41
1fe43fa009000388338f3fe1163a348da360e2af090036d232a009004dd11362
92638823028f10411042f98b3fe1163a058fa36001c8178947d22cd12221017a
4b911c3bbd6ba3602d88d78d2de1163ab68d0f88bd8d1e88098f03c93e943ad1
0b4109000ea0090021d2e6af09000388b3893fe1163a058fa36001c8cb8936d2
36d12221d22effe1122c35d135d22221047135d22221047134d22221047134d2
222101e133d2122233d200e3322204721222f07231d332221472122225d22361
92638823aa8d10421041f98b21d1a7af13620020e07f007cff7f4108e0030900
44805fa0000080003f7d170050805fa0000020000401150000007f020000df01
77df27000000100020805fa028815fa08c805fa088805fa017b7d138000010a5
00000002000018a500000080180018a54c0e00802c805fa00000180044815fa0
14805fa00c815fa01000018c000000a500a0000080841e000000400060805fa0
00e000ac0100008000e1000c0920000100000001e86c5fa0106c5fa000e0000c
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c2c00018c436128411c3404d220e142221041fc8f
04720b0009000900000000a4862f962fa62fb62fc62fd62fe62f224f60d11260
60d2292005cb022118715fd2222100e15ed2122210725ed332226c725dd33222
9c725dd3322204725cd3322210725cd332225cd25cd322235cd322235cd21222
5cd15dd222215dd15dd222215dd15ed2222100eb5dde5edd5edc5fd98a9843a0
00ea3c88208f03c985945cd10b41090059a00900809459d10b4109007d93b923
a3603fc9036108410c31184154d22c3154d231211042fc8f027147a009004fd1
0b411fe43fa009000388338f3fe1163a348da360e2af090035d232a009004ad1
136292638823028f10411042f98b3fe1163a058fa36001c8178944d22bd12221
017a4b911c3bbd6ba3602d88d78d2de1163ab68d0f88bd8d1e88098f03c93e94
36d10b4109000ea0090021d2e6af09000388b3893fe1163a058fa36001c8cb89
32d233d12221d22effe1122c31d132d22221047131d22221047131d222210471
30d2222101e130d2122230d200e3322204721222f0722ed332221472122222d2
236192638823aa8d10421041f98b1ed1a7af13620020e07f007cff7f4108e003
44805fa0000080003f7d170050805fa0000020000401150000007f020000df01
77df27000000100020805fa028815fa08c805fa088805fa017b7d138000010a5
000000022c805fa00000300044815fa00000008014805fa00c815fa01000018c
000000a500a0000080841e000000400060805fa000e000ac0100008000e1000c
0920000100000001e86c5fa0106c5fa000e0000c
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
