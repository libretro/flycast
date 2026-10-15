/* What the polygon shaders of the Vulkan raster renderers are made for, as
 * the bits of one specialization constant (constant_id 0): the same words
 * of SPIR-V serve every polygon, and a pipeline names the bits it is for.
 * Read by the C++ (pso.h, pipeline.cpp, oit_pipeline.cpp) and put in front
 * of the shaders by tools/rend/kernels_spv.sh, so both have one list.
 *
 * A pipeline made with the bits has them as constants: the device folds
 * them, and what is left is the shader the polygon had when each of them
 * was a #define. Made with the constant left at -1, the shader reads the
 * same bits from the push constants instead (PSO_PUSH_KEY): that is the
 * pipeline a polygon is drawn with while its own is still being made, and
 * it draws the same pixels. Interpolation, the sample rate and, for the
 * per-pixel renderer, the pass change what the shader declares, and are
 * modules of their own (raster_spv.inc). */
#ifndef VK_PSO_KEY_H
#define VK_PSO_KEY_H

/* per-triangle renderer: shaders/tri.frag */
#define TRI_SC_ALPHA_TEST     0
#define TRI_SC_CLIP_INSIDE    1
#define TRI_SC_USE_ALPHA      2
#define TRI_SC_TEXTURE        3
#define TRI_SC_IGNORE_TEX_A   4
#define TRI_SC_SHAD_INSTR     5	/* 2 bits */
#define TRI_SC_OFFSET         7
#define TRI_SC_FOG_CTRL       8	/* 2 bits */
#define TRI_SC_BUMP_MAP       10
#define TRI_SC_CLAMPING       11
#define TRI_SC_TRILINEAR      12
#define TRI_SC_PALETTE        13
#define TRI_SC_SHADOWED       14
#define TRI_SC_BITS           15

/* per-pixel renderer: shaders/oit.frag */
#define OIT_SC_ALPHA_TEST     0
#define OIT_SC_CLIP_INSIDE    1
#define OIT_SC_USE_ALPHA      2
#define OIT_SC_TEXTURE        3
#define OIT_SC_IGNORE_TEX_A   4
#define OIT_SC_SHAD_INSTR     5	/* 2 bits */
#define OIT_SC_OFFSET         7
#define OIT_SC_FOG_CTRL       8	/* 2 bits */
#define OIT_SC_TWO_VOLUMES    10
#define OIT_SC_BUMP_MAP       11
#define OIT_SC_CLAMPING       12
#define OIT_SC_PALETTE        13
#define OIT_SC_BITS           14

/* The per-triangle renderer's depth prepass (shaders/prepass.frag, rend/sorter.h):
 * the flags, in the push constant where the colour pass has its trilinear alpha */
#define PREPASS_CLIP_INSIDE   1u
#define PREPASS_USE_ALPHA     2u
#define PREPASS_PALETTE       4u
#define PREPASS_BUMP_MAP      8u
#define PREPASS_IGNORE_TEX_A  16u
#define PREPASS_SHAD_INSTR    32u	/* two bits: the shading instruction */
#define PREPASS_CLAMP         128u
#define PREPASS_FOG_ALPHA     256u	/* fog mode 3: the fog in the alpha */
#define PREPASS_ALWAYS        512u	/* every pixel hides what is behind it */
/* rend/sorter.h's PREPASS_ALPHA_MIN, as the shader has it */
#define PREPASS_ALPHA_MIN_GLSL 0.998039216

/* The specialization constants' ids */
#define PSO_SC_KEY            0	/* TRI_SC_* or OIT_SC_*; -1: read from the push constants */
#define PSO_SC_SSAA_SAMPLES   1	/* SSAA's samples a pixel */
#define PSO_SC_SSAA_LOG2N     2	/* and their log2 */

/* Where the bits are in the push constants of a pipeline made with -1:
 * the per-triangle renderer's block is six floats and then this; the
 * per-pixel one's, PushConstants (oit_pipeline.h) and then this */
#define TRI_PUSH_KEY_OFFSET   24
#define OIT_PUSH_KEY_OFFSET   96

#endif
