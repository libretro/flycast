/* The rules of tiled rendering that both renderers and the TA decoder
 * share: which primitives are drawn at a single rate, which pixels and
 * samples a primitive covers, the order of translucent fragments, the
 * sizes of tiles, chunks and the bin pool, and the region array's table
 * of tiles. C89, C++ and shaders alike: see pvr_lane.h. */
#ifndef PVR_TILE_H
#define PVR_TILE_H

#include "pvr_lane.h"

/* ---- Single rate ----
 *
 * A strip is drawn at a single rate when it came from a sprite, or when
 * it is flat: every vertex at the same depth (1/w), which is how the HUD
 * and 2D layers are sent. It is shaded once per pixel, not per sample;
 * a sprite is also covered per pixel (pvr_snap_edge()), so it comes out
 * the same with SSAA on or off. The background plane is one too.
 *
 * The decision is the TA decoder's, one bit in the polygon's control
 * word: the lowest of the eight bits the chip leaves reserved there. The
 * decoder sets it at the end of every strip, whatever the game had in
 * that bit, and nothing reads the reserved bits for anything else. */
#define PVR_PCW_SINGLE_RATE 0x00000100u

/* Flatness without a test per vertex: the AND and the OR of the bits of
 * all a strip's depths are the same exactly when every depth is the
 * same. The two start at these, and a strip with no vertex is not flat. */
#define PVR_FLAT_AND_INIT 0xffffffffu
#define PVR_FLAT_OR_INIT  0x00000000u
#define PVR_FLAT_RATE_BIT(z_and, z_or) ((z_and) == (z_or) ? PVR_PCW_SINGLE_RATE : 0u)

/* ---- Coverage ----
 *
 * A pixel is covered where its centre is, by the top-left rule: on an
 * edge shared by two primitives, it belongs to exactly one of them.
 *
 * Sprites, axis-aligned: pixel i is covered when x0 <= i + 0.5 < x1, so
 * the covered pixels are pvr_snap_edge(x0) <= i < pvr_snap_edge(x1), and
 * the same in y. Snapped to those whole edges, every sample of a covered
 * pixel is covered and no sample of another. */
PVR_FN pvr_s32 pvr_snap_edge(pvr_f32 x)
{
   /* (kept to where a float converts to an int; a NaN is the far left) */
   pvr_f32 c = !(x >= -1048576.0f) ? -1048576.0f : (x > 1048576.0f ? 1048576.0f : x);
   return PVR_S32(PVR_CEIL(c - 0.5f));
}

/* Triangles, in fixed point (sub-pixel or sub-sample units), screen y
 * down. The edge from a to b, at p:
 *    E = (bx - ax) (py - ay) - (by - ay) (px - ax)
 * which is positive inside a triangle whose vertices go clockwise on the
 * screen (setup swaps two of the others'). The products fit in 32 bits
 * while every coordinate difference is under 2^15 in size: setup gives
 * coordinates relative to the tile. */
PVR_FN pvr_s32 pvr_edge_eval(pvr_s32 ax, pvr_s32 ay, pvr_s32 bx, pvr_s32 by, pvr_s32 px, pvr_s32 py)
{
   return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* 1 if the edge going (@dx, @dy) is a top or a left edge of such a
 * triangle: a top edge is horizontal with the inside below, a left edge
 * has the inside to its right, going up. */
PVR_FN pvr_u32 pvr_edge_top_left(pvr_s32 dx, pvr_s32 dy)
{
   return (dy < 0 || (dy == 0 && dx > 0)) ? 1u : 0u;
}

/* 1 if a point with edge value @e is inside the edge (@dx, @dy): beyond
 * it, or on it when it is a top or left edge */
PVR_FN pvr_u32 pvr_edge_inside(pvr_s32 e, pvr_s32 dx, pvr_s32 dy)
{
   return (e > 0 || (e == 0 && pvr_edge_top_left(dx, dy) != 0u)) ? 1u : 0u;
}

/* ---- Translucent fragment order ----
 *
 * A fragment kept for the per-pixel sort is one 64-bit key, high word
 * first: depth bits : 32 | primitive index : 16 | volume parity : 1 |
 * flags : 15 (two 32-bit words, for the shaders have no 64-bit type).
 * A positive float's bits sort as unsigned integers, so the keys in
 * ascending order are the fragments from the farthest (smallest 1/w) to
 * the nearest, and fragments at the same depth in the order their
 * primitives were sent. For the nearest first, the depth bits are
 * inverted and the order of equal depths stays the same. A negative or
 * negative-zero depth counts as zero. */
PVR_FN pvr_u32 pvr_key_hi(pvr_f32 inv_w, pvr_u32 nearest_first)
{
   pvr_u32 bits = PVR_F32_BITS(inv_w);
   if ((bits & 0x80000000u) != 0u)
      bits = 0u;
   return nearest_first != 0u ? ~bits : bits;
}

PVR_FN pvr_u32 pvr_key_lo(pvr_u32 prim, pvr_u32 parity, pvr_u32 flags)
{
   return ((prim & 0xffffu) << 16) | ((parity & 1u) << 15) | (flags & 0x7fffu);
}

/* 1 if key a comes before key b */
PVR_FN pvr_u32 pvr_key_before(pvr_u32 a_hi, pvr_u32 a_lo, pvr_u32 b_hi, pvr_u32 b_lo)
{
   return (a_hi < b_hi || (a_hi == b_hi && a_lo < b_lo)) ? 1u : 0u;
}

#define PVR_KEY_PRIM(lo)   (((lo) >> 16) & 0xffffu)
#define PVR_KEY_PARITY(lo) (((lo) >> 15) & 1u)
#define PVR_KEY_FLAGS(lo)  ((lo) & 0x7fffu)

/* ---- Tiles, chunks and the bin pool ----
 *
 * A coarse tile is 2^log2 pixels square, 16 to 64: 32 is the chip's own.
 * Primitives are binned by 16-bit indexes relative to a chunk of 65536
 * of them; a frame with more is walked a chunk at a time. The bin pool is
 * one fixed array of 16-bit entries, each tile's run at its offset from a
 * prefix sum of the counts: a tile whose run does not fit is marked and
 * binned again in another pass over the same pool. Nothing grows. */
#define PVR_TILE_LOG2_MIN 4u
#define PVR_TILE_LOG2_MAX 6u
#define PVR_TILE_LOG2_CHIP 5u
#define PVR_CHUNK_LOG2 16u
#define PVR_CHUNK_PRIMS 65536u

/* tiles across @pixels */
PVR_FN pvr_u32 pvr_tiles_across(pvr_u32 pixels, pvr_u32 log2)
{
   return (pixels >> log2) + ((pixels & ((1u << log2) - 1u)) != 0u ? 1u : 0u);
}

/* chunks @prims primitives make */
PVR_FN pvr_u32 pvr_chunk_count(pvr_u32 prims)
{
   return prims == 0u ? 0u : ((prims - 1u) >> PVR_CHUNK_LOG2) + 1u;
}

/* a primitive's chunk, and its 16-bit index in it */
#define PVR_CHUNK_OF(prim)    ((prim) >> PVR_CHUNK_LOG2)
#define PVR_CHUNK_INDEX(prim) ((prim) & (PVR_CHUNK_PRIMS - 1u))

/* 1 if a run of @count entries at @offset fits a pool of @pool entries
 * (with no wrap-around of the sum) */
PVR_FN pvr_u32 pvr_bin_fits(pvr_u32 offset, pvr_u32 count, pvr_u32 pool)
{
   return (count <= pool && offset <= pool - count) ? 1u : 0u;
}

/* Bytes the bins of @tiles tiles take when every one of @prims
 * primitives is in every tile, the most it can be (saturated) */
PVR_FN pvr_u32 pvr_bin_worst_bytes(pvr_u32 tiles, pvr_u32 prims)
{
   if (tiles != 0u && prims > 0x7fffffffu / tiles)
      return 0xffffffffu;
   return tiles * prims * 2u;
}

/* ---- The region array's table of tiles ----
 *
 * One word per entry of the region array, in the order it lists them
 * (the order the chip renders them): the tile, which pass of that tile
 * the entry is (0 for the first of its consecutive entries), and the
 * entry's flags as the game wrote them. A tile with no entry is not
 * rendered; the passes of a tile are the entries in a row with the same
 * tile. Presort means what it does for a type 2 array only. */
#define PVR_RGN_X(e)        ((e) & 63u)
#define PVR_RGN_Y(e)        (((e) >> 6) & 63u)
#define PVR_RGN_TILE(e)     ((e) & 0xfffu)
#define PVR_RGN_PASS(e)     (((e) >> 12) & 15u)
#define PVR_RGN_FLUSH_ACCUM 0x00010000u
#define PVR_RGN_PRESORT     0x00020000u
#define PVR_RGN_NO_Z_CLEAR  0x00040000u
#define PVR_RGN_LAST        0x00080000u
#define PVR_RGN_PASS_MAX    15u

/* The word for the entry whose control word is @control, after the one
 * whose word was @prev (@first: there was none). The control word has
 * the tile at bits 2-7 and 8-13 and the flags at 28-31. */
PVR_FN pvr_u32 pvr_region_entry(pvr_u32 control, pvr_u32 prev, pvr_u32 first)
{
   pvr_u32 e = ((control >> 2) & 63u) | (((control >> 8) & 63u) << 6) | ((control >> 28) << 16);
   pvr_u32 pass = 0u;
   if (first == 0u && PVR_RGN_TILE(prev) == PVR_RGN_TILE(e))
   {
      pass = PVR_RGN_PASS(prev);
      if (pass < PVR_RGN_PASS_MAX)
         pass++;
   }
   return e | (pass << 12);
}

#endif
