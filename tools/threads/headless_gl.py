#!/usr/bin/env python3
"""Writes the C source of a libGLESv2 whose every function does nothing.

A core built for OpenGL ES calls gl* functions directly, so to run it
without a GPU - see headless.c - it has to be given a library that has
them. This takes the names, one a line, on standard input (from
`nm -D --undefined-only` on the core, or from a failed link's complaints)
and writes the source on standard output. The few functions a core needs
an answer from give one; the rest return 0.

  headless_gl.py [--stdcall PROTOTYPES]

--stdcall is for 32-bit Windows, where OpenGL's functions are stdcall: each
takes its arguments off the stack itself, and one that does not leaves the
caller's stack as many bytes off as they were. PROTOTYPES is the OpenGL
headers run through the preprocessor, which say how many bytes each takes.
(The DLL is then linked with --kill-at: its functions go by their plain
names, as opengl32.dll's do.)
"""
import re
import sys

SPECIAL = r'''
#if defined(_WIN32) && !defined(_WIN64)
#define GL_CC __stdcall
#else
#define GL_CC
#endif
static unsigned next_name = 1;
const unsigned char *GL_CC glGetString(unsigned name)
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
const unsigned char *GL_CC glGetStringi(unsigned name, unsigned index) { return 0; }
void GL_CC glGetIntegerv(unsigned name, int *out)
{
   switch (name)
   {
      case 0x821B: *out = getenv("HEADLESS_GLES") ? 3 : 4; break;   /* GL_MAJOR_VERSION */
      case 0x821C: *out = getenv("HEADLESS_GLES") ? 0 : 5; break;   /* GL_MINOR_VERSION */
      case 0x0D33: *out = 4096; break;   /* GL_MAX_TEXTURE_SIZE */
      /* glsm keeps a binding per unit: none would be written past (and,
       * on a 32-bit host, over the heap's own bookkeeping) */
      case 0x8B4D: *out = 16; break;     /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
      default:     *out = 0; break;
   }
}
static void status(unsigned name, int *out) { *out = (name == 0x8B81 || name == 0x8B82) ? 1 : 0; }
void GL_CC glGetShaderiv(unsigned object, unsigned name, int *out) { status(name, out); }
void GL_CC glGetProgramiv(unsigned object, unsigned name, int *out) { status(name, out); }
static void gen(int count, unsigned *names) { int i; for (i = 0; i < count; i++) names[i] = next_name++; }
void GL_CC glGenTextures(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenBuffers(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenFramebuffers(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenRenderbuffers(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenVertexArrays(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenQueries(int count, unsigned *names) { gen(count, names); }
void GL_CC glGenSamplers(int count, unsigned *names) { gen(count, names); }
unsigned GL_CC glCreateShader(unsigned type) { return next_name++; }
unsigned GL_CC glCreateProgram(void) { return next_name++; }
unsigned char GL_CC glUnmapBuffer(unsigned target) { return 1; }
void *GL_CC glFenceSync(unsigned condition, unsigned flags) { return (void *)1; }
unsigned GL_CC glCheckFramebufferStatus(unsigned target) { return 0x8CD5; }
unsigned GL_CC glClientWaitSync(void *sync, unsigned flags, unsigned long long timeout) { return 0x911A; }
static void *scratch;
static void *map(void) { if (!scratch) scratch = calloc(1, 64 << 20); return scratch; }
void *GL_CC glMapBufferRange(unsigned target, long offset, long length, unsigned access) { return map(); }
void *GL_CC glMapBuffer(unsigned target, unsigned access) { return map(); }
'''

def stack_bytes(prototypes):
    """How many bytes of arguments each function in the headers takes."""
    sizes = {}
    for name, args in re.findall(r'\b(gl[A-Z]\w*)\s*\(([^()]*)\)\s*;', prototypes):
        args = args.strip()
        if args in ('', 'void'):
            sizes[name] = 0
            continue
        total = 0
        for arg in args.split(','):
            wide = re.search(r'\b(GLdouble|GLclampd|GLint64|GLuint64|GLint64EXT|GLuint64EXT)\b', arg)
            total += 8 if wide and '*' not in arg else 4
        sizes[name] = total
    return sizes


def main():
    sizes = None
    if sys.argv[1:2] == ['--stdcall']:
        sizes = stack_bytes(open(sys.argv[2]).read())
    names = sorted({line.split()[-1] for line in sys.stdin if line.strip()})
    special = {line.split('(')[0].split()[-1].lstrip('*')
               for line in SPECIAL.splitlines() if line and not line.startswith((' ', '{', '}', 'static', '/', '#'))}
    print('/* written by tools/threads/headless_gl.py */')
    print('#include <stdlib.h>')
    print(SPECIAL)
    for name in names:
        if not name.startswith('gl') or name in special:
            continue
        if sizes is None or name not in sizes:
            print('long %s(void) { return 0; }' % name)
        else:
            # as many bytes of arguments as the real one, taken off as it does
            args = ', '.join('int a%d' % i for i in range(sizes[name] // 4)) or 'void'
            print('long __stdcall %s(%s) { return 0; }' % (name, args))

if __name__ == '__main__':
    main()
