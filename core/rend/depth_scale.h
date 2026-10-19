#ifndef REND_DEPTH_SCALE_H
#define REND_DEPTH_SCALE_H

#include <retro_inline.h>
#include <stdint.h>

/* The raster renderers' depth, as the chip has it.
 *
 * The PowerVR2 takes the z the game sends with each vertex, which is 1/w,
 * interpolates it over the triangle with a plane equation in screen space
 * in 32-bit float, and compares it once, in the ISP, with one of eight
 * modes (larger is nearer). After that a pixel is known by its tag: the
 * TSP never tests depth again.
 *
 * The renderers used to write a depth of their own from the fragment
 * shader - log2(1 + 100000/w)/34 - in every pass, and then test EQUAL
 * between passes (the per-pixel renderers' depth and colour passes, a
 * polygon drawn again for its shadow, the sorted translucent depth
 * prepass). Each pass has its own fragment shader, so each is its own
 * pipeline, and a driver may round the same expression differently in
 * each: where a pass's depth missed the other's by a unit in the last
 * place the EQUAL test failed, and the clear colour showed through a decal
 * or a shadow as a black speck. That is only seen on real GPUs: a
 * software device compiles every pipeline the same way.
 *
 * Now the depth is the vertex position's, by the fixed functions and never
 * gl_FragDepth: a vertex goes out as (x*w, y*w, S, w) with w = 1/z, so the
 * depth the device interpolates is S*z - the chip's 1/w, linear in screen
 * space, in the depth buffer's 32-bit float with larger nearer (reversed
 * Z), and the eight depth modes are the device's own compare operations.
 * The same vertex shader, whose position is declared invariant, places the
 * vertices in every pass, so between passes EQUAL holds by construction.
 * Where the fragment shaders of the per-pixel renderers keep a depth in
 * their lists, or compare one, it is gl_FragCoord.z, that same value; fog
 * goes on taking 1/w from gl_FragCoord.w. With no fragment shader writing
 * depth, early depth testing works in every pass.
 *
 * S is 1/fZ_max, fZ_max the largest z of the frame below 2^20 (which the
 * parse tracks, and GPU-side TA processing keeps on the GPU: the kernels
 * write 1/fZ_max into a word of the frame for the vertex shaders to take,
 * nothing is read back), so the depth runs up to 1 for the nearest
 * vertex. The vertex shaders clamp the position's z to w, so a vertex
 * beyond fZ_max (there are none from the parse, which caps fZ_max at 2^20
 * - a 1/w of that size is a w below a millionth) is at depth 1, not
 * clipped. A device without a float depth buffer gets the best of its
 * fixed-point range this way too. Where the device's clip space has z in
 * [-1,1] (OpenGL without clip control, OpenGL ES) the vertex goes out at
 * z = 2*S - w: a depth of 2*S*z - 1.
 *
 * The renderers that scale z on the way to the device (ExtraDepthScale,
 * in the normal matrix) give the shader that scale apart (depth_mul), so
 * that the word the GPU writes is 1/fZ_max in every case. */

/* 1/fZ_max from the parse's fZ_max, or 1 for a frame that is not sane
 * (negative, not a number, or beyond what update_fz() keeps) */
static INLINE float pvr_depth_scale(float fz_max)
{
   union { float f; uint32_t u; } z;
   z.f = fz_max;
   if (z.u >= 0x49800000u || z.u < 0x3F800000u)
      return 1.0f;
   return 1.0f / fz_max;
}

#endif
