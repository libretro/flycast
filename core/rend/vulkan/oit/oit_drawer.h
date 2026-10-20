/*
    Created on: Nov 6, 2019

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
#pragma once
#include <memory>
#include "rend/sorter.h"
#include "rend/transform_matrix.h"
#include "../vulkan.h"
#include "../buffer.h"
#include "../commandpool.h"
#include "oit_pipeline.h"
#include "oit_shaders.h"
#include "../texture.h"
#include "../quad.h"
#include "oit_buffer.h"
#include "../drawer.h"

class OITDrawer : public BaseDrawer
{
public:
	virtual ~OITDrawer() = default;
	bool Draw(const Texture *fogTexture, const Texture *paletteTexture);

	virtual vk::CommandBuffer NewFrame() = 0;
	virtual void EndFrame() = 0;

	/* Clipped to the band being drawn, when a frame is drawn in bands */
	void SetScissor(const vk::CommandBuffer& cmdBuffer, const vk::Rect2D& scissor)
	{
		if (band.extent.height == 0)
		{
			BaseDrawer::SetScissor(cmdBuffer, scissor);
			return;
		}
		const s32 top = std::max(scissor.offset.y, band.offset.y);
		const s32 bottom = std::max(top, std::min(scissor.offset.y + (s32)scissor.extent.height, band.offset.y + (s32)band.extent.height));
		BaseDrawer::SetScissor(cmdBuffer, vk::Rect2D(vk::Offset2D(scissor.offset.x, top), vk::Extent2D(scissor.extent.width, (u32)(bottom - top))));
	}

protected:
	void Init(SamplerManager *samplerManager, OITPipelineManager *pipelineManager, OITBuffers *oitBuffers)
	{
		this->pipelineManager = pipelineManager;
		this->samplerManager = samplerManager;
		if (!quadBuffer)
			quadBuffer = std::unique_ptr<QuadBuffer>(new QuadBuffer());
		this->oitBuffers = oitBuffers;
		if (descriptorSets.size() > GetContext()->GetSwapChainSize())
			descriptorSets.resize(GetContext()->GetSwapChainSize());
		else
			while (descriptorSets.size() < GetContext()->GetSwapChainSize())
			{
				descriptorSets.push_back(OITDescriptorSets());
				descriptorSets.back().Init(samplerManager,
						pipelineManager->GetPipelineLayout(),
						pipelineManager->GetPerFrameDSLayout(),
						pipelineManager->GetPerPolyDSLayout(),
						pipelineManager->GetColorInputDSLayout());
			}
	}
	void Term()
	{
		quadBuffer.reset();
		colorAttachments[0].reset();
		colorAttachments[1].reset();
		tempFramebuffers[0].reset();
		tempFramebuffers[1].reset();
		depthAttachment.reset();
		mainBuffers.clear();
		vertexUniformBuffers.clear();
		descriptorSets.clear();
	}

	int GetCurrentImage() const { return imageIndex; }

	void NewImage() { imageIndex = (imageIndex + 1) % GetContext()->GetSwapChainSize(); }

	OITDescriptorSets& GetCurrentDescSet() { return descriptorSets[GetCurrentImage()]; }

	BufferData* GetMainBuffer(u32 size)
	{
		if (mainBuffers.empty())
		{
			for (int i = 0; i < GetContext()->GetSwapChainSize(); i++)
				mainBuffers.push_back(std::unique_ptr<BufferData>(new BufferData(std::max(512 * 1024u, size),
						vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eUniformBuffer
						| vk::BufferUsageFlagBits::eStorageBuffer)));
		}
		else if (mainBuffers[GetCurrentImage()]->bufferSize < size)
		{
			u32 newSize = mainBuffers[GetCurrentImage()]->bufferSize;
			while (newSize < size)
				newSize *= 2;
			INFO_LOG(RENDERER, "Increasing main buffer size %d -> %d", (u32)mainBuffers[GetCurrentImage()]->bufferSize, newSize);
			mainBuffers[GetCurrentImage()] = std::unique_ptr<BufferData>(new BufferData(newSize,
					vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eUniformBuffer
					| vk::BufferUsageFlagBits::eStorageBuffer));
	}
		return mainBuffers[GetCurrentImage()].get();
	};

	void MakeBuffers(int width, int height);
	void MakeSampleAttachments();
	void ChooseBands();
	// (what the lists of fragments a render pass asked for: OITBuffers::CountPass())
	void CountPass(vk::CommandBuffer cmdBuffer) { oitBuffers->CountPass(cmdBuffer); }
	void SetScreenSize(int width, int height) { oitBuffers->SetScreenSize(width, height); }
	virtual vk::Format GetColorFormat() const = 0;
	virtual vk::Framebuffer GetFinalFramebuffer() const = 0;
	/* In the colour subpass of a frame's first pass, before any polygon:
	 * the screen's drawer puts the last picture there when the render will
	 * cover only part of it. True if it drew, and what was bound is gone. */
	virtual bool DrawLastPicture(vk::CommandBuffer) { return false; }

	vk::Rect2D viewport;
	/* A frame's translucent fragments that do not all fit in the buffer
	 * (OITBuffers) are not dropped: the frame is drawn in this many
	 * horizontal bands, all of it once a band, clipped to the band. Each
	 * band's lists start empty, so they need the room of one band's
	 * fragments. How many is told by the frames before (see Draw()). */
	u32 bands = 1;
	u32 roomyFrames = 0;	// frames in a row that would have fitted in fewer
	vk::Rect2D band;	// the one being drawn; no height when not in bands
	std::array<std::unique_ptr<FramebufferAttachment>, 2> colorAttachments;
	std::unique_ptr<FramebufferAttachment> depthAttachment;
	vk::CommandBuffer currentCommandBuffer;
	OITPipelineManager *pipelineManager = nullptr;
	int maxWidth = 0;
	int maxHeight = 0;
	u32 attachmentSamples = 1;	// SSAA's, of the attachments made

private:
   void DrawPoly(const vk::CommandBuffer& cmdBuffer, u32 listType, bool autosort, Pass pass,
			const PolyParam& poly, u32 first, u32 count);
   void DrawList(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, Pass pass,
			const List<PolyParam>& polys, u32 first, u32 last);
	template<bool Translucent>
	void DrawModifierVolumes(const vk::CommandBuffer& cmdBuffer, int first, int count, const ModifierVolumeParam *modVolParams);
	void UploadMainBuffer(
			const OITDescriptorSets::FragmentShaderUniforms& fragmentUniforms);

	struct {
		vk::DeviceSize indexOffset = 0;
		vk::DeviceSize modVolOffset = 0;
		vk::DeviceSize fragmentUniformOffset = 0;
		vk::DeviceSize polyParamsOffset = 0;
		vk::DeviceSize polyParamsSize = 0;
	} offsets;

	std::unique_ptr<QuadBuffer> quadBuffer;

	std::array<vk::UniqueFramebuffer, 2> tempFramebuffers;

	SamplerManager *samplerManager = nullptr;
	OITBuffers *oitBuffers = nullptr;
	bool needDepthTransition = false;
	int imageIndex = 0;
	std::vector<OITDescriptorSets> descriptorSets;
	std::vector<std::unique_ptr<BufferData>> mainBuffers;
};

class OITScreenDrawer : public OITDrawer
{
public:
	// @samples: SSAA's (1: off), which the shader manager has been given
	void Init(SamplerManager *samplerManager, OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples)
	{
		if (!screenPipelineManager)
			screenPipelineManager = std::unique_ptr<OITPipelineManager>(new OITPipelineManager());
		screenPipelineManager->Init(shaderManager, oitBuffers, samples);
		OITDrawer::Init(samplerManager, screenPipelineManager.get(), oitBuffers);

		MakeFramebuffers();
		timer.Init(GetContext()->GetSwapChainSize(), "per-pixel renderer");
	}
	// the subpass the screen's last things are drawn in (the VMUs, the OSD)
	int GetOsdSubpass() const { return screenPipelineManager->GetFinalSubpass(); }
	void Term()
	{
		timer.Term();
		screenPipelineManager.reset();
		framebuffers.clear();
		finalColorAttachments.clear();
		carriedPicture.reset();
		restoredPicture = nullptr;
		lastPicture = QuadDrawer();
		havePicture = false;
		OITDrawer::Term();
	}

	virtual vk::CommandBuffer NewFrame() override;
	virtual void EndFrame() override
	{
		havePicture = true;
		restoredPicture = nullptr;
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		currentCommandBuffer.endRenderPass();
		CountPass(currentCommandBuffer);
		timer.End(currentCommandBuffer, commandPool->GetIndex());
		currentCommandBuffer.end();
		currentCommandBuffer = nullptr;
		commandPool->EndFrame();
		GetContext()->PresentFrame(finalColorAttachments[GetCurrentImage()]->GetImage(),
				finalColorAttachments[GetCurrentImage()]->GetImageView(),
				vk::Offset2D(viewport.extent.width, viewport.extent.height));
	}
	vk::RenderPass GetRenderPass() const { return screenPipelineManager->GetRenderPass(false, true); }
	vk::CommandBuffer GetCurrentCommandBuffer() const { return currentCommandBuffer; }

protected:
	virtual vk::Framebuffer GetFinalFramebuffer() const override { return *framebuffers[GetCurrentImage()]; }
	virtual vk::Format GetColorFormat() const override { return GetContext()->GetColorFormat(); }
	virtual bool DrawLastPicture(vk::CommandBuffer cmdBuffer) override
	{
		if ((!havePicture && !restoredPicture) || lastPicturePipeline == nullptr || pvrrc.clearFramebuffer || !matrices.IsClipped())
			return false;
		const int count = (int)finalColorAttachments.size();
		const std::array<float, 4> opaque = { 1.f, 1.f, 1.f, 1.f };

		SetScissor(cmdBuffer, viewport);
		cmdBuffer.setBlendConstants(opaque.data());
		lastPicturePipeline->BindPipeline(cmdBuffer);
		/* (the picture kept from another context can be of another size) */
		if (havePicture)
			lastPicture.Draw(cmdBuffer, finalColorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView(), nullptr, true, commandPool);
		else
			lastPicture.Draw(cmdBuffer, restoredPicture, nullptr, false, commandPool);
		return true;
	}
public:
	/* rend/last_picture.h: the context is going */
	void KeepPicture();
	/* ...and in the new one, the picture that was kept is this image */
	void SetRestoredPicture(vk::ImageView view) { restoredPicture = view; }
	/* A quad pipeline made for the colour subpass of this drawer's render pass. */
	void SetLastPicturePipeline(QuadPipeline *pipeline)
	{
		lastPicturePipeline = pipeline;
		lastPicture.Init(pipeline);
	}

	/* The compute renderer draws frames into this drawer's pictures
	 * (vulkan/compute_drawer.h), which are then made for it to write too */
	void SetStoragePicture(bool on) { storagePicture = on; }
	/* Whether the frame in pvrrc can be drawn by it here; @last, whether it
	 * starts from the picture before, as DrawLastPicture() says, and
	 * @scaled, whether that is the picture kept from another context, which
	 * can be of another size and is filtered over the screen as this
	 * drawer's quad does */
	bool ComputeCanStart(bool *last, bool *scaled)
	{
		matrices.CalcMatrices(&pvrrc);
		*last = false;
		*scaled = false;
		if ((!havePicture && !restoredPicture) || lastPicturePipeline == nullptr || pvrrc.clearFramebuffer || !matrices.IsClipped())
			return true;
		*last = true;
		*scaled = !havePicture;
		return true;
	}
	struct ComputeTarget
	{
		vk::Image image;
		vk::ImageView view;
		vk::ImageView last;	// the picture before, with @last
	};
	// the picture it draws the frame in
	ComputeTarget ComputeBegin(bool last, bool scaled)
	{
		NewImage();
		const int count = (int)finalColorAttachments.size();
		ComputeTarget t;
		t.image = finalColorAttachments[GetCurrentImage()]->GetImage();
		t.view = finalColorAttachments[GetCurrentImage()]->GetImageView();
		if (last)
			t.last = scaled ? restoredPicture : finalColorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView();
		return t;
	}
	// ...and once it is recorded, as EndFrame() ends one of this drawer's
	void ComputeEnd()
	{
		havePicture = true;
		restoredPicture = nullptr;
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		commandPool->EndFrame();
		GetContext()->PresentFrame(finalColorAttachments[GetCurrentImage()]->GetImage(),
				finalColorAttachments[GetCurrentImage()]->GetImageView(),
				vk::Offset2D(viewport.extent.width, viewport.extent.height));
	}
protected:

private:
	void MakeFramebuffers();

	std::vector<std::unique_ptr<FramebufferAttachment>> finalColorAttachments;
	QuadPipeline *lastPicturePipeline = nullptr;
	QuadDrawer lastPicture;
	bool havePicture = false;	/* the image before this one holds the last frame */
	vk::ImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	/* ...or the image the last frame was drawn to at the size before this one (see the per-triangle screen drawer) */
	std::unique_ptr<FramebufferAttachment> carriedPicture;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::unique_ptr<OITPipelineManager> screenPipelineManager;
	GpuFrameTimer timer;
	bool storagePicture = false;
};

class OITTextureDrawer : public OITDrawer
{
public:
	void Init(SamplerManager *samplerManager, OITShaderManager *shaderManager,
			TextureCache *textureCache, OITBuffers *oitBuffers)
	{
		if (!rttPipelineManager)
			rttPipelineManager = std::unique_ptr<RttOITPipelineManager>(new RttOITPipelineManager());
		rttPipelineManager->Init(shaderManager, oitBuffers);
		OITDrawer::Init(samplerManager, rttPipelineManager.get(), oitBuffers);

		this->textureCache = textureCache;
	}
	void Term()
	{
		framebuffers.clear();
		rttPipelineManager.reset();
		OITDrawer::Term();
	}

	virtual void EndFrame() override;

protected:
	virtual vk::CommandBuffer NewFrame() override;
	virtual vk::Framebuffer GetFinalFramebuffer() const override { return *framebuffers[GetCurrentImage()]; }
	virtual vk::Format GetColorFormat() const override { return vk::Format::eR8G8B8A8Unorm; }

private:
	u32 textureAddr = 0;

	Texture *texture = nullptr;
	vk::Image colorImage;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::unique_ptr<RttOITPipelineManager> rttPipelineManager;

	TextureCache *textureCache = nullptr;
};
