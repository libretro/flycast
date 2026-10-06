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

Above the middle the disc draws three polygons that take shadows and a
modifier volume across them (live_prog.c says what and why). Where the
volume is, the untextured ones have to be half as bright, the one with a
decal texture has to be as it is outside, and the background between
them has to be untouched; every renderer has to agree on that.

Below those are two translucent polygons, a blue one near and a red one
far, sent nearest first. Where they overlap the red has to have been
blended first and the blue over it: purple, not the reddish colour the
order they were sent in would give.

Above the three there is a white polygon in fog that is half green at
every depth, and two punch-through polygons, one with an opaque texture
and one with a transparent one: light green, magenta, and nothing.

A second render pass draws a cyan polygon, which does not take shadows,
over a corner of the first white one, with a modifier volume of its own
over it: it has to stay cyan. And the translucent pair again, lower
down, this time marked as already sorted: where they overlap the blue,
drawn first, hides the red behind it.

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
    # The shadow scene: what is where, on a 320 by 240 screen, and the
    # colour it has to be, give or take the rounding.
    scene = (
        ('A, lit',                  50,  90, (255, 255, 255)),
        ('A, above the volume',    104,  75, (255, 255, 255)),
        ('A, in the volume',       104,  90, (127, 127, 127)),
        ('the background, in the volume', 136, 90, (255, 255, 0)),
        ('C, lit',                 160,  75, (128, 128, 128)),
        ('C, in the volume',       160,  90, (64, 64, 64)),
        ('B, in the volume',       216,  90, (0, 0, 255)),
        ('B, lit',                 270,  90, (0, 0, 255)),
        # the translucent pair: blended twice, so a little more rounding
        ('red over the background', 130, 150, (254, 127, 0)),
        ('blue over red',          160, 150, (127, 63, 127)),
        ('blue over the background', 190, 150, (127, 127, 127)),
        ('the fogged polygon',      64,  42, (127, 254, 127)),
        ('the opaque cut-out',     216,  42, (255, 0, 255)),
        ('the transparent cut-out', 266,  42, (255, 255, 0)),
        # the second render pass
        ('P, in its volume',        55, 101, (0, 255, 255)),
        ('P, outside it',           42,  96, (0, 255, 255)),
        ('unsorted: red over the background', 130, 195, (254, 127, 0)),
        ('unsorted: blue in front of red', 160, 195, (127, 127, 127)),
        ('unsorted: blue over the background', 190, 195, (127, 127, 127)),
    )
    for name, sx, sy, want in scene:
        x, y = width * sx // 320, height * sy // 240
        got = tuple(rows[y][x * bpp:x * bpp + 3])
        if any(abs(got[i] - want[i]) > 3 for i in range(3)):
            bad.append('%s is %d,%d,%d, not %d,%d,%d' % ((name,) + got + want))
    if bad:
        print('the scene: ' + '; '.join(bad))
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
