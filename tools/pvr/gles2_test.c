/* The core's OpenGL ES 2 shaders, compiled and run on an OpenGL ES 2
 * context.
 *
 *   sh tools/pvr/run.sh
 *
 * OpenGL ES 2 is the one path of the OpenGL renderer that does things
 * differently in the shaders: depth goes through the fixed-function depth
 * range instead of gl_FragDepth, the fog depth reaches the fragment shader
 * as a varying, and there are no integers to split a float with. This
 * compiles the shaders on whatever OpenGL ES 2 driver EGL gives (Mesa's
 * software one will do) and checks fog across a polygon: a quad whose
 * depth runs from 1 to 5 is drawn white, with black fog and a fog table
 * that rises steadily, and every pixel must come out with the fog
 * coefficient the hardware has for the depth at that pixel. The varying
 * used to carry depth itself, which perspective-correct interpolation gets
 * wrong inside a polygon. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "gles2_shaders.h"

#define W 256
#define H 8

static int failures;

#define CHECK(cond, what) do { \
   if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

static uint8_t table[128][2];

/* The hardware's fog coefficient, out of 256: see fog_test.c. */
static int hardware(float z)
{
   uint32_t bits;
   unsigned index, blend;

   if (z < 1.0f)
      z = 1.0f;
   if (z > 255.9999f)
      z = 255.9999f;
   memcpy(&bits, &z, sizeof(bits));
   index = ((((bits >> 23) & 0xFF) + 1) & 7) << 4 | ((bits >> 19) & 15);
   blend = (bits >> 11) & 0xFF;
   return (table[index][0] * blend + table[index][1] * (255 ^ blend)) >> 8;
}

static GLuint compile(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   GLint ok = 0;

   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[2048];
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      printf("FAIL: the %s shader does not compile:\n%s\n",
            type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
      exit(1);
   }
   return shader;
}

struct vertex
{
   float x, y, z;
   uint8_t base[4];
   uint8_t offs[4];
   float u, v;
};

/* A quad over the whole target, at depth z0 on the left and z1 on the
 * right. */
static void draw_quad(float z0, float z1, uint8_t r, uint8_t g, uint8_t b)
{
   struct vertex q[4];
   int i;

   memset(q, 0, sizeof(q));
   q[0].x = 0; q[0].y = 0; q[0].z = z0;
   q[1].x = W; q[1].y = 0; q[1].z = z1;
   q[2].x = 0; q[2].y = H; q[2].z = z0;
   q[3].x = W; q[3].y = H; q[3].z = z1;
   for (i = 0; i < 4; i++)
   {
      q[i].base[0] = r; q[i].base[1] = g; q[i].base[2] = b; q[i].base[3] = 255;
   }
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(q[0]), &q[0].x);
   glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(q[0]), q[0].base);
   glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(q[0]), q[0].offs);
   glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(q[0]), &q[0].u);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int main(void)
{
   static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   static const EGLint surface_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
   /* Screen coordinates to clip space; depth is left as it is. */
   static const GLfloat matrix[16] = {
      2.0f / W, 0, 0, 0,   0, 2.0f / H, 0, 0,   0, 0, 1, 0,   -1, -1, 0, 1 };
   EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLConfig config;
   EGLint count = 0;
   EGLSurface surface;
   EGLContext context;
   GLuint program, fbo, colour, depth, fog;
   uint8_t fog_pixels[256], row[W * 4];
   int i, x, worst = 0;

   if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)
         || !eglBindAPI(EGL_OPENGL_ES_API)
         || !eglChooseConfig(display, config_attribs, &config, 1, &count) || count < 1)
   {
      puts("FAIL: no EGL display with an OpenGL ES 2 configuration");
      return 1;
   }
   surface = eglCreatePbufferSurface(display, config, surface_attribs);
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
   if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
   {
      puts("FAIL: no OpenGL ES 2 context");
      return 1;
   }
   printf("%s, %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));

   /* 256x8, with a depth buffer */
   glGenTextures(1, &colour);
   glBindTexture(GL_TEXTURE_2D, colour);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenRenderbuffers(1, &depth);
   glBindRenderbuffer(GL_RENDERBUFFER, depth);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
   CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer");
   glViewport(0, 0, W, H);

   /* The core's shaders, bound the way the core binds them */
   program = glCreateProgram();
   glAttachShader(program, compile(GL_VERTEX_SHADER, vertex_source));
   glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment_source));
   glBindAttribLocation(program, 0, "in_pos");
   glBindAttribLocation(program, 1, "in_base");
   glBindAttribLocation(program, 2, "in_offs");
   glBindAttribLocation(program, 3, "in_uv");
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &i);
   CHECK(i, "the program links");
   glUseProgram(program);
   for (i = 0; i < 4; i++)
      glEnableVertexAttribArray(i);

   /* A fog table that rises by two each entry, as a texture the way the
    * core makes it: the next entry's value in the first row, the entry's
    * own in the second. OpenGL ES 2 takes it as GL_ALPHA. */
   for (i = 0; i < 128; i++)
   {
      table[i][1] = (uint8_t)(i * 2);
      table[i][0] = (uint8_t)(i < 127 ? (i + 1) * 2 : 255);
      fog_pixels[i] = table[i][0];
      fog_pixels[i + 128] = table[i][1];
   }
   glActiveTexture(GL_TEXTURE1);
   glGenTextures(1, &fog);
   glBindTexture(GL_TEXTURE_2D, fog);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 128, 2, 0, GL_ALPHA, GL_UNSIGNED_BYTE, fog_pixels);

   glUniformMatrix4fv(glGetUniformLocation(program, "normal_matrix"), 1, GL_FALSE, matrix);
   glUniform1f(glGetUniformLocation(program, "sp_FOG_DENSITY"), 1.0f);
   glUniform3f(glGetUniformLocation(program, "sp_FOG_COL_RAM"), 0.0f, 0.0f, 0.0f);
   glUniform1i(glGetUniformLocation(program, "fog_table"), 1);
   glUniform1i(glGetUniformLocation(program, "tex"), 0);

   /* --- Fog across a polygon: depth 1 on the left, 5 on the right, in a
    * depth range that reaches to 8. */
   glUniform4f(glGetUniformLocation(program, "depth_scale"), 2.0f / 8.0f, -1.0f, 0.0f, 0.0f);
   glDisable(GL_DEPTH_TEST);
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   draw_quad(1.0f, 5.0f, 255, 255, 255);
   glReadPixels(0, H / 2, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
   for (x = 0; x < W; x++)
   {
      const float z = 1.0f + 4.0f * ((float)x + 0.5f) / W;
      /* white, less 1/256, less the fog */
      const int expect = (255 * (255 - hardware(z)) + 128) / 256;
      const int diff = abs((int)row[x * 4] - expect);

      if (diff > worst)
         worst = diff;
   }
   printf("fog across a polygon: off by at most %d of 255\n", worst);
   CHECK(worst <= 2, "the fog at each pixel is the fog for the depth at that pixel");

   puts(failures ? "gles2: FAILED" : "gles2: ok");
   return failures ? 1 : 0;
}
