/* How large the per-pixel renderers (Vulkan, Direct3D 11, OpenGL 4) make
 * the buffer that holds every translucent fragment of a frame: from the
 * size of the picture and the video memory budget (rend/vram_budget.h),
 * when the renderer is set up or the picture changes size - never while a
 * game runs.
 *
 * - OIT_LAYERS fragments a pixel, of OIT_FRAGMENT_BYTES each. A frame
 *   that wants more than that is drawn in bands by the Vulkan renderer
 *   (OITDrawer::Draw()), the OpenGL 4 one (gl4DrawStrips()) and the
 *   Direct3D 11 one (DX11OITRenderer::DrawFrame()), so nothing is
 *   dropped.
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

static inline u64 oit_buffer_bytes_of(u64 bytes, u32 available_kib, u64 most)
{
	const u64 share = (u64)available_kib * 1024u / 2u;
	if (bytes > share)
		bytes = share;
	if (bytes < OIT_MIN_BYTES)
		bytes = OIT_MIN_BYTES;
	if (bytes > most)
		bytes = most;
	return bytes & ~(u64)(OIT_FRAGMENT_BYTES - 1u);
}

static inline u64 oit_buffer_bytes(u32 width, u32 height, u32 available_kib, u64 most)
{
	return oit_buffer_bytes_of((u64)width * height * OIT_LAYERS * OIT_FRAGMENT_BYTES, available_kib, most);
}

/* The buffer when the compute renderer draws the screen's frames (Rendering
 * Method: Compute) and the per-pixel renderer only those it cannot: a
 * quarter of a layer - what these frames need beyond it is drawn in bands,
 * so nothing is dropped - in place of OIT_LAYERS, which at a high internal
 * resolution is most of the video memory the renderer takes */
#define OIT_FALLBACK_LAYER_SHIFT 2u
static inline u64 oit_fallback_buffer_bytes(u32 width, u32 height, u32 available_kib, u64 most)
{
	return oit_buffer_bytes_of(((u64)width * height * OIT_FRAGMENT_BYTES) >> OIT_FALLBACK_LAYER_SHIFT, available_kib, most);
}

#endif
