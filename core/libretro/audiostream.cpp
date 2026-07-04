#include "types.h"
#include "emulator.h"

#include <libretro.h>

/* Fixed chunk size used only for the threaded path, where the emu thread
 * produces audio decoupled from retro_run and there is no frame boundary to
 * flush against. */
#define SAMPLE_COUNT 512

/* The non-threaded path accumulates a whole frame and flushes it in one batch
 * from retro_run, so the buffer must comfortably exceed one frame's worth of
 * samples: 735 at NTSC 60Hz, 882 at PAL 50Hz. This also absorbs the occasional
 * frame that spans slightly more cycles. If it is ever exceeded the buffer is
 * flushed early as a safety valve. */
#define AUDIO_BUFFER_SIZE 2048

extern retro_audio_sample_batch_t audio_batch_cb;

/* File-scope so the batch boundary can be reset deterministically on state
 * load / reset. If writePtr survived a load, two states saved at different
 * intra-batch offsets would replay a different partial batch, so identical
 * inputs would no longer produce identical audio output — breaking netplay and
 * runahead determinism. */
static SoundFrame Buffer[AUDIO_BUFFER_SIZE];
static u32 writePtr; /* next sample index */


void WriteSample(s16 r, s16 l)
{
   Buffer[writePtr].r = r;
   Buffer[writePtr].l = l;
   ++writePtr;

#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering)
   {
      /* Threaded: audio is generated on the emu thread, decoupled from
       * retro_run. Emit in fixed chunks as it fills, gated by LimitFPS. */
      if (writePtr == SAMPLE_COUNT)
      {
         if (dc_is_running() && settings.aica.LimitFPS)
            audio_batch_cb((const int16_t*)Buffer, SAMPLE_COUNT);
         writePtr = 0;
      }
      return;
   }
#endif

   /* Non-threaded: accumulate the whole frame; FlushAudioFrame() emits it once
    * per retro_run. The safety valve only trips if a single frame somehow
    * overruns the buffer. */
   if (writePtr == AUDIO_BUFFER_SIZE)
   {
      if (dc_is_running())
         audio_batch_cb((const int16_t*)Buffer, writePtr);
      writePtr = 0;
   }
}

/* Non-threaded: flush the frame's accumulated samples at the end of retro_run,
 * so every retro_run emits exactly one video frame's worth of audio in a single
 * consecutive batch. The samples were produced by the frame that just ran, so
 * this is not gated on dc_is_running() (the dc is momentarily stopped at the
 * frame boundary). No-op when nothing accumulated. */
void FlushAudioFrame(void)
{
   if (writePtr > 0)
   {
      audio_batch_cb((const int16_t*)Buffer, writePtr);
      writePtr = 0;
   }
}

/* Drop any partially-filled batch and rewind to a known boundary. Called after
 * (un)serialize and reset so the audio ring is in a deterministic state. */
void ResetAudioBuffer(void)
{
   writePtr = 0;
}
