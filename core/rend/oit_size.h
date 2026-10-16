/* How large the per-pixel renderers (Vulkan, Direct3D 11, OpenGL 4) make
 * the buffer that holds every translucent fragment of a frame: from the
 * size of the picture and the video memory budget (rend/vram_budget.h),
 * when the renderer is set up or the picture changes size - never while a
 * game runs.
 *
 * - OIT_LAYERS fragments a pixel, of OIT_FRAGMENT_BYTES each. A frame
 *   that wants more than that is drawn in bands by the Vulkan renderer
 *   (OITDrawer::Draw()) and the OpenGL 4 one (gl4DrawStrips()), so
 *   nothing is dropped; Direct3D 11 drops the fragments past the end, as
 *   it always has.
 * - no more than half of the budget that was left when the renderer
 *   started, and no more than @most, what the API can bind;
 * - and no less than OIT_MIN_BYTES, for a picture so small that the
 *   lists of a few layers would be smaller than what a game draws at
 *   the native resolution.
 * In bytes, a whole number of fragments. */
#ifndef REND_OIT_SIZE_H
#define REND_OIT_SIZE_H

#include "types.h"

#define OIT_FRAGMENT_BYTES 16u
#define OIT_LAYERS 2u
#define OIT_MIN_BYTES (32ull << 20)

static inline u64 oit_buffer_bytes(u32 width, u32 height, u32 available_kib, u64 most)
{
	u64 bytes = (u64)width * height * OIT_LAYERS * OIT_FRAGMENT_BYTES;
	const u64 share = (u64)available_kib * 1024u / 2u;
	if (bytes > share)
		bytes = share;
	if (bytes < OIT_MIN_BYTES)
		bytes = OIT_MIN_BYTES;
	if (bytes > most)
		bytes = most;
	return bytes & ~(u64)(OIT_FRAGMENT_BYTES - 1u);
}

#endif
