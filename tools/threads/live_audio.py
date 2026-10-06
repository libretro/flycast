#!/usr/bin/env python3
"""Check the sound of a run of the live test's disc, as audio_tap.c wrote
it: signed 16-bit stereo, the samples the core sent the frontend.

Usage: live_audio.py sound.pcm

The disc plays its audio track, looping, with the sound chip set to pass
it through at full level, left to the left and right to the right. So
after the silence before the disc gets that far, what the core sends has
to be the track of live_disc.py sample for sample: nothing dropped,
repeated, swapped or rescaled, sectors in order, and at the end of the
track back to its start.
"""
import struct
import sys

from live_disc import AUDIO_SECTORS, cdda_frame

SECTOR = 588                 # stereo samples in a CD sector
RATE = 44100


def main():
    data = open(sys.argv[1], 'rb').read()
    count = len(data) // 4
    samples = struct.unpack('<%dh' % (2 * count), data[:4 * count])
    start = next((i for i in range(count)
                  if samples[2 * i] or samples[2 * i + 1]), None)
    if start is None:
        print('silence: %d samples and not one of them is not zero' % count)
        return 1
    track = [cdda_frame(n) for n in range(SECTOR * AUDIO_SECTORS)]
    at = 0                   # where in the track the next sample is from
    loops = []
    for i in range(start, count):
        got = (samples[2 * i], samples[2 * i + 1])
        if at == len(track) or got != track[at]:
            # the only place it may jump is from the end of a sector back
            # to the start of the track
            if at % SECTOR == 0 and at and got == track[0]:
                loops.append(at // SECTOR)
                at = 0
            else:
                want = track[at] if at < len(track) else 'the end of the track'
                print('at %.3f s (sample %d, %d into the music) the core sent '
                      '%s; the track has %s there (sector %d, sample %d of it)'
                      % (i / RATE, i, i - start, got, want,
                         at // SECTOR, at % SECTOR))
                return 1
        at += 1
    played = count - start
    if played < 4 * RATE or not loops:
        print('only %.2f s of the track came out, %d times round: too little '
              'to have checked the loop' % (played / RATE, len(loops)))
        return 1
    print('sound: %.2f s of the audio track came out as it is on the disc, '
          'after %.3f s of silence; it went back to the start after sector %d'
          % (played / RATE, start / RATE, loops[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
