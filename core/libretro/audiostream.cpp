#include "types.h"
#include "emulator.h"
#include "hw/maple/vmu_beep.h"

#include <string.h>
#include <libretro.h>

/* One retro_run is one video frame, and it comes with the sound the console
 * put out during that frame, in one consecutive batch. How much sound that
 * is follows from the hardware: the AICA runs at 44100 Hz and a frame lasts
 * as long as the sync generator says, so an NTSC or VGA frame (59.94 Hz)
 * carries 735 or 736 samples and a PAL frame (50 Hz) 882.
 *
 * The AICA is emulated a tick of 32 samples at a time, so at the end of a
 * frame the sound made so far is whole ticks: 704 or 736 samples for an
 * NTSC frame, not what the hardware puts out. FlushAudioFrame() therefore
 * asks how far into the current tick the frame ended and sends exactly the
 * samples due by then, keeping the rest for the next frame. To have them to
 * send it runs one tick, 32 samples or 0.7 ms, behind.
 *
 * The buffer comfortably exceeds one frame's worth of samples. If it is
 * ever exceeded it is flushed early as a safety valve. */
#define AUDIO_BUFFER_SIZE 4096

/* Samples in one AICA tick: how far FlushAudioFrame() runs behind. */
#define AUDIO_TICK_SAMPLES 32

extern retro_audio_sample_batch_t audio_batch_cb;

/* Samples of the current AICA tick the hardware has put out by now, 0..31. */
u32 libAICA_SamplesIntoTick();

/* File-scope so the batch boundary can be reset deterministically on state
 * load / reset. If writePtr survived a load, two states saved at different
 * intra-batch offsets would replay a different partial batch, so identical
 * inputs would no longer produce identical audio output -- breaking netplay and
 * runahead determinism. */
static SoundFrame Buffer[AUDIO_BUFFER_SIZE];
static u32 writePtr; /* next sample index */


void WriteSample(s16 r, s16 l)
{
   Buffer[writePtr].r = r;
   Buffer[writePtr].l = l;
   ++writePtr;

   /* The safety valve only trips if a single frame somehow overruns the
    * buffer. */
   if (writePtr == AUDIO_BUFFER_SIZE)
   {
      if (dc_is_running())
      {
         vmu_beep_mix(Buffer, writePtr);
         audio_batch_cb((const int16_t*)Buffer, writePtr);
      }
      writePtr = 0;
   }
}

/* Send the sound of the frame that just ran, at the end of retro_run. The dc
 * is stopped at the frame boundary, so this is not gated on dc_is_running(). */
void FlushAudioFrame(void)
{
   /* Of the last tick made, the hardware has only put out the part that
    * lies before the end of this frame; hold the rest back, on top of the
    * one tick this runs behind. */
   const u32 keep = AUDIO_TICK_SAMPLES - libAICA_SamplesIntoTick();

   if (writePtr > keep)
   {
      const u32 count = writePtr - keep;

      /* (a VMU's beep goes on top, only on what is sent: the samples held
       * back, which a state carries, are the console's alone) */
      vmu_beep_mix(Buffer, count);
      audio_batch_cb((const int16_t*)Buffer, count);
      memmove(Buffer, Buffer + count, keep * sizeof(Buffer[0]));
      writePtr = keep;
   }
}

/* Drop any partially-filled batch and rewind to a known boundary. Called after
 * (un)serialize and reset so the audio ring is in a deterministic state. */
void ResetAudioBuffer(void)
{
   writePtr = 0;
}

/* A state carries the samples held back at the end of its frame: they come
 * from the part of the last tick that lies after the frame's end, which the
 * AICA in the state has already made and will not make again. (A load
 * dropped them, and the frontend heard a gap of up to a tick at every load:
 * at every frame, under run-ahead.) Between two frames there are never more
 * than a tick's worth; the room for a whole tick is always written, the part
 * not used as zeroes, so that a state is the same size and the same bytes
 * for the same machine. */
void AudioHoldbackSerialize(void **data, unsigned int *total_size)
{
   s16 samples[AUDIO_TICK_SAMPLES * 2];
   u32 count = writePtr <= AUDIO_TICK_SAMPLES ? writePtr : 0;

   memset(samples, 0, sizeof(samples));
   for (u32 i = 0; i < count; i++)
   {
      samples[i * 2]     = Buffer[i].l;
      samples[i * 2 + 1] = Buffer[i].r;
   }
   LIBRETRO_S(count);
   LIBRETRO_SA(samples, AUDIO_TICK_SAMPLES * 2);
}

void AudioHoldbackUnserialize(void **data, unsigned int *total_size)
{
   s16 samples[AUDIO_TICK_SAMPLES * 2];
   u32 count = 0;

   LIBRETRO_US(count);
   LIBRETRO_USA(samples, AUDIO_TICK_SAMPLES * 2);
   if (count > AUDIO_TICK_SAMPLES)
      count = 0;
   for (u32 i = 0; i < count; i++)
   {
      Buffer[i].l = samples[i * 2];
      Buffer[i].r = samples[i * 2 + 1];
   }
   writePtr = count;
}
