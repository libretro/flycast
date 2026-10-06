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
level: keyed on, off after a second, and on again. So the left less the
track is either nothing or that loop, from its first sample, round and
round, for two stretches with a gap between them.

On the right only, from the moment that channel first sounds, a second
channel plays a square wave and then silence, round and round, through
its low-pass filter, with the cutoff held and the resonance up. The right
less the track has to be what the filter makes of that wave, sample for
sample. lowpass() below works that out with the filter's own whole-number
arithmetic, written out again here: what the core computes has to be
this, to the last bit, through the ringing after every edge and the
silence after it.
"""
import struct
import sys

from live_disc import (AUDIO_SECTORS, CHANNEL_SAMPLES, FILTER_CUTOFF,
                       FILTER_Q, FILTER_SAMPLES, cdda_frame, channel_sample,
                       filter_sample)

SECTOR = 588                 # stereo samples in a CD sector
RATE = 44100


# How far each value of the Q register takes the damping from its neutral
# setting, in 4096ths.
QTABLE = (2048, 1536, 1024, 512, 0, -256, -512, -768,
          -1024, -1280, -1536, -1792, -2048, -2176, -2304, -2432,
          -2560, -2688, -2816, -2944, -3072, -3136, -3200, -3264,
          -3328, -3392, -3456, -3520, -3584, -3648, -3712, -3776)


def lowpass(cutoff, q_register):
    """The sound chip's low-pass filter at one setting, as a function from
    a 16-bit sample to the 16 bits that reach the mix: two poles,

        y = -a0 x + (2 - f - a0) y1 - (1 - f) y2

    on 20-bit samples with 30 bits of fraction, the fraction the output
    drops carried into the next sample."""
    exp, mant = cutoff >> 9, (cutoff & 0x1FF) | 0x200
    a0 = ((((mant << 30) >> ((15 - exp) * 2)) * ((mant - 1) // 8)) >> 17)
    f = (mant << exp) << 5
    scaled = QTABLE[q_register] * f
    f += scaled // 4096 if scaled >= 0 else -(-scaled // 4096)   # as C divides
    b1, b2 = (1 << 31) - (f + a0), (1 << 30) - f
    state = [0, 0, 0]            # the last output, the one before, the fraction

    def step(x):
        if exp == 0:
            state[2] = 0
        mac = -a0 * (x << 4) + b1 * state[0] - b2 * state[1] - state[2]
        y = mac >> 30
        state[2] = (y << 30) - mac
        y = max(-512 * 1024, min(512 * 1024 - 1, y))
        state[1], state[0] = state[0], y
        return y >> 4
    return step


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
    # The track goes round one sector short of its length: the drive stops
    # short of the last sector it is given.
    period = SECTOR * (AUDIO_SECTORS - 1)
    note = None              # where in its loop channel 0 is, when it is on
    notes = []               # [first sample, samples] of each time it was on
    filtered = None          # channel 1's filter, once it sounds
    wave = 0                 # ...and where in its loop it is
    for i in range(start, count):
        at = (i - start) % period
        left = samples[2 * i] - track[at][0]
        right = samples[2 * i + 1] - track[at][1]
        if note is None:
            if left == loop[0]:
                notes.append([i, 0])
                note = 0
                if filtered is None:
                    filtered = lowpass(FILTER_CUTOFF, FILTER_Q)
            elif left:
                print('at %.3f s (sample %d) the left is %d off the track with '
                      'the channel off; its loop starts with %d (the track is '
                      'at sector %d, sample %d of it)'
                      % (i / RATE, i, left, loop[0], at // SECTOR, at % SECTOR))
                return 1
        if note is not None:
            if left == loop[note]:
                note = (note + 1) % len(loop)
                notes[-1][1] += 1
            elif left == 0:
                note = None
            else:
                print('at %.3f s (sample %d) the left is %d off the track; '
                      'the channel, %d samples into its loop, has %d'
                      % (i / RATE, i, left, note, loop[note]))
                return 1
        want = 0
        if filtered is not None:
            want = filtered(filter_sample(wave % FILTER_SAMPLES))
            wave += 1
        if right != want:
            print('at %.3f s (sample %d) the right is %d off the track; the '
                  'filtered channel, %d samples in, should make that %d'
                  % (i / RATE, i, right, wave - 1, want))
            return 1
    loops = [AUDIO_SECTORS - 1] if count - start > period else []
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
    print('sound: the filtered channel came out as the filter makes it, bit '
          'for bit, for %.3f s' % (wave / RATE))
    return 0


if __name__ == '__main__':
    sys.exit(main())
