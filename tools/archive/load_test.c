/* Loads content into the built core from archives.
 *
 *    load_test <flycast_libretro.so> <workdir>
 *
 * <workdir> is what tools/archive/make_fixture.py wrote. The core is
 * loaded as a libretro frontend would, with the HLE BIOS and a render
 * context that is accepted but never used, and retro_load_game() is
 * called on the GD-ROM fixture as a plain .gdi, inside a stored zip, a
 * deflated zip, a zip with the image in a subdirectory, a solid 7z, and
 * as a member path the frontend hands over after browsing into an
 * archive (a track, then the image itself), then on a 7z holding no
 * disc image, which must be refused. Every
 * accepted load is unloaded again before the next, so the archive held
 * open for the members is released each time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include <compat/strl.h>

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(p)    LoadLibraryA(p)
#define LIB_SYM(h, s)  ((void*)GetProcAddress((HMODULE)(h), s))
#else
#include <dlfcn.h>
#define LIB_OPEN(p)    dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define LIB_SYM(h, s)  dlsym(h, s)
#endif

static char system_dir[512];
static struct retro_disk_control_ext_callback disk_control;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_INFO)
      return;
   va_start(ap, fmt);
   vfprintf(stdout, fmt, ap);
   va_end(ap);
}

static bool environ_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         var->value = NULL;
         if (!strcmp(var->key, "reicast_hle_bios"))
            var->value = "enabled";
         return var->value != NULL;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
         return true;   /* accepted, never reset: no frame is ever run */
      case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
         *(unsigned*)data = 1;
         return true;
      case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
         disk_control = *(const struct retro_disk_control_ext_callback*)data;
         return true;
      default:
         return false;
   }
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

/* dlsym() returns an object pointer; copy it into the function pointer
 * rather than cast, which ISO C does not allow. */
static void sym(void *lib, const char *name, void *fn)
{
   void *p = LIB_SYM(lib, name);
   memcpy(fn, &p, sizeof(p));
}

typedef void (*fn_set_environment)(retro_environment_t);
typedef void (*fn_set_video)(retro_video_refresh_t);
typedef void (*fn_set_audio)(retro_audio_sample_t);
typedef void (*fn_set_audio_batch)(retro_audio_sample_batch_t);
typedef void (*fn_set_input_poll)(retro_input_poll_t);
typedef void (*fn_set_input_state)(retro_input_state_t);
typedef void (*fn_void)(void);
typedef bool (*fn_load_game)(const struct retro_game_info*);

int main(int argc, char **argv)
{
   static const char *const loads[] =
   {
      "src/disc.gdi", "stored.zip", "deflate.zip", "subdir.zip", "solid.7z",
      /* the frontend naming a member: a track, and the image itself */
      "solid.7z#track03.bin", "stored.zip#disc.gdi"
   };
   void               *lib;
   fn_set_environment  set_environment_fn;
   fn_set_video        set_video_fn;
   fn_set_audio        set_audio_fn;
   fn_set_audio_batch  set_audio_batch_fn;
   fn_set_input_poll   set_input_poll_fn;
   fn_set_input_state  set_input_state_fn;
   fn_void             retro_init_fn, retro_deinit_fn, retro_unload_game_fn;
   fn_load_game        retro_load_game_fn;
   char          path[512];
   unsigned      i;
   int           failures = 0;

   if (argc != 3)
   {
      fprintf(stderr, "usage: load_test <core.so> <workdir>\n");
      return 2;
   }
   if (!(lib = LIB_OPEN(argv[1])))
   {
      fprintf(stderr, "cannot load %s\n", argv[1]);
      return 2;
   }
   strlcpy(system_dir, argv[2], sizeof(system_dir));

   sym(lib, "retro_set_environment", &set_environment_fn);
   sym(lib, "retro_set_video_refresh", &set_video_fn);
   sym(lib, "retro_set_audio_sample", &set_audio_fn);
   sym(lib, "retro_set_audio_sample_batch", &set_audio_batch_fn);
   sym(lib, "retro_set_input_poll", &set_input_poll_fn);
   sym(lib, "retro_set_input_state", &set_input_state_fn);
   sym(lib, "retro_init", &retro_init_fn);
   sym(lib, "retro_deinit", &retro_deinit_fn);
   sym(lib, "retro_load_game", &retro_load_game_fn);
   sym(lib, "retro_unload_game", &retro_unload_game_fn);

   set_environment_fn(environ_cb);
   set_video_fn(video_cb);
   set_audio_fn(audio_cb);
   set_audio_batch_fn(audio_batch_cb);
   set_input_poll_fn(input_poll_cb);
   set_input_state_fn(input_state_cb);

   retro_init_fn();

   for (i = 0; i < sizeof(loads) / sizeof(loads[0]); i++)
   {
      struct retro_game_info info;

      strlcpy(path, argv[2], sizeof(path));
      strlcat(path, "/", sizeof(path));
      strlcat(path, loads[i], sizeof(path));
      memset(&info, 0, sizeof(info));
      info.path = path;

      if (retro_load_game_fn(&info))
      {
         /* The disc list is this content's: image 0 must be the archive
          * or image just loaded, not a leftover of the previous load. */
         char        image[512];
         char        want[128];
         const char *base = strrchr(loads[i], '/');
         size_t      want_len;

         base     = base ? base + 1 : loads[i];
         want_len = strcspn(base, "#");
         if (want_len >= sizeof(want))
            want_len = sizeof(want) - 1;
         memcpy(want, base, want_len);
         want[want_len] = '\0';
         image[0]       = '\0';
         if (!disk_control.get_image_path
               || !disk_control.get_image_path(0, image, sizeof(image))
               || !strstr(image, want))
         {
            printf("FAIL: %s loaded but disc 0 is \"%s\"\n", loads[i], image);
            failures++;
         }
         else
            printf("loaded %s\n", loads[i]);
         retro_unload_game_fn();
      }
      else
      {
         printf("FAIL: %s did not load\n", loads[i]);
         failures++;
      }
   }

   /* no disc image inside: refused, and the core stays usable */
   {
      struct retro_game_info info;

      strlcpy(path, argv[2], sizeof(path));
      strlcat(path, "/single.7z", sizeof(path));
      memset(&info, 0, sizeof(info));
      info.path = path;
      if (retro_load_game_fn(&info))
      {
         printf("FAIL: single.7z loaded\n");
         failures++;
         retro_unload_game_fn();
      }
      else
         printf("refused single.7z\n");
   }

   retro_deinit_fn();

   printf(failures ? "load_test: %d FAIL\n" : "load_test: PASS\n", failures);
   return failures ? 1 : 0;
}
