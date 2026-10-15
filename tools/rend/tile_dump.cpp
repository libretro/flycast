/* The frames a game draws to the screen, written as the compute renderer's
 * words (rend/tile/pvr_compute.h, ComputeFrame::Pack() of
 * rend/vulkan/compute_drawer.cpp), for tile_test to draw on a device and
 * with the reference. Linked into one program with the core's objects and
 * tools/threads/headless.c as the frontend (tile_test.sh): the link puts
 * __wrap_ta_parse_vdrc() in the place of the parse, and headless.c's
 * dlopen() opens the program itself.
 *
 *   TILE_DUMP_DIR   where the frames go (frame_NNNNN.bin)
 *   TILE_DUMP_EVERY one frame in this many is written (default 100)
 *   TILE_DUMP_MAX   at most this many (default 8)
 *   TILE_DUMP_FROM  the first frame that may be (default 0)
 *   TILE_W, TILE_H  the picture's size (default the core's screen) */
#define PVR_REGS_FOR_RENDERER
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/vulkan/compute_drawer.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern int screen_width, screen_height;

extern "C" void *__real_dlopen(const char *file, int mode);
extern "C" void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}

extern "C" bool __real__Z13ta_parse_vdrcP10TA_context(TA_context *ctx);

/* Test mode draws a texture from its slot and coordinates: any slot will
 * do, so they go by the order the frame asks for them */
struct DumpSlots : public ComputeFrame::Slots
{
	std::vector<std::pair<u64, u32>> seen;
	u32 Slot(u64 texid, TSP tsp) override
	{
		const u32 mask = tsp.full & 0x7ef00;
		for (size_t i = 0; i < seen.size(); i++)
			if (seen[i].first == texid && seen[i].second == mask)
				return (u32)i;
		if (seen.size() >= PVRC_TEX_SLOTS)
			return PVRC_NONE;
		seen.push_back(std::make_pair(texid, mask));
		return (u32)seen.size() - 1;
	}
};

static ComputeFrame packer;
static std::vector<u32> words;
static u32 frames, written;

extern "C" bool __wrap__Z13ta_parse_vdrcP10TA_context(TA_context *ctx)
{
	const bool rv = __real__Z13ta_parse_vdrcP10TA_context(ctx);
	const char *dir = getenv("TILE_DUMP_DIR");
	const u32 every = getenv("TILE_DUMP_EVERY") ? atoi(getenv("TILE_DUMP_EVERY")) : 100;
	const u32 most = getenv("TILE_DUMP_MAX") ? atoi(getenv("TILE_DUMP_MAX")) : 8;
	const u32 from = getenv("TILE_DUMP_FROM") ? atoi(getenv("TILE_DUMP_FROM")) : 0;
	const u32 n = frames++;
	if (!rv || dir == NULL || ctx->rend.isRTT || ctx->rend.isRenderFramebuffer || written >= most || n < from || (n - from) % every != 0)
		return rv;
	if (words.empty())
		words.resize(ComputeFrame::MaxWords());
	ComputeFrame::Layout layout;
	layout.width = getenv("TILE_W") ? atoi(getenv("TILE_W")) : (screen_width > 0 ? screen_width : 640);
	layout.height = getenv("TILE_H") ? atoi(getenv("TILE_H")) : (screen_height > 0 ? screen_height : 480);
	layout.tileLog2 = layout.width > 1280 ? 6 : 5;
	layout.pool = 2u << 20;
	layout.chunks = (ComputeFrame::MaxPrims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	layout.runs = layout.chunks * ((layout.width + (1u << layout.tileLog2) - 1) >> layout.tileLog2)
			* ((layout.height + (1u << layout.tileLog2) - 1) >> layout.tileLog2);
	layout.lastPicture = false;
	DumpSlots slots;
	u32 prims;
	/* (the parse's vertices are changed for flat shading, as the raster
	 * renderers change them: on a copy of the frame's, so that the core's
	 * own renderer draws what it would) */
	std::vector<Vertex> keep(ctx->rend.verts.head(), ctx->rend.verts.head() + ctx->rend.verts.used());
	const u32 count = packer.Pack(layout, slots, words.data(), (u32)words.size(), &prims);
	memcpy(ctx->rend.verts.head(), keep.data(), keep.size() * sizeof(Vertex));
	if (count == 0)
	{
		printf("tile_dump: frame %u not packed\n", n);
		return rv;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/frame_%05u.bin", dir, n);
	FILE *f = fopen(path, "wb");
	if (f == NULL || fwrite(words.data(), 4, count, f) != count)
		printf("tile_dump: %s not written\n", path);
	if (f != NULL)
		fclose(f);
	printf("tile_dump: frame %u: %u x %u, %u primitives, %u render passes, %u words\n", n, layout.width, layout.height,
			prims, words[PVRC_F_PASSES], count);
	fflush(stdout);
	written++;
	return rv;
}
