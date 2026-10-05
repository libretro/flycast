#!/usr/bin/env python3
"""Compare the save states tools/threads/live.sh collects.

Usage: live_same.py threaded.state plain.state plain-again.state

The first comes from a run with Threaded Rendering on, the other two from
two runs with it off; all three ran the same frames from the same start.
The threaded state has to be the non-threaded one.

A save state of this core carries a few host addresses (pointers into the
sound RAM and the like), and those change from one process to the next.
The two non-threaded runs show where they are: any 8-byte word that
differs between them is taken to hold one, and is left out of the
comparison. Everywhere else the threaded state must match byte for byte.
"""
import sys


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    threaded, plain, again = (open(p, 'rb').read() for p in sys.argv[1:])
    if not (len(threaded) == len(plain) == len(again)):
        print('the states differ in size: %d, %d, %d'
              % (len(threaded), len(plain), len(again)))
        return 1

    def words(a, b):
        """Indices of the 8-byte words in which a and b differ."""
        out = set()
        if a == b:
            return out
        step = 1 << 16
        for base in range(0, len(a), step):
            if a[base:base + step] != b[base:base + step]:
                for i in range(base, min(base + step, len(a)), 8):
                    if a[i:i + 8] != b[i:i + 8]:
                        out.add(i >> 3)
        return out

    host = words(plain, again)
    bad = sorted(words(threaded, plain) - host)
    if bad:
        print('%d words differ outside the %d that hold host addresses; '
              'first at offset 0x%x' % (len(bad), len(host), bad[0] << 3))
        return 1
    print('same state (%d words of host addresses left out)' % len(host))
    return 0


if __name__ == '__main__':
    sys.exit(main())
