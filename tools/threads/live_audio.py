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

Over that, on the left only, the disc has a channel of the sound chip
play a short loop of PCM from the chip's memory at its own rate and full
level: keyed on, off after a second, and on again. So the right is the
track alone, and the left less the track is either nothing or that loop,
from its first sample, round and round, for two stretches with a gap
between them.
"""
import struct
import sys

from live_disc import (AUDIO_SECTORS, CHANNEL_SAMPLES, cdda_frame,
                       channel_sample)

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
    loop = [channel_sample(k) for k in range(CHANNEL_SAMPLES)]
    at = 0                   # where in the track the next sample is from
    loops = []
    note = None              # where in its loop the channel is, when it is on
    notes = []               # [first sample, samples] of each time it was on
    for i in range(start, count):
        left, right = samples[2 * i], samples[2 * i + 1]
        if at == len(track) or right != track[at][1]:
            # the only place the track may jump is from the end of a sector
            # back to its start
            if at % SECTOR == 0 and at and right == track[0][1]:
                loops.append(at // SECTOR)
                at = 0
            else:
                want = track[at][1] if at < len(track) else 'the end of the track'
                print('at %.3f s (sample %d, %d into the music) the core sent '
                      '%d on the right; the track has %s there (sector %d, '
                      'sample %d of it)'
                      % (i / RATE, i, i - start, right, want,
                         at // SECTOR, at % SECTOR))
                return 1
        extra = left - track[at][0]
        at += 1
        if note is None:
            if extra == loop[0]:
                notes.append([i, 0])
                note = 0
            elif extra:
                print('at %.3f s (sample %d) the left is %d off the track with '
                      'the channel off; its loop starts with %d'
                      % (i / RATE, i, extra, loop[0]))
                return 1
        if note is not None:
            if extra == loop[note]:
                note = (note + 1) % len(loop)
                notes[-1][1] += 1
            elif extra == 0:
                note = None
            else:
                print('at %.3f s (sample %d) the left is %d off the track; '
                      'the channel, %d samples into its loop, has %d'
                      % (i / RATE, i, extra, note, loop[note]))
                return 1
    played = count - start
    if played < 4 * RATE or not loops:
        print('only %.2f s of the track came out, %d times round: too little '
              'to have checked the loop' % (played / RATE, len(loops)))
        return 1
    # on for a second, off for most of one, on again to the end
    if (len(notes) != 2 or not 0.9 * RATE < notes[0][1] < 1.2 * RATE
            or notes[1][0] - notes[0][0] - notes[0][1] < RATE // 2
            or notes[1][1] < RATE or note is None):
        print('the channel was to play for a second, stop, and play again to '
              'the end; it played %s'
              % (', '.join('%.3f s from %.3f s' % (n / RATE, s / RATE)
                           for s, n in notes) or 'not at all'))
        return 1
    print('sound: %.2f s of the audio track came out as it is on the disc, '
          'after %.3f s of silence; it went back to the start after sector %d'
          % (played / RATE, start / RATE, loops[0]))
    print('sound: the channel played its loop as it is in memory, for %.3f s '
          'from %.3f s and for %.3f s from %.3f s'
          % (notes[0][1] / RATE, notes[0][0] / RATE,
             notes[1][1] / RATE, notes[1][0] / RATE))
    return 0


if __name__ == '__main__':
    sys.exit(main())
