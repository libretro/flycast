/* The PowerVR2's X and Y scalers, the arithmetic of the chip's tile
 * write-out to video memory (SCALER_CTL, Y_COEFF; the Dreamcast System
 * Architecture manual, 3.4.11): one text for the reference software
 * renderer (tools/rend/refsw), the CPU's reference of the compute renderer
 * (tools/rend/tile_ref.c) and its kernels, read the three ways
 * pvr_lane.h says.
 *
 * The scalers work on the accumulation buffer's 8-bit ARGB, X first then
 * Y, before the framebuffer format's packing and dither. What the manual
 * leaves unsaid - the rounding - is taken here, in one place each, and is
 * not confirmed on the hardware. */
#ifndef PVR_SCALER_H
#define PVR_SCALER_H

#include "pvr_resolve.h"

/* The X scaler (SCALER_CTL bit 16): two adjacent pixels, each weighted
 * 0.5, made one. Rounded half up, as the resolve's box rounds: so that the
 * pair is one more step of the same box filter (the compute renderer
 * resolves a pair's samples together) */
PVR_FN pvr_u32 pvr_scaler_x(pvr_u32 a, pvr_u32 b)
{
   return pvr_resolve_join(pvr_resolve_rb(a) + pvr_resolve_rb(b), pvr_resolve_ga(a) + pvr_resolve_ga(b), 1u);
}

/* 1 when the Y scaler filters: it does only when the vertical scale
 * factor (SCALER_CTL 15:0, 6.10 fixed point, the picture scaled by its
 * inverse) reduces, as 0x401 does, the factor the manual gives for
 * filtering at the drawn resolution (its flicker-free type A) */
PVR_FN pvr_u32 pvr_scaler_yfilter(pvr_u32 scaler_ctl)
{
   return (scaler_ctl & 0xffffu) > 0x400u ? 1u : 0u;
}

/* The Y scaler's filter: a line from its three-line buffer, each channel
 * the line itself times coefficient 1 (Y_COEFF 15:8) plus the lines above
 * and below times coefficient 0/2 (Y_COEFF 7:0), each coefficient n/256
 * (the manual's example: 0x80 is 0.5, 0x40 0.25). Rounded to nearest, and
 * held at 255 where coefficients summing over 256 would go past it */
PVR_FN pvr_u32 pvr_scaler_y(pvr_u32 above, pvr_u32 line, pvr_u32 below, pvr_u32 ycoeff)
{
   pvr_u32 c0 = ycoeff & 255u, c1 = (ycoeff >> 8) & 255u, out = 0u, s, v;
   for (s = 0u; s < 32u; s += 8u)
   {
      v = (c0 * (((above >> s) & 255u) + ((below >> s) & 255u)) + c1 * ((line >> s) & 255u) + 128u) >> 8;
      out |= (v > 255u ? 255u : v) << s;
   }
   return out;
}

#endif
