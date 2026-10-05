#!/usr/bin/env python3
"""Compare the save states tools/threads/live.sh collects.

Usage: live_same.py threaded.state plain.state plain-again.state

The first comes from a run with Threaded Rendering on, the other two from
two runs with it off; all three ran the same frames from the same start.
The threaded state has to be the non-threaded one.

A save state holds the real-time clock, which is the clock's, so two runs
do not leave quite the same state: the clock's value, the copy the BIOS
keeps in flash and its checksum. The two non-threaded runs show where
those are, and every byte within seven of one that differs between them
is left out of the comparison. Everywhere else the threaded state must
match byte for byte.

The clock is all that may differ between the two non-threaded runs. More
than a few bytes means something that is not state, such as a clock
address, is being written into the state again, and that fails too.
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

    # Runs of bytes that hold the clock, as [first, last] pairs.
    clock = []
    for i in differing(plain, again):
        if clock and i - 7 <= clock[-1][1]:
            clock[-1][1] = i + 7
        else:
            clock.append([i - 7, i + 7])

    # Between two runs of the same thing, only the clock may differ: the
    # RTC and what the BIOS writes of it into flash.
    noise = sum(1 for _ in differing(plain, again))
    if noise > 32:
        print('%d bytes differ between the two non-threaded runs; only the '
              'clock should' % noise)
        return 1

    bad = []
    run = 0
    for i in differing(threaded, plain):
        while run < len(clock) and clock[run][1] < i:
            run += 1
        if run == len(clock) or i < clock[run][0]:
            bad.append(i)
    if bad:
        print('%d bytes differ outside the %d places the clock is kept; '
              'first at offset 0x%x' % (len(bad), len(clock), bad[0]))
        return 1
    print('same state (%d places holding the clock left out)' % len(clock))
    return 0


if __name__ == '__main__':
    sys.exit(main())
