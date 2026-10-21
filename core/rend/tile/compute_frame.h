/* The compute renderer's frame (rend/tile/pvr_compute.h's PVRC_F_* words),
 * as both renderers that have it hand it to the kernels: Vulkan's
 * (vulkan/compute_drawer.h) and Direct3D 12's (dx12/dx12_compute.h). The
 * rules a frame's words are made by are the raster renderers': each
 * renderer gives its transform, its base scissor and its flat shading's
 * colours (Begin(), BaseScissor(), ProvokingVertices()), which are the
 * same in both. Also what the tests build their frames with
 * (tools/rend/tile_dump.cpp). */
#pragma once
#include "types.h"
#include "rend/transform_matrix.h"
#include "rend/tile/pvr_compute.h"

class ComputeFrame
{
public:
	struct Slots
	{
		// a texture's slot (PVRC_TEX_SLOTS of them), or PVRC_NONE when they are all taken
		virtual u32 Slot(u64 texid, TSP tsp) = 0;
		virtual ~Slots() = default;
	};
	struct Layout
	{
		u32 width, height;	// the picture's
		u32 tileLog2;		// of the coarse tiles
		u32 pool;		// entries of the bin pool
		u32 runs;		// runs the bin buffer has room for
		u32 chunks;		// ...for this many chunks
		bool lastPicture;	// start from the picture before
		bool lastScaled;	// ...one of another size
		u32 rttScale;		// a render to a texture: Render To Texture Upscaling's (else 0)
	};
	virtual ~ComputeFrame() = default;
	/* The words, in @out (room for @room); their count, or 0 when the frame
	 * does not fit there, has more primitives or textures than there is
	 * room for, or more render passes than the kernels take; @prims, the
	 * number of primitives. Flat polygons have their first vertex's colours
	 * made those of their last, as the raster renderers have them. */
	u32 Pack(const Layout& layout, Slots& slots, u32 *out, u32 room, u32 *prims);
	/* Why a frame is left to the raster renderer: Pack()'s reasons (after
	 * it returned 0), and the drawer's */
	enum { Why_None, Why_Passes, Why_Room, Why_Textures, Why_Rtt, Why_Osd, Why_Picture, Why_Decode, Why_Count };
	u32 why = Why_None;

	// the most words a frame of this machine takes, and the most primitives
	static u32 MaxWords();
	static u32 MaxPrims();
	// the coarse tiles' size for a picture this wide (log2)
	static u32 TileLog2(u32 width) { return width > 1280 ? 6 : 5; }
	// the most coarse tiles a texture rendered to has (at the most upscaling there is)
	static u32 MaxRttTiles();
	// Render To Texture Upscaling, as the renders to a texture have it
	static u32 RttScale();

protected:
	/* The renderer's: its matrices made for the frame in pvrrc, the
	 * viewport's (pixels) returned */
	virtual const xform& Begin() = 0;
	/* ...the screen's base scissor, in pixels (after Begin()) */
	virtual void BaseScissor(int& x, int& y, int& width, int& height) = 0;
	/* ...the flat polygons' colours where it draws them from */
	virtual void ProvokingVertices() = 0;
};
