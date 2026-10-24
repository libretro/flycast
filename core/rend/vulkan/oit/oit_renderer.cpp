#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Nov 7, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "../vulkan.h"
#include "../vulkan_renderer.h"
#include "oit_drawer.h"
#include "oit_shaders.h"
#include "oit_buffer.h"
#include "rend/ssaa_pattern.h"
#include "../compute_drawer.h"
#include "rend/rend_method.h"

class OITVulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		DEBUG_LOG(RENDERER, "OITVulkanRenderer::Init");

		oitBuffers.SetCompact(rend_compute_wanted && ComputeDrawer::DeviceLacks() == nullptr);
		oitBuffers.Init(0, 0, &texCommandPool);
		samples = SsaaSamples();
		oitShaderManager.SetSamples(samples);
		NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: with SSAA, a translucent fragment takes the samples %s",
				GetContext()->postDepthCoverage ? "the depth test passed (VK_EXT_post_depth_coverage)" : "its own depth test passes, at each sample");
		textureDrawer.SetCommandPool(&texCommandPool);
		textureDrawer.Init(&samplerManager, &rttShaderManager, &textureCache, &oitBuffers);
		/* (renders to a texture drawn by the compute renderer go to the
		 * pool's images, which it then writes as storage images) */
		texPool.SetTargetStorage(rend_compute_wanted && ComputeDrawer::DeviceLacks() == nullptr);
		textureDrawer.SetGpuDraw(taDecoder.Draw());

		screenDrawer.SetStoragePicture(rend_compute_wanted);
		screenDrawer.Init(&samplerManager, &oitShaderManager, &oitBuffers, samples);
		screenDrawer.SetCommandPool(&texCommandPool);
		screenDrawer.SetGpuDraw(taDecoder.Draw());
		computeDrawer.SetGpuDraw(taDecoder.Draw());
		BaseInit(screenDrawer.GetRenderPass(), screenDrawer.GetOsdSubpass());
		textureDrawer.SetStorageTarget(texPool.TargetsStorable());
		// (a quad pipeline is for one subpass: the one above is the last subpass's, this the colour one's)
		lastPicturePipeline.Init(&shaderManager, screenDrawer.GetRenderPass(), 1, samples);
		screenDrawer.SetLastPicturePipeline(&lastPicturePipeline);
		InitCompute();

		return true;
	}

	void Resize(int w, int h) override
	{
		NOTICE_LOG(RENDERER, "OIT Resize %d x %d", w, h);
		// what is replaced below may be in use by frames still on the GPU
		texCommandPool.WaitAll();
		oitBuffers.SetCompact(rend_compute_wanted && ComputeDrawer::DeviceLacks() == nullptr);
		const u32 was = samples;
		samples = SsaaSamples();
		if (samples != was)
		{
			// (a change of SSAA: the shaders, render passes and pipelines of the other count go)
			oitShaderManager.SetSamples(samples);
			quadPipeline.Forget();
			lastPicturePipeline.Forget();
		}
		screenDrawer.Init(&samplerManager, &oitShaderManager, &oitBuffers, samples);
		BaseInit(screenDrawer.GetRenderPass(), screenDrawer.GetOsdSubpass());
		// (a quad pipeline is for one subpass: the one above is the last subpass's, this the colour one's)
		lastPicturePipeline.Init(&shaderManager, screenDrawer.GetRenderPass(), 1, samples);
		screenDrawer.SetLastPicturePipeline(&lastPicturePipeline);
		InitCompute();
		// what this size no longer needs is given back once the frames that used it are done
		GetContext()->GetHeap().TrimSoon();
	}

	void KeepPicture() override { screenDrawer.KeepPicture(); }
	void SetRestoredPicture(vk::ImageView view) override { screenDrawer.SetRestoredPicture(view); }

protected:
	/* The per-pixel lists sort the translucent polygons, by depth and then
	 * by polygon number: a frame decoded on the GPU numbers its strips by
	 * their draws' firstInstance, and without that it has no sorted pass */
	u32 TaFlags() const override
	{
		return TA_GPU_DRAW | TA_GPU_PROVOKE | (taDecoder.HasInstance() ? TA_GPU_INSTANCE : TA_GPU_NO_SORT);
	}

public:
	void Term() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Term");
		GetContext()->LogVramUsage();
		// (the frontend lets go of the pictures before they go)
		GetContext()->ReleasePicture();
		texCommandPool.WaitAll();
		computeDrawer.Term();
		samplerManager.Term();
		screenDrawer.Term();
		lastPicturePipeline.Term();
		textureDrawer.Term();
		/* (after the drawers: the pipeline thread may be making pipelines
		 * of these shaders for them until their pipelines go) */
		oitShaderManager.Term();
		rttShaderManager.Term();
		oitBuffers.Term();
		BaseVulkanRenderer::Term();
	}

	bool Render() override
	{
		if (pvrrc.isRenderFramebuffer)
			return true;
		if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
		{
			/* A pixel clip that ends before it starts holds no pixel: the
			 * chip writes nothing. (Its size would be negative here.) */
			WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d",
					pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
			return false;
		}

		/* Rendering Method: Compute draws the frames it can
		 * (vulkan/compute_drawer.h) into the same pictures, and the renders
		 * to a texture into the same textures; the others are drawn below */
		if (computeDrawer.Ready() && pvrrc.isRTT)
		{
			if (!texPool.TargetsStorable())
				computeDrawer.Fallback(ComputeFrame::Why_Rtt);
			else if (!computeDrawer.CanDraw())
				computeDrawer.Fallback(ComputeFrame::Why_Decode);
			else if (computeDrawer.Prepare(false, false, paletteTexture.get(), OITTextureDrawer::ComputeSize()))
			{
				const OITTextureDrawer::ComputeTarget target = textureDrawer.ComputeBegin();
				computeDrawer.Record(target.image, target.view, vk::ImageView());
				textureDrawer.ComputeEnd();
				return false;
			}
		}
		else if (computeDrawer.Ready())
		{
			bool last, scaled;
			if (!computeDrawer.CanDraw())
				computeDrawer.Fallback(ComputeFrame::Why_Decode);
			else if (vmus->Drawing())
				computeDrawer.Fallback(ComputeFrame::Why_Osd);
			else if (!screenDrawer.ComputeCanStart(&last, &scaled))
				computeDrawer.Fallback(ComputeFrame::Why_Picture);
			else if (computeDrawer.Prepare(last, scaled, paletteTexture.get()))
			{
				const OITScreenDrawer::ComputeTarget target = screenDrawer.ComputeBegin(last, scaled);
				computeDrawer.Record(target.image, target.view, target.last);
				screenDrawer.ComputeEnd();
				return true;
			}
		}

		OITDrawer *drawer;
		if (pvrrc.isRTT)
			drawer = &textureDrawer;
		else
			drawer = &screenDrawer;

		drawer->Draw(fogTexture.get(), paletteTexture.get());

		if (!pvrrc.isRTT)
			vmus->DrawOSD(screenDrawer.GetCurrentCommandBuffer(), vk::Extent2D(screen_width, screen_height), &texCommandPool);

		drawer->EndFrame();

		return !pvrrc.isRTT;
	}

private:
	// the compute renderer, when it is the rendering method: made with the screen's drawer
	void InitCompute()
	{
		if (rend_compute_wanted)
			computeDrawer.Init(GetContext()->GetViewPort(), samples, &texCommandPool, &samplerManager);
		else
			computeDrawer.Term();
	}

	/* SSAA's samples a pixel, for the screen: as far as the device and
	 * the video memory budget go for its attachments, after the lists */
	u32 SsaaSamples() const
	{
		const vk::Extent2D size = GetContext()->GetViewPort();
		return GetContext()->SsaaSamples(settings.rend.Ssaa, ssaa_per_sample_kib(size.width, size.height, 4 + GetContext()->GetDepthBytes()),
				(u32)(oitBuffers.PlannedSize(size.width, size.height) >> 10), false, "Vulkan per-pixel renderer");
	}

	OITBuffers oitBuffers;
	SamplerManager samplerManager;
	OITShaderManager oitShaderManager;
	/* Textures are rendered to with one sample a pixel, SSAA or not: their
	 * sample attachments would be made as large as such a texture can be,
	 * more video memory than SSAA's screen attachments, and these renders'
	 * attachments are stored between render passes */
	OITShaderManager rttShaderManager;
	OITScreenDrawer screenDrawer;
	QuadPipeline lastPicturePipeline;
	OITTextureDrawer textureDrawer;
	ComputeDrawer computeDrawer;
};

Renderer* rend_OITVulkan()
{
	return new OITVulkanRenderer();
}
