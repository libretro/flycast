/* ta_gpu_test.c's device in OpenGL (built with TGT_GL): the kernels as the
 * OpenGL renderers run them (gles/ta_decoder_gl.cpp), with their buffers at
 * TAG_GL_BINDING on, in a context of EGL's without a surface (Mesa's
 * llvmpipe where there is no GPU: EGL_PLATFORM=surfaceless).
 *
 * The kernels are $TAG_SPV/kN.gl.spv, SPIR-V for OpenGL (ARB_gl_spirv), or
 * with TAG_GL_SPIRV=0 the text $TAG_SPV/kN.gl.comp compiled by the driver:
 * the two ways the renderers have them. The context is a debug one, and
 * any error OpenGL reports fails the test (tgt_validation_errors()).
 *
 * Every OpenGL function is taken from eglGetProcAddress(): the program has
 * an OpenGL that does nothing preloaded for the core's own renderer
 * (headless.sh's), whose names must not be these. Included by
 * ta_gpu_test.c. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V_ARB
#define GL_SHADER_BINARY_FORMAT_SPIR_V_ARB 0x9551
#endif
typedef void (APIENTRYP TGT_PFNSPECIALIZE)(GLuint, const GLchar *, GLuint, const GLuint *, const GLuint *);

static int gl_errors;
static EGLDisplay gl_dpy;
static EGLContext gl_ctx;
static PFNGLGETSTRINGPROC p_glGetString;
static PFNGLGETSTRINGIPROC p_glGetStringi;
static PFNGLGETINTEGERVPROC p_glGetIntegerv;
static PFNGLCREATESHADERPROC p_glCreateShader;
static PFNGLSHADERSOURCEPROC p_glShaderSource;
static PFNGLCOMPILESHADERPROC p_glCompileShader;
static PFNGLSHADERBINARYPROC p_glShaderBinary;
static TGT_PFNSPECIALIZE p_glSpecializeShader;
static PFNGLGETSHADERIVPROC p_glGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC p_glCreateProgram;
static PFNGLATTACHSHADERPROC p_glAttachShader;
static PFNGLLINKPROGRAMPROC p_glLinkProgram;
static PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
static PFNGLDELETESHADERPROC p_glDeleteShader;
static PFNGLUSEPROGRAMPROC p_glUseProgram;
static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERSTORAGEPROC p_glBufferStorage;
static PFNGLMAPBUFFERRANGEPROC p_glMapBufferRange;
static PFNGLBINDBUFFERBASEPROC p_glBindBufferBase;
static PFNGLCLEARBUFFERSUBDATAPROC p_glClearBufferSubData;
static PFNGLDISPATCHCOMPUTEPROC p_glDispatchCompute;
static PFNGLDISPATCHCOMPUTEINDIRECTPROC p_glDispatchComputeIndirect;
static PFNGLMEMORYBARRIERPROC p_glMemoryBarrier;
static PFNGLFINISHPROC p_glFinish;
static PFNGLDEBUGMESSAGECALLBACKPROC p_glDebugMessageCallback;
static PFNGLENABLEPROC p_glEnable;

static GLuint programs[TAG_KERNELS + 1];
enum { G_TA, G_TAB, G_VTX, G_MOD, G_IDX, G_WORK, G_REC, G_CMD, G_FRM, G_TRD, G_TRP, G_SRT, G_COUNT };
static GLuint gbuf[G_COUNT];
static void *gmap[G_COUNT];

static void APIENTRY on_gl_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
      const GLchar *message, const void *user)
{
   (void)source; (void)id; (void)length; (void)user;
   if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH)
   {
      if (gl_errors++ < 10)
         printf("validation: %s\n", message);
   }
}

static void *gl_fn(const char *name)
{
   void *f = (void *)eglGetProcAddress(name);
   if (f == NULL)
   {
      printf("no %s\n", name);
      exit(2);
   }
   return f;
}

static int has_extension(const char *name)
{
   GLint n = 0, i;
   p_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   for (i = 0; i < n; i++)
      if (!strcmp((const char *)p_glGetStringi(GL_EXTENSIONS, (GLuint)i), name))
         return 1;
   return 0;
}

static GLuint gl_kernel(const char *dir, unsigned k, int spirv)
{
   char path[1024];
   size_t size;
   unsigned char *code;
   GLuint sh = p_glCreateShader(GL_COMPUTE_SHADER), prog;
   GLint ok = 0;
   char log[4096];

   if (spirv)
   {
      snprintf(path, sizeof(path), "%s/k%u.gl.spv", dir, k);
      code = read_file(path, &size);
      p_glShaderBinary(1, &sh, GL_SHADER_BINARY_FORMAT_SPIR_V_ARB, code, (GLsizei)size);
      p_glSpecializeShader(sh, "main", 0, NULL, NULL);
   }
   else
   {
      const GLchar *src;
      snprintf(path, sizeof(path), "%s/k%u.gl.comp", dir, k);
      code = read_file(path, &size);
      code = (unsigned char *)realloc(code, size + 1);
      code[size] = 0;
      src = (const GLchar *)code;
      p_glShaderSource(sh, 1, &src, NULL);
      p_glCompileShader(sh);
   }
   free(code);
   p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      p_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
      printf("kernel %u: %s\n", k, log);
      exit(2);
   }
   prog = p_glCreateProgram();
   p_glAttachShader(prog, sh);
   p_glLinkProgram(prog);
   p_glDeleteShader(sh);
   p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok)
   {
      p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      printf("kernel %u: %s\n", k, log);
      exit(2);
   }
   return prog;
}

static void gl_buffer(int b, size_t bytes)
{
   const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
   p_glGenBuffers(1, &gbuf[b]);
   p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, gbuf[b]);
   p_glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, NULL, flags | GL_DYNAMIC_STORAGE_BIT);
   gmap[b] = p_glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, flags);
   if (gmap[b] == NULL)
   {
      printf("buffer %d not mapped\n", b);
      exit(2);
   }
}

int tgt_validation_errors(void)
{
   return gl_errors;
}

void tgt_init(const char *spv_dir, int validate, const struct tgt_sizes *sz, unsigned *layout)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLint attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
      EGL_CONTEXT_OPENGL_DEBUG, EGL_TRUE, EGL_NONE };
   EGLDisplay dpy;
   EGLContext ctx;
   EGLint major, minor;
   unsigned k, strips;
   int spirv;
   GLint blocks = 0, bindings = 0;

   dpy = get_display != NULL ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
   {
      printf("no EGL display without a surface\n");
      exit(2);
   }
   if (!validate)
      attribs[6] = EGL_NONE;
   ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
   gl_dpy = dpy;
   gl_ctx = ctx;
   if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
   {
      printf("no OpenGL 4.3 context\n");
      exit(2);
   }
   p_glGetString = (PFNGLGETSTRINGPROC)gl_fn("glGetString");
   p_glGetStringi = (PFNGLGETSTRINGIPROC)gl_fn("glGetStringi");
   p_glGetIntegerv = (PFNGLGETINTEGERVPROC)gl_fn("glGetIntegerv");
   p_glCreateShader = (PFNGLCREATESHADERPROC)gl_fn("glCreateShader");
   p_glShaderSource = (PFNGLSHADERSOURCEPROC)gl_fn("glShaderSource");
   p_glCompileShader = (PFNGLCOMPILESHADERPROC)gl_fn("glCompileShader");
   p_glShaderBinary = (PFNGLSHADERBINARYPROC)gl_fn("glShaderBinary");
   p_glGetShaderiv = (PFNGLGETSHADERIVPROC)gl_fn("glGetShaderiv");
   p_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)gl_fn("glGetShaderInfoLog");
   p_glCreateProgram = (PFNGLCREATEPROGRAMPROC)gl_fn("glCreateProgram");
   p_glAttachShader = (PFNGLATTACHSHADERPROC)gl_fn("glAttachShader");
   p_glLinkProgram = (PFNGLLINKPROGRAMPROC)gl_fn("glLinkProgram");
   p_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)gl_fn("glGetProgramiv");
   p_glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)gl_fn("glGetProgramInfoLog");
   p_glDeleteShader = (PFNGLDELETESHADERPROC)gl_fn("glDeleteShader");
   p_glUseProgram = (PFNGLUSEPROGRAMPROC)gl_fn("glUseProgram");
   p_glGenBuffers = (PFNGLGENBUFFERSPROC)gl_fn("glGenBuffers");
   p_glBindBuffer = (PFNGLBINDBUFFERPROC)gl_fn("glBindBuffer");
   p_glBufferStorage = (PFNGLBUFFERSTORAGEPROC)gl_fn("glBufferStorage");
   p_glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)gl_fn("glMapBufferRange");
   p_glBindBufferBase = (PFNGLBINDBUFFERBASEPROC)gl_fn("glBindBufferBase");
   p_glClearBufferSubData = (PFNGLCLEARBUFFERSUBDATAPROC)gl_fn("glClearBufferSubData");
   p_glDispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)gl_fn("glDispatchCompute");
   p_glDispatchComputeIndirect = (PFNGLDISPATCHCOMPUTEINDIRECTPROC)gl_fn("glDispatchComputeIndirect");
   p_glMemoryBarrier = (PFNGLMEMORYBARRIERPROC)gl_fn("glMemoryBarrier");
   p_glFinish = (PFNGLFINISHPROC)gl_fn("glFinish");
   p_glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)gl_fn("glDebugMessageCallback");
   p_glEnable = (PFNGLENABLEPROC)gl_fn("glEnable");
   if (validate)
   {
      p_glEnable(GL_DEBUG_OUTPUT);
      p_glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      p_glDebugMessageCallback(on_gl_message, NULL);
   }

   spirv = has_extension("GL_ARB_gl_spirv");
   if (getenv("TAG_GL_SPIRV") != NULL)
      spirv = spirv && atoi(getenv("TAG_GL_SPIRV")) != 0;
   if (spirv)
      p_glSpecializeShader = (TGT_PFNSPECIALIZE)gl_fn("glSpecializeShaderARB");
   p_glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
   p_glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
   printf("device: %s, OpenGL %s, the kernels as %s, %d storage buffers a kernel, %d bindings%s\n",
         (const char *)p_glGetString(GL_RENDERER), (const char *)p_glGetString(GL_VERSION),
         spirv ? "SPIR-V" : "text", blocks, bindings, validate ? ", debug context" : "");
   if (blocks < G_COUNT || bindings < TAG_GL_BINDING + G_COUNT)
   {
      printf("not enough storage buffers\n");
      exit(2);
   }
   for (k = 1; k <= TAG_KERNELS; k++)
      programs[k] = gl_kernel(spv_dir, k, spirv);

   model_init(sz, layout);
   strips = sz->list[0] + sz->list[1] + sz->list[2];
   gl_buffer(G_TA, (size_t)sz->ta_words * 4);
   gl_buffer(G_TAB, (size_t)sz->table_words * 4);
   gl_buffer(G_VTX, (size_t)sz->verts * TAG_V_WORDS * 4);
   gl_buffer(G_MOD, (size_t)sz->modtrig * 9 * 4 + 4);
   gl_buffer(G_IDX, (size_t)sz->idx * 4);
   gl_buffer(G_WORK, (size_t)work_words * 4);
   gl_buffer(G_REC, (size_t)strips * TAG_C_WORDS * 4);
   gl_buffer(G_CMD, (size_t)sz->headers * 2 * TAG_C_WORDS * 4);
   gl_buffer(G_FRM, TAG_O_WORDS * 4);
   gl_buffer(G_TRD, (size_t)(sz->verts + sz->headers) * TAG_C_WORDS * 4);
   gl_buffer(G_TRP, (size_t)sz->list[2] * TAG_P_WORDS * 4);
   /* (the sorted indices' room, as the renderers have it: six words a triangle a frame can have) */
   gl_buffer(G_SRT, (size_t)sz->idx * 6 * 4);
   for (k = 0; k < G_COUNT; k++)
      p_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + k, gbuf[k]);
   p_glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, gbuf[G_FRM]);
   /* (current only while a frame is decoded: the core's own renderer calls
    * an OpenGL of the process's too, which without a context does nothing) */
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void gl_clear(int b, size_t at, size_t bytes, unsigned value)
{
   p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, gbuf[b]);
   p_glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, (GLintptr)at, (GLsizeiptr)bytes, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}

static void gl_dispatch(unsigned k, unsigned groups)
{
   p_glUseProgram(programs[k]);
   if (groups != 0)
      p_glDispatchCompute(groups, 1, 1);
   p_glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

static void gl_dispatch_indirect(unsigned k, unsigned at)
{
   p_glUseProgram(programs[k]);
   p_glDispatchComputeIndirect((GLintptr)at * 4);
   p_glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

void tgt_gpu(const void *ta, unsigned ta_bytes, const unsigned *table, unsigned table_words, unsigned trd, struct tgt_out *o, double *ms)
{
   const unsigned nv = table[TAG_T_NVERTS], nmod = table[TAG_T_NMOD], nslot = table[TAG_T_NSLOT];
   const unsigned scan_groups = (nv + TAG_SCAN_N - 1u) / TAG_SCAN_N;
   double t0;

   const unsigned checks = srt_gpu != NULL ? srt_gpu_checks : 0;

   eglMakeCurrent(gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl_ctx);
   memcpy(gmap[G_TA], ta, ta_bytes);
   memcpy(gmap[G_TAB], table, (size_t)table_words * 4);
   if (checks != 0)
      memcpy(gmap[G_SRT], srt_gpu, (size_t)srt_gpu_words * 4);
   t0 = now_ms();
   /* what is not written this frame is left from the one before, as it
    * would be in the renderer */
   gl_clear(G_FRM, 0, TAG_O_WORDS * 4, 0);
   gl_clear(G_FRM, TAG_O_ZMAX * 4, 4, 0x3F800000u);
   if (nslot != 0)
      gl_clear(G_WORK, (size_t)table[TAG_T_W_SLOTH] * 4, (size_t)nslot * 4, 0);
   if (trd != 0)
      gl_clear(G_TRD, 0, (size_t)trd * TAG_C_WORDS * 4, 0);
   gl_dispatch(TAG_K_UNPACK, (nv + nmod + 63u) / 64u);
   if (checks != 0)
      gl_dispatch(TAG_K_PREPASS, checks < 65535u ? checks : 65535u);
   gl_dispatch(TAG_K_COUNT, scan_groups);
   gl_dispatch(TAG_K_COUNTSUM, 1);
   gl_dispatch(TAG_K_ENDS, scan_groups);
   gl_dispatch_indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
   gl_dispatch_indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
   gl_dispatch(TAG_K_GROUPSUM, 1);
   gl_dispatch_indirect(TAG_K_STATES, TAG_O_ARGSCH);
   gl_dispatch_indirect(TAG_K_EMIT, TAG_O_ARGS64);
   gl_dispatch(TAG_K_FINISH, (nslot + 63u) / 64u);
   if (table[TAG_T_NSHD] != 0)
      gl_dispatch(TAG_K_SHADOW, table[TAG_T_NSHD] < 65535u ? table[TAG_T_NSHD] : 65535u);
   if (trd != 0)
      gl_dispatch_indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
   if (table[TAG_T_FLAGS] & TAG_FLAG_BLEED)
      gl_dispatch_indirect(TAG_K_BLEED, TAG_O_ARGS1);
   if (table[TAG_T_FLAGS] & TAG_FLAG_PROVOKE)
      gl_dispatch_indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
   p_glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
   /* (a test waits for its answer; the renderers never do) */
   p_glFinish();
   *ms = now_ms() - t0;
   eglMakeCurrent(gl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   if (checks != 0)
      memcpy(srt_gpu, gmap[G_SRT], (size_t)srt_gpu_words * 4);
   srt_gpu = NULL;

   o->vtx = (const unsigned *)gmap[G_VTX];
   o->mod = (const unsigned *)gmap[G_MOD];
   o->idx = (const unsigned *)gmap[G_IDX];
   o->rec = (const unsigned *)gmap[G_REC];
   o->cmd = (const unsigned *)gmap[G_CMD];
   o->frm = (const unsigned *)gmap[G_FRM];
   o->work = (const unsigned *)gmap[G_WORK];
   o->trd = (const unsigned *)gmap[G_TRD];
   o->trp = (const unsigned *)gmap[G_TRP];
}
