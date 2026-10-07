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

class OITVulkanRenderer final : public BaseVulkanRenderer
{
public:
	bool Init() override
	{
		DEBUG_LOG(RENDERER, "OITVulkanRenderer::Init");

		oitBuffers.Init(0, 0, &texCommandPool);
		textureDrawer.Init(&samplerManager, &oitShaderManager, &textureCache, &oitBuffers);
		textureDrawer.SetCommandPool(&texCommandPool);

		screenDrawer.Init(&samplerManager, &oitShaderManager, &oitBuffers);
		screenDrawer.SetCommandPool(&texCommandPool);
		BaseInit(screenDrawer.GetRenderPass(), 2);
		// (a quad pipeline is for one subpass: the one above is the last subpass's, this the colour one's)
		lastPicturePipeline.Init(&shaderManager, screenDrawer.GetRenderPass(), 1);
		screenDrawer.SetLastPicturePipeline(&lastPicturePipeline);

		return true;
	}

	void Resize(int w, int h) override
	{
		NOTICE_LOG(RENDERER, "OIT Resize %d x %d", w, h);
		// what is replaced below may be in use by frames still on the GPU
		texCommandPool.WaitAll();
		screenDrawer.Init(&samplerManager, &oitShaderManager, &oitBuffers);
		BaseInit(screenDrawer.GetRenderPass(), 2);
		// (a quad pipeline is for one subpass: the one above is the last subpass's, this the colour one's)
		lastPicturePipeline.Init(&shaderManager, screenDrawer.GetRenderPass(), 1);
		screenDrawer.SetLastPicturePipeline(&lastPicturePipeline);
	}

	void KeepPicture() override { screenDrawer.KeepPicture(); }
	void SetRestoredPicture(vk::ImageView view) override { screenDrawer.SetRestoredPicture(view); }

	void Term() override
	{
		DEBUG_LOG(RENDERER, "VulkanRenderer::Term");
		GetContext()->WaitIdle();
		texCommandPool.WaitAll();
		oitShaderManager.Term();
		samplerManager.Term();
		screenDrawer.Term();
		lastPicturePipeline.Term();
		textureDrawer.Term();
		oitBuffers.Term();
		BaseVulkanRenderer::Term();
	}

	bool Render() override
	{
		if (pvrrc.isRenderFramebuffer)
			return true;

		OITDrawer *drawer;
		if (pvrrc.isRTT)
			drawer = &textureDrawer;
		else
			drawer = &screenDrawer;

		drawer->Draw(fogTexture.get(), paletteTexture.get());

		if (!pvrrc.isRTT)
			vmus->DrawOSD(screenDrawer.GetCurrentCommandBuffer(), vk::Extent2D(screen_width, screen_height));

		drawer->EndFrame();

		return !pvrrc.isRTT;
	}

private:
	OITBuffers oitBuffers;
	SamplerManager samplerManager;
	OITShaderManager oitShaderManager;
	OITScreenDrawer screenDrawer;
	QuadPipeline lastPicturePipeline;
	OITTextureDrawer textureDrawer;
};

Renderer* rend_OITVulkan()
{
	return new OITVulkanRenderer();
}
