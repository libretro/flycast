/* The OpenGL renderers' texture kernels against the texture cache's CPU
 * converters, byte for byte: texdecode_test.c's steps 2 and 3 on an
 * OpenGL device, with no display (EGL, surfaceless: Mesa's llvmpipe does).
 *
 * texdecode_test.c, run with "dump", writes every texture of its corpus -
 * the plan, the bytes the GPU is handed, what the CPU made - to its
 * standard output; this reads that from its standard input, as
 * texdecode_d3d11.c does, and makes each texture as gles/gl_texdecoder.cpp
 * makes it, with the kernels the core has (rend/texdecode_gl.inc: the
 * header's text, which the renderers compile, and its SPIR-V), into
 * textures of the formats the texture pool has (GL_RGB565, GL_RGB5_A1,
 * GL_RGBA4, GL_RGBA8, GL_R8), on the path the context's version takes:
 *   gl46   OpenGL 4.6 core (or 4.5 with ARB_gl_spirv), the kernels from
 *          their SPIR-V: compute kernels
 *          for 32-bit texels and palette indices (rgba8ui, r8ui images),
 *          the 16-bit ones drawn, all reading a storage buffer
 *   gl43   OpenGL 4.3 core, the same compiled from the text (GLSL 4.30)
 *   gl33   OpenGL 3.3 core: everything drawn, the ring an integer texture
 *          (GLSL 3.30)
 *   gl30   OpenGL 3.0: the same, GLSL 1.30
 *   es30   OpenGL ES 3.0: the same, GLSL ES 3.00 (the GLES builds convert
 *          on the CPU; this is the text's check on an ES compiler)
 * The levels are read back (glGetTexImage; on OpenGL ES through a
 * framebuffer, in the format the driver reads it in when that is the
 * texture's own) and have to be what the CPU made: OpenGL's 16-bit texels
 * are the converters' as they are.
 *
 * And the planar 16-bit textures the renderers copy as they are: their
 * rows uploaded from the bytes with GL_UNSIGNED_SHORT_5_6_5,
 * GL_UNSIGNED_SHORT_1_5_5_5_REV (GL_BGRA) and GL_UNSIGNED_SHORT_4_4_4_4_REV
 * (GL_BGRA), with the row length the texture has in video memory, and read
 * back in the converters' order: the same texels (not on OpenGL ES, which
 * has 5-6-5 alone).
 *
 * Built and run by texdecode_test.sh (TEXD_GL=0 leaves it out). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#include "texdecode.h"
#include "texdecode_case.h"

typedef unsigned char u8;
typedef unsigned int u32;
#include "texdecode_gl.inc"

#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif
#ifndef GL_IMPLEMENTATION_COLOR_READ_TYPE
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#endif

#define RING_BYTES (8u * 1024 * 1024)
#define BINDING 7

static int es, compute, spirv;
static GLuint programs[3];		/* 32-bit, 8-bit (compute), drawn */
static GLint locs[3][2];
static GLuint ring, ring_texture, fbo, vao;
static unsigned ring_rows;
static unsigned failures, cases, through_compute, drawn, natives, levels_made, unread;
static const char *mode = "gl43";

static void fail(const struct texd_case *c, const char *what)
{
   if (failures++ < 20)
      printf("FAIL %s: fmt %u scan %u vq %u mip %u stride %u (ctl %u) %ux%u filter %u force32 %u lod0 %u pal %u/%u at %06x\n",
            what, c->fmt, c->scan_order, c->vq, c->mip, c->stride_sel, c->text_control & 31,
            8u << c->tex_u, 8u << c->tex_v, c->filter, c->force32, c->lod0, c->pal_ctrl, c->pal_select, c->addr);
}

static int read_all(void *p, size_t n)
{
   return fread(p, 1, n, stdin) == n;
}

static void die(const char *what)
{
   printf("texdecode_gl: %s (%x)\n", what, (unsigned)glGetError());
   exit(2);
}

static GLuint shader(GLenum kind, const char *text, const u32 *spv, size_t spv_size)
{
   GLuint s = glCreateShader(kind);
   GLint ok = 0;
   if (spv)
   {
      glShaderBinary(1, &s, GL_SHADER_BINARY_FORMAT_SPIR_V, spv, (GLsizei)spv_size);
      glSpecializeShader(s, "main", 0, NULL, NULL);
   }
   else
   {
      glShaderSource(s, 1, &text, NULL);
      glCompileShader(s);
   }
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[4096];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      printf("texdecode_gl: a kernel does not compile: %s\n", log);
      exit(2);
   }
   return s;
}

static GLuint program(int n, const GLuint *s)
{
   GLuint p = glCreateProgram();
   GLint ok = 0;
   int i;
   for (i = 0; i < n; i++)
      glAttachShader(p, s[i]);
   glLinkProgram(p);
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok)
   {
      char log[4096];
      glGetProgramInfoLog(p, sizeof(log), NULL, log);
      printf("texdecode_gl: a kernel does not link: %s\n", log);
      exit(2);
   }
   return p;
}

/* The text as the renderer puts it together: its version, then the header */
static char *text_of(const char *head, int kernel)
{
   const size_t n = strlen(head) + 256 + sizeof(texdecode_glsl);
   char *t = (char *)malloc(n);
   snprintf(t, n, "%s#define TEXD_SHADER_GLSL\n#define TEXD_GL_RING %d\n#define TEXD_KERNEL_GL %d\n%s", head, compute ? 1 : 2, kernel, texdecode_glsl);
   return t;
}

static void init(void)
{
   EGLDisplay dpy;
   EGLConfig config;
   EGLContext ctx;
   EGLint n, major = 4, minor = 3;
   const char *head;
   EGLint attribs[16];
   int a = 0;
   static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES3_BIT, EGL_NONE };
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

   es = !strcmp(mode, "es30");
   compute = !strcmp(mode, "gl46") || !strcmp(mode, "gl43");
   spirv = !strcmp(mode, "gl46");
   /* (4.5 with ARB_gl_spirv where there is no 4.6: Mesa's llvmpipe) */
   if (!strcmp(mode, "gl46")) major = 4, minor = 5;
   else if (!strcmp(mode, "gl33")) major = 3, minor = 3;
   else if (!strcmp(mode, "gl30") || es) major = 3, minor = 0;

   dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
      die("no EGL display");
   if (!eglChooseConfig(dpy, config_attribs, &config, 1, &n) || n == 0)
      config = NULL;
   eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
   attribs[a++] = EGL_CONTEXT_MAJOR_VERSION;
   attribs[a++] = major;
   attribs[a++] = EGL_CONTEXT_MINOR_VERSION;
   attribs[a++] = minor;
   if (!es && (major > 3 || minor >= 2))
   {
      attribs[a++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
      attribs[a++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
   }
   attribs[a++] = EGL_NONE;
   ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, attribs);
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      die("no OpenGL context of that version");

   head = compute ? "#version 430\n" : es ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp usampler2D;\n"
         : major == 3 && minor >= 3 ? "#version 330\n" : "#version 130\n";
   if (compute)
   {
      GLuint s[2];
      size_t sz;
      s[0] = shader(GL_COMPUTE_SHADER, text_of(head, 1), spirv ? texdecode_glspv[1] : NULL, texdecode_glspv_size[1]);
      programs[0] = program(1, s);
      s[0] = shader(GL_COMPUTE_SHADER, text_of(head, 2), spirv ? texdecode_glspv[2] : NULL, texdecode_glspv_size[2]);
      programs[1] = program(1, s);
      (void)sz;
   }
   {
      GLuint s[2];
      s[0] = shader(GL_VERTEX_SHADER, text_of(head, 3), spirv ? texdecode_glspv[3] : NULL, texdecode_glspv_size[3]);
      s[1] = shader(GL_FRAGMENT_SHADER, text_of(head, 4), spirv ? texdecode_glspv[4] : NULL, texdecode_glspv_size[4]);
      programs[2] = program(2, s);
   }
   for (n = 0; n < 3; n++)
   {
      if (!programs[n])
         continue;
      locs[n][0] = spirv ? 0 : glGetUniformLocation(programs[n], "texd_lv0");
      locs[n][1] = spirv ? 1 : glGetUniformLocation(programs[n], "texd_lv1");
      if (!compute)
      {
         glUseProgram(programs[n]);
         glUniform1i(glGetUniformLocation(programs[n], "texd_ring"), BINDING);
      }
   }
   if (compute)
   {
      glGenBuffers(1, &ring);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
      glBufferData(GL_SHADER_STORAGE_BUFFER, RING_BYTES, NULL, GL_DYNAMIC_DRAW);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BINDING, ring);
   }
   else
   {
      ring_rows = RING_BYTES / (TEXD_GL_RING_WIDTH * 4);
      glGenTextures(1, &ring_texture);
      glActiveTexture(GL_TEXTURE0 + BINDING);
      glBindTexture(GL_TEXTURE_2D, ring_texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, TEXD_GL_RING_WIDTH, ring_rows, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glActiveTexture(GL_TEXTURE0);
   }
   glGenFramebuffers(1, &fbo);
   glGenVertexArrays(1, &vao);
   glDisable(GL_DITHER);
   if (glGetError() != GL_NO_ERROR)
      die("setting up");
   printf("texdecode_gl: %s: %s, %s; %s\n", mode, (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
         compute ? (spirv ? "compute and drawn, from SPIR-V" : "compute and drawn") : "drawn, the ring a texture");
}

static unsigned texel_bytes(unsigned out)
{
   return out == TEXD_OUT32 ? 4 : out == TEXD_OUT16 ? 2 : 1;
}

static size_t level_at(const struct texd_plan *plan, unsigned i)
{
   size_t at = 0;
   unsigned j;
   if (!plan->mipmapped)
      return 0;
   for (j = 0; j < i; j++)
      at += (size_t)1 << (2 * j);
   return at * texel_bytes(plan->out);
}

/* by TextureType: the pool's internal format, and the format and type its texels are read in */
static const GLenum internal_of[5] = { GL_RGB565, GL_RGB5_A1, GL_RGBA4, GL_RGBA8, GL_R8 };
static const GLenum format_of[5] = { GL_RGB, GL_RGBA, GL_RGBA, GL_RGBA, GL_RED };
static const GLenum type_of[5] = { GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE };

static GLuint make_texture(int type, unsigned w, unsigned h, unsigned levels)
{
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexStorage2D(GL_TEXTURE_2D, levels, internal_of[type], w, h);
   return t;
}

static int same_level(GLuint tex, int type, unsigned mip, unsigned w, unsigned h, unsigned px, const unsigned char *want)
{
   unsigned char *got = (unsigned char *)malloc((size_t)w * h * 4 + 16);
   unsigned x, y, k;
   int same;
   if (!es)
   {
      glBindTexture(GL_TEXTURE_2D, tex);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glGetTexImage(GL_TEXTURE_2D, mip, format_of[type], type_of[type], got);
   }
   else
   {
      GLint f = 0, t = 0;
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, mip);
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &f);
      glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &t);
      if ((GLenum)t != type_of[type] || ((GLenum)f != format_of[type] && !(type == 4 && f == GL_RED)))
      {
         /* (not read in its own format: the bytes of 32-bit texels, the
          * others left unread) */
         if (type != 3)
         {
            unread++;
            free(got);
            return 1;
         }
         f = GL_RGBA;
         t = GL_UNSIGNED_BYTE;
      }
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, f, t, got);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
   }
   same = memcmp(got, want, (size_t)w * h * px) == 0;
   for (y = 0; y < h && !same && failures < 5; y++)
      for (x = 0; x < w; x++)
         if (memcmp(got + (y * w + x) * px, want + (y * w + x) * px, px))
         {
            printf("  level %u (%ux%u): texel %u,%u is", mip, w, h, x, y);
            for (k = 0; k < px; k++)
               printf(" %02x", got[(y * w + x) * px + k]);
            printf(", the CPU's");
            for (k = 0; k < px; k++)
               printf(" %02x", want[(y * w + x) * px + k]);
            printf("\n");
            y = h;
            break;
         }
   free(got);
   return same;
}

static void fill_ring(const unsigned char *raw, unsigned nbytes, unsigned base)
{
   if (compute)
   {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
      glBufferSubData(GL_SHADER_STORAGE_BUFFER, base * 4, nbytes, raw);
   }
   else
   {
      /* (as the renderer has it: rows of TEXD_GL_RING_WIDTH words, the
       * bytes from word @base on) */
      static unsigned char *shadow;
      const unsigned row = TEXD_GL_RING_WIDTH * 4, first = base * 4 / row, last = (base * 4 + nbytes + row - 1) / row;
      if (!shadow)
         shadow = (unsigned char *)calloc(1, RING_BYTES);
      memcpy(shadow + base * 4, raw, nbytes);
      glActiveTexture(GL_TEXTURE0 + BINDING);
      glBindTexture(GL_TEXTURE_2D, ring_texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, TEXD_GL_RING_WIDTH, last - first, GL_RED_INTEGER, GL_UNSIGNED_INT, shadow + (size_t)first * row);
      glActiveTexture(GL_TEXTURE0);
   }
}

static void run_kernel(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, unsigned nbytes,
      const unsigned char *cpu)
{
   const unsigned px = texel_bytes(plan->out);
   const unsigned nlevels = plan->mipmapped ? plan->levels : 1;
   const unsigned base = 64;   /* somewhere in the ring that is not its start */
   const int by_compute = compute && plan->out != TEXD_OUT16;
   GLuint tex;
   unsigned i, d3d16 = 0;

   if (plan->out == TEXD_OUT16 ? type > 2 : plan->out == TEXD_OUT32 ? type != 3 : type != 4)
   {
      fail(c, "a texel size of another format");
      return;
   }
   if (plan->out == TEXD_OUT16)
      d3d16 = TEXD_D3D16(type == 1 ? TEXD_D3D_5551 : type == 2 ? TEXD_D3D_4444 : TEXD_D3D_565);
   tex = make_texture(type, plan->width, plan->height, nlevels);
   fill_ring(raw, nbytes, base);
   if (by_compute)
   {
      const int p = plan->out == TEXD_OUT32 ? 0 : 1;
      through_compute++;
      glUseProgram(programs[p]);
      for (i = 0; i < plan->levels; i++)
      {
         const struct texd_level *lv = &plan->level[i];
         glUniform4ui(locs[p][0], base, lv->data, plan->pal, lv->wh);
         glUniform2ui(locs[p][1], lv->sr, lv->mode);
         glBindImageTexture(BINDING, tex, lv->mip, GL_FALSE, 0, GL_WRITE_ONLY, p == 0 ? GL_RGBA8UI : GL_R8UI);
         glDispatchCompute(((lv->wh & 0xFFFF) + 7) / 8, ((lv->wh >> 16) + 7) / 8, 1);
      }
      glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
   }
   else
   {
      drawn++;
      glUseProgram(programs[2]);
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glBindVertexArray(vao);
      for (i = 0; i < plan->levels; i++)
      {
         const struct texd_level *lv = &plan->level[i];
         glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, lv->mip);
         if (i == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
         {
            fail(c, "cannot be drawn into");
            break;
         }
         glViewport(0, 0, lv->wh & 0xFFFF, lv->wh >> 16);
         glUniform4ui(locs[2][0], base, lv->data, plan->pal, lv->wh);
         glUniform2ui(locs[2][1], lv->sr, lv->mode | d3d16);
         glDrawArrays(GL_TRIANGLES, 0, 3);
      }
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
   }
   if (glGetError() != GL_NO_ERROR)
      fail(c, "an OpenGL error");
   for (i = 0; i < plan->levels; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      levels_made++;
      if (!same_level(tex, type, lv->mip, lv->wh & 0xFFFF, lv->wh >> 16, px, cpu + level_at(plan, i)))
      {
         fail(c, by_compute ? "compute kernel" : "drawn");
         break;
      }
   }
   glDeleteTextures(1, &tex);
}

/* A planar 16-bit texture the renderers copy as they are: its rows straight
 * from the bytes, in the formats that have the PowerVR's order */
static void run_native(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, const unsigned char *cpu)
{
   static const GLenum native_format[3] = { GL_RGB, GL_BGRA, GL_BGRA };
   static const GLenum native_type[3] = { GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_UNSIGNED_SHORT_4_4_4_4_REV };
   const unsigned w = plan->width, h = plan->height, stride = plan->level[0].sr & 0xFFFF;
   GLuint tex;
   if (type > 2 || (es && type != 0))
      return;
   natives++;
   tex = make_texture(type, w, h, 1);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, native_format[type], native_type[type], raw + plan->level[0].data);
   glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   if (glGetError() != GL_NO_ERROR || !same_level(tex, type, 0, w, h, 2, cpu))
      fail(c, "copied as it is");
   glDeleteTextures(1, &tex);
}

int main(int argc, char **argv)
{
   unsigned char *raw = NULL, *cpu = NULL;
   size_t raw_cap = 0, cpu_cap = 0;

   if (argc > 1)
      mode = argv[1];
   init();
   for (;;)
   {
      u32 head[4];
      struct texd_case c;
      struct texd_plan plan;
      u32 type, nbytes, cpubytes;
      if (!read_all(head, sizeof(head)) || head[0] != 0x43445854u)	/* 'TXDC' */
         break;
      type = head[1];
      nbytes = head[2];
      cpubytes = head[3];
      if (!read_all(&c, sizeof(c)) || !read_all(&plan, sizeof(plan)))
         break;
      if (nbytes + 4 > raw_cap)
         raw = (unsigned char *)realloc(raw, raw_cap = nbytes + 4);
      if (cpubytes > cpu_cap)
         cpu = (unsigned char *)realloc(cpu, cpu_cap = cpubytes);
      if (!read_all(raw, nbytes) || !read_all(cpu, cpubytes))
         break;
      cases++;
      if (nbytes + 64 * 4 > RING_BYTES)
         continue;
      run_kernel(&c, &plan, (int)type, raw, nbytes, cpu);
      if (plan.native)
         run_native(&c, &plan, (int)type, raw, cpu);
   }
   printf("texdecode_gl: %s: %u textures, %u by the compute kernels, %u drawn, %u copied as they are; %u levels%s\n",
         mode, cases, through_compute, drawn, natives, levels_made, unread ? "" : "");
   if (unread)
      printf("texdecode_gl: %s: %u levels not read back (OpenGL ES reads them in another format)\n", mode, unread);
   if (cases == 0 || failures)
   {
      printf("FAIL: %s: %u textures differ%s\n", mode, failures, cases == 0 ? " (none read)" : "");
      return 1;
   }
   printf("texdecode_gl: %s: all the same, byte for byte\n", mode);
   return 0;
}
