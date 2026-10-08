/* The place of a texture's mipmap levels in the memory they are uploaded
 * from (core/rend/upload_layout.h), for every texel size and every number
 * of levels a texture can have: each level has to be found where a copy to
 * an image may start - a multiple of four bytes and of the texel's size -
 * with the bytes the texture cache converted for it, inside the piece.
 *
 * To run: tools/rend/upload_layout_test.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upload_layout.h"

static int failures;

static void check(int ok, const char *what, unsigned pixel, unsigned levels, unsigned level)
{
   if (ok)
      return;
   printf("FAIL: %s (texels of %u bytes, %u levels, level %u)\n", what, pixel, levels, level);
   failures++;
}

int main(void)
{
   static const unsigned pixels[3] = { 1, 2, 4 };
   unsigned p, levels, checks = 0;

   for (p = 0; p < 3; p++)
   {
      const unsigned pixel = pixels[p];
      const unsigned pad   = upload_levels_pad(pixel);

      /* up to 2048x2048: twelve levels */
      for (levels = 1; levels <= 12; levels++)
      {
         size_t total = 0, at = 0, i;
         unsigned char *converted, *piece;
         unsigned level;

         for (level = 0; level < levels; level++)
            total += (size_t)pixel << (2 * level);
         converted = (unsigned char *)malloc(total);
         piece     = (unsigned char *)malloc(pad + total);
         if (!converted || !piece)
            return 2;
         /* the levels as the cache converts them, every byte telling its place */
         for (i = 0; i < total; i++)
            converted[i] = (unsigned char)(i * 131 + (i >> 8) * 17 + 5);
         /* into the piece as the upload has them, and the first texel to the start */
         memset(piece, 0xEE, pad);
         memcpy(piece + pad, converted, total);
         if (pad != 0)
            memcpy(piece, piece + pad, pixel);

         for (level = 0; level < levels; level++)
         {
            const size_t size  = (size_t)pixel << (2 * level);
            const size_t where = upload_level_at(pixel, level);

            check(where % 4 == 0, "a level does not start on a multiple of four bytes", pixel, levels, level);
            check(where % pixel == 0, "a level does not start on a multiple of its texel", pixel, levels, level);
            check(where + size <= pad + total, "a level runs past the end of the piece", pixel, levels, level);
            check(where + size <= pad + total && !memcmp(piece + where, converted + at, size),
                  "a level's bytes are not the ones converted for it", pixel, levels, level);
            at += size;
            checks += 4;
         }
         free(converted);
         free(piece);
      }
   }
   if (failures)
      return 1;
   printf("upload layout: ok (%u checks: texels of 1, 2 and 4 bytes, 1 to 12 levels)\n", checks);
   return 0;
}
