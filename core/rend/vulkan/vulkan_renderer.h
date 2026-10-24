/*
	Copyright 2020 flyinghead

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
#include "vulkan.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "commandpool.h"
#include "pipeline.h"
#include "vmu.h"

#include "rtt_read.h"
#include "tex_decoder.h"
#include "ta_decoder.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include <memory>
#include <vector>

class BaseVulkanRenderer : public Renderer
{
public:
	virtual void Term() override
	{
		ta_set_flat_strips(false);
		ta_set_modvol_clip(false);
		shaderManager.Term();
		vmus.reset();
		quadPipeline.Term();
		vk_rtt_term();
		textureCache.Clear();
		Texture::SetDecoder(nullptr);
		texDecoder.Term();
		taDecoder.Term();
		keptPicture = nullptr;
		fogTexture = nullptr;
		paletteTexture = nullptr;
		texCommandPool.Term();
		framebufferTextures.clear();
		// (what the cache's textures had is back in the pool by now)
		Texture::SetPool(nullptr);
		texPool.Term();
	}

	/* The frontend let go of the old renderer's picture before it went
	 * (VulkanContext::ReleasePicture()) and would show none until this one
	 * draws: the picture kept from it (rend/last_picture.h) is handed over,
	 * as a framebuffer read from the game's memory is. Once, after the
	 * change of renderer. */
	bool PresentKeptPicture() override
	{
		int width, height;
		const u8 *pixels = last_picture(&width, &height);
		if (pixels == nullptr)
			return false;
		// (as Process() begins a frame)
		texCommandPool.BeginFrame();
		textureCache.SetCurrentIndex(texCommandPool.GetIndex());
		texDecoder.BeginFrame(texCommandPool.GetIndex());
		taDecoder.BeginFrame(texCommandPool);
		std::unique_ptr<Texture>& curTexture = FramebufferTexture();
		curTexture->SetCommandBuffer(texCommandPool.Allocate());
		curTexture->UploadToGPU(width, height, (u8 *)pixels, false);
		curTexture->SetCommandBuffer(nullptr);
		texCommandPool.EndFrame();
		GetContext()->PresentFrame(curTexture->GetImage(), curTexture->GetImageView(), { width, height });
		return true;
	}

	// (TA processing switched on or off)
	virtual void OptionsChanged() override
	{
		taDecoder.Setup(texCommandPool, ta_gpu_wanted());
	}

	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override
	{
		Texture* tf = textureCache.getTextureCacheData(tsp, tcw, area);

		if (tf->IsNew())
		{
			tf->Create();
			tf->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
			tf->SetDevice(GetContext()->GetDevice());
			tf->SetCached();
		}

		//update if needed
		if (tf->NeedsUpdate())
		{
			// This kills performance when a frame is skipped and lots of texture updated each frame
			//if (textureCache.IsInFlight(tf))
			//	textureCache.DestroyLater(tf);
			/* a command buffer of its own if it is uploaded from here; one
			 * made on the GPU is recorded with the others (Process()) */
			tf->SetCommandPool(&texCommandPool);
			tf->Update();
		}
		else if (tf->IsCustomTextureAvailable())
		{
			textureCache.DestroyLater(tf);
			tf->SetCommandPool(&texCommandPool);
			tf->CheckCustomTexture();
		}
		tf->SetCommandBuffer(nullptr);
		tf->SetCommandPool(nullptr);
		textureCache.SetInFlight(tf);

		return tf->GetIntId();
	}

	virtual bool Process(TA_context* ctx) override
	{
		texCommandPool.BeginFrame();
		textureCache.SetCurrentIndex(texCommandPool.GetIndex());
		texDecoder.BeginFrame(texCommandPool.GetIndex());
		taDecoder.BeginFrame(texCommandPool);

      if (!ctx->rend.isRTT)
         vmus->PrepareOSD(&texCommandPool);
      if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
         CheckKeptPicture();

		if (ctx->rend.isRenderFramebuffer)
			return RenderFramebuffer();

		if (KillTex)
		{
			/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
			rtt_watch_flush();
			textureCache.ClearLater();
		}

		/* SSAA draws flat strips at a single rate: the parse and the GPU's
		 * decode's header pass look for them then, and only then */
		ta_set_flat_strips(samples > 1);
		/* the modifier volumes' triangles far off the screen cut before the
		 * GPU clips them (rend/modvol_clip.h) */
		ta_set_modvol_clip(true);
		/* GPU-side TA processing: the header pass makes the frame ready to
		 * be drawn from the GPU's decode, in the full parse's place; or it
		 * is parsed as ever */
		bool result;
		if (!taDecoder.Ready() || !taDecoder.Prepare(ctx, TaFlags(), &result))
			result = ta_parse_vdrc(ctx);
		else if (result)
			taDecoder.Record(ctx, texCommandPool);
		// the textures the frame's polygons asked for that are made on the GPU, before the draws
		texDecoder.Flush(texCommandPool);

		textureCache.Cleanup();

		if (result)
		{
			CheckFogTexture();
			CheckPaletteTexture();
		}
		else
			texCommandPool.EndFrame();

		return result;
	}

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }
	// how the drawers draw a frame decoded on the GPU (ta_gpu_prepare()'s TA_GPU_*)
	virtual u32 TaFlags() const = 0;

	bool BaseInit(vk::RenderPass renderPass, int subpass = 0)
	{
		texCommandPool.Init();
		/* where the textures converted on the CPU go to the GPU through: one
		 * buffer, a share of the video memory, between 16 and 64 MB */
		Texture::Ring().Init((size_t)std::min<u64>(std::max<u64>((u64)GetContext()->GetVramBudget().available_kib() * 1024 / 32,
				16ull << 20), 64ull << 20), &texCommandPool);
		texDecoder.Init();
		Texture::SetDecoder(&texDecoder);
		// the textures' images, all of them now (rend/tex_pool.h)
		texPool.Init(&texDecoder);
		Texture::SetPool(&texPool);
		quadPipeline.Init(&shaderManager, renderPass, subpass);
		vmus = std::unique_ptr<VulkanOSD>(new VulkanOSD());
		vmus->Init(&quadPipeline);
		// GPU-side TA processing, if it is on: here, not in a frame
		taDecoder.Setup(texCommandPool, ta_gpu_wanted());

		return true;
	}

	/* In a new context, the picture kept from the old one (rend/last_picture.h)
	 * is made a texture, and the screen's drawer starts its first frame from it. */
	virtual void SetRestoredPicture(vk::ImageView view) = 0;

	void CheckKeptPicture()
	{
		int width, height;
		const u8 *pixels = last_picture(&width, &height);
		if (pixels == nullptr)
			return;
		keptPicture = std::unique_ptr<Texture>(new Texture());
		keptPicture->tex_type = TextureType::_8888;
		keptPicture->tcw.full = 0;
		keptPicture->tsp.full = 0;
		keptPicture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
		keptPicture->SetDevice(GetContext()->GetDevice());
		keptPicture->SetCommandBuffer(texCommandPool.Allocate());
		keptPicture->UploadToGPU(width, height, (u8 *)pixels, false);
		keptPicture->SetCommandBuffer(nullptr);
		SetRestoredPicture(keptPicture->GetImageView());
	}

	bool RenderFramebuffer()
	{
		if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		{
			/* Process() began a frame on the texture command pool and
			 * reset its fence; only a submit signals it again. Leave
			 * without one and BeginFrame() waits on that fence for ever
			 * when the pool comes round to this slot. */
			texCommandPool.EndFrame();
			return false;
		}

		PixelBuffer<u32> pb;
		int width;
		int height;
		ReadFramebuffer(pb, width, height);

		std::unique_ptr<Texture>& curTexture = FramebufferTexture();
		curTexture->SetCommandBuffer(texCommandPool.Allocate());
		curTexture->UploadToGPU(width, height, (u8*)pb.data(), false);
		curTexture->SetCommandBuffer(nullptr);
		texCommandPool.EndFrame();

		GetContext()->PresentFrame(curTexture->GetImage(), curTexture->GetImageView(), { 640, 480 });

		return true;
	}

	// the texture a picture of the CPU's is handed to the frontend in, for the frontend's frame index
	std::unique_ptr<Texture>& FramebufferTexture()
	{
		const size_t imageIndex = GetContext()->GetCurrentImageIndex();
		if (framebufferTextures.size() <= imageIndex)
			framebufferTextures.resize(std::max((size_t)GetContext()->GetSwapChainSize(), imageIndex + 1));
		std::unique_ptr<Texture>& curTexture = framebufferTextures[imageIndex];
		if (!curTexture)
		{
			curTexture = std::unique_ptr<Texture>(new Texture());
			curTexture->tex_type = TextureType::_8888;
			curTexture->tcw.full = 0;
			curTexture->tsp.full = 0;
			curTexture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
			curTexture->SetDevice(GetContext()->GetDevice());
		}
		return curTexture;
	}

	void CheckFogTexture()
	{
		if (!fogTexture)
		{
			fogTexture = std::unique_ptr<Texture>(new Texture());
			fogTexture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
			fogTexture->SetDevice(GetContext()->GetDevice());
			fogTexture->tex_type = TextureType::_8;
			fogUploadedValid = false;
		}
		// Uploaded when it differs from what the GPU has: see UpdateFogTexture() in gles.cpp
		u8 texData[256];
		MakeFogTexture(texData);
		if (fogUploadedValid && !memcmp(fogUploaded, texData, sizeof(fogUploaded)))
			return;
		memcpy(fogUploaded, texData, sizeof(fogUploaded));
		fogUploadedValid = true;
		fogTexture->SetCommandBuffer(texCommandPool.Allocate());

		fogTexture->UploadToGPU(128, 2, texData, false);

		fogTexture->SetCommandBuffer(nullptr);
	}

	void CheckPaletteTexture()
	{
		if (!paletteTexture)
		{
			paletteTexture = std::unique_ptr<Texture>(new Texture());
			paletteTexture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
			paletteTexture->SetDevice(GetContext()->GetDevice());
			paletteTexture->tex_type = TextureType::_8888;
			palette_updated = true;
		}
		if (!palette_updated)
			return;
		palette_updated = false;

		paletteTexture->SetCommandBuffer(texCommandPool.Allocate());

		paletteTexture->UploadToGPU(1024, 1, (u8 *)palette32_ram, false);

		paletteTexture->SetCommandBuffer(nullptr);
	}

	u32 samples = 1;	// SSAA's, a pixel (1: off)
	ShaderManager shaderManager;
	std::unique_ptr<Texture> fogTexture;
	u8 fogUploaded[256];
	bool fogUploadedValid = false;
	std::unique_ptr<Texture> paletteTexture;
	CommandPool texCommandPool;
	std::vector<std::unique_ptr<Texture>> framebufferTextures;
	TexDecoder texDecoder;		// (before the cache: textures going away tell it)
	TexturePool texPool;		// (and they give their images back)
	TaDecoder taDecoder;
	TextureCache textureCache;
	std::unique_ptr<Texture> keptPicture;
	QuadPipeline quadPipeline;
	std::unique_ptr<VulkanOSD> vmus;
};

