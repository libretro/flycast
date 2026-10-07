/* The renderers' transforms (core/rend/xform.h) against the 4x4 matrices
 * they stand for.
 *
 *   sh tools/rend/xform_test.sh
 *
 * A transform here is a scale and a move; it used to be a 4x4 matrix from
 * a matrix library, and the claim is that nothing but multiplying by 1 and
 * adding 0 was left out. So: the same transforms built both ways - as
 * xforms, and as full matrices multiplied out the long way below - have to
 * hold the same floats, and take a point to the same place. Built as C89,
 * which the header has to stay. */
#include <stdio.h>
#include <string.h>
#include "xform.h"

typedef struct mat4 { float m[4][4]; } mat4;   /* m[column][row] */

static void mat4_from(mat4 *o, float sx, float sy, float sz, float tx, float ty, float tz)
{
   memset(o, 0, sizeof(*o));
   o->m[0][0] = sx;
   o->m[1][1] = sy;
   o->m[2][2] = sz;
   o->m[3][0] = tx;
   o->m[3][1] = ty;
   o->m[3][2] = tz;
   o->m[3][3] = 1.0f;
}

/* the whole product, every term of it */
static void mat4_mul(mat4 *o, const mat4 *a, const mat4 *b)
{
   mat4 r;
   int c, row;

   for (c = 0; c < 4; c++)
      for (row = 0; row < 4; row++)
         r.m[c][row] = a->m[0][row] * b->m[c][0] + a->m[1][row] * b->m[c][1]
                     + a->m[2][row] * b->m[c][2] + a->m[3][row] * b->m[c][3];
   *o = r;
}

static unsigned long rs = 0x2545F491UL;

static float rnd(void)
{
   rs ^= (rs << 13) & 0xFFFFFFFFUL;
   rs ^= rs >> 17;
   rs ^= (rs << 5) & 0xFFFFFFFFUL;
   /* -8 to 8 in steps that are not round */
   return (float)((long)(rs & 0xFFFF) - 32768) / 4099.0f;
}

int main(void)
{
   long n, bad = 0;

   for (n = 0; n < 200000; n++)
   {
      xform a, b, c, ab, abc;
      mat4 ma, mb, mc, mab, mabc;
      float got[16], x, y;
      int i;

      xform_set(&a, rnd(), rnd(), rnd(), rnd(), rnd(), rnd());
      xform_set(&b, rnd(), rnd(), rnd(), rnd(), rnd(), rnd());
      xform_set(&c, rnd(), rnd(), rnd(), rnd(), rnd(), rnd());
      mat4_from(&ma, a.sx, a.sy, a.sz, a.tx, a.ty, a.tz);
      mat4_from(&mb, b.sx, b.sy, b.sz, b.tx, b.ty, b.tz);
      mat4_from(&mc, c.sx, c.sy, c.sz, c.tx, c.ty, c.tz);

      xform_mul(&ab, &a, &b);
      xform_mul(&abc, &ab, &c);
      /* and into one of its own arguments */
      xform_mul(&c, &ab, &c);
      mat4_mul(&mab, &ma, &mb);
      mat4_mul(&mabc, &mab, &mc);

      xform_to_mat4(&abc, got);
      for (i = 0; i < 16; i++)
         if (got[i] != mabc.m[i / 4][i % 4])
            bad++;
      if (memcmp(&c, &abc, sizeof(c)))
         bad++;

      x = rnd() * 80.0f;
      y = rnd() * 30.0f;
      if (xform_x(&abc, x) != mabc.m[0][0] * x + mabc.m[1][0] * y + mabc.m[2][0] * 0.0f + mabc.m[3][0] * 1.0f)
         bad++;
      if (xform_y(&abc, y) != mabc.m[0][1] * x + mabc.m[1][1] * y + mabc.m[2][1] * 0.0f + mabc.m[3][1] * 1.0f)
         bad++;
      if (xform_w(&abc, x) != mabc.m[0][0] * x + mabc.m[1][0] * y + mabc.m[2][0] * 0.0f + mabc.m[3][0] * 0.0f)
         bad++;
      if (xform_h(&abc, y) != mabc.m[0][1] * x + mabc.m[1][1] * y + mabc.m[2][1] * 0.0f + mabc.m[3][1] * 0.0f)
         bad++;
   }
   if (bad)
   {
      printf("FAIL: transforms: %ld values differ from the matrices'\n", bad);
      return 1;
   }
   printf("transforms: %ld built both ways, all the same\n", n);
   return 0;
}
