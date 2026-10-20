/* The compute renderer on Vulkan (Rendering Method: Compute): a frame
 * drawn to the screen by the kernels of rend/tile/pvr_compute.h, tile by
 * tile, into the per-pixel renderer's own picture, which goes to the
 * frontend as that renderer's does.
 *
 * - Its input is the CPU's parse of the frame (ta_parse_vdrc()): the
 *   vertices, indices and modifier volume triangles go up once, with the
 *   polygon parameters and the render passes, into this frame's piece of
 *   a ring of host-visible memory the kernels read in place. Or, for a
 *   frame the GPU decoded (TA processing: GPU, vulkan/ta_decoder.h), the
 *   decode's own buffers, read in place on the device: only the polygon
 *   parameters of its draw groups and the render passes go up, and a first
 *   kernel numbers the triangles from the decode's commands and translucent
 *   draw records (PVRC_K_GPUSRC). Nothing of the frame is parsed or copied
 *   on the CPU; the decode's last barrier is for compute shaders' reads
 *   too, and the next decode's first one waits for them.
 * - Everything is made in Init(), when the renderer starts or changes
 *   size: the kernels' pipelines (from SPIR-V made with the source,
 *   vulkan/compute_spv.inc), the ring, the primitives' setup and the bin
 *   pool on the device, sized for the largest frame the machine sends and
 *   the picture's size; nothing is allocated or compiled while a game runs.
 * - A frame is one command buffer of dispatches with barriers between
 *   them; nothing is read back and nothing waits. A few counts the GPU
 *   keeps (runs walked the slow way, triangles left out) are copied to
 *   host memory and read when the frame's slot comes round, after the wait
 *   for its fence the command pool does anyway.
 * - A frame it cannot draw is left to the raster renderer (CanDraw()):
 *   renders to a texture, more textures than it has slots for, a picture
 *   from another context to start from, the VMUs or crosshairs drawn over
 *   the picture, more render passes than the kernels take. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "commandpool.h"
#include "drawer.h"
#include "gpu_timer.h"
#include "texture.h"
#include "rend/tile/pvr_compute.h"

#include <memory>
#include <vector>

/* The frame's words for the kernels (PVRC_F_*), from the render context
 * (pvrrc): the drawers' own transforms and clip rectangles. Also what the
 * tests build their frames with (tools/rend/tile_test). */
class ComputeFrame : public BaseDrawer
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
	};
	/* The words, in @out (room for @room); their count, or 0 when the frame
	 * does not fit there, has more primitives or textures than there is
	 * room for, or more render passes than the kernels take; @prims, the
	 * number of primitives. Flat polygons have their first vertex's colours
	 * made those of their last, as the raster renderers have them. */
	u32 Pack(const Layout& layout, Slots& slots, u32 *out, u32 room, u32 *prims);

	// the most words a frame of this machine takes, and the most primitives
	static u32 MaxWords();
	static u32 MaxPrims();
};

class ComputeDrawer
{
public:
	ComputeDrawer();
	~ComputeDrawer();
	/* Made for a picture of @extent and @samples a pixel, for @pool's
	 * slots; false when the device cannot have it (logged) */
	bool Init(vk::Extent2D extent, u32 samples, CommandPool *pool, SamplerManager *samplerManager);
	void Term();
	/* Whether the device has what the kernels need (the reason it has not,
	 * or NULL): what Init() checks before it looks at the frame size and
	 * the video memory, for what is set up for the compute renderer before
	 * Init() can be called */
	static const char *DeviceLacks();
	bool Ready() const { return ready; }
	// what a frame decoded on the GPU is read from (pvrrc.gpu)
	void SetGpuDraw(const TaGpuDraw *gpuDraw) { this->gpuDraw = gpuDraw; }
	u32 GetSamples() const { return samples; }

	// whether the frame in pvrrc is one this draws (before anything is recorded)
	bool CanDraw() const;
	/* The frame's words and textures, @last: started from the picture
	 * before. False: it does not fit, and the raster renderer draws it;
	 * nothing is recorded either way. */
	bool Prepare(bool last, const Texture *paletteTexture);
	/* ...then drawn into @image (its view @view), left in the layout the
	 * raster renderer leaves its picture in; @last, the picture before (or
	 * null). Its own command buffer of the pool's, submitted with the
	 * frame's others. */
	void Record(vk::Image image, vk::ImageView view, vk::ImageView last);

	// what is logged at Term()
	u64 statFrames = 0;
	u64 statFallback = 0;

private:
	struct Resources;
	std::unique_ptr<Resources> res;
	bool ready = false;
	bool failed = false;
	u32 samples = 1;
	vk::Extent2D extent;
	CommandPool *commandPool = nullptr;
	SamplerManager *samplerManager = nullptr;
	const TaGpuDraw *gpuDraw = nullptr;
	ComputeFrame frame;
	GpuFrameTimer timer;
};
