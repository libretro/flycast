/* The OpenGL functions that have to answer something, for an OpenGL that
 * does nothing (detcheck.sh writes the rest, each returning 0): the
 * version, names for objects, shaders that compiled, a framebuffer that is
 * complete, and memory for a buffer that is mapped. Nothing is drawn, so
 * no picture is read back and none is hashed. As tools/threads/headless_gl.py
 * has them. */
#include <stdlib.h>

static unsigned next_name = 1;

const unsigned char *glGetString(unsigned name)
{
   switch (name)
   {
      /* (DET_GLES: a core for OpenGL ES) */
      case 0x1F02: return (const unsigned char *)(getenv("DET_GLES") ? "OpenGL ES 3.0 detcheck" : "4.5.0 detcheck");
      case 0x8B8C: return (const unsigned char *)(getenv("DET_GLES") ? "OpenGL ES GLSL ES 3.00" : "4.50");
      case 0x1F03: return (const unsigned char *)"";                 /* GL_EXTENSIONS */
      default:     return (const unsigned char *)"detcheck";
   }
}
/* no extensions: a core may go through them until it is given none */
const unsigned char *glGetStringi(unsigned name, unsigned index) { (void)name; (void)index; return 0; }
void glGetIntegerv(unsigned name, int *out)
{
   switch (name)
   {
      case 0x821B: *out = getenv("DET_GLES") ? 3 : 4; break;   /* GL_MAJOR_VERSION */
      case 0x821C: *out = getenv("DET_GLES") ? 0 : 5; break;   /* GL_MINOR_VERSION */
      case 0x0D33: *out = 4096; break;   /* GL_MAX_TEXTURE_SIZE */
      case 0x8B4D: *out = 16; break;     /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
      default:     *out = 0; break;
   }
}
static void status(unsigned name, int *out) { *out = (name == 0x8B81 || name == 0x8B82) ? 1 : 0; }
void glGetShaderiv(unsigned object, unsigned name, int *out) { (void)object; status(name, out); }
void glGetProgramiv(unsigned object, unsigned name, int *out) { (void)object; status(name, out); }
static void gen(int count, unsigned *names) { int i; for (i = 0; i < count; i++) names[i] = next_name++; }
void glGenTextures(int count, unsigned *names) { gen(count, names); }
void glGenBuffers(int count, unsigned *names) { gen(count, names); }
void glGenFramebuffers(int count, unsigned *names) { gen(count, names); }
void glGenRenderbuffers(int count, unsigned *names) { gen(count, names); }
void glGenVertexArrays(int count, unsigned *names) { gen(count, names); }
void glGenQueries(int count, unsigned *names) { gen(count, names); }
void glGenSamplers(int count, unsigned *names) { gen(count, names); }
unsigned glCreateShader(unsigned type) { (void)type; return next_name++; }
unsigned glCreateProgram(void) { return next_name++; }
unsigned char glUnmapBuffer(unsigned target) { (void)target; return 1; }
void *glFenceSync(unsigned condition, unsigned flags) { (void)condition; (void)flags; return (void *)1; }
unsigned glCheckFramebufferStatus(unsigned target) { (void)target; return 0x8CD5; }
unsigned glClientWaitSync(void *sync, unsigned flags, unsigned long long timeout) { (void)sync; (void)flags; (void)timeout; return 0x911A; }
/* (what is read back from the card is nothing: zeros, the same each time) */
static void *scratch;
static void *map(void) { if (!scratch) scratch = calloc(1, 64 << 20); return scratch; }
void *glMapBufferRange(unsigned target, long offset, long length, unsigned access) { (void)target; (void)offset; (void)length; (void)access; return map(); }
void *glMapBuffer(unsigned target, unsigned access) { (void)target; (void)access; return map(); }
void glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void *data)
{
   (void)x; (void)y;
   if (data && w > 0 && h > 0 && format == 0x1908 && type == 0x1401)   /* GL_RGBA, GL_UNSIGNED_BYTE */
   {
      unsigned char *p = (unsigned char *)data;
      long n = (long)w * h * 4;
      while (n--)
         *p++ = 0;
   }
}
