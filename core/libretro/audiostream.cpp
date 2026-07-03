#include "types.h"
#include "emulator.h"

#include <libretro.h>

#define SAMPLE_COUNT 512

extern retro_audio_sample_batch_t audio_batch_cb;

/* Ring position and pending samples are file-scope so the batch boundary can be
 * reset deterministically on state load / reset. If writePtr survives a load,
 * two states saved at different intra-batch offsets replay a different partial
 * batch, so identical inputs no longer produce identical audio output — which
 * breaks netplay and runahead determinism. */
static SoundFrame Buffer[SAMPLE_COUNT];
static u32 writePtr; /* next sample index */

void WriteSample(s16 r, s16 l)
{
   Buffer[writePtr].r = r;
   Buffer[writePtr].l = l;

   if (++writePtr == SAMPLE_COUNT)
   {
      if ( dc_is_running() && (!settings.rend.ThreadedRendering || settings.aica.LimitFPS) )
         audio_batch_cb((const int16_t*)Buffer, SAMPLE_COUNT);
      writePtr = 0;
   }
}

/* Drop any partially-filled batch and rewind to a known boundary. Called after
 * (un)serialize and reset so the audio ring is in a deterministic state. */
void ResetAudioBuffer(void)
{
   writePtr = 0;
}
