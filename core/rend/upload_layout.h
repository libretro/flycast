/* Where a texture's mipmap levels are in the memory they are uploaded from.
 *
 * The texture cache converts the levels one after another from the
 * smallest, 1x1 first (PixelBuffer). Vulkan copies a level to its image
 * from a place in a buffer that has to be a multiple of four bytes, and of
 * the texel's size. With four-byte texels every level starts at one. With
 * two-byte texels the 1x1 level is two bytes and puts all the others out by
 * two; with one-byte texels, by one - (4^n - 1) / 3 is one more than a
 * multiple of four.
 *
 * So the levels are converted, or copied, a few bytes into their piece of
 * memory - upload_levels_pad() - which puts every level but the 1x1 one on
 * a multiple of four, and the 1x1 level's one texel is then copied to the
 * start of the piece, where it is on one too. No level is moved, and the
 * texture is not copied level by level to line them up, as it used to be. */
#ifndef REND_UPLOAD_LAYOUT_H
#define REND_UPLOAD_LAYOUT_H

#include <stddef.h>
#include <retro_inline.h>

/* Bytes to leave in front of the levels of a texture whose texels are
 * @pixel bytes (1, 2 or 4) */
static INLINE unsigned upload_levels_pad(unsigned pixel)
{
   return (4 - pixel) & 3;
}

/* Where level @level - 0 is the 1x1 level, 1 the 2x2 - is copied from, from
 * the start of the piece: once the levels are upload_levels_pad() in and
 * the first texel has been copied to the start */
static INLINE size_t upload_level_at(unsigned pixel, unsigned level)
{
   size_t at = upload_levels_pad(pixel);
   unsigned i;

   if (level == 0)
      return 0;
   for (i = 0; i < level; i++)
      at += (size_t)pixel << (2 * i);
   return at;
}

#endif
