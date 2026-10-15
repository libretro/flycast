/* Modifier volumes per sample, without a stencil buffer: the state the
 * stencil passes keep for a pixel (gles/gldraw.cpp, SetMVS_Mode()) kept
 * in two bits per sample instead, by the same rules, so the compute
 * kernel, a fragment shader reading its tile's volume triangles and the
 * stencil path agree.
 *
 * Bit 1 is the volume being sent, bit 0 the summary: inside the volumes
 * closed so far. For each volume triangle (culled as its volume says
 * before it gets here), a sample is hit when it is inside the triangle
 * on the screen and the triangle is in front of what the sample holds,
 * 1/w greater. A hit flips bit 1 - or sets it, for an open volume or a
 * quad (a parameter in an inclusion or exclusion mode that is not its
 * volume's last). A parameter in an inclusion or exclusion mode ends the
 * volume, after its own triangles: for inclusion the sample is inside
 * when it was before or is in this volume, for exclusion when it was
 * before and is not in this one, and either way bit 1 starts again from
 * 0. (The stencil path does this by drawing the volume's triangles once
 * more; a sample none of them covers has bit 1 clear, and keeps bit 0.)
 * A polygon that takes shadows is shaded with its second parameters
 * where bit 0 is set. */
#ifndef PVR_MODVOL_H
#define PVR_MODVOL_H

#include "pvr_lane.h"

#define PVR_MV_INSIDE  1u   /* bit 0: inside the volumes closed so far */
#define PVR_MV_CURRENT 2u   /* bit 1: the volume being sent */

/* the volume's modes (its ISP word's DepthMode) */
#define PVR_MV_MODE_NORMAL    0u
#define PVR_MV_MODE_INCLUSION 1u
#define PVR_MV_MODE_EXCLUSION 2u

/* 1 if the volume's triangles are OR'ed rather than XOR'ed */
PVR_FN pvr_u32 pvr_modvol_or_mode(pvr_u32 volume_last, pvr_u32 mode)
{
   return (volume_last == 0u && mode != 0u) ? 1u : 0u;
}

/* 1 if a triangle at @tri_inv_w hits a sample at @depth_inv_w that it
 * covers on the screen (@inside, from pvr_edge_inside() on all three
 * edges) */
PVR_FN pvr_u32 pvr_modvol_hit(pvr_u32 inside, pvr_f32 tri_inv_w, pvr_f32 depth_inv_w)
{
   return (inside != 0u && tri_inv_w > depth_inv_w) ? 1u : 0u;
}

/* a sample's state after a triangle */
PVR_FN pvr_u32 pvr_modvol_triangle(pvr_u32 state, pvr_u32 hit, pvr_u32 or_mode)
{
   pvr_u32 bit = hit != 0u ? PVR_MV_CURRENT : 0u;
   return or_mode != 0u ? (state | bit) : (state ^ bit);
}

/* a sample's state after the triangles of a parameter in mode @mode:
 * unchanged unless it ends a volume */
PVR_FN pvr_u32 pvr_modvol_close(pvr_u32 state, pvr_u32 mode)
{
   pvr_u32 inside = state & PVR_MV_INSIDE;
   pvr_u32 current = (state >> 1) & 1u;
   if (mode == PVR_MV_MODE_INCLUSION)
      return inside | current;
   if (mode == PVR_MV_MODE_EXCLUSION)
      return inside & (current ^ 1u);
   return state;
}

/* 1 if a sample is shaded with the second parameters */
PVR_FN pvr_u32 pvr_modvol_shadowed(pvr_u32 state)
{
   return state & PVR_MV_INSIDE;
}

#endif
