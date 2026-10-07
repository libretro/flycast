/* A libretro frontend with no screen: it loads a core, gives it an OpenGL
 * whose every function does nothing, runs some frames of a content file,
 * writes the sound to a file and prints a word of the machine's memory.
 *
 * It is here so that the test disc can be run where there is no display
 * and no RetroArch - above all a core built for another processor, under
 * qemu's user-mode emulation, which is the only way the ARM recompilers
 * get run on an x86-64 machine. Nothing is drawn, so what is checked is
 * what the disc's program says of itself (live_verdict, in live_prog.c)
 * and the sound (live_audio.py). See headless.sh.
 *
 *   headless CORE CONTENT FRAMES [key=value ...]
 *
 * key=value pairs are core options. The environment:
 *   HEADLESS_SOUND   file to write the sound to, as audio_tap.c does
 *   HEADLESS_PEEK    address in main memory (0x8c......) of a 32-bit word
 *                    to print after the last frame
 *   HEADLESS_GLES    set for a core built for OpenGL ES
 *
 * It has to be started with the do-nothing OpenGL library preloaded:
 * headless.sh does all of it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/libretro-common/include/libretro.h"

static void *core;
static struct retro_hw_render_callback hw;
static int have_hw;
static FILE *sound;
static char **options;
static int option_count;
static int gles;

#define SYM(ret, name, args) ret (*name) args = (ret (*) args)dlsym(core, #name); \
   if (!name) { fprintf(stderr, "headless: no %s in the core\n", #name); return 1; }

/* ---- an OpenGL that does nothing ----
 *
 * The functions a core calls by name come from a library written by
 * headless_gl.py and loaded ahead of everything else (headless.sh); the
 * ones it asks the frontend for are looked up in the same library, and
 * anything that is not there does nothing and returns 0. */

static long gl_nothing(void) { return 0; }

static retro_proc_address_t gl_proc(const char *name)
{
   void *fn = dlsym(RTLD_DEFAULT, name);
   return fn ? (retro_proc_address_t)fn : (retro_proc_address_t)gl_nothing;
}

static uintptr_t gl_framebuffer(void) { return 0; }

/* ---- the frontend ---- */

static void log_line(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   (void)level;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static bool environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_line;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = getenv("HEADLESS_DIR") ? getenv("HEADLESS_DIR") : "/tmp";
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = gles ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (cb->context_type == RETRO_HW_CONTEXT_VULKAN)
            return false;
         cb->get_current_framebuffer = gl_framebuffer;
         cb->get_proc_address = gl_proc;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         size_t len = strlen(var->key);
         int i;
         for (i = 0; i < option_count; i++)
            if (!strncmp(options[i], var->key, len) && options[i][len] == '=')
            {
               var->value = options[i] + len + 1;
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      default:
         return false;
   }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
   (void)data; (void)width; (void)height; (void)pitch;
}

static void audio_sample(int16_t left, int16_t right)
{
   int16_t frame[2];
   frame[0] = left;
   frame[1] = right;
   if (sound)
      fwrite(frame, sizeof(frame), 1, sound);
}

static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
   if (sound)
      fwrite(data, 2 * sizeof(int16_t), frames, sound);
   return frames;
}

static void input_poll(void) { }

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   (void)port; (void)device; (void)index; (void)id;
   return 0;
}

int main(int argc, char **argv)
{
   struct retro_game_info game;
   int frames, i;

   if (argc < 4)
   {
      fprintf(stderr, "usage: %s CORE CONTENT FRAMES [key=value ...]\n", argv[0]);
      return 2;
   }
   options = argv + 4;
   option_count = argc - 4;
   frames = atoi(argv[3]);
   gles = getenv("HEADLESS_GLES") != NULL;

   core = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
   if (!core)
   {
      fprintf(stderr, "headless: %s\n", dlerror());
      return 1;
   }
   {
      SYM(void, retro_set_environment, (retro_environment_t))
      SYM(void, retro_set_video_refresh, (retro_video_refresh_t))
      SYM(void, retro_set_audio_sample, (retro_audio_sample_t))
      SYM(void, retro_set_audio_sample_batch, (retro_audio_sample_batch_t))
      SYM(void, retro_set_input_poll, (retro_input_poll_t))
      SYM(void, retro_set_input_state, (retro_input_state_t))
      SYM(void, retro_init, (void))
      SYM(bool, retro_load_game, (const struct retro_game_info *))
      SYM(void, retro_run, (void))
      SYM(void *, retro_get_memory_data, (unsigned))
      SYM(size_t, retro_get_memory_size, (unsigned))

      if (getenv("HEADLESS_SOUND"))
         sound = fopen(getenv("HEADLESS_SOUND"), "wb");

      retro_set_environment(environment);
      retro_set_video_refresh(video_refresh);
      retro_set_audio_sample(audio_sample);
      retro_set_audio_sample_batch(audio_sample_batch);
      retro_set_input_poll(input_poll);
      retro_set_input_state(input_state);
      retro_init();

      memset(&game, 0, sizeof(game));
      game.path = argv[2];
      if (!retro_load_game(&game))
      {
         fprintf(stderr, "headless: the core would not load %s\n", argv[2]);
         return 1;
      }
      if (have_hw && hw.context_reset)
         hw.context_reset();

      for (i = 0; i < frames; i++)
         retro_run();

      if (sound)
         fclose(sound);
      if (getenv("HEADLESS_PEEK"))
      {
         unsigned long addr = strtoul(getenv("HEADLESS_PEEK"), NULL, 0);
         uint8_t *ram = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
         size_t size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
         uint32_t word;
         if (!ram || (addr & (size - 1)) + 4 > size)
         {
            fprintf(stderr, "headless: no main memory to look at\n");
            return 1;
         }
         memcpy(&word, ram + (addr & (size - 1)), 4);
         printf("peek %08lx = %08x\n", addr, (unsigned)word);
      }
      fflush(stdout);
      /* No retro_deinit: the test is over, and a core's shutdown under an
       * OpenGL that does nothing is not what is being tested. */
      _exit(0);
   }
}
