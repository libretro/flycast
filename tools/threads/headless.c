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
 *   HEADLESS_STAGE   address of bench_prog.c's stage word: time its kernels
 *   HEADLESS_RESET   frame to press reset before
 *   HEADLESS_SWAP    frame to open the drive's lid before; it is shut 20 frames on
 *   HEADLESS_SAVE    frame to save a state before
 *   HEADLESS_LOAD    frame to load that state back before
 *   HEADLESS_DUMP    "frame:file": a state saved before that frame, written to the file
 *   HEADLESS_OPTION  "frame:key=value[,key=value...]": core options changed before
 *                    that frame, as from the frontend's menu while the game runs.
 *                    Whatever size the core then tells the frontend is printed,
 *                    "size WxH".
 *   HEADLESS_OPTION2 the same, for a second change at another frame
 *
 * It has to be started with the do-nothing OpenGL library preloaded:
 * headless.sh does all of it.
 */
#define _GNU_SOURCE
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../core/libretro-common/include/libretro.h"

static void *core;
static struct retro_hw_render_callback hw;
static int have_hw;
static FILE *sound;
static char **options;
static int option_count;
static int gles;

#define SYM(ret, name, args) ret (*name) args = (ret (*) args)core_symbol(core, #name); \
   if (!name) { fprintf(stderr, "headless: no %s in the core\n", #name); return 1; }

/* ---- an OpenGL that does nothing ----
 *
 * The functions a core calls by name come from a library written by
 * headless_gl.py and loaded ahead of everything else (headless.sh); the
 * ones it asks the frontend for are looked up in the same library, and
 * anything that is not there does nothing and returns 0. */

static long gl_nothing(void) { return 0; }

#ifdef _WIN32
/* On Windows the library is an opengl32.dll put beside this program, which
 * the core's own imports find before the system's (under wine, with
 * WINEDLLOVERRIDES=opengl32=n). */
static void *core_open(const char *path)
{
   return (void *)LoadLibraryA(path);
}

static void *core_symbol(void *lib, const char *name)
{
   return (void *)GetProcAddress((HMODULE)lib, name);
}

static const char *core_error(void)
{
   static char text[64];
   snprintf(text, sizeof(text), "LoadLibrary failed, error %lu", (unsigned long)GetLastError());
   return text;
}

static retro_proc_address_t gl_proc(const char *name)
{
   HMODULE gl = GetModuleHandleA("opengl32.dll");
   void *fn = gl ? (void *)GetProcAddress(gl, name) : NULL;
   return fn ? (retro_proc_address_t)fn : (retro_proc_address_t)gl_nothing;
}
#else
static void *core_open(const char *path)
{
   return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

static void *core_symbol(void *lib, const char *name)
{
   return dlsym(lib, name);
}

static const char *core_error(void)
{
   return dlerror();
}

static retro_proc_address_t gl_proc(const char *name)
{
   void *fn = dlsym(RTLD_DEFAULT, name);
   return fn ? (retro_proc_address_t)fn : (retro_proc_address_t)gl_nothing;
}
#endif

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

/* The core's disc tray, for HEADLESS_SWAP */
static struct retro_disk_control_callback disk;
static int have_disk;

static char changed_text[2][512];    /* HEADLESS_OPTION and HEADLESS_OPTION2, from their frames on */
static const char *changed_options[16];
static int changed_count;
static int option_changed;

static void change_options(const char *name, int which, int frame)
{
   const char *env = getenv(name);
   char *next;

   if (!env || frame != atoi(env) || !strchr(env, ':'))
      return;
   snprintf(changed_text[which], sizeof(changed_text[which]), "%s", strchr(env, ':') + 1);
   for (next = changed_text[which]; next && changed_count < 16; )
   {
      changed_options[changed_count++] = next;
      next = strchr(next, ',');
      if (next)
         *next++ = '\0';
   }
   option_changed = 1;
}
static unsigned told_width, told_height;

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
         /* (the latest change of an option is the one in force) */
         for (i = changed_count - 1; i >= 0; i--)
            if (!strncmp(changed_options[i], var->key, len) && changed_options[i][len] == '=')
            {
               var->value = changed_options[i] + len + 1;
               return true;
            }
         for (i = 0; i < option_count; i++)
            if (!strncmp(options[i], var->key, len) && options[i][len] == '=')
            {
               var->value = options[i] + len + 1;
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
         disk = *(const struct retro_disk_control_callback *)data;
         have_disk = 1;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = option_changed != 0;
         option_changed = 0;
         return true;
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
         told_width  = ((const struct retro_system_av_info *)data)->geometry.base_width;
         told_height = ((const struct retro_system_av_info *)data)->geometry.base_height;
         return true;
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
         told_width  = ((const struct retro_game_geometry *)data)->base_width;
         told_height = ((const struct retro_game_geometry *)data)->base_height;
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

   core = core_open(argv[1]);
   if (!core)
   {
      fprintf(stderr, "headless: %s\n", core_error());
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
      SYM(void, retro_get_system_av_info, (struct retro_system_av_info *))
      SYM(void, retro_run, (void))
      SYM(void, retro_reset, (void))
      SYM(size_t, retro_serialize_size, (void))
      SYM(bool, retro_serialize, (void *, size_t))
      SYM(bool, retro_unserialize, (const void *, size_t))
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
      /* As every frontend does once the game is loaded: the core works out
       * its size here, and which of its cheats the game has. */
      {
         struct retro_system_av_info av_info;
         memset(&av_info, 0, sizeof(av_info));
         retro_get_system_av_info(&av_info);
      }
      if (have_hw && hw.context_reset)
         hw.context_reset();

      {
         /* HEADLESS_STAGE: a word the program counts its stages in, as
          * bench_prog.c does. Each change is reported with the processor
          * time used so far; at 0x600DBE4C the run is over and the words
          * after it are printed. */
         unsigned long stage_at = getenv("HEADLESS_STAGE")
            ? strtoul(getenv("HEADLESS_STAGE"), NULL, 0) : 0;
         uint32_t last = 0, word = 0;
         int reset_at = getenv("HEADLESS_RESET") ? atoi(getenv("HEADLESS_RESET")) : -1;
         int swap_at = getenv("HEADLESS_SWAP") ? atoi(getenv("HEADLESS_SWAP")) : -1;
         int save_at = getenv("HEADLESS_SAVE") ? atoi(getenv("HEADLESS_SAVE")) : -1;
         int load_at = getenv("HEADLESS_LOAD") ? atoi(getenv("HEADLESS_LOAD")) : -1;
         void *state = NULL;
         size_t state_size = 0;
         clock_t t0 = clock();

         for (i = 0; i < frames; i++)
         {
            uint8_t *ram;
            size_t size;

            if (i == reset_at)
               retro_reset();
            if (i == save_at)
            {
               state_size = retro_serialize_size();
               state = malloc(state_size);
               if (!state || !retro_serialize(state, state_size))
               {
                  fprintf(stderr, "the state could not be saved\n");
                  return 1;
               }
            }
            if (i == load_at && state && !retro_unserialize(state, state_size))
            {
               fprintf(stderr, "the state could not be loaded\n");
               return 1;
            }
            change_options("HEADLESS_OPTION", 0, i);
            change_options("HEADLESS_OPTION2", 1, i);
            if (getenv("HEADLESS_DUMP") && i == atoi(getenv("HEADLESS_DUMP")) && strchr(getenv("HEADLESS_DUMP"), ':'))
            {
               size_t dump_size = retro_serialize_size();
               void *dump = malloc(dump_size);
               FILE *out = fopen(strchr(getenv("HEADLESS_DUMP"), ':') + 1, "wb");

               if (dump && out && retro_serialize(dump, dump_size))
                  fwrite(dump, 1, dump_size, out);
               if (out)
                  fclose(out);
               free(dump);
            }
            /* the lid opened, and shut again on the same disc 20 frames later */
            if (have_disk && i == swap_at)
               disk.set_eject_state(true);
            if (have_disk && swap_at >= 0 && i == swap_at + 20)
               disk.set_eject_state(false);
            retro_run();
            if (!stage_at)
               continue;
            ram = (uint8_t *)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
            if (!ram || size < 64)
               continue;
            memcpy(&word, ram + (stage_at & (size - 1)), 4);
            if (word == last)
               continue;
            last = word;
            printf("stage %08x frame %d cpu %.3f\n", (unsigned)word, i,
                  (double)(clock() - t0) / CLOCKS_PER_SEC);
            if (word == 0x600DBE4C)
            {
               int k;
               for (k = 1; k <= 13; k++)
               {
                  memcpy(&word, ram + ((stage_at + 4 * k) & (size - 1)), 4);
                  printf("result %d = %08x\n", k, (unsigned)word);
               }
               break;
            }
         }
      }

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
         if (told_width != 0)
            printf("size %ux%u\n", told_width, told_height);
         printf("peek %08lx = %08x\n", addr, (unsigned)word);
      }
      fflush(stdout);
      /* No retro_deinit: the test is over, and a core's shutdown under an
       * OpenGL that does nothing is not what is being tested. */
      _exit(0);
   }
}
