/*
	Created on: Oct 8, 2019

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
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "vulkan.h"
#include "buffer.h"
#include "commandpool.h"
#include "pipeline.h"
#include "shaders.h"
#include "texture.h"
#include "quad.h"
#include "ta_decoder.h"
#include "rend/shadows.h"

class BaseDrawer
{
public:
	void SetScissor(const vk::CommandBuffer& cmdBuffer, const vk::Rect2D& scissor)
	{
		if (scissor != currentScissor)
		{
			cmdBuffer.setScissor(0, scissor);
			currentScissor = scissor;
		}
	}
	void SetCommandPool(CommandPool *commandPool) { this->commandPool = commandPool; }
	// what a frame decoded on the GPU is drawn from (pvrrc.gpu)
	void SetGpuDraw(const TaGpuDraw *gpuDraw) { this->gpuDraw = gpuDraw; }

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }
	TileClipping SetTileClip(u32 val, vk::Rect2D& clipRect);
	void SetBaseScissor();
	void SetProvokingVertices();

	u32 align(vk::DeviceSize offset, u32 alignment)
	{
		return (u32)(alignment - (offset & (alignment - 1)));
	}

	template<typename T>
	T MakeFragmentUniforms()
	{
		T fragUniforms;

		//VERT and RAM fog color constants
		u8* fog_colvert_bgra = (u8*)&FOG_COL_VERT;
		u8* fog_colram_bgra = (u8*)&FOG_COL_RAM;
		fragUniforms.sp_FOG_COL_VERT[0] = fog_colvert_bgra[2] / 255.0f;
		fragUniforms.sp_FOG_COL_VERT[1] = fog_colvert_bgra[1] / 255.0f;
		fragUniforms.sp_FOG_COL_VERT[2] = fog_colvert_bgra[0] / 255.0f;

		fragUniforms.sp_FOG_COL_RAM[0] = fog_colram_bgra[2] / 255.0f;
		fragUniforms.sp_FOG_COL_RAM[1] = fog_colram_bgra[1] / 255.0f;
		fragUniforms.sp_FOG_COL_RAM[2] = fog_colram_bgra[0] / 255.0f;

		//Fog density constant
		u8* fog_density = (u8*)&FOG_DENSITY;
		float fog_den_mant = fog_density[1] / 128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
		s32 fog_den_exp = (s8)fog_density[0];
		fragUniforms.sp_FOG_DENSITY = fog_den_mant * powf(2.0f, fog_den_exp);

		fragUniforms.colorClampMin[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
		fragUniforms.colorClampMin[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
		fragUniforms.colorClampMin[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
		fragUniforms.colorClampMin[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

		fragUniforms.colorClampMax[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
		fragUniforms.colorClampMax[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
		fragUniforms.colorClampMax[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
		fragUniforms.colorClampMax[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

		// half a step under the reference: see the same in gles/gles.cpp
		fragUniforms.cp_AlphaTestValue = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;

		return fragUniforms;
	}

	/* The frame's vertices and indices: in the main buffer, or the GPU's
	 * decode's (pvrrc.gpu) */
	void BindGeometry(const vk::CommandBuffer& cmdBuffer, vk::Buffer mainBuffer, vk::DeviceSize indexOffset) const
	{
		const vk::DeviceSize zero = 0;
		if (pvrrc.gpu)
		{
			cmdBuffer.bindVertexBuffers(0, 1, &gpuDraw->vertices, &zero);
			cmdBuffer.bindIndexBuffer(gpuDraw->indices, 0, vk::IndexType::eUint32);
		}
		else
		{
			cmdBuffer.bindVertexBuffers(0, 1, &mainBuffer, &zero);
			cmdBuffer.bindIndexBuffer(mainBuffer, indexOffset, vk::IndexType::eUint32);
		}
	}
	// ...the modifier volumes' triangles, as the vertex buffer
	void BindModVolTriangles(const vk::CommandBuffer& cmdBuffer, vk::Buffer mainBuffer, vk::DeviceSize modVolOffset) const
	{
		const vk::DeviceSize zero = 0;
		if (pvrrc.gpu)
			cmdBuffer.bindVertexBuffers(0, 1, &gpuDraw->modtrig, &zero);
		else
			cmdBuffer.bindVertexBuffers(0, 1, &mainBuffer, &modVolOffset);
	}
	// ...and back to the vertices
	void BindVertices(const vk::CommandBuffer& cmdBuffer, vk::Buffer mainBuffer) const
	{
		const vk::DeviceSize zero = 0;
		cmdBuffer.bindVertexBuffers(0, 1, pvrrc.gpu ? &gpuDraw->vertices : &mainBuffer, &zero);
	}
	u32 ModVolTriangles() const { return pvrrc.gpu ? pvrrc.gpu_modtrig : (u32)pvrrc.modtrig.used(); }

	/* How DrawIndices() draws a frame decoded on the GPU: its draw groups;
	 * the sorted translucent triangles, whose indices the CPU wrote
	 * (TA_GPU_SORT); or the draw groups drawn again for shadows, by the
	 * commands the GPU wrote for that (TA_GPU_SHADOWS) */
	enum class GpuDraws { Groups, Sorted, Shadows };

	/* A polygon parameter's indices drawn: @count from @first; or, of a
	 * frame decoded on the GPU, a draw group's (PolyParam::first and count:
	 * see TA_GPU_DRAW), by its command, or for a translucent one by its
	 * draw records, with their count or all of its room */
	void DrawIndices(const vk::CommandBuffer& cmdBuffer, u32 listType, u32 first, u32 count) const
	{
		const u32 stride = sizeof(VkDrawIndexedIndirectCommand);	// (rend/ta_gpu.h's records)
		if (!pvrrc.gpu || gpuDraws == GpuDraws::Sorted)
			cmdBuffer.drawIndexed(count, 1, first, 0, 0);
		else if (gpuDraws == GpuDraws::Shadows)
			cmdBuffer.drawIndexedIndirect(gpuDraw->commands, gpuDraw->shadowCommands + (vk::DeviceSize)first * stride, 1, stride);
		else if (listType != ListType_Translucent)
			cmdBuffer.drawIndexedIndirect(gpuDraw->commands, (vk::DeviceSize)first * stride, 1, stride);
		else if (gpuDraw->drawCount != nullptr)
			gpuDraw->drawCount((VkCommandBuffer)cmdBuffer, (VkBuffer)gpuDraw->trDraws, (VkDeviceSize)(first + 1) * stride,
					(VkBuffer)gpuDraw->trDraws, (VkDeviceSize)first * stride, count, stride);
		else
			cmdBuffer.drawIndexedIndirect(gpuDraw->trDraws, (vk::DeviceSize)(first + 1) * stride, count, stride);
	}

	vk::Rect2D baseScissor;
	vk::Rect2D currentScissor;
	TransformMatrix<false> matrices;
	CommandPool *commandPool = nullptr;
	const TaGpuDraw *gpuDraw = nullptr;
	GpuDraws gpuDraws = GpuDraws::Groups;
};

class Drawer : public BaseDrawer
{
public:
	virtual ~Drawer() = default;
	bool Draw(const Texture *fogTexture, const Texture *paletteTexture);
	virtual void EndRenderPass() = 0;

protected:
	virtual vk::CommandBuffer BeginRenderPass() = 0;
	void NewImage()
	{
		imageIndex = (imageIndex + 1) % GetContext()->GetSwapChainSize();
		if (alphaSortMode != settings.pvr.Emulation.AlphaSortMode)
		{
			alphaSortMode = settings.pvr.Emulation.AlphaSortMode;
			pipelineManager->Reset();
		}
	}

	void Init(SamplerManager *samplerManager, PipelineManager *pipelineManager)
	{
		this->pipelineManager = pipelineManager;
		this->samplerManager = samplerManager;

		int size = GetContext()->GetSwapChainSize();
		if (descriptorSets.size() > size)
			descriptorSets.resize(size);
		else
			while (descriptorSets.size() < size)
			{
				descriptorSets.emplace_back();
				descriptorSets.back().Init(samplerManager, pipelineManager->GetPipelineLayout(), pipelineManager->GetPerFrameDSLayout(), pipelineManager->GetPerPolyDSLayout());
			}
	}
	int GetCurrentImage() const { return imageIndex; }
	DescriptorSets& GetCurrentDescSet() { return descriptorSets[GetCurrentImage()]; }

	BufferData* GetMainBuffer(u32 size)
	{
		if (mainBuffers.empty())
		{
			for (int i = 0; i < GetContext()->GetSwapChainSize(); i++)
				mainBuffers.push_back(std::unique_ptr<BufferData>(new BufferData(std::max(512 * 1024u, size),
						vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eUniformBuffer)));
		}
		else if (mainBuffers[GetCurrentImage()]->bufferSize < size)
		{
			u32 newSize = mainBuffers[GetCurrentImage()]->bufferSize;
			while (newSize < size)
				newSize *= 2;
			INFO_LOG(RENDERER, "Increasing main buffer size %d -> %d", (u32)mainBuffers[GetCurrentImage()]->bufferSize, newSize);
			mainBuffers[GetCurrentImage()] = std::unique_ptr<BufferData>(new BufferData(newSize,
					vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eUniformBuffer));
		}
		return mainBuffers[GetCurrentImage()].get();
	};

	vk::CommandBuffer currentCommandBuffer;
	SamplerManager *samplerManager = nullptr;

private:
	void SortTriangles();
	void DrawPoly(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
			PipelineManager::ShadowPass shadowPass = PipelineManager::ShadowPass::None, const vk::Rect2D *within = nullptr);
	void DrawShadowed(const vk::CommandBuffer& cmdBuffer, u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
			const ScreenBounds& area, const vk::Rect2D& scissor);
	void DrawSorted(const vk::CommandBuffer& cmdBuffer, const SortTrigDrawParam *polys, u32 count, bool multipass);
   void DrawList(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModVols(const vk::CommandBuffer& cmdBuffer, int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass,
			int render_pass);
	void UploadMainBuffer(const VertexShaderUniforms& vertexUniforms, const FragmentShaderUniforms& fragmentUniforms);

	int imageIndex = 0;
	struct {
		vk::DeviceSize indexOffset = 0;
		vk::DeviceSize modVolOffset = 0;
		vk::DeviceSize vertexUniformOffset = 0;
		vk::DeviceSize fragmentUniformOffset = 0;
	} offsets;
	std::vector<DescriptorSets> descriptorSets;
	std::vector<std::unique_ptr<BufferData>> mainBuffers;
	PipelineManager *pipelineManager = nullptr;

	// Per-triangle sort results
	std::vector<std::vector<SortTrigDrawParam>> sortedPolys;
	std::vector<std::vector<u32>> sortedIndexes;
	u32 sortedIndexCount = 0;
	bool alphaSortMode = false;
};

class ScreenDrawer : public Drawer
{
public:
	void Init(SamplerManager *samplerManager, ShaderManager *shaderManager);
	vk::RenderPass GetRenderPass() const { return *renderPass; }
	/* rend/last_picture.h: the context is going */
	void KeepPicture();
	/* ...and in the new one, the picture that was kept is this image */
	void SetRestoredPicture(vk::ImageView view) { restoredPicture = view; }
	/* What draws the last picture back: see BeginRenderPass(). */
	void SetQuadPipeline(QuadPipeline *pipeline)
	{
		quadPipeline = pipeline;
		lastPicture.Init(pipeline);
	}
	virtual void EndRenderPass() override;
	vk::CommandBuffer GetCurrentCommandBuffer() const { return currentCommandBuffer; }

protected:
	virtual vk::CommandBuffer BeginRenderPass() override;

private:
	std::unique_ptr<PipelineManager> screenPipelineManager;

	vk::UniqueRenderPass renderPass;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::vector<std::unique_ptr<FramebufferAttachment>> colorAttachments;
	QuadPipeline *quadPipeline = nullptr;
	QuadDrawer lastPicture;
	bool havePicture = false;	/* the image before this one holds the last frame */
	vk::ImageView restoredPicture;	/* ...or this does: the picture kept from the context before */
	/* ...or the image the last frame was drawn to at the size before this
	 * one, which is then this: kept as it is, with nothing copied */
	std::unique_ptr<FramebufferAttachment> carriedPicture;
	std::unique_ptr<FramebufferAttachment> depthAttachment;
	vk::Extent2D viewport;
	ShaderManager *shaderManager = nullptr;
};

class TextureDrawer : public Drawer
{
public:
	void Init(SamplerManager *samplerManager, ShaderManager *shaderManager, TextureCache *textureCache);
	virtual void EndRenderPass() override;

protected:
	virtual vk::CommandBuffer BeginRenderPass() override;

private:
	u32 width = 0;
	u32 height = 0;
	u32 textureAddr = 0;
	std::unique_ptr<RttPipelineManager> rttPipelineManager;

	Texture *texture = nullptr;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	std::unique_ptr<FramebufferAttachment> depthAttachment;
	TextureCache *textureCache = nullptr;
};
