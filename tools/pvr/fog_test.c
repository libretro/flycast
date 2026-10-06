/* The fog coefficient the shaders compute, against the hardware's.
 *
 *   sh tools/pvr/run.sh
 *
 * The PowerVR2 takes the fog depth (1/W times the fog density, kept between
 * 1 and just under 256) apart as a float. Three bits of exponent and the
 * top four bits of mantissa pick one of the 128 fog table entries; the next
 * eight bits of mantissa blend that entry's value with the following
 * entry's, in whole numbers out of 256. That is hardware(), written from
 * the description of the hardware and from what reicast's reference
 * renderer (refsw) does.
 *
 * shader() is the arithmetic of fog_mode2() in the OpenGL and Vulkan
 * shaders, in the single-precision floats a GPU does it in: no bit
 * operations, so that it also runs under GLSL ES 1.00. The two are
 * compared for every float there is between 1 and 256, about 67 million
 * of them, with several tables.
 *
 * before() is what the shaders did until now: log2() and pow() to split
 * the float, and the two values blended by the texture filter. It is here
 * to say how far off that was. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fog table as the registers hold it: byte 1 is the entry's value,
 * byte 0 the next entry's. */
static uint8_t table[128][2];

static int hardware(float z)
{
   uint32_t bits;
   unsigned index, blend;

   memcpy(&bits, &z, sizeof(bits));
   index = ((((bits >> 23) & 0xFF) + 1) & 7) << 4 | ((bits >> 19) & 15);
   blend = (bits >> 11) & 0xFF;
   return (table[index][0] * blend + table[index][1] * (255 ^ blend)) >> 8;
}

/* What a texture lookup of an 8-bit value returns, and what the shader
 * makes of it. */
static float texel(uint8_t v)
{
   return floorf((float)v / 255.0f * 255.0f + 0.5f);
}

static int shader(float z)
{
   float e = 0.0f, t, i, blend, next, cur;
   int index;

   if (z >= 16.0f) { z *= 0.0625f; e = 4.0f; }
   if (z >= 4.0f) { z *= 0.25f; e += 2.0f; }
   if (z >= 2.0f) { z *= 0.5f; e += 1.0f; }
   t     = (z - 1.0f) * 16.0f;
   i     = floorf(t);
   blend = floorf((t - i) * 256.0f);
   index = (int)(e * 16.0f + i);
   next  = texel(table[index][0]);
   cur   = texel(table[index][1]);
   /* the shader returns this over 256 */
   return (int)floorf((next * blend + cur * (255.0f - blend)) / 256.0f);
}

/* The coefficient out of 255, as the old shader's mix() used it. */
static double before(float z)
{
   double e = floor(log2((double)z));
   double m = (double)z * 16.0 / pow(2.0, e) - 16.0;
   int index = (int)(floor(m) + e * 16.0);
   double f = m - floor(m);

   if (index < 0)
      index = 0;
   if (index > 127)
      index = 127;
   return table[index][1] * (1.0 - f) + table[index][0] * f;
}

int main(void)
{
   static const char *names[] = { "ramp", "random", "steps" };
   unsigned which, i, seed = 12345;
   int failed = 0;

   for (which = 0; which < 3; which++)
   {
      uint32_t bits, first, last;
      float lo = 1.0f, hi = 255.999985f;
      unsigned long checked = 0, wrong = 0;
      double worst_before = 0.0;

      for (i = 0; i < 128; i++)
      {
         if (which == 0)
         {
            /* what a game sets: rising with depth, each entry's "next"
             * the one after it */
            table[i][1] = (uint8_t)(i * 2);
            table[i][0] = (uint8_t)(i < 127 ? (i + 1) * 2 : 255);
         }
         else if (which == 1)
         {
            seed = seed * 1664525u + 1013904223u;
            table[i][1] = (uint8_t)(seed >> 24);
            table[i][0] = (uint8_t)(seed >> 16);
         }
         else
         {
            table[i][1] = (i & 8) ? 255 : 0;
            table[i][0] = (i & 4) ? 255 : 0;
         }
      }

      memcpy(&first, &lo, sizeof(first));
      memcpy(&last, &hi, sizeof(last));
      for (bits = first; bits <= last; bits++)
      {
         float z;
         int hw;

         memcpy(&z, &bits, sizeof(z));
         hw = hardware(z);
         if (shader(z) != hw)
            wrong++;
         /* every 97th float is plenty to find the old method's worst */
         if (bits % 97 == 0)
         {
            double d = fabs(before(z) * 256.0 / 255.0 - (double)hw);
            if (d > worst_before)
               worst_before = d;
         }
         checked++;
      }
      printf("%-6s table: %lu depths, %lu differ from the hardware "
            "(the old shader was off by up to %.2f of 256)\n",
            names[which], checked, wrong, worst_before);
      if (wrong)
         failed = 1;
   }
   puts(failed ? "fog: FAILED" : "fog: ok");
   return failed;
}
