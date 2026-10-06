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
01df02d02b40090000f0008c3800018c43631843436128411b234b2313641844
4b2304d210e132221041fc8f04720b0009000900000000a4862f962fa62fb62f
c62fd62fe62f224ff47f89d1126189d226652666226788d2266322625b216b21
7b213b212b21121f84d11060fec90021ffe283d322230473222300e204732123
106001cb00217fd112607fd3392005cb022118717dd33221f471222110717cd3
32216c717bd332219c717bd3322104717ad3322110717ad332217ad17ad31223
7ad312237ad12221fc717ad222217ad17ad2222104717ad22221047179d22221
79d1bf9222210471bd92222104711fe222210471b892222174d275d30ce13222
1041fc8f047200e170d212220472122270d171d2222171d171d2222100ebffe0
011f00e1122f6fde69dd6fdc6fd99c9883a000ea088d1e8801891ca103c96cd1
0b4102e434a0090069d10b4101e4a36308433c330843ac3318430943ac338391
1923a3603fc9036108410c31184161d22c3161d231211042fc8f02711ba00900
5bd10b4103e413a0090059d10b4104e40ea0090003c903880189f1a03fe13fe1
163ad48fa36001c8598954d254d12221d22effe1122c53d153d22221047153d2
2221047152d22221047152d2222101e151d2122251d200e3322204721222f072
4fd3322214721222926189214dd3336292661367636189211637028d10431042
f68b49d11261a3639c732f922633028ff2628fa009001832f1532633008b211f
223b0089236ba36001c8018b99a0090032d23ed32223017a122fa3602d889f8d
2de1163a018975af0f883c889d8d4688a08b37d10b9222210dd2a7af09000cd2
a4af0900007ce003ff7fff039500e07f846c5fa0f46c5fa0c0785fa00400d8ff
0800d8ff44805fa0000080003f7d1700000020000401150000007f020000df01
77df27000000100020805fa028815fa08c805fa017b7d138000010a500000002
000000200000003004905fa0000018a500000080180018a54c0e00802c805fa0
0000180044815fa014805fa00c815fa01000018c000000a500a0000000004000
60805fa000e000ac0100008000e1000c0920000100000001e86c5fa0106c5fa0
00e0000c80841e000c00d8ff50805fa010905fa02a92203a168fb363f1500833
ce7226330889f2522822028911d21e93322211d26daf09000ed219933222f8af
09000dd265af09003fe2263a018963af017a59afa3600388018be9aea3633fe1
163a018b0fafa36012af0900fa001f7c007c090010905fa000002000
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c3800018c43631843436128411b234b2313641844
4b2304d210e132221041fc8f04720b0009000900000000a4862f962fa62fb62f
c62fd62fe62f224ff47f8dd112618dd22665266622678cd2266322625b216b21
7b213b212b21121f88d11060fec90021ffe287d322230473222300e204732123
106001cb002183d1126083d3392005cb0221187181d33221f4712221107180d3
32216c717fd332219c717fd3322104717ed3322110717ed332217ed17ed31223
7ed312237ed12221fc717ed222217ed17ed2222104717ed2222104717dd22221
7dd1c59222210471c392222104711fe222210471be92222178d179d2222100eb
ffe0011f00e1122f76de77dd77dc78d9b19881a000ea088d1e88018916a103c9
74d10b4102e434a0090072d10b4101e4a36308433c330843ac3318430943ac33
98911923a3603fc9036108410c31184169d22c3169d231211042fc8f02711ba0
090064d10b4103e413a0090061d10b4104e40ea0090003c903880189eba03fe1
3fe1163ad48fa36001c857895cd25dd12221d22effe1122c5bd15cd222210471
5bd2222104715bd2222104715ad2222101e15ad212225ad200e3322204721222
f07258d33222147212229261892156d3336292661367636189211637028d1043
1042f68b51d11261a3639c7344922633278df2621832f1532633008b211f223b
0089236ba36001c8018b95a009003cd247d32223017a122fa3602d88a18d2de1
163a018977af0f883c889f8d4688a28b40d1229222211ad2a9af090018d2a6af
09001b92203a7a8fb363f1500833ce7226330889f2522822028936d20f933222
0fd2d5af090033d202933222f8af0900007ce003ff7fff039500e07ffa001f7c
846c5fa0f46c5fa0c0785fa00400d8ff0800d8ff44805fa0000080003f7d1700
000020000401150000007f020000df0177df27000000100020805fa028815fa0
8c805fa017b7d138000010a500000002000000200000003004905fa02c805fa0
0000300044815fa00000008014805fa00c815fa01000018c000000a500a00000
0000400060805fa000e000ac0100008000e1000c0920000100000001e86c5fa0
106c5fa000e0000c80841e000c00d8ff50805fa010905fa00ad269af09003fe2
263a018967af017a5dafa3600388018befaea3633fe1163a018b15afa36018af
0900090000002000
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
