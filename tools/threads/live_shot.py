#!/usr/bin/env python3
"""Check the screenshot tools/threads/live.sh has RetroArch take on exit.

Usage: live_shot.py shot.png

The test disc ends up drawing its background texture in yellow, the last
of four colours it paints it. The screenshot has to be yellow at the
centre and towards each corner: any other colour means a repaint of the
texture never made it from video memory to the screen.

The top left corner has to be something else: the first VMU's screen,
which live.sh turns on and the renderer draws there from the picture the
emulation thread last published.

Reads the PNG itself (8-bit RGB or RGBA, not interlaced), so nothing
beyond the standard library is needed.
"""
import struct
import sys
import zlib


def png_rows(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat = 8, b''
    while pos < len(data):
        size, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + size]
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + size
    if depth != 8 or interlace != 0 or colour not in (2, 6):
        raise ValueError('unsupported PNG layout')
    bpp = 3 if colour == 2 else 4
    stride = width * bpp
    raw = zlib.decompress(idat)
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(height):
        kind = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                best = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + best) & 255
        rows.append(line)
        prev = line
    return width, height, bpp, rows


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    width, height, bpp, rows = png_rows(sys.argv[1])
    bad = []
    for fx, fy in ((2, 2), (1, 1), (3, 1), (1, 3), (3, 3)):
        x, y = width * fx // 4, height * fy // 4
        r, g, b = rows[y][x * bpp:x * bpp + 3]
        if not (r > 200 and g > 200 and b < 60):
            bad.append('(%d,%d) is %d,%d,%d' % (x, y, r, g, b))
    if bad:
        print('not yellow: ' + '; '.join(bad))
        return 1
    # live.sh turns the first VMU's screen on, which is drawn over the top
    # left corner: the corner is the LCD's colour, not the background's.
    r, g, b = rows[8][8 * bpp:8 * bpp + 3]
    if r > 200 and g > 200 and b < 60:
        print("the VMU's screen is not drawn in the top left corner")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
