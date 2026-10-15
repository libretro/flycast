/* Where the samples of a pixel are under SSAA, and how many a renderer
 * can afford.
 *
 * The positions are the standard ones for 1, 2, 4 and 8 samples, which
 * Vulkan (standardSampleLocations) and Direct3D 11 and 12 (the standard
 * multisample pattern) give the same; a raster renderer gets them from
 * the hardware, the compute kernels and the tests from here. In 1/16 of
 * a pixel from its top left corner, four bits each, sample i in bits
 * 4i to 4i+3 of one word for x and one for y - so a shader reads them as
 * C does, with no table to upload.
 *
 * The output stays the Internal Resolution's grid; SSAA adds N samples to
 * each of its pixels. The count is decided when video starts (a change is
 * a change of resolution) against the budget the API reports, and never
 * grown later: ssaa_clamp_samples(). */
#ifndef SSAA_PATTERN_H
#define SSAA_PATTERN_H

#include "tile/pvr_lane.h"

#define SSAA_MAX_SAMPLES 8u

#define SSAA_PAT1_X 0x00000008u
#define SSAA_PAT1_Y 0x00000008u
#define SSAA_PAT2_X 0x0000004cu   /* (12,12) (4,4) */
#define SSAA_PAT2_Y 0x0000004cu
#define SSAA_PAT4_X 0x0000a2e6u   /* (6,2) (14,6) (2,10) (10,14) */
#define SSAA_PAT4_Y 0x0000ea62u
#define SSAA_PAT8_X 0xfb135d79u   /* (9,5) (7,11) (13,9) (5,3) (3,13) (1,7) (11,15) (15,1) */
#define SSAA_PAT8_Y 0x1f7d39b5u

/* the x or y word of the pattern of @n samples (1, 2, 4 or 8) */
#define SSAA_PAT_X(n) ((n) >= 8u ? SSAA_PAT8_X : (n) >= 4u ? SSAA_PAT4_X : (n) >= 2u ? SSAA_PAT2_X : SSAA_PAT1_X)
#define SSAA_PAT_Y(n) ((n) >= 8u ? SSAA_PAT8_Y : (n) >= 4u ? SSAA_PAT4_Y : (n) >= 2u ? SSAA_PAT2_Y : SSAA_PAT1_Y)

/* sample @i of @n, in 1/16 pixel */
#define SSAA_SAMPLE_X16(n, i) ((SSAA_PAT_X(n) >> (4u * (i))) & 15u)
#define SSAA_SAMPLE_Y16(n, i) ((SSAA_PAT_Y(n) >> (4u * (i))) & 15u)

/* log2 of a sample count of 1, 2, 4 or 8 */
#define SSAA_LOG2(n) ((n) >= 8u ? 3u : (n) >= 4u ? 2u : (n) >= 2u ? 1u : 0u)

/* KiB an image of @width x @height pixels takes at @bytes per pixel per
 * sample, for one sample, rounded up (saturated) */
PVR_FN pvr_u32 ssaa_per_sample_kib(pvr_u32 width, pvr_u32 height, pvr_u32 bytes)
{
   pvr_u32 px, b;
   if (width != 0u && height > 0xffffffffu / width)
      return 0xffffffffu;
   px = width * height;
   if (bytes != 0u && px > 0xffffffffu / bytes)
   {
      /* (in KiB first, then) */
      px = (px >> 10) + ((px & 1023u) != 0u ? 1u : 0u);
      if (px > 0xffffffffu / bytes)
         return 0xffffffffu;
      return px * bytes;
   }
   b = px * bytes;
   return (b >> 10) + ((b & 1023u) != 0u ? 1u : 0u);
}

/* The most samples - @requested (1, 2, 4 or 8) or fewer, by halving - for
 * which @fixed_kib plus @per_sample_kib for each sample fit the
 * @avail_kib the budget leaves. 1 when even that does not fit: SSAA is
 * then off, the renderer as it is without it. */
PVR_FN pvr_u32 ssaa_clamp_samples(pvr_u32 requested, pvr_u32 avail_kib, pvr_u32 fixed_kib, pvr_u32 per_sample_kib)
{
   pvr_u32 n = requested >= 8u ? 8u : requested >= 4u ? 4u : requested >= 2u ? 2u : 1u;
   pvr_u32 room;
   if (fixed_kib >= avail_kib)
      return 1u;
   room = avail_kib - fixed_kib;
   while (n > 1u && (per_sample_kib != 0u && n > room / per_sample_kib))
      n >>= 1;
   return n;
}

#endif
