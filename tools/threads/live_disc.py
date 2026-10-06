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
c62fd62fe62f224ff47f88d1126188d226652666226787d2266322625b216b21
7b213b212b21121f83d11060fec90021ffe282d322230473222300e204732123
106001cb00217ed112607ed3392005cb022118717cd33221f471222110717bd3
32216c717ad332219c717ad33221047179d33221107179d3322179d179d31223
79d3122379d12221fc7179d2222179d179d22221087179d2222179d1bf922221
0471bd92222104711fe222210471b892222174d274d30ce132221041fc8f0472
00e170d212220472122270d170d2222170d171d2222100ebffe0011f00e1122f
6ede69dd6edc6fd99c9883a000ea088d1e8801891ba103c96bd10b4102e434a0
090069d10b4101e4a36308433c330843ac3318430943ac3383911923a3603fc9
036108410c31184160d22c3160d231211042fc8f02711ba009005bd10b4103e4
13a0090058d10b4104e40ea0090003c903880189f0a03fe13fe1163ad48fa360
01c8598953d254d12221d22effe1122c52d153d22221047152d22221047152d2
2221047151d2222101e151d2122251d200e3322204721222f0724fd332221472
1222926189214dd3336292661367636189211637028d10431042f68b48d11261
a3639c732f922633028ff2628ea009001832f1532633008b211f223b0089236b
a36001c8018b98a0090032d23dd32223017a122fa3602d889f8d2de1163a0189
75af0f883c889d8d4688a08b36d10b9222210ed2a7af09000cd2a4af0900007c
e003ff7fff039500e07f0900846c5fa0f46c5fa0c0785fa00400d8ff0800d8ff
44805fa0000080003f7d1700000020000401150000007f020000df0177df2700
0000100020805fa028815fa08c805fa017b7d138000010a50000000200000030
04905fa0000018a500000080180018a54c0e00802c805fa00000180044815fa0
14805fa00c815fa01000018c000000a500a000000000400060805fa000e000ac
0100008000e1000c0920000100000001e86c5fa0106c5fa000e0000c80841e00
0c00d8ff50805fa010905fa02a92203a168fb363f1500833ce7226330889f252
2822028911d21e93322211d26eaf09000ed219933222f8af09000dd266af0900
3fe2263a018964af017a5aafa3600388018beaaea3633fe1163a018b10afa360
13af0900fa001f7c007c090010905fa000002000
""")

# The same program built with -DNO_REGION_ARRAY.
PROGRAM_NO_REGION_ARRAY = bytes.fromhex("""
01df02d02b40090000f0008c3800018c43631843436128411b234b2313641844
4b2304d210e132221041fc8f04720b0009000900000000a4862f962fa62fb62f
c62fd62fe62f224ff47f8cd112618cd22665266622678bd2266322625b216b21
7b213b212b21121f87d11060fec90021ffe286d322230473222300e204732123
106001cb002182d1126082d3392005cb0221187180d33221f471222110717fd3
32216c717ed332219c717ed3322104717dd3322110717dd332217dd17dd31223
7dd312237dd12221fc717dd222217dd17dd2222108717dd222217dd1c5922221
0471c392222104711fe222210471be92222178d178d2222100ebffe0011f00e1
122f76de76dd77dc77d9b19881a000ea088d1e88018915a103c974d10b4102e4
34a0090071d10b4101e4a36308433c330843ac3318430943ac3398911923a360
3fc9036108410c31184169d22c3169d231211042fc8f02711ba0090063d10b41
03e413a0090061d10b4104e40ea0090003c903880189eaa03fe13fe1163ad48f
a36001c857895cd25cd12221d22effe1122c5bd15bd2222104715bd222210471
5ad2222104715ad2222101e159d2122259d200e3322204721222f07257d33222
147212229261892155d3336292661367636189211637028d10431042f68b51d1
1261a3639c7344922633278df2621832f1532633008b211f223b0089236ba360
01c8018b94a009003bd247d32223017a122fa3602d88a18d2de1163a018977af
0f883c889f8d4688a28b40d1229222211ad2a9af090019d2a6af09001b92203a
798fb363f1500833ce7226330889f2522822028935d20f93322210d2d5af0900
32d202933222f8af0900007ce003ff7fff039500e07ffa001f7c0900846c5fa0
f46c5fa0c0785fa00400d8ff0800d8ff44805fa0000080003f7d170000002000
0401150000007f020000df0177df27000000100020805fa028815fa08c805fa0
17b7d138000010a5000000020000003004905fa02c805fa00000300044815fa0
0000008014805fa00c815fa01000018c000000a500a000000000400060805fa0
00e000ac0100008000e1000c0920000100000001e86c5fa0106c5fa000e0000c
80841e000c00d8ff50805fa010905fa00ad26aaf09003fe2263a018968af017a
5eafa3600388018bf0aea3633fe1163a018b16afa36019af0900090000002000
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
