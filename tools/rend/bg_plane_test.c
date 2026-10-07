/* The background plane's arithmetic (core/hw/pvr/bg_plane.h), on its own.
 *
 *   sh tools/rend/bg_plane_test.sh
 *
 * Built as C89, which the header has to stay. */
#include <stdio.h>
#include "bg_plane.h"

static int failed;

static void near(const char *what, float got, float want)
{
   float d = got - want;
   if (d < 0)
      d = -d;
   if (!(d < 1e-4f))
   {
      printf("FAIL: %s: %f, not %f\n", what, got, want);
      failed = 1;
   }
}

static void is(const char *what, int got, int want)
{
   if (got != want)
   {
      printf("FAIL: %s: %d, not %d\n", what, got, want);
      failed = 1;
   }
}

int main(void)
{
   bg_plane p;
   float zero = 0.0f;

   /* three corners of the screen, the picture once across and once down:
    * what the fixed mapping there used to be gave, 0.4 of the width more
    * on either side for the 256 pixels drawn past each edge */
   is("corners are a plane", bg_plane_init(&p, 0, 0, 640, 0, 0, 480), 1);
   near("u at the left edge drawn", bg_plane_at(&p, 0, 1, 0, -256, 0), -0.4f);
   near("u at the right edge drawn", bg_plane_at(&p, 0, 1, 0, 896, 480), 1.4f);
   near("v at the top", bg_plane_at(&p, 0, 0, 1, 896, 0), 0.0f);
   near("v at the bottom", bg_plane_at(&p, 0, 0, 1, -256, 480), 1.0f);

   /* the third vertex in the other bottom corner: the same picture */
   is("other corner is a plane", bg_plane_init(&p, 0, 0, 640, 0, 640, 480), 1);
   near("other corner: u left", bg_plane_at(&p, 0, 1, 1, -256, 480), -0.4f);
   near("other corner: u right", bg_plane_at(&p, 0, 1, 1, 896, 0), 1.4f);
   near("other corner: v bottom", bg_plane_at(&p, 0, 0, 1, -256, 480), 1.0f);

   /* the picture the other way round stays the other way round */
   is("mirrored is a plane", bg_plane_init(&p, 0, 0, 640, 0, 0, 480), 1);
   near("mirrored: u at 0", bg_plane_at(&p, 1, 0, 1, 0, 240), 1.0f);
   near("mirrored: u at 640", bg_plane_at(&p, 1, 0, 1, 640, 240), 0.0f);

   /* vertices over a quarter of the screen: the plane goes on past them */
   is("quarter is a plane", bg_plane_init(&p, 0, 0, 320, 0, 0, 240), 1);
   near("quarter: u at the far corner", bg_plane_at(&p, 0, 1, 0, 640, 480), 2.0f);
   near("quarter: v at the far corner", bg_plane_at(&p, 0, 0, 1, 640, 480), 2.0f);

   /* colours: to the nearest step, and no further than black and white */
   is("gradient is a plane", bg_plane_init(&p, 0, 0, 640, 0, 0, 480), 1);
   is("colour at the top", bg_plane_colour(&p, 0, 0, 255, 320, 0), 0);
   is("colour at the bottom", bg_plane_colour(&p, 0, 0, 255, 896, 480), 255);
   is("colour half way", bg_plane_colour(&p, 0, 0, 255, 0, 240), 128);
   is("colour past white", bg_plane_colour(&p, 0, 255, 0, 896, 0), 255);
   is("colour past black", bg_plane_colour(&p, 0, 255, 0, -256, 0), 0);
   is("a flat colour stays", bg_plane_colour(&p, 77, 77, 77, 896, 480), 77);

   /* not a plane */
   is("three on a line", bg_plane_init(&p, 0, 0, 320, 0, 640, 0), 0);
   is("one point", bg_plane_init(&p, 5, 5, 5, 5, 5, 5), 0);
   is("not a number", bg_plane_init(&p, zero / zero, 0, 640, 0, 0, 480), 0);

   if (!failed)
      printf("background plane: all as expected\n");
   return failed;
}
