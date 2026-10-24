/* Is the core deterministic? A libretro frontend that runs a game from a
 * state with inputs of its own making, the same each time, and writes down
 * after every frame what the machine is - the state, part by part - and
 * what came out of it, the frame's sound and its picture. Two runs that
 * should be the same are then held against each other, and the first frame
 * they part at is named, and the parts of the state that differ there.
 *
 * What netplay needs of a core is all of that: a state and the inputs after
 * it make the same frames, on another machine too, and a state saved and
 * loaded again anywhere along the way changes nothing. See detcheck.sh,
 * which runs the ways below and holds their traces against each other.
 *
 *   detcheck CORE CONTENT FRAMES [key=value ...]     a run; key=value are core options
 *   detcheck --compare A.trace B.trace [A.state B.state]
 *               (with DET_NO_PICTURE set, the pictures are left out)
 *
 * The environment, for a run:
 *   DET_TRACE   file to write the trace to: the state's parts' hashes, and
 *               the sound's and the picture's, after each frame
 *   DET_STATE   a state to load before the first frame (after DET_WARM
 *               frames of the content as it starts, default 1)
 *   DET_SEED    the inputs: a number they are made from (default 1)
 *   DET_MODE    plain       the frames one after another
 *               reload:K    a state saved after every K-th frame, and loaded
 *                           straight back
 *               ahead       as a frontend's run-ahead of one frame: each
 *                           frame run, a state saved, the frame after it
 *                           run, the state loaded. The frame after it is
 *                           then run again from the state loaded, and has
 *                           to be what it was the first time - state,
 *                           sound and picture
 *               rollback:D  as netplay rolls back: every D frames, the state
 *                           of D frames ago loaded and the D frames run
 *                           again, which have to be what they were
 *   DET_DUMP    "frame:file": the state after that frame, to the file
 *   DET_REF     a trace to compare with as the run goes: at the first frame
 *               that is not the same, the state goes to DET_DUMP's file
 *               (if DET_DUMP has no frame in it) and the run ends, status 3
 *   DET_SYMS    where the core keeps the table of its state's parts, from
 *               nm: "base=ADDR parts=ADDR at=ADDR name=ADDR", base being
 *               retro_init's (see detcheck.sh). Without it the state is one
 *               part.
 *   DET_GL      "egl": a real OpenGL, through EGL with no display, and the
 *               picture read back and hashed (built with -DDET_EGL).
 *               Otherwise the core is given an OpenGL that does nothing,
 *               which detcheck.sh puts in front of the system's.
 *   DET_DIR     the system and save directory (default /tmp)
 *   DET_HIDE    "all": the core is told the frontend shows none of the
 *               frames (GET_AUDIO_VIDEO_ENABLE, as for fast-forward's
 *               frameskip); "ahead": with DET_MODE ahead, it is told so for
 *               the frames that are kept and not for the frames run ahead,
 *               as RetroArch's run-ahead has it. A frame not shown has to
 *               leave the machine as one shown does. The pictures are not
 *               compared (a frame not shown has none).
 *   DET_GLES    set for a core built for OpenGL ES (with the OpenGL that
 *               does nothing)
 *
 * Every 50 frames the state is also saved twice over, into memory filled
 * with 0x00 and with 0xff: a byte that comes out different is one the core
 * does not write, or writes from memory it never set. Status 0 when all was
 * the same, 3 when a comparison failed, 1 when the run did not go.
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

#ifdef DET_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#endif

/* ---- hashing ---- */

static uint64_t hash_bytes(const void *data, size_t size, uint64_t h)
{
   const unsigned char *p = (const unsigned char *)data;
   const uint64_t m = 0x9e3779b97f4a7c15ull;
   while (size >= 8)
   {
      uint64_t w;
      memcpy(&w, p, 8);
      h = (h ^ (w * m)) * 0xff51afd7ed558ccdull;
      h ^= h >> 29;
      p += 8;
      size -= 8;
   }
   while (size--)
      h = (h ^ *p++) * 0x100000001b3ull;
   return h ^ (h >> 32);
}

/* ---- the state's parts ----
 *
 * dc_serialize() keeps where each part began in the state it wrote last
 * (core/serialize.cpp). Each part is cut into units of at most 64 KB, and
 * the trace has a hash of each unit: a difference in main memory is then
 * found to the 64 KB it is in, from the traces alone. */

#define UNIT_MAX 65536
#define PARTS_MAX 1024

struct part { char name[64]; uint32_t at, size; };
static struct part parts[PARTS_MAX];
static unsigned part_count;
struct unit { uint32_t at, size, part; };
static struct unit *units;
static unsigned unit_count;

static uint32_t *core_parts, *core_part_at;
static const char **core_part_name;

static void syms_read(void *core)
{
   const char *env = getenv("DET_SYMS");
   unsigned long base = 0, n = 0, at = 0, name = 0;
   const char *p;
   char *here;

   if (!env)
      return;
   if ((p = strstr(env, "base="))) base = strtoul(p + 5, NULL, 16);
   if ((p = strstr(env, "parts="))) n = strtoul(p + 6, NULL, 16);
   if ((p = strstr(env, "at="))) at = strtoul(p + 3, NULL, 16);
   if ((p = strstr(env, "name="))) name = strtoul(p + 5, NULL, 16);
   here = (char *)dlsym(core, "retro_init");
   if (!here || !base || !n || !at || !name)
      return;
   core_parts = (uint32_t *)(here - base + n);
   core_part_at = (uint32_t *)(here - base + at);
   core_part_name = (const char **)(here - base + name);
}

/* the parts as the core has them now, and the units made of them */
static void parts_make(size_t size)
{
   unsigned i, n = 0;

   part_count = 0;
   if (core_parts && *core_parts > 0 && *core_parts <= PARTS_MAX)
      n = *core_parts;
   for (i = 0; i < n; i++)
   {
      if (core_part_at[i] > size)
         break;
      snprintf(parts[part_count].name, sizeof(parts[0].name), "%s", core_part_name[i] ? core_part_name[i] : "?");
      parts[part_count].at = core_part_at[i];
      part_count++;
   }
   if (part_count == 0 || parts[0].at != 0)
   {
      memmove(parts + 1, parts, sizeof(parts[0]) * (part_count < PARTS_MAX - 1 ? part_count : PARTS_MAX - 1));
      snprintf(parts[0].name, sizeof(parts[0].name), "%s", part_count ? "(before the first part)" : "state");
      parts[0].at = 0;
      if (part_count < PARTS_MAX - 1)
         part_count++;
   }
   /* (and what the frontend's room has past the end of what was written) */
   for (i = 0; i < part_count; i++)
      parts[i].size = (i + 1 < part_count ? parts[i + 1].at : (uint32_t)size) - parts[i].at;

   free(units);
   unit_count = 0;
   for (i = 0; i < part_count; i++)
      unit_count += parts[i].size ? (parts[i].size + UNIT_MAX - 1) / UNIT_MAX : 1;
   units = (struct unit *)calloc(unit_count, sizeof(*units));
   unit_count = 0;
   for (i = 0; i < part_count; i++)
   {
      uint32_t off = 0;
      do
      {
         uint32_t len = parts[i].size - off;
         if (len > UNIT_MAX)
            len = UNIT_MAX;
         units[unit_count].at = parts[i].at + off;
         units[unit_count].size = len;
         units[unit_count].part = i;
         unit_count++;
         off += len;
      } while (off < parts[i].size);
   }
}

/* The parts two states differ in, and where, as far as @limit lines. */
static void parts_diff(const unsigned char *a, const unsigned char *b, size_t size,
      const char *name_a, const char *name_b)
{
   unsigned i, lines = 0;
   size_t total = 0;

   for (i = 0; i < part_count; i++)
   {
      uint32_t k, end = parts[i].at + parts[i].size, first = 0, count = 0;
      if (parts[i].at >= size)
         break;
      if (end > size)
         end = (uint32_t)size;
      for (k = parts[i].at; k < end; k++)
         if (a[k] != b[k])
         {
            if (!count)
               first = k;
            count++;
         }
      if (!count)
         continue;
      total += count;
      if (lines++ >= 40)
         continue;
      printf("  part %-36s (%u bytes at %u): %u bytes differ, the first at +%u",
            parts[i].name, parts[i].size, parts[i].at, count, first - parts[i].at);
      /* the first few runs of them, byte by byte */
      {
         uint32_t k2 = first, shown = 0;
         printf("\n");
         while (k2 < end && shown < 3)
         {
            uint32_t from = k2, j;
            while (k2 < end && a[k2] != b[k2] && k2 - from < 16)
               k2++;
            printf("    +%-8u %s:", from - parts[i].at, name_a);
            for (j = from; j < k2; j++)
               printf(" %02x", a[j]);
            printf("   %s:", name_b);
            for (j = from; j < k2; j++)
               printf(" %02x", b[j]);
            printf("\n");
            shown++;
            while (k2 < end && a[k2] == b[k2])
               k2++;
         }
      }
   }
   if (lines > 40)
      printf("  ... and %u parts more\n", lines - 40);
   printf("  %lu bytes differ in all\n", (unsigned long)total);
}

/* ---- the trace ----
 *
 * "DETTRAC1", the state's size, the parts (offset, size, a name of 64
 * bytes), the units; then for each frame: its number, the sound's hash, the
 * picture's hash and size, and each unit's hash. */

struct frame_rec { uint32_t frame, video_w, video_h, pad; uint64_t audio, video; };

static FILE *trace_out;

static void trace_head(FILE *f, size_t size)
{
   uint32_t v;
   unsigned i;
   fwrite("DETTRAC1", 1, 8, f);
   v = (uint32_t)size; fwrite(&v, 4, 1, f);
   v = part_count; fwrite(&v, 4, 1, f);
   for (i = 0; i < part_count; i++)
   {
      fwrite(&parts[i].at, 4, 1, f);
      fwrite(&parts[i].size, 4, 1, f);
      fwrite(parts[i].name, 1, sizeof(parts[i].name), f);
   }
   v = unit_count; fwrite(&v, 4, 1, f);
   for (i = 0; i < unit_count; i++)
      fwrite(&units[i], sizeof(units[i]), 1, f);
}

struct trace
{
   uint32_t size, nparts, nunits;
   struct part *parts;
   struct unit *units;
   FILE *f;
};

static int trace_open(struct trace *t, const char *path)
{
   char magic[8];
   unsigned i;
   memset(t, 0, sizeof(*t));
   t->f = fopen(path, "rb");
   if (!t->f || fread(magic, 1, 8, t->f) != 8 || memcmp(magic, "DETTRAC1", 8)
         || fread(&t->size, 4, 1, t->f) != 1 || fread(&t->nparts, 4, 1, t->f) != 1 || t->nparts > PARTS_MAX)
   {
      fprintf(stderr, "detcheck: %s is not a trace\n", path);
      return 0;
   }
   t->parts = (struct part *)calloc(t->nparts, sizeof(struct part));
   for (i = 0; i < t->nparts; i++)
      if (fread(&t->parts[i].at, 4, 1, t->f) != 1 || fread(&t->parts[i].size, 4, 1, t->f) != 1
            || fread(t->parts[i].name, 1, 64, t->f) != 64)
         return 0;
   if (fread(&t->nunits, 4, 1, t->f) != 1 || t->nunits > (1u << 20))
      return 0;
   t->units = (struct unit *)calloc(t->nunits, sizeof(struct unit));
   if (fread(t->units, sizeof(struct unit), t->nunits, t->f) != t->nunits)
      return 0;
   return 1;
}

static int trace_frame(struct trace *t, struct frame_rec *rec, uint64_t *hashes)
{
   return fread(rec, sizeof(*rec), 1, t->f) == 1
      && fread(hashes, 8, t->nunits, t->f) == t->nunits;
}

/* (--compare with DET_NO_PICTURE set: the pictures are not compared, as
 * between a run that draws them and one that does not) */
static int no_picture;

/* The two frames' records: 0 if the same, else what differs is printed. */
static int frames_differ(const struct frame_rec *ra, const uint64_t *ha, const struct frame_rec *rb,
      const uint64_t *hb, const struct part *pt, const struct unit *ut, unsigned nunits, int print)
{
   unsigned i, n = 0, lines = 0;
   int last_part = -1;
   int state = 0;

   for (i = 0; i < nunits; i++)
      if (ha[i] != hb[i])
         state = 1;
   if (!state && ra->audio == rb->audio && (ra->video == rb->video || no_picture))
      return 0;
   if (!print)
      return 1;
   if (ra->audio != rb->audio)
      printf("  the frame's sound differs\n");
   if (ra->video != rb->video && !no_picture)
      printf("  the frame's picture differs (%ux%u, %ux%u)\n", ra->video_w, ra->video_h, rb->video_w, rb->video_h);
   for (i = 0; i < nunits; i++)
   {
      if (ha[i] == hb[i])
         continue;
      n++;
      if ((int)ut[i].part != last_part && lines++ < 40)
      {
         const struct part *p = &pt[ut[i].part];
         printf("  state part %-36s (%u bytes at %u) differs", p->name, p->size, p->at);
         if (p->size > UNIT_MAX)
            printf(", from the 64 KB at +%u", ut[i].at - p->at);
         printf("\n");
      }
      last_part = (int)ut[i].part;
   }
   return 1;
}

static long file_read(const char *path, unsigned char **out)
{
   FILE *f = fopen(path, "rb");
   long size = -1;
   *out = NULL;
   if (f && !fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET)
         && (*out = (unsigned char *)malloc(size)) && fread(*out, 1, size, f) == (size_t)size)
   {
      fclose(f);
      return size;
   }
   if (f)
      fclose(f);
   free(*out);
   *out = NULL;
   return -1;
}

static int compare_main(int argc, char **argv)
{
   struct trace a, b;
   struct frame_rec ra, rb;
   uint64_t *ha, *hb;
   long frames = 0;
   int same_layout;

   if (argc < 4)
   {
      fprintf(stderr, "usage: %s --compare A.trace B.trace [A.state B.state]\n", argv[0]);
      return 2;
   }
   no_picture = getenv("DET_NO_PICTURE") != NULL;
   if (!trace_open(&a, argv[2]) || !trace_open(&b, argv[3]))
      return 1;
   same_layout = a.nunits == b.nunits && a.size == b.size
      && !memcmp(a.units, b.units, sizeof(struct unit) * a.nunits);
   if (!same_layout)
   {
      printf("the two states are laid out differently: %u and %u bytes, %u and %u parts\n",
            a.size, b.size, a.nparts, b.nparts);
      return 3;
   }
   ha = (uint64_t *)malloc(8 * (size_t)a.nunits);
   hb = (uint64_t *)malloc(8 * (size_t)b.nunits);
   for (;;)
   {
      int ga = trace_frame(&a, &ra, ha), gb = trace_frame(&b, &rb, hb);
      if (!ga || !gb)
      {
         if (ga != gb)
            printf("the same for %ld frames, where the shorter trace ends\n", frames);
         else
            printf("the same, all %ld frames\n", frames);
         return 0;
      }
      if (ra.frame != rb.frame)
      {
         printf("frame %u against frame %u: the traces are not of the same frames\n", ra.frame, rb.frame);
         return 3;
      }
      if (frames_differ(&ra, ha, &rb, hb, a.parts, a.units, a.nunits, 0))
      {
         printf("the first difference is after frame %u (%ld frames the same before it):\n", ra.frame, frames);
         frames_differ(&ra, ha, &rb, hb, a.parts, a.units, a.nunits, 1);
         if (argc >= 6)
         {
            unsigned char *sa, *sb;
            long za = file_read(argv[4], &sa), zb = file_read(argv[5], &sb);
            if (za > 0 && za == zb)
            {
               memcpy(parts, a.parts, sizeof(struct part) * a.nparts);
               part_count = a.nparts;
               printf(" byte by byte, from the states after that frame:\n");
               parts_diff(sa, sb, (size_t)za, "A", "B");
            }
         }
         return 3;
      }
      frames++;
   }
}

/* ---- OpenGL ---- */

static struct retro_hw_render_callback hw;
static int have_hw;
static int real_gl;

static long gl_nothing(void) { return 0; }

#ifdef DET_EGL
static EGLDisplay egl_display;
static EGLContext egl_context;
static GLuint fbo, fbo_color, fbo_depth;
#define FBO_W 2560
#define FBO_H 1920
static unsigned char *pixels;

static int egl_start(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
   static const EGLint context_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
   EGLConfig config = NULL;
   EGLint major, minor, n = 0;

   if (!get_display)
      return 0;
   egl_display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!egl_display || !eglInitialize(egl_display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
      return 0;
   eglChooseConfig(egl_display, config_attribs, &config, 1, &n);
   egl_context = eglCreateContext(egl_display, n ? config : NULL, EGL_NO_CONTEXT, context_attribs);
   if (!egl_context || !eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context))
      return 0;
   {
      PFNGLGENFRAMEBUFFERSPROC gen_fb = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
      PFNGLBINDFRAMEBUFFERPROC bind_fb = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
      PFNGLGENRENDERBUFFERSPROC gen_rb = (PFNGLGENRENDERBUFFERSPROC)eglGetProcAddress("glGenRenderbuffers");
      PFNGLBINDRENDERBUFFERPROC bind_rb = (PFNGLBINDRENDERBUFFERPROC)eglGetProcAddress("glBindRenderbuffer");
      PFNGLRENDERBUFFERSTORAGEPROC storage = (PFNGLRENDERBUFFERSTORAGEPROC)eglGetProcAddress("glRenderbufferStorage");
      PFNGLFRAMEBUFFERRENDERBUFFERPROC attach = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)eglGetProcAddress("glFramebufferRenderbuffer");
      gen_fb(1, &fbo);
      bind_fb(GL_FRAMEBUFFER, fbo);
      gen_rb(1, &fbo_color);
      bind_rb(GL_RENDERBUFFER, fbo_color);
      storage(GL_RENDERBUFFER, GL_RGBA8, FBO_W, FBO_H);
      attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fbo_color);
      gen_rb(1, &fbo_depth);
      bind_rb(GL_RENDERBUFFER, fbo_depth);
      storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, FBO_W, FBO_H);
      attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
   }
   pixels = (unsigned char *)malloc((size_t)FBO_W * FBO_H * 4);
   fprintf(stderr, "detcheck: OpenGL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   return 1;
}

static uint64_t egl_picture(unsigned w, unsigned h)
{
   PFNGLBINDFRAMEBUFFERPROC bind_fb = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
   if (w > FBO_W || h > FBO_H)
      return 0;
   bind_fb(GL_READ_FRAMEBUFFER, fbo);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   return hash_bytes(pixels, (size_t)w * h * 4, 0x5bd1e995);
}
#endif

static retro_proc_address_t gl_proc(const char *name)
{
   void *fn;
#ifdef DET_EGL
   if (real_gl)
   {
      fn = (void *)eglGetProcAddress(name);
      return fn ? (retro_proc_address_t)fn : NULL;
   }
#endif
   fn = dlsym(RTLD_DEFAULT, name);
   return fn ? (retro_proc_address_t)fn : (retro_proc_address_t)gl_nothing;
}

static uintptr_t gl_framebuffer(void)
{
#ifdef DET_EGL
   return real_gl ? fbo : 0;
#else
   return 0;
#endif
}

/* ---- the frontend ---- */

static char **options;
static int option_count;
static uint64_t audio_hash, video_hash;
static unsigned video_w, video_h;
static int video_new;
static uint32_t seed = 1;
static int frame_now;

static void log_line(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("DET_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static int video_on = 1;   /* DET_HIDE: whether the frame being run is shown */

static bool environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
         if (data)
            *(int *)data = video_on ? 3 : 2;   /* audio always, video when shown */
         return true;
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
         *(const char **)data = getenv("DET_DIR") ? getenv("DET_DIR") : "/tmp";
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = getenv("DET_GLES") ? RETRO_HW_CONTEXT_OPENGLES3 : RETRO_HW_CONTEXT_OPENGL_CORE;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (cb->context_type == RETRO_HW_CONTEXT_VULKAN || cb->context_type >= RETRO_HW_CONTEXT_D3D11
               || (real_gl && cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL))
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
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
         return true;
      default:
         return false;
   }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
   uint64_t h = 0x2545f4914f6cdd1dull;
   unsigned y;

   if (!data)
      return;   /* the frame before it again */
   video_new = 1;
   video_w = width;
   video_h = height;
   if (data == RETRO_HW_FRAME_BUFFER_VALID)
   {
#ifdef DET_EGL
      if (real_gl)
         h = egl_picture(width, height);
#endif
   }
   else
      for (y = 0; y < height; y++)
         h = hash_bytes((const unsigned char *)data + y * pitch, width * 2, h);
   video_hash = h;
}

static void audio_sample(int16_t left, int16_t right)
{
   int16_t frame[2];
   frame[0] = left;
   frame[1] = right;
   audio_hash = hash_bytes(frame, sizeof(frame), audio_hash);
}

static size_t audio_sample_batch(const int16_t *data, size_t frames)
{
   audio_hash = hash_bytes(data, frames * 4, audio_hash);
   return frames;
}

static void input_poll(void) { }

/* The inputs: a function of the frame, the port and the input alone, so
 * that a frame run again gets what it got the first time. Buttons are held
 * for a few frames at a time, as a player holds them, and the sticks move. */
static uint32_t mix(uint32_t x)
{
   x ^= x >> 16; x *= 0x7feb352d;
   x ^= x >> 15; x *= 0x846ca68b;
   x ^= x >> 16;
   return x;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   const uint32_t span = (uint32_t)frame_now / 6;   /* a new hand every 6 frames */
   uint32_t r;

   if (port > 1)
      return 0;
   device &= RETRO_DEVICE_MASK;
   if (device == RETRO_DEVICE_JOYPAD)
   {
      /* never START (3) with SELECT: that is a menu in some games, and the
       * same for each - but a reset combination in others */
      uint32_t held = mix(seed ^ (span * 0x9e3779b9u) ^ (port << 28)) & 0xfff7;
      held &= mix(seed ^ (span * 0x85ebca6bu) ^ (port << 24));   /* about a quarter held */
      if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         return (int16_t)held;
      return id < 16 ? (held >> id) & 1 : 0;
   }
   if (device == RETRO_DEVICE_ANALOG)
   {
      r = mix(seed ^ ((uint32_t)frame_now / 4 * 0xc2b2ae35u) ^ (port << 20) ^ (index << 8) ^ id);
      if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON)
         return (int16_t)((r & 1) ? 0x7fff : 0);
      return (int16_t)(r & 0xffff);
   }
   return 0;
}

/* ---- running ---- */

static void (*core_run)(void);
static size_t (*core_size)(void);
static bool (*core_save)(void *, size_t);
static bool (*core_load)(const void *, size_t);

static size_t room;
static unsigned char *now_state, *spare_state, *check_a, *check_b;
static uint64_t *unit_hashes;
static struct frame_rec rec;
static struct trace ref;
static int have_ref;
static uint64_t *ref_hashes;
static int failed;

static void fail(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("FAIL: ");
   vprintf(fmt, ap);
   printf("\n");
   va_end(ap);
   fflush(stdout);
   failed = 1;
}

static void save(unsigned char *into)
{
   if (!core_save(into, room))
   {
      fprintf(stderr, "detcheck: the state could not be saved (frame %d)\n", frame_now);
      exit(1);
   }
}

static void load(const unsigned char *from)
{
   if (!core_load(from, room))
   {
      fprintf(stderr, "detcheck: the state could not be loaded (frame %d)\n", frame_now);
      exit(1);
   }
}

/* one frame, its sound and its picture */
static void run_frame(int frame)
{
   frame_now = frame;
   audio_hash = 0xcbf29ce484222325ull;
   video_new = 0;
   core_run();
   rec.frame = (uint32_t)frame;
   rec.audio = audio_hash;
   if (video_new)
   {
      rec.video = video_hash;
      rec.video_w = video_w;
      rec.video_h = video_h;
   }
   else
      rec.video = 1;   /* the frame before it again */
}

static void unit_hash(const unsigned char *state, uint64_t *out)
{
   unsigned i;
   for (i = 0; i < unit_count; i++)
      out[i] = hash_bytes(state + units[i].at, units[i].size, 0x84222325cbf29ce4ull);
}

static void dump(const unsigned char *state, const char *path)
{
   FILE *f = fopen(path, "wb");
   if (!f || fwrite(state, 1, room, f) != room)
      fprintf(stderr, "detcheck: %s could not be written\n", path);
   if (f)
      fclose(f);
}

/* The frame just run, as the state @state has it after it: into the trace,
 * held against the reference. */
static void record(const unsigned char *state)
{
   const char *dump_at = getenv("DET_DUMP");

   unit_hash(state, unit_hashes);
   if (trace_out)
   {
      fwrite(&rec, sizeof(rec), 1, trace_out);
      fwrite(unit_hashes, 8, unit_count, trace_out);
   }
   if (dump_at && strchr(dump_at, ':') && atoi(dump_at) == (int)rec.frame)
      dump(state, strchr(dump_at, ':') + 1);
   if (have_ref)
   {
      struct frame_rec rr;
      if (!trace_frame(&ref, &rr, ref_hashes))
      {
         have_ref = 0;
         return;
      }
      if (frames_differ(&rr, ref_hashes, &rec, unit_hashes, parts, units, unit_count, 0))
      {
         printf("not the reference's after frame %u:\n", rec.frame);
         frames_differ(&rr, ref_hashes, &rec, unit_hashes, parts, units, unit_count, 1);
         if (dump_at && !strchr(dump_at, ':'))
            dump(state, dump_at);
         fflush(stdout);
         if (trace_out)
            fclose(trace_out);
         _exit(3);
      }
   }
}

/* A byte the core does not write is whatever the frontend's memory had. */
static void check_written(void)
{
   memset(check_a, 0, room);
   memset(check_b, 0xff, room);
   save(check_a);
   save(check_b);
   if (memcmp(check_a, check_b, room))
   {
      fail("after frame %d, the state has bytes that are not the core's to say: they are what the frontend's memory had", frame_now);
      parts_diff(check_a, check_b, room, "into 00", "into ff");
   }
}

int main(int argc, char **argv)
{
   struct retro_game_info game;
   int frames, i, warm, k = 0, d = 0;
   int ahead, ahead_pending = 0, ahead_count = 0, hide = 0;
   struct frame_rec ahead_rec;
   uint64_t *ahead_hashes;
   const char *mode;
   void *core;

   if (argc >= 2 && !strcmp(argv[1], "--compare"))
      return compare_main(argc, argv);
   if (argc < 4)
   {
      fprintf(stderr, "usage: %s CORE CONTENT FRAMES [key=value ...]\n"
            "       %s --compare A.trace B.trace [A.state B.state]\n", argv[0], argv[0]);
      return 2;
   }
   options = argv + 4;
   option_count = argc - 4;
   frames = atoi(argv[3]);
   warm = getenv("DET_WARM") ? atoi(getenv("DET_WARM")) : 1;
   if (getenv("DET_HIDE"))
      hide = !strcmp(getenv("DET_HIDE"), "ahead") ? 2 : 1;
   /* (a frame not shown has no picture to compare) */
   no_picture = hide != 0 || getenv("DET_NO_PICTURE") != NULL;
   if (getenv("DET_SEED"))
      seed = (uint32_t)strtoul(getenv("DET_SEED"), NULL, 0);
   mode = getenv("DET_MODE") ? getenv("DET_MODE") : "plain";
   if (!strncmp(mode, "reload:", 7))
      k = atoi(mode + 7);
   else if (!strncmp(mode, "rollback:", 9))
      d = atoi(mode + 9);
   else if (strcmp(mode, "plain") && strcmp(mode, "ahead"))
   {
      fprintf(stderr, "detcheck: no such mode %s\n", mode);
      return 2;
   }
#ifdef DET_EGL
   if (getenv("DET_GL") && !strcmp(getenv("DET_GL"), "egl"))
   {
      if (!egl_start())
      {
         fprintf(stderr, "detcheck: no OpenGL through EGL\n");
         return 1;
      }
      real_gl = 1;
   }
#endif

   core = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
   if (!core)
   {
      fprintf(stderr, "detcheck: %s\n", dlerror());
      return 1;
   }
#define SYM(var, name) *(void **)&var = dlsym(core, name); \
   if (!var) { fprintf(stderr, "detcheck: no %s in the core\n", name); return 1; }
   {
      void (*set_environment)(retro_environment_t);
      void (*set_video)(retro_video_refresh_t);
      void (*set_audio)(retro_audio_sample_t);
      void (*set_audio_batch)(retro_audio_sample_batch_t);
      void (*set_poll)(retro_input_poll_t);
      void (*set_input)(retro_input_state_t);
      void (*init)(void);
      bool (*load_game)(const struct retro_game_info *);
      void (*get_av_info)(struct retro_system_av_info *);
      void (*set_device)(unsigned, unsigned);
      struct retro_system_av_info av;

      SYM(set_environment, "retro_set_environment")
      SYM(set_video, "retro_set_video_refresh")
      SYM(set_audio, "retro_set_audio_sample")
      SYM(set_audio_batch, "retro_set_audio_sample_batch")
      SYM(set_poll, "retro_set_input_poll")
      SYM(set_input, "retro_set_input_state")
      SYM(init, "retro_init")
      SYM(load_game, "retro_load_game")
      SYM(get_av_info, "retro_get_system_av_info")
      SYM(set_device, "retro_set_controller_port_device")
      SYM(core_run, "retro_run")
      SYM(core_size, "retro_serialize_size")
      SYM(core_save, "retro_serialize")
      SYM(core_load, "retro_unserialize")
      syms_read(core);

      set_environment(environment);
      set_video(video_refresh);
      set_audio(audio_sample);
      set_audio_batch(audio_sample_batch);
      set_poll(input_poll);
      set_input(input_state);
      init();
      memset(&game, 0, sizeof(game));
      game.path = argv[2];
      if (!load_game(&game))
      {
         fprintf(stderr, "detcheck: the core would not load %s\n", argv[2]);
         return 1;
      }
      memset(&av, 0, sizeof(av));
      get_av_info(&av);
      set_device(0, RETRO_DEVICE_JOYPAD);
      set_device(1, RETRO_DEVICE_JOYPAD);
      if (have_hw && hw.context_reset)
         hw.context_reset();
   }

   /* the content as it starts, and then the state */
   for (i = 0; i < warm; i++)
      run_frame(-warm + i);
   room = core_size();
   now_state = (unsigned char *)calloc(1, room);
   spare_state = (unsigned char *)calloc(1, room);
   check_a = (unsigned char *)calloc(1, room);
   check_b = (unsigned char *)calloc(1, room);
   if (getenv("DET_STATE") && *getenv("DET_STATE"))
   {
      unsigned char *in;
      long size = file_read(getenv("DET_STATE"), &in);
      if (size <= 0 || !core_load(in, (size_t)size))
      {
         fprintf(stderr, "detcheck: the state %s could not be loaded\n", getenv("DET_STATE"));
         return 1;
      }
      free(in);
      if (core_size() > room)
      {
         fprintf(stderr, "detcheck: the state grew from %lu bytes to %lu after the load\n",
               (unsigned long)room, (unsigned long)core_size());
         return 1;
      }
   }
   save(now_state);
   parts_make(room);
   unit_hashes = (uint64_t *)calloc(unit_count, 8);
   if (getenv("DET_TRACE"))
   {
      trace_out = fopen(getenv("DET_TRACE"), "wb");
      if (!trace_out)
         return 1;
      trace_head(trace_out, room);
   }
   if (getenv("DET_REF") && *getenv("DET_REF"))
   {
      if (!trace_open(&ref, getenv("DET_REF")))
         return 1;
      if (ref.nunits != unit_count || ref.size != room || memcmp(ref.units, units, sizeof(struct unit) * unit_count))
      {
         printf("the reference's state is laid out differently: %u bytes, %u parts; here %lu bytes, %u parts\n",
               ref.size, ref.nparts, (unsigned long)room, part_count);
         return 3;
      }
      ref_hashes = (uint64_t *)calloc(unit_count, 8);
      have_ref = 1;
   }

   ahead = !strcmp(mode, "ahead");
   ahead_hashes = (uint64_t *)calloc(unit_count, 8);
   for (i = 0; i < frames; i++)
   {
      video_on = !(hide == 1 || (hide == 2 && ahead));
      run_frame(i);
      save(now_state);
      if (i % 50 == 49)
         check_written();
      record(now_state);

      if (k > 0 && i % k == k - 1)
         load(now_state);
      else if (ahead)
      {
         /* The frame run ahead the time before, from the state the frame
          * before left: run again now from that state loaded, it has to
          * be what it was. */
         if (ahead_pending)
         {
            if (frames_differ(&ahead_rec, ahead_hashes, &rec, unit_hashes, parts, units, unit_count, 0))
            {
               fail("frame %d run from a state loaded is not what it was when it was run ahead", i);
               frames_differ(&ahead_rec, ahead_hashes, &rec, unit_hashes, parts, units, unit_count, 1);
               parts_diff(spare_state, now_state, room, "ahead", "again");
               break;
            }
            ahead_count++;
         }
         /* and the frame after it, ahead, and the state back */
         ahead_pending = 0;
         if (i + 1 < frames)
         {
            video_on = 1;
            run_frame(i + 1);
            ahead_rec = rec;
            save(spare_state);
            unit_hash(spare_state, ahead_hashes);
            load(now_state);
            ahead_pending = 1;
         }
      }
      else if (d > 0 && i % d == d - 1 && i + d < frames)
      {
         /* D frames from here, written down; then back to here and the D
          * frames again, which have to be what they were */
         struct frame_rec *recs = (struct frame_rec *)malloc(sizeof(*recs) * d);
         uint64_t *hashes = (uint64_t *)malloc(8 * (size_t)unit_count * d);
         uint64_t *again = (uint64_t *)malloc(8 * (size_t)unit_count);
         unsigned char *start = check_a;   /* (free till the next check) */
         int j, bad = 0;

         memcpy(start, now_state, room);
         for (j = 1; j <= d; j++)
         {
            run_frame(i + j);
            save(now_state);
            recs[j - 1] = rec;
            unit_hash(now_state, hashes + (size_t)unit_count * (j - 1));
         }
         memcpy(spare_state, now_state, room);
         load(start);
         for (j = 1; j <= d && !bad; j++)
         {
            run_frame(i + j);
            save(now_state);
            unit_hash(now_state, again);
            if (frames_differ(&recs[j - 1], hashes + (size_t)unit_count * (j - 1), &rec, again,
                     parts, units, unit_count, 0))
            {
               fail("frame %d run again after a rollback of %d frames is not what it was", i + j, d);
               frames_differ(&recs[j - 1], hashes + (size_t)unit_count * (j - 1), &rec, again,
                     parts, units, unit_count, 1);
               if (j == d)
                  parts_diff(spare_state, now_state, room, "first", "again");
               bad = 1;
            }
            else
               record(now_state);
         }
         free(recs);
         free(hashes);
         free(again);
         if (bad)
            break;
         i += d;
      }
   }
   if (trace_out)
      fclose(trace_out);
   if (ahead)
      printf("%d frames run ahead and then again\n", ahead_count);
   printf("%s: %d frames%s\n", mode, i, failed ? ", FAILED" : "");
   fflush(stdout);
   /* (no retro_deinit: what is tested is over) */
   _exit(failed ? 3 : 0);
}
