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
 *   vulkan/compute_spv.inc);
 * - by an HLSL compiler (shader model 6.0, HLSL 2021), with
 *   PVRC_SHADER_HLSL defined instead, and PVRC_SAMPLES, PVRC_KEEP and
 *   PVRC_TEST, Vulkan's specialization constants, defined too: the
 *   kernels' text is GLSL's, which PVRC_SHADER_HLSL's words below make
 *   HLSL of, but for what the preprocessor cannot change, which
 *   tools/rend/kernels_dx12.sh changes as it makes the Direct3D 12
 *   renderers' DXIL of them (dx12/dx12_compute_dxil.inc): a buffer's
 *   words "b.w[i]" read "b_w[i]", a kernel's "layout(local_size_x = N)
 *   in;" its threads, a global variable is "static", a vector of one
 *   value "vec4(v)" is "(vec4)(v)", a 64-bit constant "Nl" is
 *   "int64_t(N)", and an atomic whose old value is used has it in a
 *   variable.
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
 *             every coarse tile (2^log2 pixels square) those pixels touch
 *             and its edges do not all leave out, for its chunk (65536
 *             primitives, pvr_tile.h).
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
 *             A list's run is read 256 entries at a time, an entry a lane,
 *             and only the primitives that can cover a sample of the fine
 *             tile are kept, in their order (a bit a lane in shared
 *             memory, counted): the lanes walk those alone. The walks
 *             that test against the depth the depth subpass leaves (the
 *             colour subpass's, the sorted translucent's) keep only the
 *             primitives that can pass that test somewhere in the tile, by
 *             its least and most depth; the walks that write depth pass
 *             over the primitives whose depth is below the least any lane
 *             of the tile has as they come. In the colour subpass a lane
 *             shades the primitive that wrote its depth and looks at no
 *             other (unless another could have the same depth there).
 *             A fine tile of a coarse tile with nothing binned to it is
 *             written straight away.
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
 * translucent fragment is shaded once a pixel, depth tested at each sample
 * it covers with its depth there, and kept and sorted with its depth at
 * the centroid of the samples it is seen at (the centre's where it is seen
 * at all of them); a modifier volume's depths are the centre's. Texture filtering is the
 * device's sampler, given the derivatives a fragment shader has: the
 * differences across a 2x2 block of pixels, from the setup's planes
 * (frag_grads()).
 *
 * Test mode (spec constant PVRC_TEST, tools/rend/tile_test): the depth
 * compared is 1/w itself and a texture is a function of its coordinates,
 * so that the CPU's reference can do exactly what the kernels do. */
#ifndef PVR_COMPUTE_H
#define PVR_COMPUTE_H

/* (either shading language's kernel) */
#if defined(PVRC_SHADER_GLSL) || defined(PVRC_SHADER_HLSL)
#define PVRC_SHADER
#endif

#if !defined(PVRC_SHADER)
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
#define PVRC_K_GPUSRC  7   /* (a frame of the decode's, first) */
#define PVRC_KERNELS   7

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
#define PVRC_F_ZCLIP      45u  /* (float) the raster renderers' depth scale (1/fZ_max, rend/depth_scale.h) over the 1/w scale (XFORM's fifth): where they clip */
#define PVRC_F_GPU        46u  /* a frame of the GPU's TA decode (PVRC_K_GPUSRC): the indices the decode has room for; 0 for a frame of the CPU's parse */
#define PVRC_F_PRIM_ROOM  47u  /* (a frame of the decode's) the primitives the setup has room for */
#define PVRC_F_FOG        48u  /* the fog table, 256 bytes as MakeFogTexture() lays them out */
#define PVRC_F_PASS       112u /* the render passes, PVRC_PASS_WORDS each */
#define PVRC_PASS_WORDS   8u
#define PVRC_PASS_MAX     10u
#define PVRC_F_WORDS      192u

#define PVRC_FF_LAST_PICTURE 1u   /* start from the last picture, not the clear colour */
#define PVRC_FF_LAST_SCALED  2u   /* ...which is of another size (kept from another context): filtered over the picture, as the raster renderer's quad does */

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
 * triangles, then its kind and index, then (a polygon's) its first index,
 * in number order.
 *
 * A frame of the GPU's TA decode (PVRC_F_GPU) has its polygons' index
 * ranges only on the GPU, so the CPU writes the table without the starts
 * and PVRC_K_GPUSRC fills them in. The CPU's third word is then a
 * volume's number of triangles, or a polygon's draw group: the slot of its
 * indirect command (TAG_H_SLOT), or with PVRC_SRC_TRD its place in the
 * translucent draw records (TAG_H_TRB: a count of strips, then a record
 * for each, whose indices follow one another); and a render pass's words
 * (PVRC_P_*, but the flags) and a volume's PVRC_M_CLOSE_END are entries of
 * the table, which it makes the numbers of their first (last) triangles.
 * A command's third word becomes its first index; a translucent group is
 * one parameter, whose triangles are those of its index range from its
 * first strip's first index to its last's last, and the ones that reach
 * past the end of their strip draw nothing (the setup looks its strip up). */
#define PVRC_SRC_WORDS 3u
#define PVRC_SRC_MODVOL 0x80000000u
#define PVRC_SRC_TRD    0x40000000u
#define PVRC_SRC_INDEX  0x3fffffffu

/* What PVRC_K_GPUSRC reads of the decode's buffers (rend/ta_gpu.h: TAG_C_*,
 * TAG_O_*; vulkan/compute_drawer.cpp checks that they are the same) */
#define PVRC_TAG_C_WORDS   5u
#define PVRC_TAG_C_COUNT   0u
#define PVRC_TAG_C_FIRST   2u
#define PVRC_TAG_O_OVERRUN 3u
#define PVRC_TAG_O_ZSCALE  176u

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
#define PVRC_RX_VTX   0x003fffffu  /* (a polygon's) its first vertex */
#define PVRC_RX_NEG_AT 22u   /* (either's) a bit for each vertex behind the eye, in the order kept: see pvrc_rec_hits() */
#define PVRC_RX_NEG   (7u << PVRC_RX_NEG_AT)
#define PVRC_RX_SHIFT_AT 26u  /* the shift its x and y are kept at, 4 bits (either's) */

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
#define PVRC_ST_DROPPED   5u   /* triangles with a vertex at no depth (or not a number), left out */
#define PVRC_ST_PEELS     6u   /* the most rounds of translucent fragments a tile took */
#define PVRC_ST_HIZ       7u   /* primitives the walks passed over by the tiles' depths */
#define PVRC_ST_CLIPPED   8u   /* triangles with a vertex behind the eye, clipped */
#define PVRC_STATS        9u
#define PVRC_ST_SETUP_X   9u   /* (a frame of the decode's) the setup's and the scatter's dispatch: x, y, z */
#define PVRC_ST_SORT_X    12u  /* ...the sort's */

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
#define PVRC_WALK_SHARED_FIXED 16896u
#define PVRC_FRAGMENT_BYTES 12u
#if !defined(PVRC_SHADER)
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

#if PVRC_KERNEL == PVRC_K_GPUSRC
layout(std430, set = 0, binding = 0) coherent buffer Frame { uint w[]; } frm;
#else
layout(std430, set = 0, binding = 0) readonly buffer Frame { uint w[]; } frm;
#endif
layout(std430, set = 0, binding = 1) buffer Records { uint w[]; } rec;
layout(std430, set = 0, binding = 2) buffer Bins { uint w[]; } bin;
/* The vertices, indices and volume triangles (a frame of the parse's:
 * the frame's own words, at PVRC_F_O_VTX and the others; of the decode's,
 * its buffers, from 0); the decode's commands, translucent draw records
 * and frame words (PVRC_K_GPUSRC, and the setup's translucent strips) */
layout(std430, set = 0, binding = 7) readonly buffer Vertices { uint w[]; } vtx;
layout(std430, set = 0, binding = 8) readonly buffer Indices { uint w[]; } idx;
layout(std430, set = 0, binding = 9) readonly buffer VolumeTriangles { uint w[]; } mvt;
layout(std430, set = 0, binding = 10) readonly buffer Commands { uint w[]; } dcmd;
layout(std430, set = 0, binding = 11) readonly buffer TrDraws { uint w[]; } dtrd;
layout(std430, set = 0, binding = 12) readonly buffer DecodeFrame { uint w[]; } dfrm;

#define FRMF(i) uintBitsToFloat(frm.w[i])

#elif defined(PVRC_SHADER_HLSL) && defined(PVRC_KERNEL)
/* Direct3D 12 (dx12/dx12_compute.cpp): the buffers as a table of views,
 * unordered access views all of them (the frame's words are in memory the
 * CPU writes, which a shader resource view could have too, but the
 * decode's frame has them written by PVRC_K_GPUSRC), in the order of
 * GLSL's bindings - views and not root descriptors: what a kernel reads
 * past a buffer's end is 0 then, as Vulkan's robust buffers have it,
 * where a compiler may read a word before the test that would have left
 * it unread (the walk's rec_poly() of PVRC_NONE); for the walk, the textures' slots (a word each: the
 * texture's place in the renderer's bindless table | its sampler's in the
 * sampler table << 16), the bindless table, the samplers, the places in
 * the table of the picture to start from and of the palette, and the
 * picture drawn, a view of its own. */
#ifndef PVRC_SAMPLES
#define PVRC_SAMPLES 1u
#endif
#define PVRC_LOG2N SSAA_LOG2(PVRC_SAMPLES)
#ifndef PVRC_KEEP
#define PVRC_KEEP 4u
#endif
#ifndef PVRC_TEST
#define PVRC_TEST 0u
#endif

#if PVRC_KERNEL == PVRC_K_GPUSRC
globallycoherent RWStructuredBuffer<uint> frm_w : register(u0);
#else
RWStructuredBuffer<uint> frm_w : register(u0);
#endif
RWStructuredBuffer<uint> rec_w : register(u1);
RWStructuredBuffer<uint> bin_w : register(u2);
RWStructuredBuffer<uint> vtx_w : register(u3);
RWStructuredBuffer<uint> idx_w : register(u4);
RWStructuredBuffer<uint> mvt_w : register(u5);
RWStructuredBuffer<uint> dcmd_w : register(u6);
RWStructuredBuffer<uint> dtrd_w : register(u7);
RWStructuredBuffer<uint> dfrm_w : register(u8);
#if PVRC_KERNEL == PVRC_K_WALK
RWStructuredBuffer<uint> slot_w : register(u9);
cbuffer PvrcPictures : register(b0)
{
   uint pvrc_last;		/* the picture to start from, in the table */
   uint pvrc_palette;	/* the palette */
};
Texture2D<float4> pvrc_table[] : register(t0, space1);
SamplerState pvrc_samplers[] : register(s0, space1);
SamplerState pvrc_linear : register(s0);
RWTexture2D<unorm float4> outImage : register(u0, space2);
#define lastPicture pvrc_table[pvrc_last]
#define palette pvrc_table[pvrc_palette]
#define PVRC_TEXTURE_GRAD(slot, uv, dx, dy) pvrc_table[NonUniformResourceIndex(slot_w[slot] & 0xffffu)].SampleGrad( \
      pvrc_samplers[NonUniformResourceIndex(slot_w[slot] >> 16)], uv, dx, dy)
#define textureLod(t, uv, lod) t.SampleLevel(pvrc_linear, uv, lod)
#define texelFetch(t, p, lod) t.Load(int3(p, lod))
#define imageStore(image, p, v) image[p] = (v)
#endif

/* (GLSL's words in the kernels, in HLSL's) */
#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define uvec2 uint2
#define uvec4 uint4
#define i64vec3 int64_t3
#define uintBitsToFloat asfloat
#define floatBitsToUint asuint
#define bitCount countbits
#define findLSB firstbitlow
#define shared groupshared
#define barrier GroupMemoryBarrierWithGroupSync
#define memoryBarrierBuffer DeviceMemoryBarrier
#define atomicAdd(a, b) InterlockedAdd(a, b)
#define atomicOr(a, b) InterlockedOr(a, b)
#define atomicMin(a, b) InterlockedMin(a, b)
#define atomicMax(a, b) InterlockedMax(a, b)
/* (the invocation's numbers, which HLSL has as the entry point's
 * arguments: set by pvrc_entry(), at the end, which calls the kernel's
 * main(); the sort, the one kernel that asks how many workgroups there
 * are, is always dispatched a workgroup a coarse tile across) */
#define main pvrc_main
static uint3 gl_GlobalInvocationID;
static uint3 gl_WorkGroupID;
static uint gl_LocalInvocationIndex;
#define gl_NumWorkGroups uint3(pvrc_tiles(), 1u, 1u)

#define FRMF(i) uintBitsToFloat(frm.w[i])
#endif

/* ---- The rules ---- */

/* Every operation as it is written, none fused (tools/rend/tile_test builds
 * the kernels with PVRC_EXACT, for the reference to have their bits; the
 * renderer's are made without, and the device fuses where it likes) */
#if defined(PVRC_SHADER) && defined(PVRC_EXACT)
#define PVRC_PRECISE precise
#else
#define PVRC_PRECISE
#endif
/* ...but where a fragment is and its depth, always: the depth a pass
 * writes is compared for equality with the one another pass takes at the
 * same place, and the two have to be the same bits (as the raster
 * renderer's depth and colour passes have them, from one formula) */
#if defined(PVRC_SHADER)
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
 * with the places it is tested at divided the same way, to the nearest
 * (rec_edges()), its edges are the same lines to 2^shift / 512 pixel at
 * the vertices that are near (a vertex out there has no bits that fine
 * anyway: a float that far). Up to 4 a sample's place (1/16 pixels)
 * divides exactly, up to 7 a pixel's centre. Games put vertices near the
 * eye that far out: Virtua Fighter 3tb's floor has them 8.6 million
 * pixels out at 1x, 69 million at 8x (a shift of 6). Up to 15, 2^36
 * pixels; past that a vertex is taken at the edge. */
#define PVRC_SHIFT_MOST 15u
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
#if defined(PVRC_SHADER)
#define PVRC_FOG_BYTE(i) ((frm.w[PVRC_F_FOG + ((i) >> 2)] >> (8u * ((i) & 3u))) & 255u)
#endif
PVR_FN pvr_f32 pvrc_fog_lut(pvr_f32 w, pvr_f32 density
#if !defined(PVRC_SHADER)
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
#if defined(PVRC_SHADER)
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
#if defined(PVRC_SHADER)
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

#if !defined(PVRC_SHADER)
/* Words of the bin buffer for @runs runs and a pool of @pool entries */
PVR_FN pvr_u32 pvrc_bin_words(pvr_u32 runs, pvr_u32 pool)
{
   return PVRC_B_POOL(runs) + (pool + 1u) / 2u;
}
#endif

/* ---- The kernels ---- */

#if defined(PVRC_SHADER) && defined(PVRC_KERNEL)



uint pvrc_runs() { return frm.w[PVRC_F_RUNS]; }
uint pvrc_tiles() { return frm.w[PVRC_F_TILES_X] * frm.w[PVRC_F_TILES_Y]; }

#if PVRC_KERNEL == PVRC_K_SETUP || PVRC_KERNEL == PVRC_K_SCATTER || PVRC_KERNEL == PVRC_K_WALK
/* The shift a primitive's x and y are kept at (pvrc_xy_shift()) */
uint rec_shift(uint r)
{
   return (rec.w[r + PVRC_R_EXTRA] >> PVRC_RX_SHIFT_AT) & 15u;
}

/* The most the edge from (@ax, @ay) to (@bx, @by) has over the pixels
 * @x0 to @x1, @y0 to @y1 (all in them), at shift @sh: at one of their
 * corners, for it is linear (a sample's place, shifted, is inside them;
 * the corners rounded outwards where they do not divide exactly); with
 * @neg, the most the edge turned the other way has */
int64_t pvrc_edge_max(int ax, int ay, int bx, int by, int x0, int y0, int x1, int y1, uint sh, bool neg)
{
   const int64_t dx = int64_t(bx - ax), dy = int64_t(by - ay);
   const int64_t px0 = int64_t((x0 * 256 >> sh) - ax), px1 = int64_t(-(-(x1 + 1) * 256 >> sh) - ax);
   const int64_t py0 = int64_t((y0 * 256 >> sh) - ay), py1 = int64_t(-(-(y1 + 1) * 256 >> sh) - ay);
   if (neg)
      return max(dy * px0, dy * px1) - min(dx * py0, dx * py1);
   return max(dx * py0, dx * py1) - min(dy * px0, dy * px1);
}

/* The edges at (@px, @py), a place already at the primitive's shift: the
 * one facing each vertex */
i64vec3 rec_edges_at(uint r, int px, int py)
{
   const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
   const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
   const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
   return i64vec3(int64_t(x2 - x1) * int64_t(py - y1) - int64_t(y2 - y1) * int64_t(px - x1),
         int64_t(x0 - x2) * int64_t(py - y2) - int64_t(y0 - y2) * int64_t(px - x2),
         int64_t(x1 - x0) * int64_t(py - y0) - int64_t(y1 - y0) * int64_t(px - x0));
}

/* The edges at (@px, @py) in 24.8 (not negative), at the primitive's
 * shift: the place divided down to the nearest, as the vertices are. Up to
 * a shift of 4 a sample's place, a multiple of 16, divides exactly, and up
 * to 7 a pixel's centre; past that a place moves by less than half a unit
 * at the shift, as the triangle's near vertices do, and every triangle at
 * that shift is tested at the same places, so two that share an edge
 * still share its samples. */
i64vec3 rec_edges(uint r, int px, int py)
{
   const uint sh = rec_shift(r);
   const int hf = (1 << sh) >> 1;
   return rec_edges_at(r, (px + hf) >> sh, (py + hf) >> sh);
}

/* A triangle with a vertex behind the eye (its 1/w below 0), as the raster
 * renderers draw it: the device clips it in its clip space, where a vertex
 * is (x*w, y*w, Zc, w) with w = 1/z and Zc = min(S, w) (shaders/position.glsl,
 * S its depth scale), to Zc >= 0, and draws what is left. That is the
 * triangle's plane in homogeneous coordinates: where a place's barycentrics
 * there are all positive and its Zc is not negative.
 *
 * A vertex's x and y on the screen are the same point whatever the sign of
 * its w, and edge i times vertex i's 1/w is, up to one factor for the
 * whole triangle, its homogeneous edge function: so the edges stay as they
 * are, but the one facing a vertex behind the eye is turned the other way
 * (the triangle is then the part of the plane beyond the other two: what
 * lies before the eye of the two wedges its edges make), its facing is the
 * other one for an odd number of them behind, and the barycentrics, 1/w
 * and every attribute come from the same edges and 1/w as before
 * (rec_bary()). Zc over w at a vertex is its depth in the raster renderer,
 * min(S z, 1) for one in front and 1 for one behind, so the clip is one
 * more edge, a linear function of the place too:
 *    sum of edge i times 1/w of vertex i times min(S z, 1) of vertex i
 * not below 0, z here 1/w over the 1/w scale (PVRC_F_ZCLIP is S over it).
 * In floats, from the edges' integers, the same in the setup, the walk and
 * the reference; nothing is clipped where no vertex is behind the eye. */
vec3 rec_clip_plane(uint r)
{
   const float zs = FRMF(PVRC_F_ZCLIP);
   vec3 g;
   for (uint k = 0u; k < 3u; k++)
   {
      const float q = uintBitsToFloat(rec.w[r + PVRC_R_Q + k]);
      PVRC_INVARIANT float d = q * zs;
      PVRC_INVARIANT float gk = q * d;
      g[k] = q > 0.0 && d < 1.0 ? gk : q;
   }
   return g;
}

float rec_clip_at(vec3 g, i64vec3 e)
{
   PVRC_INVARIANT float a = float(e.x) * g.x;
   PVRC_INVARIANT float b = float(e.y) * g.y;
   PVRC_INVARIANT float c = float(e.z) * g.z;
   PVRC_INVARIANT float n = (a + b) + c;
   return n;
}

/* Whether no edge of primitive @r has all those pixels' samples outside
 * it, nor the clip of a triangle with a vertex behind the eye: the clip,
 * whose floats have rounded, with room for that */
bool pvrc_rec_hits(uint r, int x0, int y0, int x1, int y1)
{
   const int ax = int(rec.w[r]), ay = int(rec.w[r + 1u]);
   const int bx = int(rec.w[r + 2u]), by = int(rec.w[r + 3u]);
   const int cx = int(rec.w[r + 4u]), cy = int(rec.w[r + 5u]);
   const uint sh = rec_shift(r);
   const uint neg = (rec.w[r + PVRC_R_EXTRA] >> PVRC_RX_NEG_AT) & 7u;
   if (!(pvrc_edge_max(bx, by, cx, cy, x0, y0, x1, y1, sh, (neg & 1u) != 0u) >= 0
         && pvrc_edge_max(cx, cy, ax, ay, x0, y0, x1, y1, sh, (neg & 2u) != 0u) >= 0
         && pvrc_edge_max(ax, ay, bx, by, x0, y0, x1, y1, sh, (neg & 4u) != 0u) >= 0))
      return false;
   if (neg == 0u)
      return true;
   const vec3 g = rec_clip_plane(r);
   float most = -3.4e38, room = 0.0;
   for (uint c = 0u; c < 4u; c++)
   {
      /* (the corners rounded outwards, as pvrc_edge_max() has them) */
      const i64vec3 e = rec_edges_at(r, (c & 1u) != 0u ? -(-(x1 + 1) * 256 >> sh) : x0 * 256 >> sh,
            (c & 2u) != 0u ? -(-(y1 + 1) * 256 >> sh) : y0 * 256 >> sh);
      most = max(most, rec_clip_at(g, e));
      room = max(room, dot(abs(vec3(float(e.x), float(e.y), float(e.z))), abs(g)));
   }
   return most + 1e-5 * room >= 0.0;
}
#endif

#if PVRC_KERNEL == PVRC_K_SETUP || PVRC_KERNEL == PVRC_K_SCATTER
/* The coarse tiles a primitive's pixels touch, first x | y << 16 and last */
uvec2 pvrc_tile_range(uint r)
{
   uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   uint l2 = frm.w[PVRC_F_TILE_LOG2];
   return uvec2(((b0 & 0xffffu) >> l2) | (((b0 >> 16) >> l2) << 16),
         ((b1 & 0xffffu) >> l2) | (((b1 >> 16) >> l2) << 16));
}

/* Whether a primitive is binned to a coarse tile of its range: one its
 * edges do not all leave out (the setup counts, the scatter places, by
 * the same test) */
bool pvrc_tile_hit(uint r, uint tx, uint ty)
{
   const uint l2 = frm.w[PVRC_F_TILE_LOG2];
   const int x0 = int(tx << l2), y0 = int(ty << l2);
   return pvrc_rec_hits(r, x0, y0, x0 + int(1u << l2) - 1, y0 + int(1u << l2) - 1);
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
   const uint kind = frm.w[osrc + lo * PVRC_SRC_WORDS + 1u];
   const uint third = frm.w[osrc + lo * PVRC_SRC_WORDS + 2u];
   const bool modvol = (kind & PVRC_SRC_MODVOL) != 0u;
   const uint index = kind & PVRC_SRC_INDEX;
   const uint src = kind & ~PVRC_SRC_TRD;
   /* a triangle of a translucent group that reaches past the end of its
    * strip draws nothing: its indices are not read; the others are their
    * strip's, whose first is even (a strip's triangles turn by their
    * place in it) */
   bool between = false;
   uint first = third, parity = 0u;
   if ((kind & PVRC_SRC_TRD) != 0u)
   {
      const uint n = dtrd.w[third * PVRC_TAG_C_WORDS];
      first = dtrd.w[(third + 1u) * PVRC_TAG_C_WORDS + PVRC_TAG_C_FIRST];
      const uint at = first + i;
      uint slo = 0u, shi = n;
      while (shi - slo > 1u)
      {
         const uint mid = (slo + shi) >> 1;
         if (dtrd.w[(third + 1u + mid) * PVRC_TAG_C_WORDS + PVRC_TAG_C_FIRST] <= at)
            slo = mid;
         else
            shi = mid;
      }
      const uint sr = (third + 1u + slo) * PVRC_TAG_C_WORDS;
      between = !(at + 2u < dtrd.w[sr + PVRC_TAG_C_FIRST] + dtrd.w[sr + PVRC_TAG_C_COUNT]);
      parity = (dtrd.w[sr + PVRC_TAG_C_FIRST] - first) & 1u;
   }
   if (between)
   {
      const uint r = prim * PVRC_REC_WORDS;
      rec.w[r + PVRC_R_SRC] = src;
      rec.w[r + PVRC_R_EXTRA] = 0u;
      rec.w[r + PVRC_R_BBOX0] = 0xffffffffu;
      rec.w[r + PVRC_R_BBOX1] = 0u;
      return;
   }

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
         vx[k] = uintBitsToFloat(mvt.w[t + k * 3u]);
         vy[k] = uintBitsToFloat(mvt.w[t + k * 3u + 1u]);
         vz[k] = uintBitsToFloat(mvt.w[t + k * 3u + 2u]);
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
      const uint at = frm.w[PVRC_F_O_IDX] + first + i;
      /* a strip's triangle i, the device's way: i, i + 1, i + 2, and for
       * an odd one i, i + 2, i + 1; its first vertex is the provoking one */
      const uint odd = (i ^ parity) & 1u;
      vi[0] = idx.w[at];
      vi[1] = idx.w[at + 1u + odd];
      vi[2] = idx.w[at + 2u - odd];
      provoking = vi[0];
      const uint ov = frm.w[PVRC_F_O_VTX];
      for (uint k = 0u; k < 3u; k++)
      {
         vx[k] = uintBitsToFloat(vtx.w[ov + vi[k] * PVRC_VTX_WORDS]);
         vy[k] = uintBitsToFloat(vtx.w[ov + vi[k] * PVRC_VTX_WORDS + 1u]);
         vz[k] = uintBitsToFloat(vtx.w[ov + vi[k] * PVRC_VTX_WORDS + 2u]);
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
      /* at no depth, or one past what a float takes: such a triangle is
       * left out (and counted); behind the eye, it is clipped
       * (rec_clip_plane()) */
      if (!(abs(q[k]) > 0.0) || abs(q[k]) > 3.4e37 || isnan(px[k]) || isnan(py[k]))
         bad = true;
   }
   const int64_t cross = int64_t(x[1] - x[0]) * int64_t(y[2] - y[0]) - int64_t(x[2] - x[0]) * int64_t(y[1] - y[0]);
   /* counter-clockwise the device's way (a positive area in its formula)
    * when cross is negative: that is its front, as the pipelines have it;
    * the other way round with an odd number of vertices behind the eye */
   const uint behind = (q[0] < 0.0 ? 1u : 0u) + (q[1] < 0.0 ? 1u : 0u) + (q[2] < 0.0 ? 1u : 0u);
   const bool front = (cross < 0) != ((behind & 1u) != 0u);
   const uint cull = (isp >> 27) & 3u;
   const bool culledOwn = (cull == 3u && !front) || (cull == 2u && front);
   uint extra = provoking;
   bool empty = bad || cross == 0 || behind == 3u;
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
   else if (behind != 0u && !empty)
      atomicAdd(bin.w[PVRC_B_STATS(pvrc_runs()) + PVRC_ST_CLIPPED], 1u);
   if (cross < 0)
   {
      int t = x[1]; x[1] = x[2]; x[2] = t;
      t = y[1]; y[1] = y[2]; y[2] = t;
      float f = q[1]; q[1] = q[2]; q[2] = f;
      uint u = vi[1]; vi[1] = vi[2]; vi[2] = u;
   }
   extra |= shift << PVRC_RX_SHIFT_AT;
   const uint neg = (q[0] < 0.0 ? 1u : 0u) | (q[1] < 0.0 ? 2u : 0u) | (q[2] < 0.0 ? 4u : 0u);
   extra |= neg << PVRC_RX_NEG_AT;
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
   /* the pixels whose samples it can cover, inside its clip and the picture
    * (with a vertex behind the eye, its part of the plane is not inside
    * its vertices: all of them; a vertex far out, at a large shift, is
    * past what an int has, and taken at its end) */
   const int64_t bmin = -0x7fffffffl, bmax = 0x7fffffffl;
   int bx0 = int(clamp(((int64_t(min(min(x[0], x[1]), x[2])) << shift) - 255) >> 8, bmin, bmax));
   int by0 = int(clamp(((int64_t(min(min(y[0], y[1]), y[2])) << shift) - 255) >> 8, bmin, bmax));
   int bx1 = int(clamp((int64_t(max(max(x[0], x[1]), x[2])) << shift) >> 8, bmin, bmax));
   int by1 = int(clamp((int64_t(max(max(y[0], y[1]), y[2])) << shift) >> 8, bmin, bmax));
   if (neg != 0u)
   {
      bx0 = by0 = 0;
      bx1 = by1 = 0x7fffffff;
   }
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
         if (pvrc_tile_hit(r, tx, ty))
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
         if (!pvrc_tile_hit(r, tx, ty))
            continue;
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

#if defined(PVRC_SHADER_GLSL)
layout(set = 0, binding = 3, rgba8) uniform writeonly image2D outImage;
layout(set = 0, binding = 4) uniform sampler2D lastPicture;
layout(set = 0, binding = 5) uniform sampler2D palette;
layout(set = 0, binding = 6) uniform sampler2D textures[PVRC_TEX_SLOTS];
/* Each lane's pixel has its own polygon, so the slot can differ between
 * the lanes of a subgroup, and the array is indexed by it as a non-uniform
 * index (shaderSampledImageArrayNonUniformIndexing); without that, the
 * device given PVRC_TEX_WATERFALL: the lanes take their textures in turns,
 * a slot at a time */
#if defined(PVRC_TEX_WATERFALL)
#extension GL_KHR_shader_subgroup_ballot : require
vec4 pvrc_texture_grad(uint slot, vec2 uv, vec2 dx, vec2 dy)
{
   for (;;)
   {
      const uint first = subgroupBroadcastFirst(slot);
      if (first == slot)
         return textureGrad(textures[first], uv, dx, dy);
   }
}
#define PVRC_TEXTURE_GRAD(slot, uv, dx, dy) pvrc_texture_grad(slot, uv, dx, dy)
#else
#extension GL_EXT_nonuniform_qualifier : require
#define PVRC_TEXTURE_GRAD(slot, uv, dx, dy) textureGrad(textures[nonuniformEXT(slot)], uv, dx, dy)
#endif
#endif

shared uint s_prim[512];	/* the primitives of a batch kept, in order (two batches' in turn, the walk of one going on as the next is read) */
shared uint s_zlo[512], s_zhi[512];	/* the depth each has over the tile, least and most (float bits) */
shared uint s_kh[PVRC_KEEP * 256u];
shared uint s_kl[PVRC_KEEP * 256u];
shared uint s_kc[PVRC_KEEP * 256u];	/* (each fragment's colour, as the lists keep it) */
shared uint s_flag;
shared uint s_mask[16];	/* the entries of a batch kept, a bit each (two batches' words, in turn) */
shared uint s_zr[4];	/* the lanes' least and most depth, as pvrc_zkey() has them (and two batches' least, in turn) */
shared uint s_x[4u * 256u];	/* a word or a translucent fragment (key, colour, samples) from each lane, for the lanes of its pixel */
shared uint s_xs[5u * 256u];	/* each lane's translucent list as it stands: fragments and whether a round is begun, the boundary, the nearest kept */

uint g_tid;
uint g_ct;	/* the coarse tile */
uvec4 g_fine;	/* the fine tile's pixels: x0, y0, x1, y1 (all in it) */
int g_px, g_py;	/* this lane's pixel */
int g_sx, g_sy;	/* ...its sample, in 24.8 */
int g_cx, g_cy;	/* ...the pixel's centre */
int g_s0x, g_s0y;	/* ...its first sample */
bool g_in;	/* inside the picture */
uint g_col;	/* the colour attachment's, 8 bits a channel */
float g_depth;
uint g_stencil;	/* the shadow bit of what is there, 0x80, and the volume bits */
uint g_win;	/* the primitive that wrote the lane's depth in this render pass (PVRC_NONE: none) */
bool g_multi;	/* ...or another could have the same depth there: the colour subpass has to look at them all */
bool g_seen;	/* a polygon covered the lane in this render pass's depth subpass */

/* ---- The primitives of a range of numbers in the tile, in order ---- */

/* The depth test a walk's primitives are passed over by, from the tile's
 * depths: those that test against what the depth subpass left and write
 * none (the colour subpass's, the sorted translucent's), by its least and
 * most; those that write (the depth subpass's, the translucent depth's,
 * the presorted translucent's), by a depth no lane's is below as they go */
#define HIZ_NONE 0u
#define HIZ_RUN  1u	/* the running bound */
#define HIZ_OP   2u	/* the colour subpass's opaque: equal, or its own test without a depth write */
#define HIZ_PT   3u	/* ...its punch-through: equal, or greater or equal */
#define HIZ_TR   4u	/* sorted translucent: in front of it, or at it */

uint it_lo, it_hi, it_c, it_clast, it_j, it_end, it_k, it_n, it_cbase, it_run;
uint it_batch = 0u;
uint it_base = 0u;	/* where the batch being walked is in s_prim */
uint it_hiz;
float it_zlo, it_zhi;	/* the depth the primitive it_next() gave has over the tile */
bool it_skip, it_started;
/* A range whose run was read in one batch is walked again from that
 * batch, while it is still there: the walk over the same list by
 * another test (the colour subpass after the depth subpass's, the next
 * round of translucent fragments, the translucent depth) reads the run
 * once more otherwise. What the last fill was: its range, its filter,
 * where its batch is, how many it kept, and which fill it was. */
uint it_fills;	/* fills this walk made */
uint rp_lo = PVRC_NONE, rp_hi = 0u, rp_hiz, rp_base, rp_n, rp_batch;
bool it_replay;
float g_zmin, g_zmax;	/* the tile's depths after the depth subpass (the lanes in the picture) */
float g_zbound;	/* a depth no lane in the picture has less than, now */
uint g_hiz_skipped;

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

/* A depth as a word whose order is the depth's (for the shared atomics) */
uint pvrc_zkey(float z)
{
   const uint b = floatBitsToUint(z);
   return (b & 0x80000000u) != 0u ? ~b : b | 0x80000000u;
}

float pvrc_zfrom(uint k)
{
   return uintBitsToFloat((k & 0x80000000u) != 0u ? k & 0x7fffffffu : ~k);
}

/* Whether a primitive can cover a sample of the fine tile: its pixels
 * meet the tile's, and no edge has the whole tile outside it. Asked of
 * each entry of a run once, by one lane, when it_next() takes the run's
 * entries in, so that the walk's every lane looks only at the primitives
 * that pass */
bool rec_in_tile(uint r)
{
   const uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   if (!((b0 & 0xffffu) <= g_fine.z && (b1 & 0xffffu) >= g_fine.x
         && (b0 >> 16) <= g_fine.w && (b1 >> 16) >= g_fine.y))
      return false;
   return pvrc_rec_hits(r, int(g_fine.x), int(g_fine.y), int(g_fine.z), int(g_fine.w));
}

float pvrc_depth(float w);
uint rec_poly(uint r);

/* The depths a polygon's triangle @r can have at the places the lanes of
 * the fine tile take it (their samples, or the pixels' centres): its 1/w
 * is an affine function of the place, so it is between those at the
 * corners of the pixels of the tile it can cover, taken with room for
 * every rounding of the lanes' own 1/w and depth (pvrc_depth() keeps the
 * order of 1/w) */
vec2 rec_zrange(uint r)
{
   const uint b0 = rec.w[r + PVRC_R_BBOX0], b1 = rec.w[r + PVRC_R_BBOX1];
   const int x0 = int(max(b0 & 0xffffu, g_fine.x)) * 256, y0 = int(max(b0 >> 16, g_fine.y)) * 256;
   const int x1 = int(min(b1 & 0xffffu, g_fine.z) + 1u) * 256, y1 = int(min(b1 >> 16, g_fine.w) + 1u) * 256;
   const vec3 q = vec3(uintBitsToFloat(rec.w[r + PVRC_R_Q]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 1u]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 2u]));
   float lo = 3.4e38, hi = -3.4e38;
   vec3 most = vec3(0.0);
   float area = 0.0;
   for (uint c = 0u; c < 4u; c++)
   {
      const i64vec3 e = rec_edges(r, (c & 1u) != 0u ? x1 : x0, (c & 2u) != 0u ? y1 : y0);
      const vec3 ef = vec3(float(e.x), float(e.y), float(e.z));
      area = float(e.x + e.y + e.z);
      most = max(most, abs(ef));
      const float w = dot(ef, q);
      lo = min(lo, w);
      hi = max(hi, w);
   }
   const float room = 1e-5 * dot(most, abs(q));
   const float zlo = pvrc_depth((lo - room) / area), zhi = pvrc_depth((hi + room) / area);
   return vec2(zlo - 1e-6 * abs(zlo), zhi + 1e-6 * abs(zhi));
}

/* Whether a polygon's triangle whose depths over the tile are @zl to
 * @zh can pass the test @mode (ISP DepthMode) against the tile's depths
 * after the depth subpass, at some lane */
bool hiz_keep(uint mode, float zl, float zh)
{
   if (mode == 5u || mode == 7u)
      return true;
   if (mode == 0u)
      return false;
   if (mode == 1u || mode == 3u)
      return zl <= g_zmax;
   if (mode == 2u)
      return zh >= g_zmin && zl <= g_zmax;
   return zh >= g_zmin;
}

/* The walk over the primitives @lo to @hi (not in it) begins, their
 * batches filtered by @hiz. A batch kept from the last fill serves again
 * when it holds the whole range, its half of s_prim has not been written
 * since (at most one fill has come after it, which went to the other
 * half), and its filter let through everything this walk's does: the
 * walk's own, or the running bound's (none), or - with @tr_depth, the
 * translucent depth after the sorted translucent of the same pass, both
 * testing for at or in front of the depths - the sorted translucent's. */
void it_begin(uint lo, uint hi, uint hiz, bool tr_depth)
{
   it_hiz = hiz;
   it_lo = lo;
   it_hi = hi;
   it_c = lo >> PVR_CHUNK_LOG2;
   it_clast = hi > lo ? (hi - 1u) >> PVR_CHUNK_LOG2 : 0u;
   it_j = it_end = it_k = it_n = 0u;
   it_started = false;
   it_fills = 0u;
   it_replay = lo == rp_lo && hi == rp_hi && it_batch - rp_batch <= 2u
         && (rp_hiz == hiz || rp_hiz == HIZ_RUN || (tr_depth && rp_hiz == HIZ_TR && hiz == HIZ_RUN));
   if (it_replay)
   {
      it_base = rp_base;
      it_n = rp_n;
      it_started = true;
      it_c = it_clast + 1u;	/* (nothing more to read) */
      if (hiz == HIZ_RUN)
      {
         /* the bound, as a fill would have it */
         barrier();
         if (g_tid == 0u)
            s_zr[0] = 0xffffffffu;
         barrier();
         if (g_in)
            atomicMin(s_zr[0], pvrc_zkey(g_depth));
         barrier();
         g_zbound = pvrc_zfrom(s_zr[0]);
      }
   }
}

/* The next primitive (PVRC_NONE: there are no more): in uniform control
 * flow, as every lane walks the same */
uint it_next()
{
   for (;;)
   {
      if (it_k < it_n)
      {
         uint p = s_prim[it_base + it_k];
         it_zlo = uintBitsToFloat(s_zlo[it_base + it_k]);
         it_zhi = uintBitsToFloat(s_zhi[it_base + it_k]);
         it_k++;
         if (it_replay && it_hiz != HIZ_NONE && it_hiz != HIZ_RUN && rp_hiz != it_hiz)
         {
            /* (the batch was kept by a looser filter: this walk's now) */
            uint mode = 6u;
            if (it_hiz != HIZ_TR)
            {
               const uint isp = frm.w[rec_poly(p * PVRC_REC_WORDS) + PVRC_Q_ISP];
               mode = (isp & (1u << 26)) == 0u ? 2u : it_hiz == HIZ_PT ? 6u : isp >> 29;
            }
            if (!hiz_keep(mode, it_zlo, it_zhi))
               continue;
         }
         return p;
      }
      if (it_j >= it_end)
      {
         if (it_started)
            it_c++;
         it_started = true;
         if (it_hi <= it_lo || it_c > it_clast)
         {
            if (it_fills == 1u && !it_replay)
            {
               rp_lo = it_lo;
               rp_hi = it_hi;
               rp_hiz = it_hiz;
               rp_base = it_base;
               rp_n = it_n;
               rp_batch = it_batch - 1u;
            }
            else if (!it_replay)
               rp_lo = PVRC_NONE;
            return PVRC_NONE;
         }
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
      /* 256 entries of the run at a time, a lane each: those whose
       * primitive can cover a sample of the fine tile, and can pass the
       * walk's depth test somewhere in it, kept in order. Two barriers: a
       * batch goes to the half of s_prim the batch before did not, and
       * its mask words were cleared in that batch after every lane had
       * counted that one's, so no lane still walking the batch before
       * reads what this one writes. */
      const uint at = it_j + g_tid;
      uint v = PVRC_NONE;
      if (at < it_end)
         v = it_skip ? it_cbase + at : it_cbase | pvrc_pool_entry(it_run + at);
      bool keep = v != PVRC_NONE && rec_in_tile(v * PVRC_REC_WORDS);
      vec2 zr = vec2(0.0);
      if (keep && it_hiz != HIZ_NONE)
      {
         zr = rec_zrange(v * PVRC_REC_WORDS);
         if (it_hiz != HIZ_RUN)
         {
            uint mode = 6u;
            if (it_hiz != HIZ_TR)
            {
               const uint isp = frm.w[rec_poly(v * PVRC_REC_WORDS) + PVRC_Q_ISP];
               mode = (isp & (1u << 26)) == 0u ? 2u : it_hiz == HIZ_PT ? 6u : isp >> 29;
            }
            keep = hiz_keep(mode, zr.x, zr.y);
         }
      }
      const uint side = it_batch & 1u;
      const uint mb = side * 8u, word = g_tid >> 5, bit = 1u << (g_tid & 31u);
      if (g_tid < 8u)
         s_mask[(mb ^ 8u) + g_tid] = 0u;	/* (the next batch's, which no lane reads now) */
      if (g_tid == 0u)
         s_zr[2u + side] = 0xffffffffu;
      if (keep)
         atomicOr(s_mask[mb + word], bit);
      it_j += min(256u, it_end - it_j);
      barrier();
      uint before = 0u, total = 0u;
      for (uint q = 0u; q < 8u; q++)
      {
         const uint c = uint(bitCount(s_mask[mb + q]));
         before += q < word ? c : 0u;
         total += c;
      }
      if (keep)
      {
         const uint slot = side * 256u + before + uint(bitCount(s_mask[mb + word] & (bit - 1u)));
         s_prim[slot] = v;
         s_zlo[slot] = floatBitsToUint(zr.x);
         s_zhi[slot] = floatBitsToUint(zr.y);
      }
      if (it_hiz == HIZ_RUN && g_in)
         atomicMin(s_zr[2u + side], pvrc_zkey(g_depth));
      it_batch++;
      it_fills++;
      it_base = side * 256u;
      it_n = total;
      it_k = 0u;
      barrier();
      if (it_hiz == HIZ_RUN)
         g_zbound = pvrc_zfrom(s_zr[2u + side]);
   }
}

/* ---- A primitive at a lane ---- */

bool in_rect(uint r0, uint r1)
{
   return g_in && g_px >= int(r0 & 0xffffu) && g_py >= int(r0 >> 16) && g_px < int(r1 & 0xffffu) && g_py < int(r1 >> 16);
}

bool edge_in(int64_t e, int dx, int dy)
{
   return e > 0 || (e == 0 && (dy < 0 || (dy == 0 && dx > 0)));
}

/* Whether the place whose edges are @e is inside: each edge (turned for a
 * vertex behind the eye), and the clip for such a triangle
 * (rec_clip_plane()) */
bool rec_inside(uint r, i64vec3 e)
{
   const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
   const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
   const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
   const uint neg = (rec.w[r + PVRC_R_EXTRA] >> PVRC_RX_NEG_AT) & 7u;
   if (neg == 0u)
      return edge_in(e.x, x2 - x1, y2 - y1) && edge_in(e.y, x0 - x2, y0 - y2) && edge_in(e.z, x1 - x0, y1 - y0);
   const int64_t s0 = (neg & 1u) != 0u ? -1 : 1, s1 = (neg & 2u) != 0u ? -1 : 1, s2 = (neg & 4u) != 0u ? -1 : 1;
   return edge_in(s0 * e.x, int(s0) * (x2 - x1), int(s0) * (y2 - y1)) && edge_in(s1 * e.y, int(s1) * (x0 - x2), int(s1) * (y0 - y2))
         && edge_in(s2 * e.z, int(s2) * (x1 - x0), int(s2) * (y1 - y0)) && rec_clip_at(rec_clip_plane(r), e) >= 0.0;
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
   return vtx.w[frm.w[PVRC_F_O_VTX] + v * PVRC_VTX_WORDS + k];
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

/* A perspective-correct coordinate's change from the place whose edges are
 * @e to the place a step of the edges by @d away, from its numerator and
 * denominator: (num + dnum) / (den + dden) - num / den */
vec2 frag_duv(uint r, vec3 e, vec3 d, uint ku)
{
   const vec3 q = vec3(uintBitsToFloat(rec.w[r + PVRC_R_Q]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 1u]), uintBitsToFloat(rec.w[r + PVRC_R_Q + 2u]));
   const vec3 eq = e * q;
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
      res[c] = (dnum * den - num * dden) / (den * (den + dden));
   }
   return res;
}

/* A fragment's texture coordinates' derivatives, at the place whose edges
 * are @e (for texturing; test mode has no use for them): what a fragment
 * shader's implicit ones are, the differences of the coordinates across
 * the 2x2 block of pixels (at even x and y) the pixel is in - from its top
 * left pixel to the one right of it and to the one below it, at the same
 * place in each pixel, all four from the triangle's planes wherever it
 * covers - rather than the exact derivatives at the place. A perspective
 * triangle's coordinates are not linear over a pixel; seen at a grazing
 * angle the two differ enough to move the level of detail the device
 * picks, and so its mipmap level and its anisotropic filtering's taps.
 * The edges' steps in 64 bits: a vertex far off the screen (one behind
 * the eye) has differences that times 256 do not fit in 32. Past a shift
 * of 8 a pixel is less than a unit at the shift, and the steps and the
 * top left pixel's edges are floats. */
void frag_grads(uint r, uint flags, i64vec3 e, inout Frag f)
{
   if (PVRC_TEST == 0u && (flags & PVRC_QF_TEXTURE) != 0u)
   {
      const int x0 = int(rec.w[r]), y0 = int(rec.w[r + 1u]);
      const int x1 = int(rec.w[r + 2u]), y1 = int(rec.w[r + 3u]);
      const int x2 = int(rec.w[r + 4u]), y2 = int(rec.w[r + 5u]);
      /* a pixel across: each edge changes by -dy * 256, down by dx * 256
       * (256 at the primitive's shift) */
      const int sh = 8 - int(rec_shift(r));
      vec3 tl, dx, dy;
      if (sh >= 0)
      {
         const i64vec3 ix = i64vec3(-(y2 - y1), -(y0 - y2), -(y1 - y0)) << sh;
         const i64vec3 iy = i64vec3(x2 - x1, x0 - x2, x1 - x0) << sh;
         /* (the block's top left pixel, exactly) */
         i64vec3 itl = e;
         if ((g_px & 1) != 0)
            itl -= ix;
         if ((g_py & 1) != 0)
            itl -= iy;
         tl = vec3(itl);
         dx = vec3(ix);
         dy = vec3(iy);
      }
      else
      {
         const float s = exp2(float(sh));
         dx = vec3(float(-(y2 - y1)), float(-(y0 - y2)), float(-(y1 - y0))) * s;
         dy = vec3(float(x2 - x1), float(x0 - x2), float(x1 - x0)) * s;
         tl = vec3(e);
         if ((g_px & 1) != 0)
            tl -= dx;
         if ((g_py & 1) != 0)
            tl -= dy;
      }
      f.duvx = frag_duv(r, tl, dx, 5u);
      f.duvy = frag_duv(r, tl, dy, 5u);
      if ((flags & PVRC_QF_TWO_VOLUMES) != 0u)
      {
         f.duv1x = frag_duv(r, tl, dx, 9u);
         f.duv1y = frag_duv(r, tl, dy, 9u);
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
   return PVRC_TEXTURE_GRAD(slot, uv, dx, dy);
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

/* The same, from the samples of the pixel the triangle covers, a bit
 * each (what rate_pos() finds out) */
ivec2 rate_pos_cov(uint cov)
{
   if (PVRC_SAMPLES == 1u || cov == 0u || cov == (1u << PVRC_SAMPLES) - 1u)
      return ivec2(g_cx, g_cy);
   const uint first = uint(findLSB(cov));
   return ivec2(g_px * 256 + int(SSAA_SAMPLE_X16(PVRC_SAMPLES, first)) * 16, g_py * 256 + int(SSAA_SAMPLE_Y16(PVRC_SAMPLES, first)) * 16);
}

/* ...its inputs there, with gl_FragCoord (its 1/w for the depth and the
 * fog) and its textures' derivatives at the centre, whose edges are @ec */
Frag rate_frag_at(uint r, uint pp, i64vec3 ec, ivec2 at)
{
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

Frag rate_frag(uint r, uint pp, i64vec3 ec)
{
   return rate_frag_at(r, pp, ec, rate_pos(r));
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

   if ((frm.w[PVRC_F_FLAGS] & PVRC_FF_LAST_SCALED) != 0u && g_in)
      g_col = col_pack(textureLod(lastPicture, (vec2(float(g_px), float(g_py)) + 0.5)
            / vec2(float(frm.w[PVRC_F_WIDTH]), float(frm.w[PVRC_F_HEIGHT])), 0.0));
   else if ((frm.w[PVRC_F_FLAGS] & PVRC_FF_LAST_PICTURE) != 0u && g_in)
      g_col = col_pack(texelFetch(lastPicture, ivec2(g_px, g_py), 0));
   else
      g_col = frm.w[PVRC_F_CLEAR];
   g_depth = 0.0;
   g_hiz_skipped = 0u;
   uint peels = 0u;
   if (g_tid < 16u)
      s_mask[g_tid] = 0u;	/* (the first batch's barrier is before any lane reads it) */

   /* A coarse tile nothing is binned to: its fine tiles are the picture
    * as it starts */
   uint binned = 0u;
   for (uint c = 0u; c < frm.w[PVRC_F_CHUNKS]; c++)
      binned += bin.w[PVRC_B_COUNT(pvrc_runs()) + c * pvrc_tiles() + g_ct];
   const uint passes = binned == 0u ? 0u : frm.w[PVRC_F_PASSES];
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
      g_win = PVRC_NONE;
      g_multi = false;
      g_seen = false;

      /* The depth subpass: opaque, then punch-through. The walk passes
       * over a primitive whose depth is below every lane's where it
       * tests for more (or equal); a primitive that can write a lesser
       * depth than the lanes have lowers the bound they are known to
       * keep. A lane notes which primitive wrote its depth, and whether
       * another could have the same depth there - one tested at the depth
       * the lane had, one that passed but was discarded, or any after a
       * lesser depth was written over a greater - for the colour subpass
       * to look at that one alone when none could. */
      it_begin(opLo, mvoLo, HIZ_RUN, false);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         const uint pp = rec_poly(r);
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const uint mode = isp >> 29;
         const bool writes = (isp & (1u << 26)) == 0u;
         if (mode == 0u || ((mode & 1u) == 0u && it_zhi < g_zbound))
         {
            g_hiz_skipped++;
            continue;
         }
         i64vec3 e;
         ivec2 pos;
         if (lane_cover(r, pp, false, e, pos))
         {
            const float z = pvrc_depth(rec_bary(r, e).w);
            const bool seen = g_seen;
            g_seen = true;
            if (z == g_depth)
               g_multi = true;
            if (pvrc_depth_pass(mode, z, g_depth) != 0u)
            {
               bool killed = false;
               if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
                  shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
               if (killed)
                  g_multi = true;
               else
               {
                  if (writes)
                  {
                     if ((mode & 1u) != 0u && seen)
                        g_multi = true;
                     g_depth = z;
                     g_win = prim;
                  }
                  g_stencil = (g_stencil & 0x7fu) | ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u ? 0x80u : 0u);
               }
            }
         }
         if (writes && (mode & 1u) != 0u)
            g_zbound = min(g_zbound, it_zlo);
      }
      it_begin(ptLo, trLo, HIZ_RUN, false);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         if (it_zhi < g_zbound)
         {
            g_hiz_skipped++;
            continue;
         }
         const uint pp = rec_poly(r);
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         const float z = pvrc_depth(rec_bary(r, e).w);
         g_seen = true;
         if (z == g_depth)
            g_multi = true;
         if (pvrc_depth_pass(6u, z, g_depth) == 0u)
            continue;
         bool killed;
         shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
         if (killed)
         {
            g_multi = true;
            continue;
         }
         g_depth = z;	/* (punch-through always writes it) */
         g_win = prim;
         g_stencil = (g_stencil & 0x7fu) | ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) != 0u ? 0x80u : 0u);
      }

      /* The tile's depths now, for the walks that test against them */
      barrier();
      if (g_tid == 0u)
      {
         s_zr[0] = 0xffffffffu;
         s_zr[1] = 0u;
      }
      barrier();
      if (g_in)
      {
         atomicMin(s_zr[0], pvrc_zkey(g_depth));
         atomicMax(s_zr[1], pvrc_zkey(g_depth));
      }
      barrier();
      g_zmin = pvrc_zfrom(s_zr[0]);
      g_zmax = pvrc_zfrom(s_zr[1]);
      barrier();

      /* The opaque modifier volumes: the volume bits of each sample */
      {
         uint cov = 0u;	/* what the volume being sent covers, front and back */
         uint pending = PVRC_NONE;	/* the last triangle of the parameter that ends it */
         uint closer = 0u;	/* ...the triangle that tells which one */
         it_begin(mvoLo, ptLo, HIZ_NONE, false);
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
            if (!g_in)
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

      /* The colour subpass: opaque, punch-through. A polygon that writes
       * depth is drawn where its depth is the lane's: the one that wrote
       * it, when no other can have it there (its depth needs no
       * computing then: it is the lane's) */
      it_begin(opLo, mvoLo, HIZ_OP, false);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         const uint pp = rec_poly(r);
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const bool known = (isp & (1u << 26)) == 0u && !g_multi;
         if (known && prim != g_win)
            continue;
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         if (!known)
         {
            const float z = pvrc_depth(rec_bary(r, e).w);
            if (pvrc_depth_pass((isp & (1u << 26)) == 0u ? 2u : isp >> 29, z, g_depth) == 0u)
               continue;
         }
         bool killed;
         const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
         if (!killed)
            g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
      }
      it_begin(ptLo, trLo, HIZ_PT, false);
      while ((prim = it_next()) != PVRC_NONE)
      {
         const uint r = prim * PVRC_REC_WORDS;
         const uint pp = rec_poly(r);
         const uint isp = frm.w[pp + PVRC_Q_ISP];
         const bool known = (isp & (1u << 26)) == 0u && !g_multi;
         if (known && prim != g_win)
            continue;
         i64vec3 e;
         ivec2 pos;
         if (!lane_cover(r, pp, false, e, pos))
            continue;
         if (!known)
         {
            const float z = pvrc_depth(rec_bary(r, e).w);
            if (pvrc_depth_pass((isp & (1u << 26)) == 0u ? 2u : 6u, z, g_depth) == 0u)
               continue;
         }
         bool killed;
         const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
         if (!killed)
            g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
      }

      if (!autosort)
      {
         /* Translucent, in the order sent, blended as they come */
         it_begin(trLo, trHi, HIZ_RUN, false);
         while ((prim = it_next()) != PVRC_NONE)
         {
            const uint r = prim * PVRC_REC_WORDS;
            const uint pp = rec_poly(r);
            const uint isp = frm.w[pp + PVRC_Q_ISP];
            const uint mode = isp >> 29;
            const bool writes = (isp & (1u << 26)) == 0u;
            if (mode == 0u || ((mode & 1u) == 0u && it_zhi < g_zbound))
            {
               g_hiz_skipped++;
               continue;
            }
            i64vec3 e;
            ivec2 pos;
            if (lane_cover(r, pp, false, e, pos))
            {
               const float z = pvrc_depth(rec_bary(r, e).w);
               if (pvrc_depth_pass(mode, z, g_depth) != 0u)
               {
                  bool killed;
                  const vec4 c = shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_COLOR, shadowed, killed);
                  if (!killed)
                  {
                     if (writes)
                        g_depth = z;
                     g_col = blend8(frm.w[pp + PVRC_Q_TSP], c, g_col);
                  }
               }
            }
            if (writes && (mode & 1u) != 0u)
               g_zbound = min(g_zbound, it_zlo);
         }
      }
      else if (trHi > trLo)
      {
         /* Translucent, sorted: K fragments at a time, the farthest
          * first, blended in floats onto the colour as the last subpass
          * does; then the next K beyond the last one, until there are none.
          * A fragment is shaded once a pixel, at its centre, whichever of
          * the pixel's samples it covers: so the N lanes of a pixel take N
          * primitives at a time, a lane shading one, and hand what they
          * made to the lanes of the samples it covers through shared
          * memory, each of which keeps its own K. (At one sample a pixel,
          * the lane is the pixel's and hands nothing over.) */
         vec4 fc = col_unpack(g_col);
         vec4 sec = vec4(0.0);
         uint bh = 0u, bl = 0u;
         bool started = false;
         const uint pbase = g_tid & ~(PVRC_SAMPLES - 1u);	/* the pixel's first lane */
         float pz[8];	/* the depths of the pixel's samples */
         pz[0] = g_depth;
         if (PVRC_SAMPLES > 1u)
         {
            s_x[g_tid] = floatBitsToUint(g_depth);
            barrier();
            for (uint j = 0u; j < PVRC_SAMPLES; j++)
               pz[j] = uintBitsToFloat(s_x[pbase + j]);
            barrier();
         }
         for (;;)
         {
            uint n = 0u;
            bool more = false;
            if (PVRC_SAMPLES > 1u)
            {
               /* the list as the round starts, for the lanes that shade */
               s_xs[g_tid * 5u] = started ? 256u : 0u;
               s_xs[g_tid * 5u + 1u] = bh;
               s_xs[g_tid * 5u + 2u] = bl;
               barrier();
            }
            it_begin(trLo, trHi, HIZ_TR, false);
            for (;;)
            {
               /* the step's primitives: the lane of sample j takes the j-th */
               uint mine = PVRC_NONE;
               bool any = false;
               for (uint j = 0u; j < PVRC_SAMPLES; j++)
               {
                  const uint p = any || j == 0u ? it_next() : PVRC_NONE;
                  if (p != PVRC_NONE)
                     any = true;
                  else if (j == 0u)
                     break;
                  if (j == smp)
                     mine = p;
               }
               if (!any)
                  break;
               /* this lane's fragment: for which samples it is a
                * candidate (covered, in front of the depth, inside its
                * clips, beyond the boundary of what the round before took),
                * and which of those can keep it as their list stands */
               uint xh = 0u, xl = 0u, xc = 0u, cand = 0u, kept = 0u;
               if (mine != PVRC_NONE)
               {
                  const uint r = mine * PVRC_REC_WORDS;
                  const uint pp = rec_poly(r);
                  if (in_rect(frm.w[pp + PVRC_Q_RECT], frm.w[pp + PVRC_Q_RECT + 1u]) && rec_near(r))
                  {
                     /* each sample it covers tested with its depth at
                      * that sample, as the depth subpass took the depth
                      * there; the edges of those it is seen at summed:
                      * the edges are linear in the place, so the sum is
                      * the edges at their centroid times their number,
                      * which gives the same 1/w (rec_bary()) */
                     uint cov = 0u;
                     i64vec3 es = i64vec3(0);
                     for (uint j = 0u; j < PVRC_SAMPLES; j++)
                     {
                        const int sx = g_px * 256 + int(SSAA_SAMPLE_X16(PVRC_SAMPLES, j)) * 16;
                        const int sy = g_py * 256 + int(SSAA_SAMPLE_Y16(PVRC_SAMPLES, j)) * 16;
                        const i64vec3 e = rec_edges(r, sx, sy);
                        if (rec_inside(r, e))
                        {
                           cov |= 1u << j;
                           if (pvrc_depth(rec_bary(r, e).w) >= pz[j])
                           {
                              cand |= 1u << j;
                              es += e;
                           }
                        }
                     }
                     if (cand != 0u)
                     {
                        /* kept and sorted with its depth at the centroid
                         * of the samples it is seen at, which is in the
                         * triangle; the centre's, which that centroid is,
                         * where it is seen at all of them. The centre's for
                         * all of it was the plane extrapolated past the
                         * triangle where the triangle only clips the
                         * pixel, and sorted the fragments of a model's
                         * neighbouring polygons wrongly along its edges. */
                        const i64vec3 ec = rec_edges(r, g_cx, g_cy);
                        if (cand == (1u << PVRC_SAMPLES) - 1u)
                           es = ec;
                        const float z = pvrc_depth(rec_bary(r, es).w);
                        if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
                        {
                           const uint c0 = frm.w[pp + PVRC_Q_CLIP], c1 = frm.w[pp + PVRC_Q_CLIP + 1u];
                           const float x = float(g_px) + 0.5, y = float(g_py) + 0.5;
                           if (x >= float(c0 & 0xffffu) && x <= float(c1 & 0xffffu) && y >= float(c0 >> 16) && y <= float(c1 >> 16))
                              cand = 0u;
                        }
                        if (cand != 0u)
                        {
                           xh = pvr_key_hi(z, 0u);
                           xl = mine << 2;
                           for (uint j = 0u; j < PVRC_SAMPLES; j++)
                           {
                              if ((cand & (1u << j)) == 0u)
                                 continue;
                              uint st = started ? 256u : 0u, sbh = bh, sbl = bl, skh = 0u, skl = 0u;
                              if (PVRC_SAMPLES > 1u)
                              {
                                 const uint xs = (pbase + j) * 5u;
                                 st = s_xs[xs];
                                 sbh = s_xs[xs + 1u];
                                 sbl = s_xs[xs + 2u];
                                 skh = s_xs[xs + 3u];
                                 skl = s_xs[xs + 4u];
                              }
                              else
                              {
                                 st |= n;
                                 if (n == PVRC_KEEP)
                                 {
                                    skh = s_kh[(PVRC_KEEP - 1u) * 256u + g_tid];
                                    skl = s_kl[(PVRC_KEEP - 1u) * 256u + g_tid] & ~3u;
                                 }
                              }
                              if ((st & 256u) != 0u && pvr_key_before(sbh, sbl, xh, xl) == 0u)
                                 cand &= ~(1u << j);	/* (taken already) */
                              else if ((st & 255u) < PVRC_KEEP || pvr_key_before(xh, xl, skh, skl) != 0u)
                                 kept |= 1u << j;
                           }
                        }
                        if (kept != 0u)
                        {
                           /* shaded now, as the per-pixel renderer shades a
                            * fragment it puts in its lists */
                           bool killed;
                           xc = col_pack(clamp(shade(pp, rate_frag_at(r, pp, ec, rate_pos_cov(cov)), PVRC_SHADE_OIT, false, killed), 0.0, 1.0));
                        }
                     }
                  }
               }
               /* the step's fragments to the lanes of their samples */
               uint got = PVRC_SAMPLES == 1u ? 1u : PVRC_SAMPLES;
               if (PVRC_SAMPLES > 1u)
               {
                  s_x[g_tid * 4u] = xh;
                  s_x[g_tid * 4u + 1u] = xl;
                  s_x[g_tid * 4u + 2u] = xc;
                  s_x[g_tid * 4u + 3u] = cand | (kept << 8);
                  barrier();
               }
               for (uint j = 0u; j < got; j++)
               {
                  uint kh = xh, kl = xl, kc = xc, m = cand | (kept << 8);
                  if (PVRC_SAMPLES > 1u)
                  {
                     const uint x = (pbase + j) * 4u;
                     kh = s_x[x];
                     kl = s_x[x + 1u];
                     kc = s_x[x + 2u];
                     m = s_x[x + 3u];
                  }
                  if ((m & (1u << smp)) == 0u)
                     continue;
                  uint at;
                  if (n < PVRC_KEEP)
                     at = n++;
                  else
                  {
                     more = true;
                     const uint li = (PVRC_KEEP - 1u) * 256u + g_tid;
                     if ((m & (256u << smp)) == 0u || pvr_key_before(kh, kl, s_kh[li], s_kl[li] & ~3u) == 0u)
                        continue;
                     at = PVRC_KEEP - 1u;
                  }
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
               if (PVRC_SAMPLES > 1u)
               {
                  /* the list as it stands now, for the next step's shading */
                  s_xs[g_tid * 5u] = (started ? 256u : 0u) | n;
                  if (n == PVRC_KEEP)
                  {
                     s_xs[g_tid * 5u + 3u] = s_kh[(PVRC_KEEP - 1u) * 256u + g_tid];
                     s_xs[g_tid * 5u + 4u] = s_kl[(PVRC_KEEP - 1u) * 256u + g_tid] & ~3u;
                  }
                  barrier();
               }
            }

            /* The translucent modifier volumes, on the fragments kept: at
             * the pixel, where they cover its first sample */
            {
               uint cov = 0u;
               uint pending = PVRC_NONE;
               uint closer = 0u;
               it_begin(mvtLo, mvtHi, HIZ_NONE, false);
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
                  if (!g_in || n == 0u)
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
         it_begin(trLo, trHi, HIZ_RUN, autosort);
         while ((prim = it_next()) != PVRC_NONE)
         {
            const uint r = prim * PVRC_REC_WORDS;
            const uint pp = rec_poly(r);
            const uint isp = frm.w[pp + PVRC_Q_ISP];
            const uint mode = autosort ? 6u : isp >> 29;
            const bool writes = (isp & (1u << 26)) == 0u;
            if (mode == 0u || ((mode & 1u) == 0u && it_zhi < g_zbound))
            {
               g_hiz_skipped++;
               continue;
            }
            i64vec3 e;
            ivec2 pos;
            if (lane_cover(r, pp, false, e, pos))
            {
               const float z = pvrc_depth(rec_bary(r, e).w);
               if (pvrc_depth_pass(mode, z, g_depth) != 0u)
               {
                  bool killed = false;
                  if ((frm.w[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) != 0u)
                     shade(pp, lane_frag(r, pp, e, pos), PVRC_SHADE_DEPTH, false, killed);
                  if (!killed && writes)
                     g_depth = z;
               }
            }
            if (writes && (mode & 1u) != 0u)
               g_zbound = min(g_zbound, it_zlo);
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
   if (g_tid == 0u && g_hiz_skipped != 0u)
      atomicAdd(bin.w[PVRC_B_STATS(pvrc_runs()) + PVRC_ST_HIZ], g_hiz_skipped);
}
#endif

#if PVRC_KERNEL == PVRC_K_GPUSRC
/* A frame of the GPU's TA decode, before the setup (PVRC_SRC_WORDS): one
 * workgroup. Each entry's number of triangles - a volume's from the CPU, a
 * polygon draw group's from its indices, kept inside the room the decode
 * has (none if it ran out) - and their prefix sum, the table's starts;
 * then the render passes' and the volumes' entries made numbers of
 * triangles, the frame's count of them and of chunks, the clip's depth
 * scale (the decode's 1/fZ_max) and the dispatches of the setup, the
 * scatter and the sort. */
layout(local_size_x = 256) in;
shared uint s_sum[256];

void main()
{
   const uint tid = gl_LocalInvocationIndex;
   const uint n = frm.w[PVRC_F_SOURCES], osrc = frm.w[PVRC_F_O_SRC];
   const uint room = frm.w[PVRC_F_GPU];
   const bool overrun = dfrm.w[PVRC_TAG_O_OVERRUN] != 0u;
   uint base = 0u;
   for (uint b = 0u; b < n; b += 256u)
   {
      const uint s = b + tid;
      uint count = 0u;
      if (s < n)
      {
         const uint e = osrc + s * PVRC_SRC_WORDS;
         const uint kind = frm.w[e + 1u], third = frm.w[e + 2u];
         uint f = 0u, c = 0u;
         if ((kind & PVRC_SRC_MODVOL) != 0u)
            count = third;
         else
         {
            if ((kind & PVRC_SRC_TRD) != 0u)
            {
               const uint strips = dtrd.w[third * PVRC_TAG_C_WORDS];
               if (strips != 0u)
               {
                  const uint last = (third + strips) * PVRC_TAG_C_WORDS;
                  f = dtrd.w[(third + 1u) * PVRC_TAG_C_WORDS + PVRC_TAG_C_FIRST];
                  const uint end = dtrd.w[last + PVRC_TAG_C_FIRST] + dtrd.w[last + PVRC_TAG_C_COUNT];
                  c = end > f ? end - f : 0u;
               }
            }
            else
            {
               f = dcmd.w[third * PVRC_TAG_C_WORDS + PVRC_TAG_C_FIRST];
               c = dcmd.w[third * PVRC_TAG_C_WORDS + PVRC_TAG_C_COUNT];
               frm.w[e + 2u] = f;
            }
            c = f >= room ? 0u : min(c, room - f);
            count = c > 2u ? c - 2u : 0u;
         }
         if (overrun)
            count = 0u;
      }
      s_sum[tid] = count;
      barrier();
      for (uint d = 1u; d < 256u; d <<= 1)
      {
         uint v = tid >= d ? s_sum[tid - d] : 0u;
         barrier();
         s_sum[tid] += v;
         barrier();
      }
      if (s < n)
         frm.w[osrc + s * PVRC_SRC_WORDS] = base + s_sum[tid] - count;
      base += s_sum[255];
      barrier();
   }
   const uint prims = min(base, frm.w[PVRC_F_PRIM_ROOM]);
   memoryBarrierBuffer();
   barrier();
   /* (an entry's start, or the end for the one past the last) */
   const uint passes = frm.w[PVRC_F_PASSES];
   for (uint k = tid; k < passes * PVRC_PASS_WORDS; k += 256u)
   {
      if ((k % PVRC_PASS_WORDS) == PVRC_P_FLAGS)
         continue;
      const uint s = frm.w[PVRC_F_PASS + k];
      frm.w[PVRC_F_PASS + k] = min(s < n ? frm.w[osrc + s * PVRC_SRC_WORDS] : base, prims);
   }
   const uint omvp = frm.w[PVRC_F_O_MVP];
   const uint mvps = (osrc - omvp) / PVRC_MVP_WORDS;
   for (uint m = tid; m < mvps; m += 256u)
   {
      const uint s = frm.w[omvp + m * PVRC_MVP_WORDS + PVRC_M_CLOSE_END];
      if (s < n)
         frm.w[omvp + m * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = frm.w[osrc + s * PVRC_SRC_WORDS] + frm.w[osrc + s * PVRC_SRC_WORDS + 2u] - 1u;
   }
   if (tid == 0u)
   {
      const uint chunks = (prims + PVR_CHUNK_PRIMS - 1u) >> PVR_CHUNK_LOG2;
      frm.w[PVRC_F_PRIMS] = prims;
      frm.w[PVRC_F_CHUNKS] = chunks;
      PVRC_INVARIANT float zs = uintBitsToFloat(dfrm.w[PVRC_TAG_O_ZSCALE]) * FRMF(PVRC_F_ZCLIP);
      frm.w[PVRC_F_ZCLIP] = floatBitsToUint(zs);
      const uint st = PVRC_B_STATS(pvrc_runs());
      bin.w[st + PVRC_ST_SETUP_X] = (prims + PVRC_WG_SETUP - 1u) / PVRC_WG_SETUP;
      bin.w[st + PVRC_ST_SETUP_X + 1u] = 1u;
      bin.w[st + PVRC_ST_SETUP_X + 2u] = 1u;
      bin.w[st + PVRC_ST_SORT_X] = pvrc_tiles();
      bin.w[st + PVRC_ST_SORT_X + 1u] = chunks;
      bin.w[st + PVRC_ST_SORT_X + 2u] = 1u;
   }
}
#endif

#if defined(PVRC_SHADER_HLSL)
/* The entry point: the invocation's numbers where the kernels look for
 * GLSL's (and pvr_lane.h's exchanges for the thread's index), then the
 * kernel */
[numthreads(PVRC_THREADS, 1, 1)]
void pvrc_entry(uint3 global : SV_DispatchThreadID, uint3 group : SV_GroupID, uint local : SV_GroupIndex)
{
   gl_GlobalInvocationID = global;
   gl_WorkGroupID = group;
   gl_LocalInvocationIndex = local;
#if !defined(PVR_LANE_NONE) && !defined(PVR_LANE_SUBGROUP)
   pvr_lane_self = local;
#endif
   pvrc_main();
}
#endif

#endif /* the kernels */

#endif
