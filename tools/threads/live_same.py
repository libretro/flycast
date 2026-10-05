#!/usr/bin/env python3
"""Compare the save states tools/threads/live.sh collects.

Usage: live_same.py threaded.state plain.state plain-again.state

The first comes from a run with Threaded Rendering on, the other two from
two runs with it off; all three ran the same frames from the same start.
The threaded state has to be the non-threaded one.

A save state of this core carries a few host addresses (pointers into the
sound RAM and the like), and those change from one process to the next.
The two non-threaded runs show where they are. The addresses are eight
bytes long and sit at any offset, and two runs need not differ in every
byte of one, so every byte within seven of a byte that differs between
the two non-threaded runs is taken to belong to an address and left out
of the comparison. Everywhere else the threaded state must match byte for
byte.
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

    def differing(a, b):
        """Offsets of the bytes in which a and b differ."""
        out = []
        if a == b:
            return out
        step = 1 << 16
        for base in range(0, len(a), step):
            ca, cb = a[base:base + step], b[base:base + step]
            if ca != cb:
                out.extend(base + i for i in range(len(ca)) if ca[i] != cb[i])
        return out

    # Runs of bytes that belong to host addresses, as [first, last] pairs.
    host = []
    for i in differing(plain, again):
        if host and i - 7 <= host[-1][1]:
            host[-1][1] = i + 7
        else:
            host.append([i - 7, i + 7])

    bad = []
    run = 0
    for i in differing(threaded, plain):
        while run < len(host) and host[run][1] < i:
            run += 1
        if run == len(host) or i < host[run][0]:
            bad.append(i)
    if bad:
        print('%d bytes differ outside the %d places that hold host addresses; '
              'first at offset 0x%x' % (len(bad), len(host), bad[0]))
        return 1
    print('same state (%d places holding host addresses left out)' % len(host))
    return 0


if __name__ == '__main__':
    sys.exit(main())
