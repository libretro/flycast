#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Oct 2, 2019

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
#include <memory>
#include <math.h>
#include "vulkan.h"
#include "vulkan_renderer.h"
#include "drawer.h"
#include "shaders.h"
#include "rend/ssaa_pattern.h"

class VulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Init");

		samples = SsaaSamples();
		textureDrawer.SetCommandPool(&texCommandPool);
		textureDrawer.Init(&samplerManager, &shaderManager, &textureCache, RttSamples());
		textureDrawer.SetGpuDraw(taDecoder.Draw());

		screenDrawer.Init(&samplerManager, &shaderManager, samples);
		screenDrawer.SetCommandPool(&texCommandPool);
		screenDrawer.SetGpuDraw(taDecoder.Draw());
		taDecoder.WantSort(true);
		BaseInit(screenDrawer.GetRenderPass(), screenDrawer.GetOsdSubpass());
		screenDrawer.SetQuadPipeline(&quadPipeline);

		return true;
	}

	void Resize(int w, int h) override
	{
		// what is replaced below may be in use by frames still on the GPU
		texCommandPool.WaitAll();
		const u32 was = samples;
		samples = SsaaSamples();
		if (samples != was)
		{
			textureDrawer.Init(&samplerManager, &shaderManager, &textureCache, RttSamples());
			quadPipeline.Forget();
		}
		screenDrawer.Init(&samplerManager, &shaderManager, samples);
		BaseInit(screenDrawer.GetRenderPass(), screenDrawer.GetOsdSubpass());
		screenDrawer.SetQuadPipeline(&quadPipeline);
		// what this size no longer needs is given back once the frames that used it are done
		GetContext()->GetHeap().TrimSoon();
	}

	void KeepPicture() override { screenDrawer.KeepPicture(); }
	void SetRestoredPicture(vk::ImageView view) override { screenDrawer.SetRestoredPicture(view); }

protected:
	/* A sorted pass's translucent triangles are sorted from the frame's
	 * depths (TA_GPU_SORT); sorted by strip, the polygons are sorted from
	 * the CPU's vertices, and a frame that may have a sorted pass is
	 * parsed. The polygons drawn again for shadows are those the GPU says
	 * reach into the volumes. */
	u32 TaFlags() const override
	{
		return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
				| (!settings.pvr.Emulation.AlphaSortMode && taDecoder.CanSort() ? TA_GPU_SORT : TA_GPU_NO_SORT);
	}

public:
	void Term() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Term");
		GetContext()->LogVramUsage();
		// (the frontend lets go of the pictures before they go)
		GetContext()->ReleasePicture();
		texCommandPool.WaitAll();
		screenDrawer.Term();
		textureDrawer.Term();
		samplerManager.Term();
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

		Drawer *drawer;
		if (pvrrc.isRTT)
			drawer = &textureDrawer;
		else
			drawer = &screenDrawer;

		drawer->Draw(fogTexture.get(), paletteTexture.get());

		if (!pvrrc.isRTT)
			vmus->DrawOSD(screenDrawer.GetCurrentCommandBuffer(), vk::Extent2D(screen_width, screen_height), &texCommandPool);

		drawer->EndRenderPass();

		return !pvrrc.isRTT;
	}

private:
	/* SSAA's samples a pixel, for the screen: what the option asks, as far
	 * as the device and the video memory budget go for its attachments */
	u32 SsaaSamples() const
	{
		const vk::Extent2D size = GetContext()->GetViewPort();
		return GetContext()->SsaaSamples(settings.rend.Ssaa, ssaa_per_sample_kib(size.width, size.height, 4 + GetContext()->GetDepthBytes()),
				0, true, "Vulkan per-triangle renderer");
	}
	/* ...and for the textures rendered to: the same where their sample
	 * attachments cost nothing (transient, never stored, in memory that
	 * is only backed when used); one elsewhere, since they are made as
	 * large as such a texture can be, which is more video memory than the
	 * screen's */
	u32 RttSamples() const { return GetContext()->HasLazyMemory() ? samples : 1; }

	SamplerManager samplerManager;
	ScreenDrawer screenDrawer;
	TextureDrawer textureDrawer;
};

Renderer* rend_Vulkan()
{
	return new VulkanRenderer();
}
