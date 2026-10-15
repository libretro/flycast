/* OpenGL programs made without a frame waiting for one: the OpenGL side of
 * vulkan/pso.h.
 *
 * - Every program the renderers make goes through glprog_make() (and
 *   gl_CompileAndLink(), which calls it): where the driver can give a
 *   program back as a binary (OpenGL 4.1, OpenGL ES 3.0 or
 *   ARB_get_program_binary, with a format to give it in), what it gives is
 *   kept in the system directory (gl_programs.cache) under a hash of the
 *   program's text, for this driver only (its vendor, renderer and version
 *   strings), and taken from there the next time instead of compiling.
 * - With the binaries the file keeps each renderer's polygon programs' keys
 *   (glprog_note()): at its start a renderer makes all of those it was
 *   drawn with before (glprog_known()), and a set most games draw with. An
 *   OpenGL ES 2 context gives no binaries; the keys are kept all the same,
 *   and made when the renderer starts.
 * - Where the driver makes programs in the background
 *   (KHR_parallel_shader_compile, or the ARB one, and it is seen to: a
 *   driver can have the extension and still make them as it is asked)
 *   those made at the start are asked for all at once and waited for
 *   together, and a polygon's program first wanted in a frame is asked
 *   for (glprog_make() with @background) and polled (glprog_poll()) in the
 *   frames after; the polygon is drawn with the renderer's general program
 *   meanwhile, which reads the key's bits from a uniform and draws the
 *   same pixels. Where it does not, such a polygon is drawn with the
 *   general program for the rest of the run, and its key kept for the next
 *   one. (A libretro core has no context of its own to compile in on a
 *   thread.)
 * - The file is read when a renderer starts (glprog_open()), written when it
 *   has made something new - at its start, once those programs are made,
 *   and when it goes (glprog_close(), or glprog_save() when the context
 *   goes) - never in a frame. Nothing takes a lock: one thread, the one
 *   with the context, does all of it.
 *
 * Other programs (a texture decode's, the TA decode's kernels) go through
 * glprog_make() and glprog_make_compute() and are kept the same way. */
#pragma once
#include "types.h"

// (a program is a GLuint; the header is included where there is no OpenGL too)

// whose polygon programs a key is
enum GlProgramList { GLPROG_GLES = 0, GLPROG_GL4 = 1, GLPROG_LISTS = 2 };

enum GlProgramStatus { GLPROG_FAILED = -1, GLPROG_PENDING = 0, GLPROG_READY = 1 };

/* When a renderer starts (the context bound, after findGLVersion()): what the
 * driver can do found out and the file read. Again for each renderer. */
void glprog_open();
// A renderer going (the context bound): what is new written, and all let go
void glprog_close();
/* What is new written to the file now (the context bound, between frames).
 * @contextGoing: the programs are the context's and go with it; nothing
 * is asked of them after this. */
void glprog_save(bool contextGoing = false);
/* The renderer's start is done: a program made after this one is made in a
 * frame, and counted as one (glprog_log()). */
void glprog_started();

// whether the programs are kept on disk (binaries)
bool glprog_binaries();
// whether the driver makes programs in the background, as far as was seen
bool glprog_background();

/* A program of a vertex and a fragment shader, its attributes bound as the
 * renderers have them (gles.h). From the file if it is there, else compiled
 * and linked. @background: not waited for (the driver's background
 * compile; glprog_poll() says when it is done). 0 if it failed (with
 * @background, only glprog_poll() knows). */
u32 glprog_make(const char *vertex, const char *fragment, bool background = false);
/* Whether the last glprog_make() without @background failed, and the
 * driver said why (then it gave 0) */
bool glprog_failed();
// A compute shader's program from its text, @count pieces of it; 0 if it failed
u32 glprog_make_compute(const char *const *pieces, u32 count);
// What a program made in the background is now
GlProgramStatus glprog_poll(u32 program);
// ...waited for (at the renderer's start, never in a frame): whether it linked
bool glprog_wait(u32 program);

// A key of @list drawn this run, kept for the next one
void glprog_note(GlProgramList list, u32 key);
// The keys of @list from earlier runs (until glprog_close())
const u32 *glprog_known(GlProgramList list, u32 *count);
// a polygon drawn with the general program, as its own was not there yet (counted only)
void glprog_general_used();
// how many programs were made, from where, and when
void glprog_log(const char *who);
