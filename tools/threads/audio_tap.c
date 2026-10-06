/* A libretro core that is another libretro core with a tap on its sound.
 *
 * It loads the core named by $AUDIO_TAP_CORE, passes every call through to
 * it, and writes every sample that core sends the frontend to the file
 * named by $AUDIO_TAP_OUT as it goes by: signed 16-bit stereo, as sent.
 * What the frontend then does with the sound, or whether it has a sound
 * driver at all, makes no difference to what is written.
 *
 * tools/threads/live.sh builds this and has RetroArch load it in place of
 * the core, to check the sound the core makes.
 *
 *   cc -shared -fPIC -o audio_tap.so tools/threads/audio_tap.c -ldl
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../core/libretro-common/include/libretro.h"

static void *core;
static FILE *out;
static retro_audio_sample_t       frontend_sample;
static retro_audio_sample_batch_t frontend_batch;

static void *sym(const char *name)
{
   void *p;

   if (!core)
   {
      const char *path = getenv("AUDIO_TAP_CORE");
      const char *file = getenv("AUDIO_TAP_OUT");

      if (!path || !(core = dlopen(path, RTLD_NOW | RTLD_LOCAL)))
      {
         fprintf(stderr, "audio_tap: cannot load the core (AUDIO_TAP_CORE=%s): %s\n",
               path ? path : "", path ? dlerror() : "not set");
         abort();
      }
      if (!file || !(out = fopen(file, "wb")))
      {
         fprintf(stderr, "audio_tap: cannot write AUDIO_TAP_OUT=%s\n", file ? file : "");
         abort();
      }
   }
   if (!(p = dlsym(core, name)))
   {
      fprintf(stderr, "audio_tap: the core has no %s\n", name);
      abort();
   }
   return p;
}

static void tap_sample(int16_t left, int16_t right)
{
   int16_t frame[2];

   frame[0] = left;
   frame[1] = right;
   fwrite(frame, sizeof(frame), 1, out);
   if (frontend_sample)
      frontend_sample(left, right);
}

static size_t tap_batch(const int16_t *data, size_t frames)
{
   fwrite(data, 2 * sizeof(int16_t), frames, out);
   return frontend_batch ? frontend_batch(data, frames) : frames;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   frontend_sample = cb;
   ((void (*)(retro_audio_sample_t))sym("retro_set_audio_sample"))(tap_sample);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   frontend_batch = cb;
   ((void (*)(retro_audio_sample_batch_t))sym("retro_set_audio_sample_batch"))(tap_batch);
}

void retro_deinit(void)
{
   ((void (*)(void))sym("retro_deinit"))();
   if (out)
      fflush(out);
}

/* Everything else goes straight through. */
#define PASS0(ret, name) \
   ret name(void) { return ((ret (*)(void))sym(#name))(); }
#define PASS1(ret, name, t1) \
   ret name(t1 a) { return ((ret (*)(t1))sym(#name))(a); }
#define PASS2(ret, name, t1, t2) \
   ret name(t1 a, t2 b) { return ((ret (*)(t1, t2))sym(#name))(a, b); }
#define PASS3(ret, name, t1, t2, t3) \
   ret name(t1 a, t2 b, t3 c) { return ((ret (*)(t1, t2, t3))sym(#name))(a, b, c); }

PASS1(void, retro_set_environment, retro_environment_t)
PASS1(void, retro_set_video_refresh, retro_video_refresh_t)
PASS1(void, retro_set_input_poll, retro_input_poll_t)
PASS1(void, retro_set_input_state, retro_input_state_t)
PASS0(void, retro_init)
PASS0(unsigned, retro_api_version)
PASS1(void, retro_get_system_info, struct retro_system_info *)
PASS1(void, retro_get_system_av_info, struct retro_system_av_info *)
PASS2(void, retro_set_controller_port_device, unsigned, unsigned)
PASS0(void, retro_reset)
PASS0(void, retro_run)
PASS0(size_t, retro_serialize_size)
PASS2(bool, retro_serialize, void *, size_t)
PASS2(bool, retro_unserialize, const void *, size_t)
PASS0(void, retro_cheat_reset)
PASS3(void, retro_cheat_set, unsigned, bool, const char *)
PASS1(bool, retro_load_game, const struct retro_game_info *)
PASS3(bool, retro_load_game_special, unsigned, const struct retro_game_info *, size_t)
PASS0(void, retro_unload_game)
PASS0(unsigned, retro_get_region)
PASS1(void *, retro_get_memory_data, unsigned)
PASS1(size_t, retro_get_memory_size, unsigned)
