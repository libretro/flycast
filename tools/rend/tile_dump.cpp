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
 *   TILE_W, TILE_H  the picture's size (default the core's screen)
 *
 * And, for the reference software renderer (tools/rend/refsw), the same
 * frames as the parse left them, with the registers and video memory
 * (refsw/refsw_dump.h: refsw_NNNNN.bin), chosen by the same variables:
 *   REFSW_DUMP_DIR  where they go; nothing is written without it */
#define PVR_REGS_FOR_RENDERER
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/vulkan/compute_drawer.h"
#include "refsw/refsw_dump.h"

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

template<class T>
static bool put(FILE *f, const List<T>& list)
{
	return list.used() == 0 || fwrite(list.head(), sizeof(T), list.used(), f) == (size_t)list.used();
}

/* The frame for the reference renderer: the context's lists as the parse
 * left them, the registers (the renderer's copy: the ones this frame is
 * drawn with) and video memory, whole. */
static void refsw_dump(TA_context *ctx, const char *dir, u32 n, u32 width, u32 height)
{
	const rend_context& r = ctx->rend;
	refsw_dump_header h;
	memset(&h, 0, sizeof(h));
	h.magic = REFSW_DUMP_MAGIC;
	h.version = REFSW_DUMP_VERSION;
	h.width = width;
	h.height = height;
	h.regs_size = pvr_RendRegSize;
	h.vram_size = VRAM_SIZE;
	h.sizeof_vertex = sizeof(Vertex);
	h.sizeof_polyparam = sizeof(PolyParam);
	h.sizeof_modtriangle = sizeof(ModTriangle);
	h.sizeof_mvparam = sizeof(ModifierVolumeParam);
	h.sizeof_renderpass = sizeof(RenderPass);
	h.n_verts = r.verts.used();
	h.n_idx = r.idx.used();
	h.n_op = r.global_param_op.used();
	h.n_pt = r.global_param_pt.used();
	h.n_tr = r.global_param_tr.used();
	h.n_modtrig = r.modtrig.used();
	h.n_mvo = r.global_param_mvo.used();
	h.n_mvo_tr = r.global_param_mvo_tr.used();
	h.n_passes = r.render_passes.used();
	h.n_region_tiles = r.region_tiles != NULL && r.region_tile_count <= REGION_TILES_MAX ? r.region_tile_count : 0;
	h.fZ_min = r.fZ_min;
	h.fZ_max = r.fZ_max;
	h.fb_x_clip = r.fb_X_CLIP.full;
	h.fb_y_clip = r.fb_Y_CLIP.full;
	h.fog_clamp_min = r.fog_clamp_min;
	h.fog_clamp_max = r.fog_clamp_max;
	h.frame = n;
	char path[1024];
	snprintf(path, sizeof(path), "%s/refsw_%05u.bin", dir, n);
	FILE *f = fopen(path, "wb");
	bool ok = f != NULL
		&& fwrite(&h, sizeof(h), 1, f) == 1
		&& fwrite(rend_pvr_regs, 1, pvr_RendRegSize, f) == pvr_RendRegSize
		&& fwrite(vram.data, 1, VRAM_SIZE, f) == VRAM_SIZE
		&& put(f, r.verts) && put(f, r.idx) && put(f, r.global_param_op) && put(f, r.global_param_pt) && put(f, r.global_param_tr)
		&& put(f, r.modtrig) && put(f, r.global_param_mvo) && put(f, r.global_param_mvo_tr) && put(f, r.render_passes)
		&& (h.n_region_tiles == 0 || fwrite(r.region_tiles, 4, h.n_region_tiles, f) == h.n_region_tiles);
	if (f != NULL)
		fclose(f);
	if (!ok)
		printf("tile_dump: %s not written\n", path);
	else
		printf("tile_dump: %s: %u vertices, %u indices, %u/%u/%u polygons, %u/%u modifier volumes (%u triangles), %u passes\n", path,
				h.n_verts, h.n_idx, h.n_op, h.n_pt, h.n_tr, h.n_mvo, h.n_mvo_tr, h.n_modtrig, h.n_passes);
	fflush(stdout);
}

extern "C" bool __wrap__Z13ta_parse_vdrcP10TA_context(TA_context *ctx)
{
	const bool rv = __real__Z13ta_parse_vdrcP10TA_context(ctx);
	const char *dir = getenv("TILE_DUMP_DIR");
	const char *refsw_dir = getenv("REFSW_DUMP_DIR");
	const u32 every = getenv("TILE_DUMP_EVERY") ? atoi(getenv("TILE_DUMP_EVERY")) : 100;
	const u32 most = getenv("TILE_DUMP_MAX") ? atoi(getenv("TILE_DUMP_MAX")) : 8;
	const u32 from = getenv("TILE_DUMP_FROM") ? atoi(getenv("TILE_DUMP_FROM")) : 0;
	const u32 n = frames++;
	if (rv && refsw_dir != NULL)
		printf("tile_dump: render %u: %s%u/%u/%u polygons, %u vertices\n", n, ctx->rend.isRTT ? "to a texture, " : "",
				ctx->rend.global_param_op.used(), ctx->rend.global_param_pt.used(), ctx->rend.global_param_tr.used(), ctx->rend.verts.used());
	if (!rv || (dir == NULL && refsw_dir == NULL) || ctx->rend.isRTT || ctx->rend.isRenderFramebuffer || written >= most || n < from || (n - from) % every != 0)
		return rv;
	if (refsw_dir != NULL)
		refsw_dump(ctx, refsw_dir, n, screen_width > 0 ? screen_width : 640, screen_height > 0 ? screen_height : 480);
	if (dir == NULL)
	{
		written++;
		return rv;
	}
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
