#!/usr/bin/env python3
"""Writes the C source of a libGLESv2 whose every function does nothing.

A core built for OpenGL ES calls gl* functions directly, so to run it
without a GPU - see headless.c - it has to be given a library that has
them. This takes the names, one a line, on standard input (from
`nm -D --undefined-only` on the core, or from a failed link's complaints)
and writes the source on standard output. The few functions a core needs
an answer from give one; the rest return 0.
"""
import sys

SPECIAL = r'''
static unsigned next_name = 1;
const unsigned char *glGetString(unsigned name)
{
   switch (name)
   {
      case 0x1F02: return (const unsigned char *)(getenv("HEADLESS_GLES") ? "OpenGL ES 3.0 headless" : "4.5.0 headless");
      case 0x8B8C: return (const unsigned char *)(getenv("HEADLESS_GLES") ? "OpenGL ES GLSL ES 3.00" : "4.50");
      case 0x1F03: return (const unsigned char *)"";
      default:     return (const unsigned char *)"headless";
   }
}
/* no extensions: a core may go through them until it is given none */
const unsigned char *glGetStringi(unsigned name, unsigned index) { return 0; }
void glGetIntegerv(unsigned name, int *out)
{
   switch (name)
   {
      case 0x821B: *out = getenv("HEADLESS_GLES") ? 3 : 4; break;   /* GL_MAJOR_VERSION */
      case 0x821C: *out = getenv("HEADLESS_GLES") ? 0 : 5; break;   /* GL_MINOR_VERSION */
      case 0x0D33: *out = 4096; break;   /* GL_MAX_TEXTURE_SIZE */
      default:     *out = 0; break;
   }
}
static void status(unsigned name, int *out) { *out = (name == 0x8B81 || name == 0x8B82) ? 1 : 0; }
void glGetShaderiv(unsigned object, unsigned name, int *out) { status(name, out); }
void glGetProgramiv(unsigned object, unsigned name, int *out) { status(name, out); }
static void gen(int count, unsigned *names) { int i; for (i = 0; i < count; i++) names[i] = next_name++; }
void glGenTextures(int count, unsigned *names) { gen(count, names); }
void glGenBuffers(int count, unsigned *names) { gen(count, names); }
void glGenFramebuffers(int count, unsigned *names) { gen(count, names); }
void glGenRenderbuffers(int count, unsigned *names) { gen(count, names); }
void glGenVertexArrays(int count, unsigned *names) { gen(count, names); }
void glGenQueries(int count, unsigned *names) { gen(count, names); }
void glGenSamplers(int count, unsigned *names) { gen(count, names); }
unsigned glCreateShader(unsigned type) { return next_name++; }
unsigned glCreateProgram(void) { return next_name++; }
unsigned char glUnmapBuffer(unsigned target) { return 1; }
void *glFenceSync(unsigned condition, unsigned flags) { return (void *)1; }
unsigned glCheckFramebufferStatus(unsigned target) { return 0x8CD5; }
unsigned glClientWaitSync(void *sync, unsigned flags, unsigned long long timeout) { return 0x911A; }
static void *scratch;
static void *map(void) { if (!scratch) scratch = calloc(1, 64 << 20); return scratch; }
void *glMapBufferRange(unsigned target, long offset, long length, unsigned access) { return map(); }
void *glMapBuffer(unsigned target, unsigned access) { return map(); }
'''

def main():
    names = sorted({line.split()[-1] for line in sys.stdin if line.strip()})
    special = {line.split('(')[0].split()[-1].lstrip('*')
               for line in SPECIAL.splitlines() if line and not line.startswith((' ', '{', '}', 'static', '/'))}
    print('/* written by tools/threads/headless_gl.py */')
    print('#include <stdlib.h>')
    print(SPECIAL)
    for name in names:
        if name.startswith('gl') and name not in special:
            print('long %s(void) { return 0; }' % name)

if __name__ == '__main__':
    main()
