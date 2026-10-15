/* The resolve of a pixel's samples to its colour, the one both renderers
 * use - the compute kernel at the end of a tile, the raster path in its
 * last subpass - so the two give the same pixel to the bit. A fixed
 * function MSAA resolve rounds as the implementation likes, and is not
 * used.
 *
 * A box filter in unorm8, per channel: out = (sum + N/2) / N, N = 2^k
 * samples, in integers. The four channels of an RGBA8 word are summed
 * two at a time in 16-bit fields of a 32-bit word (red and blue, green
 * and alpha): eight samples sum to at most 8 x 255 = 2040, so a field
 * never carries into the next, and the division by N is a shift.
 *
 * A pixel whose N samples are all equal is uniform: it resolves to that
 * value exactly, and counts as single-sampled - none of its samples are
 * kept apart from it.
 *
 * The dither of the console's framebuffer formats comes after this, on
 * the resolved colour, never per sample. */
#ifndef PVR_RESOLVE_H
#define PVR_RESOLVE_H

#include "pvr_lane.h"

/* the red and blue bytes of an RGBA8 word, each in a 16-bit field */
PVR_FN pvr_u32 pvr_resolve_rb(pvr_u32 c)
{
   return c & 0x00ff00ffu;
}

/* the green and alpha bytes, the same way */
PVR_FN pvr_u32 pvr_resolve_ga(pvr_u32 c)
{
   return (c >> 8) & 0x00ff00ffu;
}

/* The resolved colour from the sums of pvr_resolve_rb() and
 * pvr_resolve_ga() over 2^@log2n samples (@log2n 0 to 3) */
PVR_FN pvr_u32 pvr_resolve_join(pvr_u32 rb_sum, pvr_u32 ga_sum, pvr_u32 log2n)
{
   pvr_u32 bias = ((1u << log2n) >> 1) * 0x00010001u;
   pvr_u32 rb = ((rb_sum + bias) >> log2n) & 0x00ff00ffu;
   pvr_u32 ga = ((ga_sum + bias) >> log2n) & 0x00ff00ffu;
   return rb | (ga << 8);
}

#if defined(LANE_SHUFFLE_XOR)

/* The resolved colour of the pixel this lane holds a sample of, for every
 * lane of it: its 2^@log2n samples are in adjacent lanes, starting at a
 * multiple of 2^@log2n. A butterfly, for D3D12 and GL have no clustered
 * reductions. */
pvr_u32 pvr_resolve_lanes(pvr_u32 c, pvr_u32 log2n)
{
   pvr_u32 rb = pvr_resolve_rb(c);
   pvr_u32 ga = pvr_resolve_ga(c);
   pvr_u32 m;
   for (m = 1u; m < (1u << log2n); m <<= 1u)
   {
      rb += LANE_SHUFFLE_XOR(rb, m);
      ga += LANE_SHUFFLE_XOR(ga, m);
   }
   return pvr_resolve_join(rb, ga, log2n);
}

/* 1 if all the samples of this lane's pixel are equal, 0 if not */
pvr_u32 pvr_resolve_lanes_uniform(pvr_u32 c, pvr_u32 log2n)
{
   pvr_u32 differ = 0u;
   pvr_u32 m;
   for (m = 1u; m < (1u << log2n); m <<= 1u)
   {
      differ |= (c != LANE_SHUFFLE_XOR(c, m)) ? 1u : 0u;
      differ |= LANE_SHUFFLE_XOR(differ, m);
   }
   return differ ^ 1u;
}

#elif !defined(PVR_SHADER_GLSL) && !defined(PVR_SHADER_HLSL)

/* The same, in C, over the 2^@log2n samples of one pixel */
PVR_FN pvr_u32 pvr_resolve_samples(const pvr_u32 *s, pvr_u32 log2n)
{
   pvr_u32 rb = 0, ga = 0, i;
   for (i = 0; i < (1u << log2n); i++)
   {
      rb += pvr_resolve_rb(s[i]);
      ga += pvr_resolve_ga(s[i]);
   }
   return pvr_resolve_join(rb, ga, log2n);
}

PVR_FN pvr_u32 pvr_resolve_samples_uniform(const pvr_u32 *s, pvr_u32 log2n)
{
   pvr_u32 i;
   for (i = 1; i < (1u << log2n); i++)
      if (s[i] != s[0])
         return 0;
   return 1;
}

#endif

#endif
