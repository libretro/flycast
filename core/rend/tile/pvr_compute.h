/* The compute renderer: a frame drawn by compute kernels, a tile at a time,
 * the way the PowerVR2 draws it, with no render target, no depth or stencil
 * buffer and no list of translucent fragments in video memory.
 *
 * The header is read two ways, like rend/ta_gpu.h:
 * - by a C89 or C++ compiler (nothing defined): the layouts of what the
 *   CPU hands over and of what the kernels keep, and the rules both the
 *   kernels and the CPU's reference (tools/rend/tile_ref.c) follow;
 * - by a GLSL compiler, with PVRC_SHADER_GLSL defined and PVRC_KERNEL the
 *   kernel wanted (PVRC_K_*), after pvr_lane.h, pvr_tile.h, pvr_modvol.h,
 *   pvr_resolve.h and ssaa_pattern.h (tools/rend/kernels_spv.sh puts them
 *   in front, and the core has the kernels as SPIR-V in
 *   vulkan/compute_spv.inc).
 *
 * The frame, as the CPU's parse leaves it (ta_parse_vdrc()), goes to the
 * GPU once, in one buffer of words (PVRC_F_*): a header, the render passes,
 * the polygon parameters, the modifier volumes' parameters, a table of
 * where each parameter's triangles start in the frame's numbering, and the
 * vertices, indices and modifier volume triangles as the parse made them.
 * Triangles - "primitives" - are numbered in the order the chip draws them:
 * for each render pass, its opaque polygons' triangles, its opaque modifier
 * volume triangles, its punch-through, its translucent and its translucent
 * modifier volume triangles. A list of a render pass is then a range of
 * those numbers, and a tile's primitives in number order are each list in
 * the order it was sent.
 *
 * The kernels, in the order they run, all in one submission:
 *  1 setup    one thread a primitive: its vertices on the screen in 24.8
 *             fixed point, turned so that its edges are positive inside,
 *             its 1/w, and the pixels around it, cut to its clip; culled
 *             and empty ones get no pixels. The primitive is counted in
 *             every coarse tile (2^log2 pixels square) those pixels touch,
 *             for its chunk (65536 primitives, pvr_tile.h).
 *  2 scan     one workgroup: where each tile's run of each chunk starts in
 *             the bin pool, a fixed array of 16-bit entries (two a word),
 *             each run at an even entry. A run that does not fit, or is
 *             longer than the sort takes, is marked: its tile is walked
 *             over all of that chunk's primitives instead, which is
 *             slower and the same.
 *  3 scatter  one thread a primitive: its number, in the run of each tile
 *             it was counted in, in no particular order.
 *  4 sort     one workgroup a run: the run sorted, in shared memory, so
 *             that it is in the order the primitives were sent.
 *  5 walk     one workgroup a fine tile (256 / N pixels, N samples each: a
 *             lane a sample, the samples of a pixel in adjacent lanes):
 *             every render pass of the tile, every list of each, in the
 *             order and by the rules of the per-pixel raster renderer
 *             (vulkan/oit/), with the depth, the stencil bits and the
 *             translucent fragments in registers and shared memory. The
 *             translucent fragments of a sample are kept K at a time,
 *             the K farthest first, and blended; then the next K beyond
 *             those, and so on until none are left - none is ever
 *             dropped, whatever their number. Then the samples of each
 *             pixel are resolved (pvr_resolve.h) and the pixel written.
 *  6 clear    the bin pool's words the frame used set back to 0 for the
 *             next one (the scatter ORs its entries in).
 * Nothing is read back: the counts are the GPU's, and the clear's size an
 * indirect dispatch's.
 *
 * Where the raster renderer is the rule: the depth compared is the one it
 * writes (log2(1 + 100000 / w) / 34, as a 32-bit float); its depth buffer
 * is cleared once a frame, not at each render pass; its stencil once a
 * render pass; its colour attachment holds 8 bits a channel between draws;
 * translucent fragments are shaded with their first parameters, put in the
 * lists as 8-bit colours, sorted farthest first and by polygon (here: by
 * triangle, which is the same order) and blended in floats; under SSAA a
 * translucent fragment is shaded and its depth taken once a pixel, at its
 * centre, as are a modifier volume's depths. Texture filtering is the
 * device's sampler, given the derivatives the setup's planes have.
 *
 * Test mode (spec constant PVRC_TEST, tools/rend/tile_test): the depth
 * compared is 1/w itself and a texture is a function of its coordinates,
 * so that the CPU's reference can do exactly what the kernels do. */
#ifndef PVR_COMPUTE_H
#define PVR_COMPUTE_H

#if !defined(PVRC_SHADER_GLSL)
#include "pvr_lane.h"
#include "pvr_tile.h"
#include "pvr_modvol.h"
#include "pvr_resolve.h"
#include "../ssaa_pattern.h"
#endif

/* The kernels (PVRC_KERNEL), in the order they run */
#define PVRC_K_SETUP   1
#define PVRC_K_SCAN    2
#define PVRC_K_SCATTER 3
#define PVRC_K_SORT    4
#define PVRC_K_WALK    5
#define PVRC_K_CLEAR   6
#define PVRC_KERNELS   6

#define PVRC_NONE 0xffffffffu

/* ---- The frame's words ---- */

/* The header */
#define PVRC_F_WIDTH      0u   /* of the picture, pixels */
#define PVRC_F_HEIGHT     1u
#define PVRC_F_TILES_X    2u   /* coarse tiles */
#define PVRC_F_TILES_Y    3u
#define PVRC_F_TILE_LOG2  4u
#define PVRC_F_PRIMS      5u
#define PVRC_F_PASSES     6u
#define PVRC_F_FLAGS      7u   /* PVRC_FF_* */
#define PVRC_F_CLEAR      8u   /* the colour the picture starts as, RGBA8 */
#define PVRC_F_SCISSOR    9u   /* the frame's pixel clip: x0, y0, x1, y1 (x1, y1 not in it) */
#define PVRC_F_XFORM      13u  /* guest to pixels: scale x, y, move x, y (floats); 1/w scale */
#define PVRC_F_FOG_DEN    18u  /* floats from here: the fog density */
#define PVRC_F_FOG_RAM    19u  /* the fog colours, RGB */
#define PVRC_F_FOG_VERT   22u
#define PVRC_F_CLAMP_MIN  25u  /* RGBA */
#define PVRC_F_CLAMP_MAX  29u
#define PVRC_F_ALPHA_REF  33u
#define PVRC_F_SHADE      34u  /* the shadow's colour scale */
#define PVRC_F_CHUNKS     35u
#define PVRC_F_SOURCES    36u  /* entries of the table of starts */
#define PVRC_F_POOL       37u  /* entries the bin pool has room for */
#define PVRC_F_O_POLY     38u  /* where the polygon parameters start, in words */
#define PVRC_F_O_MVP      39u  /* ...the modifier volumes' */
#define PVRC_F_O_SRC      40u  /* ...the table of starts */
#define PVRC_F_O_VTX      41u
#define PVRC_F_O_IDX      42u
#define PVRC_F_O_MOD      43u
#define PVRC_F_RUNS       44u  /* runs the bin buffer has room for (chunks x tiles) */
#define PVRC_F_FOG        48u  /* the fog table, 256 bytes as MakeFogTexture() lays them out */
#define PVRC_F_PASS       112u /* the render passes, PVRC_PASS_WORDS each */
#define PVRC_PASS_WORDS   8u
#define PVRC_PASS_MAX     10u
#define PVRC_F_WORDS      192u

#define PVRC_FF_LAST_PICTURE 1u   /* start from the last picture, not the clear colour */

/* A render pass: where its lists start in the numbering (opaque, opaque
 * modifier volumes, punch-through, translucent, the translucent's end),
 * its translucent modifier volumes (start, end: the opaque ones' range
 * when it shares them), and its flags */
#define PVRC_P_OP    0u
#define PVRC_P_MVO   1u
#define PVRC_P_PT    2u
#define PVRC_P_TR    3u
#define PVRC_P_TR_END 4u
#define PVRC_P_MVT   5u
#define PVRC_P_MVT_END 6u
#define PVRC_P_FLAGS 7u
#define PVRC_PF_AUTOSORT 1u

/* A polygon parameter */
#define PVRC_POLY_WORDS 16u
#define PVRC_Q_ISP    0u
#define PVRC_Q_TSP    1u
#define PVRC_Q_TCW    2u
#define PVRC_Q_PCW    3u
#define PVRC_Q_TSP1   4u
#define PVRC_Q_TCW1   5u
#define PVRC_Q_TEX    6u   /* texture slots, the first's and the second's (0xffff: none) */
#define PVRC_Q_FLAGS  7u   /* PVRC_QF_* */
#define PVRC_Q_TRILIN 8u   /* (float) the trilinear pass's alpha */
#define PVRC_Q_PAL    9u   /* the palette's first entry */
#define PVRC_Q_RECT   10u  /* drawn inside: x0 | y0 << 16, x1 | y1 << 16 (x1, y1 not in it) */
#define PVRC_Q_CLIP   12u  /* not drawn inside, when PVRC_QF_CLIP_INSIDE: x0, y0, x1, y1 the same way, all in it */
#define PVRC_Q_FIRST  14u  /* its first index */
#define PVRC_Q_LIST   15u  /* ListType_* */

/* (the bits of the per-pixel renderer's pipeline key that a polygon has) */
#define PVRC_QF_ALPHA_TEST  (1u << 0)
#define PVRC_QF_BUMP        (1u << 1)
#define PVRC_QF_CLAMP       (1u << 2)
#define PVRC_QF_CLIP_INSIDE (1u << 3)
#define PVRC_QF_FOG_SHIFT   4u
#define PVRC_QF_IGNORE_TEXA (1u << 6)
#define PVRC_QF_OFFSET      (1u << 7)
#define PVRC_QF_SHAD_SHIFT  8u
#define PVRC_QF_TEXTURE     (1u << 10)
#define PVRC_QF_USE_ALPHA   (1u << 11)
#define PVRC_QF_TWO_VOLUMES (1u << 12)
#define PVRC_QF_PALETTE     (1u << 13)
#define PVRC_QF_GOURAUD     (1u << 14)
#define PVRC_QF_SHADOW      (1u << 15)   /* takes shadows (its PCW's Shadow bit) */
#define PVRC_QF_SINGLE_RATE (1u << 16)
/* (two volumes: what the second volume has, and the first's own bits) */
#define PVRC_QF_SHAD1_SHIFT 17u
#define PVRC_QF_FOG1_SHIFT  19u
#define PVRC_QF_USE_ALPHA1  (1u << 21)
#define PVRC_QF_IGNORE_TEXA1 (1u << 22)
#define PVRC_QF_IGNORE_TEXA0 (1u << 23)
#define PVRC_QF_USE_ALPHA0  (1u << 24)

/* A modifier volume's parameter: its ISP word, its clip rectangle (as a
 * polygon's PVRC_Q_RECT), its first triangle; and the volume it ends in:
 * the number of the last triangle of the parameter that ends it, and that
 * parameter's index (PVRC_NONE: none does, in this render pass) */
#define PVRC_MVP_WORDS 8u
#define PVRC_M_ISP   0u
#define PVRC_M_RECT  1u
#define PVRC_M_FIRST 3u
#define PVRC_M_CLOSE_END 4u
#define PVRC_M_CLOSE_BY  5u

/* The table of starts: a word for the first number of each parameter's
 * triangles, then its kind and index, in number order */
#define PVRC_SRC_WORDS 2u
#define PVRC_SRC_MODVOL 0x80000000u

/* A vertex: the parse's Vertex, word for word */
#define PVRC_VTX_WORDS 11u

/* ---- What the kernels keep ---- */

/* A primitive's setup: the vertices' x and y on the screen in 24.8 fixed
 * point (divided by 2^shift, PVRC_RX_SHIFT: pvrc_xy_shift()), in the order
 * that makes its edges positive inside; their 1/w
 * (scaled); for a polygon's triangle, its vertices' numbers in that order;
 * its parameter (PVRC_SRC_MODVOL for a volume's); the pixels around it,
 * first x | y << 16 and last (a primitive with none has the first after
 * the last); and for a polygon's triangle its first vertex in the draw's
 * order, whose colours a flat one has, or for a volume's its facing */
#define PVRC_REC_WORDS 16u
#define PVRC_R_XY    0u
#define PVRC_R_Q     6u
#define PVRC_R_VTX   9u
#define PVRC_R_SRC   12u
#define PVRC_R_BBOX0 13u
#define PVRC_R_BBOX1 14u
#define PVRC_R_EXTRA 15u
#define PVRC_RX_FRONT 1u   /* (a volume's) facing the front, the device's way */
#define PVRC_RX_BACK  2u
#define PVRC_RX_CULLED 4u  /* culled by its own parameter */
#define PVRC_RX_OR    8u   /* OR'ed, not XOR'ed */
#define PVRC_RX_MODE_SHIFT 4u  /* its parameter's mode */
#define PVRC_RX_VTX   0x03ffffffu  /* (a polygon's) its first vertex */
#define PVRC_RX_SHIFT_AT 26u  /* the shift its x and y are kept at, 3 bits (either's) */

/* The bin buffer: counts of each run (chunk x tiles of them), starts
 * (PVRC_RUN_SKIP: walked over the whole chunk), the scatter's cursors,
 * the clear's dispatch and a few counts for the log, then the pool, two
 * entries a word */
#define PVRC_RUN_SKIP 0x80000000u
#define PVRC_B_COUNT(runs) 0u
#define PVRC_B_START(runs) (runs)
#define PVRC_B_CURSOR(runs) (2u * (runs))
#define PVRC_B_STATS(runs) (3u * (runs))
#define PVRC_B_POOL(runs) (3u * (runs) + 16u)
#define PVRC_ST_CLEAR_X   0u   /* the clear's dispatch: x, y, z */
#define PVRC_ST_WORDS     3u   /* pool words used */
#define PVRC_ST_SKIPPED   4u   /* runs walked over their whole chunk */
#define PVRC_ST_DROPPED   5u   /* triangles with a vertex behind the eye, left out */
#define PVRC_ST_PEELS     6u   /* the most rounds of translucent fragments a tile took */
#define PVRC_STATS        8u

/* The textures a frame can have, slots of the kernels' array */
#define PVRC_TEX_SLOTS 1024

/* The longest run the sort takes, in shared memory (16 KiB) */
#define PVRC_SORT_MAX 4096u

/* Workgroup sizes */
#define PVRC_WG_SETUP 64u
#define PVRC_WG       256u

/* The fine tile of a workgroup of the walk, for N samples: 256 lanes */
#define PVRC_FINE_W(n) ((n) <= 2u ? 16u : 8u)
#define PVRC_FINE_H(n) (256u / (n) / PVRC_FINE_W(n))

/* The translucent fragments a lane keeps at once, by the shared memory
 * the device gives a workgroup: three words each (the key's two and the
 * colour), 256 lanes, and room for the rest of what the walk keeps there */
#define PVRC_K_MAX 8u
#define PVRC_WALK_SHARED_FIXED 2048u
#define PVRC_FRAGMENT_BYTES 12u
#if !defined(PVRC_SHADER_GLSL)
PVR_FN pvr_u32 pvrc_fragments_kept(pvr_u32 shared_bytes)
{
   pvr_u32 k = PVRC_K_MAX;
   while (k > 1u && PVRC_WALK_SHARED_FIXED + k * PVRC_WG * PVRC_FRAGMENT_BYTES > shared_bytes)
      k >>= 1;
   return k;
}
#endif

#if defined(PVRC_SHADER_GLSL) && defined(PVRC_KERNEL)
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(constant_id = 0) const uint PVRC_SAMPLES = 1u;
layout(constant_id = 1) const uint PVRC_LOG2N = 0u;
layout(constant_id = 2) const uint PVRC_KEEP = 4u;
layout(constant_id = 3) const uint PVRC_TEST = 0u;

layout(std430, set = 0, binding = 0) readonly buffer Frame { uint w[]; } frm;
layout(std430, set = 0, binding = 1) buffer Records { uint w[]; } rec;
layout(std430, set = 0, binding = 2) buffer Bins { uint w[]; } bin;

#define FRMF(i) uintBitsToFloat(frm.w[i])
#endif

/* ---- The rules ---- */

/* Every operation as it is written, none fused (tools/rend/tile_test builds
 * the kernels with PVRC_EXACT, for the reference to have their bits; the
 * renderer's are made without, and the device fuses where it likes) */
#if defined(PVRC_SHADER_GLSL) && defined(PVRC_EXACT)
#define PVRC_PRECISE precise
#else
#define PVRC_PRECISE
#endif
/* ...but where a fragment is and its depth, always: the depth a pass
 * writes is compared for equality with the one another pass takes at the
 * same place, and the two have to be the same bits (as the raster
 * renderer's depth and colour passes have them, from one formula) */
#if defined(PVRC_SHADER_GLSL)
#define PVRC_INVARIANT precise
#else
#define PVRC_INVARIANT
#endif

/* x or y on the screen in 24.8 fixed point, to the nearest, kept to what
 * the edge arithmetic takes exactly, 2^29 (and a NaN at the far left): a
 * difference of two in 32 bits, an edge's product of two differences and
 * the sum of two of those in 64 */
#define PVRC_XY_MOST 536870912.0f
PVR_FN pvr_s32 pvrc_fixed(pvr_f32 v)
{
   PVRC_PRECISE pvr_f32 f = v * 256.0f + 0.5f;
   if (!(f >= -PVRC_XY_MOST))
      f = -PVRC_XY_MOST;
   if (f > PVRC_XY_MOST)
      f = PVRC_XY_MOST;
   return PVR_S32(floor(f));
}

/* A triangle whose vertices go farther out than that (2097152 pixels: a
 * game's near the eye can be far more, the raster renderers clipping them
 * at the edges of their guard band) has its x and y kept divided by 2^the
 * shift, the least that brings its farthest, @most pixels out, inside:
 * with the places it is tested at divided the same way, its edges are the
 * same lines (to 1/16 pixel at the vertices that are near, which a
 * vertex out there is not anyway: a float that far has no bits that
 * fine). Up to 4, which keeps every sample's place (1/16 pixels) exact;
 * past that, 33554432 pixels, a vertex is taken at the edge. */
#define PVRC_SHIFT_MOST 4u
PVR_FN pvr_u32 pvrc_xy_shift(pvr_f32 most)
{
   pvr_u32 k = 0u;
   while (k < PVRC_SHIFT_MOST && !(most * 256.0f <= PVRC_XY_MOST * PVR_F32(1u << k)))
      k++;
   return k;
}

/* A depth test of the device's: @mode an ISP DepthMode, @z the fragment's
 * depth, @d what the depth buffer has */
PVR_FN pvr_u32 pvrc_depth_pass(pvr_u32 mode, pvr_f32 z, pvr_f32 d)
{
   switch (mode)
   {
   case 0u: return 0u;
   case 1u: return z < d ? 1u : 0u;
   case 2u: return z == d ? 1u : 0u;
   case 3u: return z <= d ? 1u : 0u;
   case 4u: return z > d ? 1u : 0u;
   case 5u: return z != d ? 1u : 0u;
   case 6u: return z >= d ? 1u : 0u;
   default: return 1u;
   }
}

/* What a blend's factor for one channel is (TSP SrcInstr / DstInstr),
 * @src for the source's: the colours and alphas of the source and of what
 * it is blended onto */
PVR_FN pvr_f32 pvrc_blend_factor(pvr_u32 instr, pvr_u32 src, pvr_f32 sc, pvr_f32 sa, pvr_f32 dc, pvr_f32 da)
{
   switch (instr)
   {
   case 0u: return 0.0f;
   case 1u: return 1.0f;
   case 2u: return src != 0u ? dc : sc;
   case 3u: return src != 0u ? 1.0f - dc : 1.0f - sc;
   case 4u: return sa;
   case 5u: return 1.0f - sa;
   case 6u: return da;
   default: return 1.0f - da;
   }
}

/* A channel as 8 bits, as an attachment and the lists keep it */
PVR_FN pvr_u32 pvrc_unorm8(pvr_f32 c)
{
   PVRC_PRECISE pvr_f32 v = (c > 0.0f ? (c < 1.0f ? c : 1.0f) : 0.0f) * 255.0f + 0.5f;
   return PVR_U32(floor(v));
}

PVR_FN pvr_f32 pvrc_from8(pvr_u32 b)
{
   return PVR_F32(b & 255u) / 255.0f;
}

/* The fog coefficient for a 1/w, the PowerVR2's way: see fog_mode2() in
 * vulkan/shaders/oit.frag, whose table lookups these are */
#if defined(PVRC_SHADER_GLSL)
#define PVRC_FOG_BYTE(i) ((frm.w[PVRC_F_FOG + ((i) >> 2)] >> (8u * ((i) & 3u))) & 255u)
#endif
PVR_FN pvr_f32 pvrc_fog_lut(pvr_f32 w, pvr_f32 density
#if !defined(PVRC_SHADER_GLSL)
      , const unsigned char *table
#endif
      )
{
   PVRC_PRECISE pvr_f32 z = w * density;
   PVRC_PRECISE pvr_f32 e = 0.0f;
   PVRC_PRECISE pvr_f32 t, i, blend;
   pvr_u32 idx;
   pvr_f32 next, cur;
   if (!(z >= 1.0f))
      z = 1.0f;
   if (z > 255.9999f)
      z = 255.9999f;
   if (z >= 16.0f) { z *= 0.0625f; e = 4.0f; }
   if (z >= 4.0f) { z *= 0.25f; e += 2.0f; }
   if (z >= 2.0f) { z *= 0.5f; e += 1.0f; }
   t = (z - 1.0f) * 16.0f;
   i = floor(t);
   blend = floor((t - i) * 256.0f);
   idx = PVR_U32(e * 16.0f + i);
#if defined(PVRC_SHADER_GLSL)
   next = PVR_F32(PVRC_FOG_BYTE(idx));
   cur = PVR_F32(PVRC_FOG_BYTE(idx + 128u));
#else
   next = PVR_F32(table[idx]);
   cur = PVR_F32(table[idx + 128u]);
#endif
   return floor((next * blend + cur * (255.0f - blend)) / 256.0f) / 256.0f;
}

/* The depth the raster renderer writes for a fragment at @w (its 1/w, as
 * gl_FragCoord.w has it); in test mode 1/w itself */
#if defined(PVRC_SHADER_GLSL)
PVR_FN pvr_f32 pvrc_depth(pvr_f32 w)
{
   PVRC_INVARIANT float z;
   if (PVRC_TEST != 0u)
      return w;
   z = log2(1.0f + max(100000.0f * w, -0.999999f)) / 34.0f;
   return z;
}
#endif

/* Test mode: a texture's texel at @u, @v (for any slot), the same for the
 * kernels and the reference */
PVR_FN pvr_u32 pvrc_test_texel(pvr_u32 slot, pvr_f32 u, pvr_f32 v)
{
   PVRC_PRECISE pvr_f32 fu = (u - floor(u)) * 64.0f;
   PVRC_PRECISE pvr_f32 fv = (v - floor(v)) * 64.0f;
   pvr_u32 x = PVR_U32(floor(fu)) & 63u;
   pvr_u32 y = PVR_U32(floor(fv)) & 63u;
   pvr_u32 h = (x * 73856093u) ^ (y * 19349663u) ^ ((slot + 1u) * 83492791u);
   h ^= h >> 13;
   h *= 0x5bd1e995u;
   h ^= h >> 15;
   return h;
}

/* ---- The CPU's side: sizes ---- */

#if !defined(PVRC_SHADER_GLSL)
/* Words of the bin buffer for @runs runs and a pool of @pool entries */
PVR_FN pvr_u32 pvrc_bin_words(pvr_u32 runs, pvr_u32 pool)
{
   return PVRC_B_POOL(runs) + (pool + 1u) / 2u;
}
#endif

/* ---- The kernels ---- */

#if defined(PVRC_SHADER_GLSL) && defined(PVRC_KERNEL)



uint pvrc_runs() { return frm.w[PVRC_F_RUNS]; }
uint pvrc_tiles() { return frm.w[PVRC_F_TILES_X] * frm.w[PVRC_F_TILES_Y]; }

#if PVRC_KERNEL == PVRC_K_SETUP || PVRC_KERNEL == PVRC_K_SCATTER
/* The coarse tiles a primitive's pixels touch, first x | y << 16 and last */
uvec2 pvrc_tile_range(uint r)
{
   uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   uint l2 = frm.w[PVRC_F_TILE_LOG2];
   return uvec2(((b0 & 0xffffu) >> l2) | (((b0 >> 16) >> l2) << 16),
         ((b1 & 0xffffu) >> l2) | (((b1 >> 16) >> l2) << 16));
}
#endif

#if PVRC_KERNEL == PVRC_K_SETUP
layout(local_size_x = 64) in;

uint pvrc_rect0(uint at) { return frm.w[at]; }

void main()
{
   const uint prim = gl_GlobalInvocationID.x;
   if (prim >= frm.w[PVRC_F_PRIMS])
      return;
   /* its parameter: the last start at or before it */
   const uint osrc = frm.w[PVRC_F_O_SRC];
   uint lo = 0u, hi = frm.w[PVRC_F_SOURCES];
   while (hi - lo > 1u)
   {
      uint mid = (lo + hi) >> 1;
      if (frm.w[osrc + mid * PVRC_SRC_WORDS] <= prim)
         lo = mid;
      else
         hi = mid;
   }
   const uint i = prim - frm.w[osrc + lo * PVRC_SRC_WORDS];
   const uint src = frm.w[osrc + lo * PVRC_SRC_WORDS + 1u];
   const bool modvol = (src & PVRC_SRC_MODVOL) != 0u;
   const uint index = src & ~PVRC_SRC_MODVOL;

   float vx[3], vy[3], vz[3];
   uint vi[3];
   uint provoking = 0u;
   uint rect0, rect1, isp;
   if (modvol)
   {
      const uint mp = frm.w[PVRC_F_O_MVP] + index * PVRC_MVP_WORDS;
      const uint t = frm.w[PVRC_F_O_MOD] + (frm.w[mp + PVRC_M_FIRST] + i) * 9u;
      for (uint k = 0u; k < 3u; k++)
      {
         vx[k] = uintBitsToFloat(frm.w[t + k * 3u]);
         vy[k] = uintBitsToFloat(frm.w[t + k * 3u + 1u]);
         vz[k] = uintBitsToFloat(frm.w[t + k * 3u + 2u]);
         vi[k] = 0u;
      }
      isp = frm.w[mp + PVRC_M_ISP];
      /* (a volume's triangles reach the whole frame: what of them its own
       * clip leaves out still counts for the volume's end) */
      rect0 = frm.w[PVRC_F_SCISSOR] | (frm.w[PVRC_F_SCISSOR + 1u] << 16);
      rect1 = frm.w[PVRC_F_SCISSOR + 2u] | (frm.w[PVRC_F_SCISSOR + 3u] << 16);
   }
   else
   {
      const uint pp = frm.w[PVRC_F_O_POLY] + index * PVRC_POLY_WORDS;
      const uint at = frm.w[PVRC_F_O_IDX] + frm.w[pp + PVRC_Q_FIRST] + i;
      /* a strip's triangle i, the device's way: i, i + 1, i + 2, and for
       * an odd one i, i + 2, i + 1; its first vertex is the provoking one */
      const uint odd = i & 1u;
      vi[0] = frm.w[at];
      vi[1] = frm.w[at + 1u + odd];
      vi[2] = frm.w[at + 2u - odd];
      provoking = vi[0];
      const uint ov = frm.w[PVRC_F_O_VTX];
      for (uint k = 0u; k < 3u; k++)
      {
         vx[k] = uintBitsToFloat(frm.w[ov + vi[k] * PVRC_VTX_WORDS]);
         vy[k] = uintBitsToFloat(frm.w[ov + vi[k] * PVRC_VTX_WORDS + 1u]);
         vz[k] = uintBitsToFloat(frm.w[ov + vi[k] * PVRC_VTX_WORDS + 2u]);
      }
      isp = frm.w[pp + PVRC_Q_ISP];
      rect0 = frm.w[pp + PVRC_Q_RECT];
      rect1 = frm.w[pp + PVRC_Q_RECT + 1u];
   }

   const float sx = FRMF(PVRC_F_XFORM), sy = FRMF(PVRC_F_XFORM + 1u);
   const float tx = FRMF(PVRC_F_XFORM + 2u), ty = FRMF(PVRC_F_XFORM + 3u);
   const float sz = FRMF(PVRC_F_XFORM + 4u);
   int x[3], y[3];
   float q[3];
   float px[3], py[3];
   bool bad = false;
   float most = 0.0;
   for (uint k = 0u; k < 3u; k++)
   {
      PVRC_PRECISE float fx = sx * vx[k] + tx;
      PVRC_PRECISE float fy = sy * vy[k] + ty;
      px[k] = fx;
      py[k] = fy;
      if (!(abs(fx) <= most))
         most = abs(fx);
      if (!(abs(fy) <= most))
         most = abs(fy);
   }
   const uint shift = pvrc_xy_shift(most);
   const float down = 1.0 / float(1u << shift);	/* (a power of two: exact) */
   for (uint k = 0u; k < 3u; k++)
   {
      x[k] = pvrc_fixed(px[k] * down);
      y[k] = pvrc_fixed(py[k] * down);
      q[k] = sz * vz[k];
      /* behind the eye, or at no depth: the raster renderers clip it
       * there; such a triangle is left out (and counted) */
      if (!(q[k] > 0.0) || q[k] > 3.4e37 || isnan(px[k]) || isnan(py[k]))
         bad = true;
   }
   const int64_t cross = int64_t(x[1] - x[0]) * int64_t(y[2] - y[0]) - int64_t(x[2] - x[0]) * int64_t(y[1] - y[0]);
   /* counter-clockwise the device's way (a positive area in its formula)
    * when cross is negative: that is its front, as the pipelines have it */
   const bool front = cross < 0;
   const uint cull = (isp >> 27) & 3u;
   const bool culledOwn = (cull == 3u && !front) || (cull == 2u && front);
   uint extra = provoking;
   bool empty = bad || cross == 0;
   if (modvol)
   {
      const uint mode = isp >> 29;
      extra = (front ? PVRC_RX_FRONT : PVRC_RX_BACK) | (culledOwn ? PVRC_RX_CULLED : 0u)
            | (pvr_modvol_or_mode((isp >> 26) & 1u, mode) != 0u ? PVRC_RX_OR : 0u)
            | (mode << PVRC_RX_MODE_SHIFT);
   }
   else if (culledOwn)
      empty = true;
   if (bad)
      atomicAdd(bin.w[PVRC_B_STATS(pvrc_runs()) + PVRC_ST_DROPPED], 1u);
   if (cross < 0)
   {
      int t = x[1]; x[1] = x[2]; x[2] = t;
      t = y[1]; y[1] = y[2]; y[2] = t;
      float f = q[1]; q[1] = q[2]; q[2] = f;
      uint u = vi[1]; vi[1] = vi[2]; vi[2] = u;
   }
   extra |= shift << PVRC_RX_SHIFT_AT;
   const uint r = prim * PVRC_REC_WORDS;
   for (uint k = 0u; k < 3u; k++)
   {
      rec.w[r + PVRC_R_XY + 2u * k] = uint(x[k]);
      rec.w[r + PVRC_R_XY + 2u * k + 1u] = uint(y[k]);
      rec.w[r + PVRC_R_Q + k] = floatBitsToUint(q[k]);
      rec.w[r + PVRC_R_VTX + k] = vi[k];
   }
   rec.w[r + PVRC_R_SRC] = src;
   rec.w[r + PVRC_R_EXTRA] = extra;
   /* the pixels whose samples it can cover, inside its clip and the picture */
   int bx0 = int(((int64_t(min(min(x[0], x[1]), x[2])) << shift) - 255) >> 8);
   int by0 = int(((int64_t(min(min(y[0], y[1]), y[2])) << shift) - 255) >> 8);
   int bx1 = int((int64_t(max(max(x[0], x[1]), x[2])) << shift) >> 8);
   int by1 = int((int64_t(max(max(y[0], y[1]), y[2])) << shift) >> 8);
   bx0 = max(bx0, int(rect0 & 0xffffu));
   by0 = max(by0, int(rect0 >> 16));
   bx1 = min(bx1, int(rect1 & 0xffffu) - 1);
   by1 = min(by1, int(rect1 >> 16) - 1);
   bx1 = min(bx1, int(frm.w[PVRC_F_WIDTH]) - 1);
   by1 = min(by1, int(frm.w[PVRC_F_HEIGHT]) - 1);
   if (empty || bx0 > bx1 || by0 > by1)
   {
      rec.w[r + PVRC_R_BBOX0] = 0xffffffffu;
      rec.w[r + PVRC_R_BBOX1] = 0u;
      return;
   }
   rec.w[r + PVRC_R_BBOX0] = uint(bx0) | (uint(by0) << 16);
   rec.w[r + PVRC_R_BBOX1] = uint(bx1) | (uint(by1) << 16);
   const uvec2 tr = pvrc_tile_range(r);
   const uint tilesX = frm.w[PVRC_F_TILES_X];
   const uint base = (prim >> PVR_CHUNK_LOG2) * pvrc_tiles();
   for (uint ty = tr.x >> 16; ty <= (tr.y >> 16); ty++)
      for (uint tx = tr.x & 0xffffu; tx <= (tr.y & 0xffffu); tx++)
         atomicAdd(bin.w[PVRC_B_COUNT(pvrc_runs()) + base + ty * tilesX + tx], 1u);
}
#endif

#if PVRC_KERNEL == PVRC_K_SCAN
layout(local_size_x = 256) in;
shared uint s_sum[256];

void main()
{
   const uint tid = gl_LocalInvocationIndex;
   const uint runs = pvrc_runs();
   const uint n = frm.w[PVRC_F_CHUNKS] * pvrc_tiles();
   const uint pool = frm.w[PVRC_F_POOL];
   uint base = 0u;
   uint skipped = 0u;
   for (uint b = 0u; b < n; b += 256u)
   {
      const uint e = b + tid;
      const uint count = e < n ? bin.w[PVRC_B_COUNT(runs) + e] : 0u;
      /* a run too long to sort takes no room: it is walked another way */
      const uint size = count > PVRC_SORT_MAX ? 0u : (count + 1u) & ~1u;
      s_sum[tid] = size;
      barrier();
      for (uint d = 1u; d < 256u; d <<= 1)
      {
         uint v = tid >= d ? s_sum[tid - d] : 0u;
         barrier();
         s_sum[tid] += v;
         barrier();
      }
      const uint start = base + s_sum[tid] - size;
      if (e < n)
      {
         const bool skip = count > PVRC_SORT_MAX || start + count > pool;
         bin.w[PVRC_B_START(runs) + e] = skip ? PVRC_RUN_SKIP : start;
         bin.w[PVRC_B_CURSOR(runs) + e] = 0u;
         if (skip && count != 0u)
            skipped++;
      }
      base += s_sum[255];
      barrier();
   }
   if (skipped != 0u)
      atomicAdd(bin.w[PVRC_B_STATS(runs) + PVRC_ST_SKIPPED], skipped);
   if (tid == 0u)
   {
      const uint words = (min(base, pool) + 1u) / 2u;
      bin.w[PVRC_B_STATS(runs) + PVRC_ST_WORDS] = words;
      bin.w[PVRC_B_STATS(runs) + PVRC_ST_CLEAR_X] = (words + 255u) / 256u;
      bin.w[PVRC_B_STATS(runs) + PVRC_ST_CLEAR_X + 1u] = 1u;
      bin.w[PVRC_B_STATS(runs) + PVRC_ST_CLEAR_X + 2u] = 1u;
   }
}
#endif

#if PVRC_KERNEL == PVRC_K_SCATTER
layout(local_size_x = 64) in;

void main()
{
   const uint prim = gl_GlobalInvocationID.x;
   if (prim >= frm.w[PVRC_F_PRIMS])
      return;
   const uint r = prim * PVRC_REC_WORDS;
   if (rec.w[r + PVRC_R_BBOX0] == 0xffffffffu)
      return;
   const uint runs = pvrc_runs();
   const uvec2 tr = pvrc_tile_range(r);
   const uint tilesX = frm.w[PVRC_F_TILES_X];
   const uint base = (prim >> PVR_CHUNK_LOG2) * pvrc_tiles();
   const uint pool = PVRC_B_POOL(runs);
   for (uint ty = tr.x >> 16; ty <= (tr.y >> 16); ty++)
      for (uint tx = tr.x & 0xffffu; tx <= (tr.y & 0xffffu); tx++)
      {
         const uint e = base + ty * tilesX + tx;
         const uint start = bin.w[PVRC_B_START(runs) + e];
         if ((start & PVRC_RUN_SKIP) != 0u)
            continue;
         const uint at = start + atomicAdd(bin.w[PVRC_B_CURSOR(runs) + e], 1u);
         atomicOr(bin.w[pool + (at >> 1)], (prim & 0xffffu) << (16u * (at & 1u)));
      }
}
#endif

#if PVRC_KERNEL == PVRC_K_SORT
layout(local_size_x = 256) in;
shared uint s_run[PVRC_SORT_MAX];

void main()
{
   const uint tid = gl_LocalInvocationIndex;
   const uint runs = pvrc_runs();
   const uint e = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
   if (e >= frm.w[PVRC_F_CHUNKS] * pvrc_tiles())
      return;
   const uint n = bin.w[PVRC_B_COUNT(runs) + e];
   const uint start = bin.w[PVRC_B_START(runs) + e];
   if (n < 2u || (start & PVRC_RUN_SKIP) != 0u)
      return;
   const uint pool = PVRC_B_POOL(runs) + (start >> 1);
   uint m = 2u;
   while (m < n)
      m <<= 1;
   for (uint i = tid; i < m; i += 256u)
      s_run[i] = i < n ? (bin.w[pool + (i >> 1)] >> (16u * (i & 1u))) & 0xffffu : 0x10000u;
   barrier();
   for (uint k = 2u; k <= m; k <<= 1)
      for (uint j = k >> 1; j > 0u; j >>= 1)
      {
         for (uint i = tid; i < m; i += 256u)
         {
            const uint l = i ^ j;
            if (l > i)
            {
               const uint a = s_run[i], b = s_run[l];
               if (((i & k) == 0u) == (a > b))
               {
                  s_run[i] = b;
                  s_run[l] = a;
               }
            }
         }
         barrier();
      }
   for (uint w = tid; w < (n + 1u) / 2u; w += 256u)
      bin.w[pool + w] = (s_run[2u * w] & 0xffffu) | ((2u * w + 1u < n ? s_run[2u * w + 1u] & 0xffffu : 0u) << 16);
}
#endif

#if PVRC_KERNEL == PVRC_K_CLEAR
layout(local_size_x = 256) in;

void main()
{
   const uint runs = pvrc_runs();
   const uint w = gl_GlobalInvocationID.x;
   if (w < bin.w[PVRC_B_STATS(runs) + PVRC_ST_WORDS])
      bin.w[PVRC_B_POOL(runs) + w] = 0u;
}
#endif

#if PVRC_KERNEL == PVRC_K_WALK
layout(local_size_x = 256) in;

layout(set = 0, binding = 3, rgba8) uniform writeonly image2D outImage;
layout(set = 0, binding = 4) uniform sampler2D lastPicture;
layout(set = 0, binding = 5) uniform sampler2D palette;
layout(set = 0, binding = 6) uniform sampler2D textures[PVRC_TEX_SLOTS];

shared uint s_prim[256];
shared uint s_kh[PVRC_KEEP * 256u];
shared uint s_kl[PVRC_KEEP * 256u];
shared uint s_kc[PVRC_KEEP * 256u];	/* (each fragment's colour, as the lists keep it) */
shared uint s_flag;

uint g_tid;
uint g_ct;	/* the coarse tile */
uvec4 g_fine;	/* the fine tile's pixels: x0, y0, x1, y1 (all in it) */
int g_px, g_py;	/* this lane's pixel */
int g_sx, g_sy;	/* ...its sample, in 24.8 */
int g_cx, g_cy;	/* ...the pixel's centre */
int g_s0x, g_s0y;	/* ...its first sample */
bool g_in;	/* inside the picture */

/* ---- The primitives of a range of numbers in the tile, in order ---- */

uint it_lo, it_hi, it_c, it_clast, it_j, it_end, it_k, it_n, it_cbase, it_run;
bool it_skip, it_started;

uint pvrc_pool_entry(uint at)
{
   return (bin.w[PVRC_B_POOL(pvrc_runs()) + (at >> 1)] >> (16u * (at & 1u))) & 0xffffu;
}

/* the first entry of a sorted run not below @v */
uint pvrc_lower_bound(uint start, uint count, uint v)
{
   uint lo = 0u, hi = count;
   while (lo < hi)
   {
      uint mid = (lo + hi) >> 1;
      if (pvrc_pool_entry(start + mid) < v)
         lo = mid + 1u;
      else
         hi = mid;
   }
   return lo;
}

void it_begin(uint lo, uint hi)
{
   it_lo = lo;
   it_hi = hi;
   it_c = lo >> PVR_CHUNK_LOG2;
   it_clast = hi > lo ? (hi - 1u) >> PVR_CHUNK_LOG2 : 0u;
   it_j = it_end = it_k = it_n = 0u;
   it_started = false;
}

/* The next primitive (PVRC_NONE: there are no more): in uniform control
 * flow, as every lane walks the same */
uint it_next()
{
   for (;;)
   {
      if (it_k < it_n)
      {
         uint p = s_prim[it_k];
         it_k++;
         return p;
      }
      if (it_j >= it_end)
      {
         if (it_started)
            it_c++;
         it_started = true;
         if (it_hi <= it_lo || it_c > it_clast)
            return PVRC_NONE;
         it_cbase = it_c << PVR_CHUNK_LOG2;
         const uint lo16 = max(it_lo, it_cbase) - it_cbase;
         const uint hi16 = min(it_hi - it_cbase, 65536u);
         const uint e = it_c * pvrc_tiles() + g_ct;
         const uint count = bin.w[PVRC_B_COUNT(pvrc_runs()) + e];
         const uint start = bin.w[PVRC_B_START(pvrc_runs()) + e];
         it_skip = (start & PVRC_RUN_SKIP) != 0u;
         if (count == 0u)
         {
            it_j = it_end = 0u;
         }
         else if (it_skip)
         {
            it_j = lo16;
            it_end = hi16;
         }
         else
         {
            it_run = start;
            it_j = pvrc_lower_bound(start, count, lo16);
            it_end = pvrc_lower_bound(start, count, hi16);
         }
         continue;
      }
      barrier();
      const uint at = it_j + g_tid;
      uint v = PVRC_NONE;
      if (at < it_end)
         v = it_skip ? it_cbase + at : it_cbase | pvrc_pool_entry(it_run + at);
      s_prim[g_tid] = v;
      it_n = min(256u, it_end - it_j);
      it_k = 0u;
      it_j += it_n;
      barrier();
   }
}

/* ---- A primitive at a lane ---- */

/* The shift a primitive's x and y are kept at (pvrc_xy_shift()) */
uint rec_shift(uint r)
{
   return (rec.w[r + PVRC_R_EXTRA] >> PVRC_RX_SHIFT_AT) & 7u;
}

/* The most an edge from (@ax, @ay) to (@bx, @by) has over the fine tile:
 * at one of its corners, for it is linear (at shift @sh) */
int64_t edge_max(int ax, int ay, int bx, int by, uint sh)
{
   const int64_t dx = int64_t(bx - ax), dy = int64_t(by - ay);
   const int64_t x0 = int64_t((int(g_fine.x) * 256 >> sh) - ax), x1 = int64_t((int(g_fine.z + 1u) * 256 >> sh) - ax);
   const int64_t y0 = int64_t((int(g_fine.y) * 256 >> sh) - ay), y1 = int64_t((int(g_fine.w + 1u) * 256 >> sh) - ay);
   return max(dx * y0, dx * y1) - min(dy * x0, dy * x1);
}

/* Whether a primitive can cover a sample of the fine tile: its pixels
 * meet the tile's, and no edge has the whole tile outside it (the same
 * for every lane, so the walk passes over it as one) */
bool rec_in_tile(uint r)
{
   const uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   if (!((b0 & 0xffffu) <= g_fine.z && (b1 & 0xffffu) >= g_fine.x
         && (b0 >> 16) <= g_fine.w && (b1 >> 16) >= g_fine.y))
      return false;
   const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
   const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
   const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
   const uint sh = rec_shift(r);
   return edge_max(x1, y1, x2, y2, sh) >= 0 && edge_max(x2, y2, x0, y0, sh) >= 0 && edge_max(x0, y0, x1, y1, sh) >= 0;
}

bool in_rect(uint r0, uint r1)
{
   return g_in && g_px >= int(r0 & 0xffffu) && g_py >= int(r0 >> 16) && g_px < int(r1 & 0xffffu) && g_py < int(r1 >> 16);
}

bool edge_in(int64_t e, int dx, int dy)
{
   return e > 0 || (e == 0 && (dy < 0 || (dy == 0 && dx > 0)));
}

/* The edges at (@px, @py) in 24.8: the one facing each vertex (at the
 * primitive's shift: a sample's place, a multiple of 16, divides exactly) */
i64vec3 rec_edges(uint r, int px, int py)
{
   const uint sh = rec_shift(r);
   px >>= sh;
   py >>= sh;
   const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
   const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
   const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
   return i64vec3(int64_t(x2 - x1) * int64_t(py - y1) - int64_t(y2 - y1) * int64_t(px - x1),
         int64_t(x0 - x2) * int64_t(py - y2) - int64_t(y0 - y2) * int64_t(px - x2),
         int64_t(x1 - x0) * int64_t(py - y0) - int64_t(y1 - y0) * int64_t(px - x0));
}

bool rec_inside(uint r, i64vec3 e)
{
   const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
   const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
   const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
   return edge_in(e.x, x2 - x1, y2 - y1) && edge_in(e.y, x0 - x2, y0 - y2) && edge_in(e.z, x1 - x0, y1 - y0);
}

/* Whether the lane's pixel is in a primitive's pixels: a few compares
 * before the edges, which most lanes of a tile need not reach */
bool rec_near(uint r)
{
   const uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   return uint(g_px) >= (b0 & 0xffffu) && uint(g_px) <= (b1 & 0xffffu) && uint(g_py) >= (b0 >> 16) && uint(g_py) <= (b1 >> 16);
}

bool rec_covers(uint r, int px, int py)
{
   return rec_near(r) && rec_inside(r, rec_edges(r, px, py));
}

/* What a fragment is shaded from at a place: its weights, and 1/w there */
struct Bary
{
   float f0, f1, f2, s;	/* barycentrics times each vertex's 1/w, and their sum */
   float w;	/* 1/w (gl_FragCoord.w) */
};

Bary rec_bary(uint r, i64vec3 e)
{
   Bary b;
   PVRC_INVARIANT float f0, f1, f2;
   PVRC_INVARIANT float b0 = float(e.x), b1 = float(e.y), b2 = float(e.z);
   PVRC_INVARIANT float area = float(e.x + e.y + e.z);
   f0 = b0 * uintBitsToFloat(rec.w[r + PVRC_R_Q]);
   f1 = b1 * uintBitsToFloat(rec.w[r + PVRC_R_Q + 1u]);
   f2 = b2 * uintBitsToFloat(rec.w[r + PVRC_R_Q + 2u]);
   b.f0 = f0;
   b.f1 = f1;
   b.f2 = f2;
   PVRC_INVARIANT float s = (f0 + f1) + f2;
   b.s = s;
   PVRC_INVARIANT float w = s / area;
   b.w = w;
   return b;
}

float bary_mix(Bary b, float a0, float a1, float a2)
{
   PVRC_PRECISE float v = ((b.f0 * a0 + b.f1 * a1) + b.f2 * a2) / b.s;
   return v;
}

uint vtxw(uint v, uint k)
{
   return frm.w[frm.w[PVRC_F_O_VTX] + v * PVRC_VTX_WORDS + k];
}

vec4 vtx_color(uint v, uint k)
{
   uint c = vtxw(v, k);
   return vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0;
}

/* A fragment's inputs, as the vertex shader hands them over */
struct Frag
{
   vec4 base, offs, base1, offs1;
   vec2 uv, uv1;
   vec2 duvx, duvy, duv1x, duv1y;	/* the coordinates' change a pixel across and down */
   float w;
   vec2 pos;	/* gl_FragCoord.xy */
};

vec4 frag_mix4(uint r, Bary b, uint k)
{
   const vec4 c0 = vtx_color(rec.w[r + PVRC_R_VTX], k);
   const vec4 c1 = vtx_color(rec.w[r + PVRC_R_VTX + 1u], k);
   const vec4 c2 = vtx_color(rec.w[r + PVRC_R_VTX + 2u], k);
   return vec4(bary_mix(b, c0.x, c1.x, c2.x), bary_mix(b, c0.y, c1.y, c2.y),
         bary_mix(b, c0.z, c1.z, c2.z), bary_mix(b, c0.w, c1.w, c2.w));
}

float frag_mix1(uint r, Bary b, uint k)
{
   return bary_mix(b, uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX], k)),
         uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX + 1u], k)),
         uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX + 2u], k)));
}

/* A perspective-correct coordinate's change for a step of the edges by
 * (@d0, @d1, @d2), from its numerator and denominator */
vec2 frag_duv(uint r, i64vec3 e, vec3 d, uint ku)
{
   const vec3 q = vec3(uintBitsToFloat(rec.w[r + PVRC_R_Q]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 1u]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 2u]));
   const vec3 eq = vec3(float(e.x), float(e.y), float(e.z)) * q;
   const vec3 dq = d * q;
   const float den = eq.x + eq.y + eq.z;
   const float dden = dq.x + dq.y + dq.z;
   vec2 res;
   for (uint c = 0u; c < 2u; c++)
   {
      const vec3 a = vec3(uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX], ku + c)),
            uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX + 1u], ku + c)),
            uintBitsToFloat(vtxw(rec.w[r + PVRC_R_VTX + 2u], ku + c)));
      const float num = dot(eq, a);
      const float dnum = dot(dq, a);
      res[c] = (dnum * den - num * dden) / (den * den);
   }
   return res;
}

/* A fragment's texture coordinates' derivatives, at the place whose edges
 * are @e (for texturing; test mode has no use for them) */
void frag_grads(uint r, uint flags, i64vec3 e, inout Frag f)
{
   if (PVRC_TEST == 0u && (flags & PVRC_QF_TEXTURE) != 0u)
   {
      const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
      const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
      const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
      /* a pixel across: each edge changes by -dy * 256, down by dx * 256
       * (256 at the primitive's shift) */
      const float step = float(256u >> rec_shift(r));
      const vec3 dx = vec3(float(-(y2 - y1)), float(-(y0 - y2)), float(-(y1 - y0))) * step;
      const vec3 dy = vec3(float(x2 - x1), float(x0 - x2), float(x1 - x0)) * step;
      f.duvx = frag_duv(r, e, dx, 5u);
      f.duvy = frag_duv(r, e, dy, 5u);
      if ((flags & PVRC_QF_TWO_VOLUMES) != 0u)
      {
         f.duv1x = frag_duv(r, e, dx, 9u);
         f.duv1y = frag_duv(r, e, dy, 9u);
      }
   }
}

/* The fragment of polygon triangle @r at a place in 24.8, whose edges are @e */
Frag rec_frag(uint r, uint pp, i64vec3 e, int px, int py)
{
   Frag f;
   const Bary b = rec_bary(r, e);
   const uint flags = frm.w[pp + PVRC_Q_FLAGS];
   const bool two = (flags & PVRC_QF_TWO_VOLUMES) != 0u;
   if ((flags & PVRC_QF_GOURAUD) != 0u)
   {
      f.base = frag_mix4(r, b, 3u);
      f.offs = frag_mix4(r, b, 4u);
      if (two)
      {
         f.base1 = frag_mix4(r, b, 7u);
         f.offs1 = frag_mix4(r, b, 8u);
      }
   }
   else
   {
      const uint v = rec.w[r + PVRC_R_EXTRA] & PVRC_RX_VTX;
      f.base = vtx_color(v, 3u);
      f.offs = vtx_color(v, 4u);
      if (two)
      {
         f.base1 = vtx_color(v, 7u);
         f.offs1 = vtx_color(v, 8u);
      }
   }
   f.uv = vec2(frag_mix1(r, b, 5u), frag_mix1(r, b, 6u));
   if (two)
      f.uv1 = vec2(frag_mix1(r, b, 9u), frag_mix1(r, b, 10u));
   f.w = b.w;
   f.pos = vec2(float(px), float(py)) / 256.0;
   frag_grads(r, flags, e, f);
   return f;
}

/* ---- Shading: vulkan/shaders/oit.frag's ---- */

#define PVRC_SHADE_DEPTH 0u	/* its depth pass: no stencil */
#define PVRC_SHADE_COLOR 1u	/* its colour pass: the stencil's shadow */
#define PVRC_SHADE_OIT 2u	/* into the lists: no stencil */

vec4 tex_fetch(uint slot, vec2 uv, vec2 dx, vec2 dy)
{
   if (PVRC_TEST != 0u)
   {
      const uint h = pvrc_test_texel(slot, uv.x, uv.y);
      return vec4(pvrc_from8(h), pvrc_from8(h >> 8), pvrc_from8(h >> 16), pvrc_from8(h >> 24));
   }
   return textureGrad(textures[slot], uv, dx, dy);
}

float fog_lut(float w)
{
   return pvrc_fog_lut(w, FRMF(PVRC_F_FOG_DEN));
}

#define PI 3.1415926

/* The colour of polygon @pp's fragment @f; @killed when it is discarded */
vec4 shade(uint pp, Frag f, uint pass, bool shadowed, out bool killed)
{
   killed = false;
   const uint flags = frm.w[pp + PVRC_Q_FLAGS];
   const uint tsp = frm.w[pp + PVRC_Q_TSP];
   const bool two = (flags & PVRC_QF_TWO_VOLUMES) != 0u;
   if ((flags & PVRC_QF_CLIP_INSIDE) != 0u)
   {
      const uint c0 = frm.w[pp + PVRC_Q_CLIP], c1 = frm.w[pp + PVRC_Q_CLIP + 1u];
      if (f.pos.x >= float(c0 & 0xffffu) && f.pos.x <= float(c1 & 0xffffu)
            && f.pos.y >= float(c0 >> 16) && f.pos.y <= float(c1 >> 16))
      {
         killed = true;
         return vec4(0.0);
      }
   }
   PVRC_PRECISE vec4 color = f.base;
   PVRC_PRECISE vec4 offset = f.offs;
   vec2 uv = f.uv;
   vec2 dx = f.duvx, dy = f.duvy;
   bool area1 = false;
   bool useAlpha = (flags & (two ? PVRC_QF_USE_ALPHA0 : PVRC_QF_USE_ALPHA)) != 0u;
   bool ignoreTexA = (flags & (two ? PVRC_QF_IGNORE_TEXA0 : PVRC_QF_IGNORE_TEXA)) != 0u;
   uint shadInstr = (flags >> PVRC_QF_SHAD_SHIFT) & 3u;
   uint fogCtrl = (flags >> PVRC_QF_FOG_SHIFT) & 3u;
   if (pass == PVRC_SHADE_COLOR && shadowed)
   {
      if (two)
      {
         color = f.base1;
         offset = f.offs1;
         uv = f.uv1;
         dx = f.duv1x;
         dy = f.duv1y;
         area1 = true;
         useAlpha = (flags & PVRC_QF_USE_ALPHA1) != 0u;
         ignoreTexA = (flags & PVRC_QF_IGNORE_TEXA1) != 0u;
         shadInstr = (flags >> PVRC_QF_SHAD1_SHIFT) & 3u;
         fogCtrl = (flags >> PVRC_QF_FOG1_SHIFT) & 3u;
      }
      else
      {
         const float scale = FRMF(PVRC_F_SHADE);
         color.rgb *= scale;
         offset.rgb *= scale;
      }
   }
   if (!useAlpha)
      color.a = 1.0;
   if ((flags & PVRC_QF_TEXTURE) != 0u)
   {
      const uint slots = frm.w[pp + PVRC_Q_TEX];
      PVRC_PRECISE vec4 texcol;
      if (two && area1)
         texcol = tex_fetch(slots >> 16, uv, dx, dy);
      else if ((flags & PVRC_QF_PALETTE) == 0u || PVRC_TEST != 0u)
         texcol = tex_fetch(slots & 0xffffu, uv, dx, dy);
      else
      {
         const float index = tex_fetch(slots & 0xffffu, uv, dx, dy).r;
         texcol = texelFetch(palette, ivec2(int(floor(index * 255.0 + 0.5)) + int(frm.w[pp + PVRC_Q_PAL]), 0), 0);
      }
      if ((flags & PVRC_QF_BUMP) != 0u && PVRC_TEST == 0u)
      {
         const float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
         const float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
         texcol.a = clamp(f.offs.a + f.offs.r * sin(s) + f.offs.g * cos(s) * cos(r - 2.0 * PI * f.offs.b), 0.0, 1.0);
         texcol.rgb = vec3(1.0);
      }
      else if (ignoreTexA)
         texcol.a = 1.0;
      if (shadInstr == 0u)
         color = texcol;
      else if (shadInstr == 1u)
      {
         color.rgb *= texcol.rgb;
         color.a = texcol.a;
      }
      else if (shadInstr == 2u)
         color.rgb = color.rgb * (1.0 - texcol.a) + texcol.rgb * texcol.a;	/* (mix(), its formula) */
      else
         color *= texcol;
      if ((flags & PVRC_QF_OFFSET) != 0u && ((flags & PVRC_QF_BUMP) == 0u || PVRC_TEST != 0u))
         color.rgb += offset.rgb;
   }
   if ((flags & PVRC_QF_CLAMP) != 0u)
      color = clamp(color, vec4(FRMF(PVRC_F_CLAMP_MIN), FRMF(PVRC_F_CLAMP_MIN + 1u), FRMF(PVRC_F_CLAMP_MIN + 2u), FRMF(PVRC_F_CLAMP_MIN + 3u)),
            vec4(FRMF(PVRC_F_CLAMP_MAX), FRMF(PVRC_F_CLAMP_MAX + 1u), FRMF(PVRC_F_CLAMP_MAX + 2u), FRMF(PVRC_F_CLAMP_MAX + 3u)));
   const vec3 fogRam = vec3(FRMF(PVRC_F_FOG_RAM), FRMF(PVRC_F_FOG_RAM + 1u), FRMF(PVRC_F_FOG_RAM + 2u));
   if (fogCtrl == 3u)
      color = vec4(fogRam, fog_lut(f.w) * (256.0 / 255.0));
   if (fogCtrl == 0u)
   {
      const float fog = fog_lut(f.w);
      color.rgb = color.rgb * (255.0 / 256.0 - fog) + fogRam * fog;
   }
   if ((flags & PVRC_QF_OFFSET) != 0u && ((flags & PVRC_QF_BUMP) == 0u || PVRC_TEST != 0u) && fogCtrl == 1u)
   {
      const float fog = offset.a * (255.0 / 256.0);
      color.rgb = color.rgb * (255.0 / 256.0 - fog) + vec3(FRMF(PVRC_F_FOG_VERT), FRMF(PVRC_F_FOG_VERT + 1u), FRMF(PVRC_F_FOG_VERT + 2u)) * fog;
   }
   color *= uintBitsToFloat(frm.w[pp + PVRC_Q_TRILIN]);
   if ((flags & PVRC_QF_ALPHA_TEST) != 0u)
   {
      if (FRMF(PVRC_F_ALPHA_REF) > color.a)
      {
         killed = true;
         return color;
      }
      color.a = 1.0;
   }
   return color;
}

uint col_pack(vec4 c)
{
   return pvrc_unorm8(c.r) | (pvrc_unorm8(c.g) << 8) | (pvrc_unorm8(c.b) << 16) | (pvrc_unorm8(c.a) << 24);
}

vec4 col_unpack(uint c)
{
   return vec4(pvrc_from8(c), pvrc_from8(c >> 8), pvrc_from8(c >> 16), pvrc_from8(c >> 24));
}

/* The device's blend of @src onto the 8-bit @dst by @tsp's instructions */
uint blend8(uint tsp, vec4 src, uint dst)
{
   const vec4 s = clamp(src, 0.0, 1.0);
   const vec4 d = col_unpack(dst);
   const uint si = tsp >> 29, di = (tsp >> 26) & 7u;
   PVRC_PRECISE vec4 res;
   for (uint c = 0u; c < 4u; c++)
   {
      const float sf = pvrc_blend_factor(si, 1u, s[c], s.a, d[c], d.a);
      const float df = pvrc_blend_factor(di, 0u, s[c], s.a, d[c], d.a);
      res[c] = s[c] * sf + d[c] * df;
   }
   return col_pack(res);
}

/* ---- The walk ---- */

/* Where a polygon's fragment is shaded and its depth taken: at the
 * sample, or once a pixel at the pixel's centre */
bool single_rate(uint pp)
{
   return PVRC_SAMPLES == 1u || (frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_SINGLE_RATE) != 0u;
}

uint rec_poly(uint r)
{
   return frm.w[PVRC_F_O_POLY] + rec.w[r + PVRC_R_SRC] * PVRC_POLY_WORDS;
}

/* Whether a polygon triangle covers this lane's sample, in its clip; @e
 * then its edges at the place the fragment is shaded, @pos (the sample, or
 * the centre for @centre or a single rate one). Its depth needs no more,
 * and only a fragment that passes the depth test is made (rec_frag()). */
bool lane_cover(uint r, uint pp, bool centre, out i64vec3 e, out ivec2 pos)
{
   if (!rec_near(r) || !in_rect(frm.w[pp + PVRC_Q_RECT], frm.w[pp + PVRC_Q_RECT + 1u]))
      return false;
   e = rec_edges(r, g_sx, g_sy);
   if (!rec_inside(r, e))
      return false;
   pos = ivec2(g_sx, g_sy);
   if (centre || single_rate(pp))
   {
      if (g_cx != g_sx || g_cy != g_sy)
         e = rec_edges(r, g_cx, g_cy);
      pos = ivec2(g_cx, g_cy);
   }
   return true;
}

/* Where a fragment shaded once a pixel takes its inputs: the centre where
 * the triangle covers all the pixel's samples, else the first sample it
 * covers (the device's centroid, Mesa's choice; the per-pixel renderer's
 * AT_CENTRE) */
ivec2 rate_pos(uint r)
{
   if (PVRC_SAMPLES == 1u)
      return ivec2(g_cx, g_cy);
   uint first = PVRC_SAMPLES;
   bool all = true;
   ivec2 at = ivec2(g_cx, g_cy);
   for (uint s = 0u; s < PVRC_SAMPLES; s++)
   {
      const int sx = g_px * 256 + int(SSAA_SAMPLE_X16(PVRC_SAMPLES, s)) * 16;
      const int sy = g_py * 256 + int(SSAA_SAMPLE_Y16(PVRC_SAMPLES, s)) * 16;
      const bool c = rec_inside(r, rec_edges(r, sx, sy));
      if (c && first == PVRC_SAMPLES)
      {
         first = s;
         at = ivec2(sx, sy);
      }
      all = all && c;
   }
   return all || first == PVRC_SAMPLES ? ivec2(g_cx, g_cy) : at;
}

/* ...its inputs there, with gl_FragCoord (its 1/w for the depth and the
 * fog) and its textures' derivatives at the centre, whose edges are @ec */
Frag rate_frag(uint r, uint pp, i64vec3 ec)
{
   const ivec2 at = rate_pos(r);
   if (at.x == g_cx && at.y == g_cy)
      return rec_frag(r, pp, ec, g_cx, g_cy);
   Frag f = rec_frag(r, pp, rec_edges(r, at.x, at.y), at.x, at.y);
   f.w = rec_bary(r, ec).w;
   f.pos = vec2(float(g_cx), float(g_cy)) / 256.0;
   /* (its textures filtered with the derivatives at the centre, as the
    * per-pixel renderer's once-a-pixel polygons are under SSAA) */
   frag_grads(r, frm.w[pp + PVRC_Q_FLAGS], ec, f);
   return f;
}

/* The fragment lane_cover() found: at the sample, or once a pixel */
Frag lane_frag(uint r, uint pp, i64vec3 e, ivec2 pos)
{
   if (PVRC_SAMPLES > 1u && single_rate(pp))
      return rate_frag(r, pp, e);
   return rec_frag(r, pp, e, pos.x, pos.y);
}

/* A volume triangle's 1/w at the pixel's centre */
float vol_w(uint r)
{
   return rec_bary(r, rec_edges(r, g_cx, g_cy)).w;
}

uint mvp_word(uint r, uint k)
{
   return frm.w[frm.w[PVRC_F_O_MVP] + (rec.w[r + PVRC_R_SRC] & ~PVRC_SRC_MODVOL) * PVRC_MVP_WORDS + k];
}

/* Which of a volume's covered samples a parameter that ends it with cull
 * mode @cull counts, of those the volume's front and back cover */
bool vol_close_counts(uint cov, uint cull)
{
   if (cull == 3u)
      cov &= PVRC_RX_FRONT;
   else if (cull == 2u)
      cov &= PVRC_RX_BACK;
   return cov != 0u;
}

uint g_col;	/* the colour attachment's, 8 bits a channel */
float g_depth;
uint g_stencil;	/* the shadow bit of what is there, 0x80, and the volume bits */

void main()
{
   g_tid = gl_LocalInvocationIndex;
   const uint fw = PVRC_FINE_W(PVRC_SAMPLES), fh = PVRC_FINE_H(PVRC_SAMPLES);
   const uint pix = g_tid >> PVRC_LOG2N, smp = g_tid & (PVRC_SAMPLES - 1u);
   g_fine.xy = gl_WorkGroupID.xy * uvec2(fw, fh);
   g_fine.zw = g_fine.xy + uvec2(fw - 1u, fh - 1u);
   g_ct = (g_fine.x >> frm.w[PVRC_F_TILE_LOG2]) + (g_fine.y >> frm.w[PVRC_F_TILE_LOG2]) * frm.w[PVRC_F_TILES_X];
   g_px = int(g_fine.x + pix % fw);
   g_py = int(g_fine.y + pix / fw);
   g_in = g_px < int(frm.w[PVRC_F_WIDTH]) && g_py < int(frm.w[PVRC_F_HEIGHT]);
   g_cx = g_px * 256 + 128;
   g_cy = g_py * 256 + 128;
   g_sx = g_px * 256 + int(SSAA_SAMPLE_X16(PVRC_SAMPLES, smp)) * 16;
   g_sy = g_py * 256 + int(SSAA_SAMPLE_Y16(PVRC_SAMPLES, smp)) * 16;
   g_s0x = g_px * 256 + int(SSAA_SAMPLE_X16(PVRC_SAMPLES, 0u)) * 16;
   g_s0y = g_py * 256 + int(SSAA_SAMPLE_Y16(PVRC_SAMPLES, 0u)) * 16;

   if ((frm.w[PVRC_F_FLAGS] & PVRC_FF_LAST_PICTURE) != 0u && g_in)
      g_col = col_pack(texelFetch(lastPicture, ivec2(g_px, g_py), 0));
   else
      g_col = frm.w[PVRC_F_CLEAR];
   g_depth = 0.0;
   uint peels = 0u;

   const uint passes = frm.w[PVRC_F_PASSES];
   for (uint pass = 0u; pass < passes; pass++)
   {
      const uint pw = PVRC_F_PASS + pass * PVRC_PASS_WORDS;
      const uint opLo = frm.w[pw + PVRC_P_OP], mvoLo = frm.w[pw + PVRC_P_MVO], ptLo = frm.w[pw + PVRC_P_PT];
      const uint trLo = frm.w[pw + PVRC_P_TR], trHi = frm.w[pw + PVRC_P_TR_END];
      const uint mvtLo = frm.w[pw + PVRC_P_MVT], mvtHi = frm.w[pw + PVRC_P_MVT_END];
      const bool autosort = (frm.w[pw + PVRC_P_FLAGS] & PVRC_PF_AUTOSORT) != 0u;
      const bool lastPass = pass + 1u == passes;
      uint prim;
      g_stencil = 0u;

      /* The depth subpass: opaque, then punch-through */
      it_begin(opLo, mvoLo);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         if (!rec_in_tile(r))
            continue;
         const uint pp = rec_poly(r);
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const float z = pvrc_depth(rec_bary(r, e).w);
         if (pvrc_depth_pass(isp >> 29, z, g_depth) == 0u)
            continue;
         if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
         {
            bool killed;
            shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
            if (killed)
               continue;
         }
         if ((isp & (1u << 26)) == 0u)
            g_depth = z;
         g_stencil = (g_stencil & 0x7fu) | ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u ? 0x80u : 0u);
      }
      it_begin(ptLo, trLo);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         if (!rec_in_tile(r))
            continue;
         const uint pp = rec_poly(r);
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         const float z = pvrc_depth(rec_bary(r, e).w);
         if (pvrc_depth_pass(6u, z, g_depth) == 0u)
            continue;
         bool killed;
         shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
         if (killed)
            continue;
         g_depth = z;	/* (punch-through always writes it) */
         g_stencil = (g_stencil & 0x7fu) | ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u ? 0x80u : 0u);
      }

      /* The opaque modifier volumes: the volume bits of each sample */
      {
         uint cov = 0u;	/* what the volume being sent covers, front and back */
         uint pending = PVRC_NONE;	/* the last triangle of the parameter that ends it */
         uint closer = 0u;	/* ...the triangle that tells which one */
         it_begin(mvoLo, ptLo);
         for (;;)
         {
            prim = it_next();
            if (pending != PVRC_NONE && (prim == PVRC_NONE || prim > pending))
            {
               /* the volume ends: in the closing parameter's clip, at the
                * samples its triangles cover as it culls them */
               const uint mp = frm.w[PVRC_F_O_MVP] + mvp_word(closer, PVRC_M_CLOSE_BY) * PVRC_MVP_WORDS;
               const uint cisp = frm.w[mp + PVRC_M_ISP];
               if (vol_close_counts(cov, (cisp >> 27) & 3u) && in_rect(frm.w[mp + PVRC_M_RECT], frm.w[mp + PVRC_M_RECT + 1u]))
                  g_stencil = (g_stencil & 0x80u) | pvr_modvol_close(g_stencil & 3u, cisp >> 29);
               cov = 0u;
               pending = PVRC_NONE;
            }
            if (prim == PVRC_NONE)
               break;
            const uint r = prim * PVRC_REC_WORDS;
            const uint end = mvp_word(r, PVRC_M_CLOSE_END);
            if (end != PVRC_NONE)
            {
               pending = end;
               closer = r;
            }
            if (!rec_in_tile(r) || !g_in)
               continue;
            if (!rec_covers(r, g_sx, g_sy))
               continue;
            const uint extra = rec.w[r + PVRC_R_EXTRA];
            cov |= extra & (PVRC_RX_FRONT | PVRC_RX_BACK);
            if ((extra & PVRC_RX_CULLED) != 0u || !in_rect(mvp_word(r, PVRC_M_RECT), mvp_word(r, PVRC_M_RECT + 1u)))
               continue;
            if (pvrc_depth(vol_w(r)) > g_depth)
               g_stencil = (g_stencil & 0x80u) | pvr_modvol_triangle(g_stencil & 3u, 1u, (extra & PVRC_RX_OR) != 0u ? 1u : 0u);
         }
      }
      /* (the stencil is 0x81 where a polygon that takes shadows is in one) */
      const bool shadowed = g_stencil == 0x81u;

      /* The colour subpass: opaque, punch-through */
      it_begin(opLo, mvoLo);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         if (!rec_in_tile(r))
            continue;
         const uint pp = rec_poly(r);
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const float z = pvrc_depth(rec_bary(r, e).w);
         if (pvrc_depth_pass((isp & (1u << 26)) == 0u ? 2u : isp >> 29, z, g_depth) == 0u)
            continue;
         bool killed;
         const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
         if (!killed)
            g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
      }
      it_begin(ptLo, trLo);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         if (!rec_in_tile(r))
            continue;
         const uint pp = rec_poly(r);
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const float z = pvrc_depth(rec_bary(r, e).w);
         if (pvrc_depth_pass((isp & (1u << 26)) == 0u ? 2u : 6u, z, g_depth) == 0u)
            continue;
         bool killed;
         const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
         if (!killed)
            g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
      }

      if (!autosort)
      {
         /* Translucent, in the order sent, blended as they come */
         it_begin(trLo, trHi);
         while ((prim = it_next()) != PVRC_NONE)
         {
            const uint r = prim * PVRC_REC_WORDS;
            if (!rec_in_tile(r))
               continue;
            const uint pp = rec_poly(r);
            i64vec3 e;
            ivec2 pos;
            if (!lane_cover(r, pp, false, e, pos))
               continue;
            const uint isp = frm.w[pp + PVRC_Q_ISP];
            const float z = pvrc_depth(rec_bary(r, e).w);
            if (pvrc_depth_pass(isp >> 29, z, g_depth) == 0u)
               continue;
            bool killed;
            const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
            if (killed)
               continue;
            if ((isp & (1u << 26)) == 0u)
               g_depth = z;
            g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
         }
      }
      else if (trHi > trLo)
      {
         /* Translucent, sorted: K fragments at a time, the farthest
          * first, blended in floats onto the colour as the last subpass
          * does; then the next K beyond the last one, until there are none */
         vec4 fc = col_unpack(g_col);
         vec4 sec = vec4(0.0);
         uint bh = 0u, bl = 0u;
         bool started = false;
         for (;;)
         {
            uint n = 0u;
            bool more = false;
            it_begin(trLo, trHi);
            while ((prim = it_next()) != PVRC_NONE)
            {
               const uint r = prim * PVRC_REC_WORDS;
               if (!rec_in_tile(r))
                  continue;
               const uint pp = rec_poly(r);
               if (!in_rect(frm.w[pp + PVRC_Q_RECT], frm.w[pp + PVRC_Q_RECT + 1u]))
                  continue;
               if (!rec_covers(r, g_sx, g_sy))
                  continue;
               const float w = vol_w(r);
               const float z = pvrc_depth(w);
               if (!(z >= g_depth))
                  continue;
               if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
               {
                  const uint c0 = frm.w[pp + PVRC_Q_CLIP], c1 = frm.w[pp + PVRC_Q_CLIP + 1u];
                  const float x = float(g_px) + 0.5, y = float(g_py) + 0.5;
                  if (x >= float(c0 & 0xffffu) && x <= float(c1 & 0xffffu) && y >= float(c0 >> 16) && y <= float(c1 >> 16))
                     continue;
               }
               const uint kh = pvr_key_hi(z, 0u);
               const uint kl = prim << 2;
               if (started && pvr_key_before(bh, bl, kh, kl) == 0u)
                  continue;	/* (taken already) */
               uint at;
               if (n < PVRC_KEEP)
                  at = n++;
               else
               {
                  more = true;
                  const uint li = (PVRC_KEEP - 1u) * 256u + g_tid;
                  if (pvr_key_before(kh, kl, s_kh[li], s_kl[li] & ~3u) == 0u)
                     continue;
                  at = PVRC_KEEP - 1u;
               }
               /* shaded now, as the per-pixel renderer shades a fragment
                * it puts in its lists: every lane here has the same
                * primitive, so the texture it reads is one for all */
               bool killed;
               const uint kc = col_pack(clamp(shade(pp, rate_frag(r, pp, rec_edges(r, g_cx, g_cy)), PVRC_SHADE_OIT, false, killed), 0.0, 1.0));
               while (at > 0u && pvr_key_before(kh, kl, s_kh[(at - 1u) * 256u + g_tid], s_kl[(at - 1u) * 256u + g_tid] & ~3u) != 0u)
               {
                  s_kh[at * 256u + g_tid] = s_kh[(at - 1u) * 256u + g_tid];
                  s_kl[at * 256u + g_tid] = s_kl[(at - 1u) * 256u + g_tid];
                  s_kc[at * 256u + g_tid] = s_kc[(at - 1u) * 256u + g_tid];
                  at--;
               }
               s_kh[at * 256u + g_tid] = kh;
               s_kl[at * 256u + g_tid] = kl;
               s_kc[at * 256u + g_tid] = kc;
            }

            /* The translucent modifier volumes, on the fragments kept: at
             * the pixel, where they cover its first sample */
            {
               uint cov = 0u;
               uint pending = PVRC_NONE;
               uint closer = 0u;
               it_begin(mvtLo, mvtHi);
               for (;;)
               {
                  prim = it_next();
                  if (pending != PVRC_NONE && (prim == PVRC_NONE || prim > pending))
                  {
                     const uint mp = frm.w[PVRC_F_O_MVP] + mvp_word(closer, PVRC_M_CLOSE_BY) * PVRC_MVP_WORDS;
                     const uint cisp = frm.w[mp + PVRC_M_ISP];
                     if (n != 0u && vol_close_counts(cov, (cisp >> 27) & 3u) && in_rect(frm.w[mp + PVRC_M_RECT], frm.w[mp + PVRC_M_RECT + 1u]))
                        for (uint j = 0u; j < n; j++)
                        {
                           const uint li = j * 256u + g_tid;
                           const uint fpp = frm.w[PVRC_F_O_POLY] + rec.w[(s_kl[li] >> 2) * PVRC_REC_WORDS + PVRC_R_SRC] * PVRC_POLY_WORDS;
                           if ((frm.w[fpp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u)
                              s_kl[li] = (s_kl[li] & ~3u) | pvr_modvol_close(s_kl[li] & 3u, cisp >> 29);
                        }
                     cov = 0u;
                     pending = PVRC_NONE;
                  }
                  if (prim == PVRC_NONE)
                     break;
                  const uint r = prim * PVRC_REC_WORDS;
                  const uint end = mvp_word(r, PVRC_M_CLOSE_END);
                  if (end != PVRC_NONE)
                  {
                     pending = end;
                     closer = r;
                  }
                  if (!rec_in_tile(r) || !g_in || n == 0u)
                     continue;
                  if (!rec_covers(r, g_s0x, g_s0y))
                     continue;
                  const uint extra = rec.w[r + PVRC_R_EXTRA];
                  cov |= extra & (PVRC_RX_FRONT | PVRC_RX_BACK);
                  if ((extra & PVRC_RX_CULLED) != 0u || !in_rect(mvp_word(r, PVRC_M_RECT), mvp_word(r, PVRC_M_RECT + 1u)))
                     continue;
                  const float vz = pvrc_depth(vol_w(r));
                  for (uint j = 0u; j < n; j++)
                  {
                     const uint li = j * 256u + g_tid;
                     const uint fpp = frm.w[PVRC_F_O_POLY] + rec.w[(s_kl[li] >> 2) * PVRC_REC_WORDS + PVRC_R_SRC] * PVRC_POLY_WORDS;
                     if ((frm.w[fpp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u && vz >= uintBitsToFloat(s_kh[li]))
                        s_kl[li] = (s_kl[li] & ~3u) | pvr_modvol_triangle(s_kl[li] & 3u, 1u, (extra & PVRC_RX_OR) != 0u ? 1u : 0u);
                  }
               }
            }

            /* ...blended, farthest first */
            for (uint j = 0u; j < n; j++)
            {
               const uint li = j * 256u + g_tid;
               const uint r = (s_kl[li] >> 2) * PVRC_REC_WORDS;
               const uint pp = rec_poly(r);
               const bool inVol = (s_kl[li] & PVR_MV_INSIDE) != 0u;
               const bool two = frm.w[pp + PVRC_Q_TSP1] != 0xffffffffu;	/* (isTwoVolumes() of oit_header.glsl) */
               const uint tsp = (inVol && two) ? frm.w[pp + PVRC_Q_TSP1] : frm.w[pp + PVRC_Q_TSP];
               vec4 src;
               if (((tsp >> 25) & 1u) != 0u)
                  src = sec;
               else
               {
                  src = col_unpack(s_kc[li]);
                  if (inVol && !two)
                     src.rgb = src.rgb * FRMF(PVRC_F_SHADE);
               }
               const bool toSec = ((tsp >> 24) & 1u) != 0u;
               const vec4 dst = toSec ? sec : fc;
               PVRC_PRECISE vec4 res;
               for (uint c = 0u; c < 4u; c++)
               {
                  const float sf = pvrc_blend_factor(tsp >> 29, 1u, src[c], src.a, dst[c], dst.a);
                  const float df = pvrc_blend_factor((tsp >> 26) & 7u, 0u, src[c], src.a, dst[c], dst.a);
                  res[c] = dst[c] * df + src[c] * sf;
               }
               res = clamp(res, 0.0, 1.0);
               if (toSec)
                  sec = res;
               else
                  fc = res;
            }
            if (n != 0u)
            {
               bh = s_kh[(n - 1u) * 256u + g_tid];
               bl = s_kl[(n - 1u) * 256u + g_tid] & ~3u;
               started = true;
            }
            peels++;
            /* again, for every lane, while any has more */
            if (g_tid == 0u)
               s_flag = 0u;
            barrier();
            if (more)
               atomicOr(s_flag, 1u);
            barrier();
            const bool again = s_flag != 0u;
            barrier();
            if (!again)
               break;
         }
         g_col = col_pack(fc);
      }

      if (!lastPass)
      {
         /* the translucent polygons' depth, for the render passes after */
         it_begin(trLo, trHi);
         while ((prim = it_next()) != PVRC_NONE)
         {
            const uint r = prim * PVRC_REC_WORDS;
            if (!rec_in_tile(r))
               continue;
            const uint pp = rec_poly(r);
            i64vec3 e;
            ivec2 pos;
            if (!lane_cover(r, pp, false, e, pos))
               continue;
            const uint isp = frm.w[pp + PVRC_Q_ISP];
            const float z = pvrc_depth(rec_bary(r, e).w);
            if (pvrc_depth_pass(autosort ? 6u : isp >> 29, z, g_depth) == 0u)
               continue;
            if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
            {
               bool killed;
               shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
               if (killed)
                  continue;
            }
            if ((isp & (1u << 26)) == 0u)
               g_depth = z;
         }
      }
   }

   /* The samples of each pixel resolved, and the pixel written */
   const uint c = pvr_resolve_lanes(g_col, PVRC_LOG2N);
   /* (a quarter of a step up: the device's conversion to 8 bits, whether
    * it rounds or truncates, and however it divides, gives the byte) */
   if (smp == 0u && g_in)
      imageStore(outImage, ivec2(g_px, g_py), (vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) + 0.25) / 255.0);
   if (g_tid == 0u && peels > 1u)
      atomicMax(bin.w[PVRC_B_STATS(pvrc_runs()) + PVRC_ST_PEELS], peels);
}
#endif

#endif /* the kernels */

#endif
