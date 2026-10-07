#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/* See rtt_read.h. */
#include "rtt_read.h"
#include "rend/rtt_watch.h"
#include "vulkan.h"
#include "texture.h"
#include "commandpool.h"
#include "hw/pvr/pvr_regs.h"
#include <memory>

static CommandPool read_pool;       /* its own: a read can come in the middle of a frame being set up */
static bool read_pool_ready;
static std::unique_ptr<FramebufferAttachment> staging;   /* the console's size, with memory this side can read */

/* The picture as the console would have it: w by h, R G B A. Waits for the
 * card - which is what not doing this until the game asks saves. */
static void vk_rtt_read(const RttWatch *watch, u8 *rgba)
{
	Texture *texture = (Texture *)watch->tex;
	VulkanContext *context = VulkanContext::Instance();
	vk::Device device = context->GetDevice();
	const u32 w = watch->w, h = watch->h, scale = watch->scale;
	const vk::Format format = vk::Format::eR8G8B8A8Unorm;
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);

	if (!staging || staging->getExtent().width != w || staging->getExtent().height != h)
	{
		/* (every read waits for its own commands, so the last one is idle) */
		staging = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), device));
		staging->Init(w, h, format, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst
				| vk::ImageUsageFlagBits::eSampled);
	}
	read_pool.Init();
	read_pool_ready = true;
	read_pool.BeginFrame();
	vk::CommandBuffer cmd = read_pool.Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	vk::Image source = texture->GetImage();
	setImageLayout(cmd, source, format, 1, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal);
	if (scale > 1)
	{
		/* made the console's size on the card, filtered, and read at that */
		const std::array<vk::Offset3D, 2> from = { vk::Offset3D(0, 0, 0), vk::Offset3D(w * scale, h * scale, 1) };
		const std::array<vk::Offset3D, 2> to = { vk::Offset3D(0, 0, 0), vk::Offset3D(w, h, 1) };

		setImageLayout(cmd, staging->GetImage(), format, 1, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
		cmd.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, staging->GetImage(), vk::ImageLayout::eTransferDstOptimal,
				vk::ImageBlit(layers, from, layers, to), vk::Filter::eLinear);
		setImageLayout(cmd, staging->GetImage(), format, 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal);
		source = staging->GetImage();
	}
	cmd.copyImageToBuffer(source, vk::ImageLayout::eTransferSrcOptimal, *staging->GetBufferData()->buffer,
			vk::BufferImageCopy(0, w, h, layers, vk::Offset3D(0, 0, 0), vk::Extent3D(w, h, 1)));
	vk::BufferMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *staging->GetBufferData()->buffer, 0, VK_WHOLE_SIZE);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, nullptr, barrier, nullptr);
	setImageLayout(cmd, texture->GetImage(), format, 1, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	cmd.end();
	read_pool.EndFrame();

	vk::Fence fence = read_pool.GetCurrentFence();
	device.waitForFences(1, &fence, true, UINT64_MAX);
	staging->GetBufferData()->download(w * h * 4, rgba);
}

/* The texture cache's, never this module's to delete. */
static void vk_rtt_release(uintptr_t)
{
}

static const RttWatchBackend vk_rtt_backend = { vk_rtt_read, vk_rtt_release };

/* What the render covers of video memory, as EndRenderPass() has it. */
static void covered(u32& width, u32 height, u32& stride, u32& bytes)
{
	stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = width * 2;
	else if (width * 2 > stride)
		// Happens for Virtua Tennis
		width = stride / 2;
	bytes = width == 0 || height == 0 ? 0 : stride * (height - 1) + width * 2;
}

void vk_rtt_supersede(u32 addr, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(width, height, stride, bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void vk_rtt_watch(Texture *texture, u32 addr, u32 width, u32 height)
{
	RttWatch watch = {};

	covered(width, height, watch.stride, watch.bytes);
	watch.addr = addr;
	watch.w = width;
	watch.h = height;
	watch.packmode = FB_W_CTRL.fb_packmode;
	watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
	watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
	watch.tex = (uintptr_t)texture;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes != 0)
		rtt_watch_add(&watch, &vk_rtt_backend);
}

void vk_rtt_term(void)
{
	rtt_watch_term();
	staging.reset();
	if (read_pool_ready)
		read_pool.Term();
	read_pool_ready = false;
}
