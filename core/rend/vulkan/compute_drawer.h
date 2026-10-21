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
 * - A render to a texture is drawn the same way, at one sample a pixel
 *   as the per-pixel renderer draws it, into the very texture that
 *   renderer would have drawn it in (OITTextureDrawer::ComputeBegin()):
 *   the size of the frame from its first pixel to the pixel clip's last,
 *   in powers of two, times Render To Texture Upscaling, cleared outside
 *   the clip. The texture cache, the lock on its video memory and the
 *   copy to video memory when the game touches it (rtt_read.h) are that
 *   renderer's, unchanged. Its bins are made for the largest such texture.
 * - A frame it cannot draw is left to the raster renderer (CanDraw()):
 *   more textures than it has slots for, a texture rendered to that the
 *   device cannot write from a compute shader, the VMUs or crosshairs
 *   drawn over the picture, more render passes than the kernels take. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "commandpool.h"
#include "drawer.h"
#include "gpu_timer.h"
#include "texture.h"
#include "rend/tile/compute_frame.h"

#include <memory>
#include <vector>

/* The frame's words for the kernels (PVRC_F_*), from the render context
 * (pvrrc), by the Vulkan renderers' transforms and clip rectangles. Also
 * what the tests build their frames with (tools/rend/tile_dump). */
class VulkanComputeFrame : public ComputeFrame, public BaseDrawer
{
protected:
	const xform& Begin() override
	{
		matrices.CalcMatrices(&pvrrc);
		return matrices.GetViewportMatrix();
	}
	void BaseScissor(int& x, int& y, int& width, int& height) override
	{
		SetBaseScissor();
		x = baseScissor.offset.x;
		y = baseScissor.offset.y;
		width = (int)baseScissor.extent.width;
		height = (int)baseScissor.extent.height;
	}
	void ProvokingVertices() override { SetProvokingVertices(); }
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
	 * before (@scaled: one of another size, kept from another context);
	 * a render to a texture, into one of @rtt's size. False: it does not
	 * fit, and the raster renderer draws it; nothing is recorded either
	 * way, and nothing of the textures is looked at. */
	bool Prepare(bool last, bool scaled, const Texture *paletteTexture, vk::Extent2D rtt = vk::Extent2D());
	/* ...then drawn into @image (its view @view), left in the layout the
	 * raster renderer leaves its picture in; @last, the picture before (or
	 * null). Its own command buffer of the pool's, submitted with the
	 * frame's others. */
	void Record(vk::Image image, vk::ImageView view, vk::ImageView last);

	// a frame left to the raster renderer, and why (ComputeFrame::Why_*)
	void Fallback(u32 why) { statFallback++; statWhy[why < ComputeFrame::Why_Count ? why : 0]++; }
	// what is logged at Term()
	u64 statFrames = 0;
	u64 statGpuFrames = 0;
	u64 statRttFrames = 0;
	u64 statFallback = 0;
	u64 statWhy[ComputeFrame::Why_Count] = {};

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
	VulkanComputeFrame frame;
	GpuFrameTimer timer;
};
