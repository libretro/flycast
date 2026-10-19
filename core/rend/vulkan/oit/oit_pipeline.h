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
#include <tuple>

#include "../vulkan.h"
#include "oit_shaders.h"
#include "oit_renderpass.h"
#include "oit_buffer.h"
#include "../texture.h"
#include "../ssaa.h"
#include "../pso.h"
#include "hw/pvr/ta_ctx.h"

class OITDescriptorSets
{
public:
	// std140 alignment required (the per-triangle renderer's: shaders.h)
	struct VertexShaderUniforms
	{
		float normal_matrix[16];
		float depth_scale;
		float depth_mul;
		float pad[2];
	};

	// std140 alignment required
	struct FragmentShaderUniforms
	{
		float colorClampMin[4];
		float colorClampMax[4];
		float sp_FOG_COL_RAM[4];	// Only using 3 elements but easier for std140
		float sp_FOG_COL_VERT[4];	// same comment
		float cp_AlphaTestValue;
		float sp_FOG_DENSITY;
		float shade_scale_factor;	// new for OIT
	};

	struct PushConstants
	{
		float clipTest[4];
		int blend_mode0[4];	// Only using 2 elements but easier for std140
		float trilinearAlpha;
		int pp_Number;
		float palette_index;
		int _pad;

		// two volume mode
		int blend_mode1[4];	// Only using 2 elements but easier for std140
		int shading_instr0;
		int shading_instr1;
		int fog_control0;
		int fog_control1;
		int use_alpha0;
		int use_alpha1;
		int ignore_tex_alpha0;
		int ignore_tex_alpha1;
		int key;	// the general pipelines' (pso_key.h): at OIT_PUSH_KEY_OFFSET
	};
	static_assert(sizeof(PushConstants) == OIT_PUSH_KEY_OFFSET + 4, "the shaders' push constants (shaders/oit.frag)");

	void Init(SamplerManager* samplerManager, vk::PipelineLayout pipelineLayout, vk::DescriptorSetLayout perFrameLayout,
			vk::DescriptorSetLayout perPolyLayout, vk::DescriptorSetLayout colorInputLayout)
	{
		this->samplerManager = samplerManager;
		this->pipelineLayout = pipelineLayout;
		this->perFrameLayout = perFrameLayout;
		this->perPolyLayout = perPolyLayout;
		this->colorInputLayout = colorInputLayout;
	}
	// FIXME way too many params
	// (the vertex uniforms are a buffer of their own: BaseDrawer::WriteVertexUniforms())
	void UpdateUniforms(vk::Buffer vertexUniformBuffer, vk::Buffer buffer, u32 fragmentUniformOffset, vk::ImageView fogImageView,
			u32 polyParamsOffset, u32 polyParamsSize, vk::ImageView stencilImageView, vk::ImageView depthImageView,
			vk::ImageView paletteImageView, vk::ImageLayout depthInputLayout,
			vk::Buffer gpuPolyParams = vk::Buffer(), vk::DeviceSize gpuPolyParamsSize = 0)
	{
		if (!perFrameDescSet)
		{
			perFrameDescSet = std::move(GetContext()->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), 1, &perFrameLayout)).front());
		}
		std::vector<vk::DescriptorBufferInfo> bufferInfos;
		bufferInfos.push_back(vk::DescriptorBufferInfo(vertexUniformBuffer, 0, sizeof(VertexShaderUniforms)));
		bufferInfos.push_back(vk::DescriptorBufferInfo(buffer, fragmentUniformOffset, sizeof(FragmentShaderUniforms)));

		std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfos[0], nullptr));
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfos[1], nullptr));
		if (fogImageView)
		{
			TSP fogTsp = {};
			// single entries, read at the middle of their texels: see UpdateFogTexture() in gles.cpp
			fogTsp.FilterMode = 0;
			fogTsp.ClampU = 1;
			fogTsp.ClampV = 1;
			vk::Sampler fogSampler = samplerManager->GetSampler(fogTsp);
			static vk::DescriptorImageInfo imageInfo;
			imageInfo = { fogSampler, fogImageView, vk::ImageLayout::eShaderReadOnlyOptimal };
			writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 2, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
			/* What an untextured polygon has as its textures: the general
			 * pipelines read them only for a textured polygon, but have them
			 * in their shader (pso.h) */
			if (!noTextureDescSet)
				noTextureDescSet = std::move(GetContext()->GetDevice().allocateDescriptorSetsUnique(
						vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), 1, &perPolyLayout)).front());
			if (noTextureView != fogImageView)
			{
				noTextureView = fogImageView;
				writeDescriptorSets.push_back(vk::WriteDescriptorSet(*noTextureDescSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
				writeDescriptorSets.push_back(vk::WriteDescriptorSet(*noTextureDescSet, 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
			}
		}
		if (paletteImageView)
		{
			TSP palTsp = {};
			palTsp.FilterMode = 0;
			palTsp.ClampU = 1;
			palTsp.ClampV = 1;
			vk::Sampler palSampler = samplerManager->GetSampler(palTsp);
			static vk::DescriptorImageInfo imageInfo;
			imageInfo = { palSampler, paletteImageView, vk::ImageLayout::eShaderReadOnlyOptimal };
			writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 6, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
		}
		if (polyParamsSize > 0)
		{
			static vk::DescriptorBufferInfo polyParamsBufferInfo;
			/* (of a frame decoded on the GPU, the translucent polygon
			 * parameters are the GPU's: see TaGpuDraw) */
			polyParamsBufferInfo = gpuPolyParams ? vk::DescriptorBufferInfo(gpuPolyParams, 0, gpuPolyParamsSize)
					: vk::DescriptorBufferInfo(buffer, polyParamsOffset, polyParamsSize);
			writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 3, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &polyParamsBufferInfo, nullptr));
		}
		vk::DescriptorImageInfo stencilImageInfo(vk::Sampler(), stencilImageView, depthInputLayout);
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 4, 0, 1, vk::DescriptorType::eInputAttachment, &stencilImageInfo, nullptr, nullptr));
		vk::DescriptorImageInfo depthImageInfo(vk::Sampler(), depthImageView, depthInputLayout);
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 5, 0, 1, vk::DescriptorType::eInputAttachment, &depthImageInfo, nullptr, nullptr));

		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
	}

	// @layout: the colour attachment's where it is read (SSAA's are in the general one)
	void UpdateColorInputDescSet(int index, vk::ImageView colorImageView, vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal)
	{
		if (!colorInputDescSets[index])
			colorInputDescSets[index] = std::move(GetContext()->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), 1, &colorInputLayout)).front());
		std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
		vk::DescriptorImageInfo colorImageInfo(vk::Sampler(), colorImageView, layout);
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*colorInputDescSets[index], 0, 0, 1, vk::DescriptorType::eInputAttachment, &colorImageInfo, nullptr, nullptr));

		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
	}

	void SetTexture(u64 textureId0, TSP tsp0, u64 textureId1, TSP tsp1)
	{
		auto index = std::make_tuple(textureId0, tsp0.full & SamplerManager::TSP_Mask,
				textureId1, tsp1.full & SamplerManager::TSP_Mask);
		if (perPolyDescSetsInFlight.find(index) != perPolyDescSetsInFlight.end())
			return;

		if (perPolyDescSets.empty())
		{
			std::vector<vk::DescriptorSetLayout> layouts(10, perPolyLayout);
			perPolyDescSets = GetContext()->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), layouts.size(), &layouts[0]));
		}
		Texture *texture = reinterpret_cast<Texture *>(textureId0);
		vk::DescriptorImageInfo imageInfo0(samplerManager->GetSampler(tsp0), texture->GetReadOnlyImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);

		std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perPolyDescSets.back(), 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo0, nullptr, nullptr));

		// (a polygon with one volume has the first texture as its second: the general pipelines' shader has both)
		vk::DescriptorImageInfo imageInfo1 = imageInfo0;
		if (textureId1 != (u64)-1)
		{
			Texture *texture1 = reinterpret_cast<Texture *>(textureId1);
			imageInfo1 = vk::DescriptorImageInfo(samplerManager->GetSampler(tsp1), texture1->GetReadOnlyImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);
		}
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perPolyDescSets.back(), 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo1, nullptr, nullptr));
		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
		perPolyDescSetsInFlight[index] = std::move(perPolyDescSets.back());
		perPolyDescSets.pop_back();
	}

	void BindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &perFrameDescSet.get(), 0, nullptr);
		// (a texture's replaces it; until one does, an untextured polygon has this)
		if (noTextureDescSet)
			cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, 1, &noTextureDescSet.get(), 0, nullptr);
	}

	void BindColorInputDescSet(vk::CommandBuffer cmdBuffer, int index)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 2, 1, &colorInputDescSets[index].get(), 0, nullptr);
	}

	void BindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, u64 textureId0, TSP tsp0, u64 textureId1, TSP tsp1)
	{
		auto index = std::make_tuple(textureId0, tsp0.full & SamplerManager::TSP_Mask, textureId1, tsp1.full & SamplerManager::TSP_Mask);
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, 1,
				&perPolyDescSetsInFlight[index].get(), 0, nullptr);
	}

	void Reset()
	{
		for (auto& pair : perPolyDescSetsInFlight)
			perPolyDescSets.emplace_back(std::move(pair.second));
		perPolyDescSetsInFlight.clear();

	}

private:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }

	vk::DescriptorSetLayout perFrameLayout;
	vk::DescriptorSetLayout perPolyLayout;
	vk::DescriptorSetLayout colorInputLayout;
	vk::PipelineLayout pipelineLayout;

	vk::UniqueDescriptorSet perFrameDescSet;
	vk::UniqueDescriptorSet noTextureDescSet;
	vk::ImageView noTextureView;
	std::array<vk::UniqueDescriptorSet, 2> colorInputDescSets;
	std::vector<vk::UniqueDescriptorSet> perPolyDescSets;
	std::map<std::tuple<u64, u32, u64, u32>, vk::UniqueDescriptorSet> perPolyDescSetsInFlight;

	SamplerManager* samplerManager;
};

/* What a draw is made with: its pipeline, and whether that is the general
 * one (pso.h), which is then told the key's bits with the push constants
 * and its dynamic states with SetDynamicState() */
struct OITPsoChoice
{
	VkPipeline pipeline;
	u64 key;
	bool general;
};

/* The per-pixel renderer's pipelines (pso.h). A polygon's is kept under a
 * key of everything it is made of: its shader's bits (pso_key.h), the
 * module and subpass it takes, and its depth, stencil, cull and blend
 * states as they are made. */
class OITPipelineManager : public PsoBuilder
{
public:
	OITPipelineManager() : renderPasses(&ownRenderPasses) {}
	~OITPipelineManager() override { Reset(); }

	/* @samples: SSAA's (1: off), which the shader manager has been given too.
	 * The general pipelines and the fixed ones are made here. */
	virtual void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1)
	{
		this->shaderManager = shaderManager;
		shaderManager->Init();
		if (samples != renderPasses->GetSamples())
		{
			// everything was made for the render passes of the other count
			Reset();
			renderPasses->SetSamples(samples);
		}
		renderPasses->MakeAll();

		if (!perFrameLayout)
		{
			// Descriptor set and pipeline layout
			vk::DescriptorSetLayoutBinding perFrameBindings[] = {
					{ 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex },			// vertex uniforms
					{ 1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment },		// fragment uniforms
					{ 2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// fog texture
					{ 3, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment },		// Tr poly params
					{ 4, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment },		// stencil input attachment
					{ 5, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment },		// depth input attachment
					{ 6, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// palette texture
			};
			perFrameLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(perFrameBindings), perFrameBindings));

			vk::DescriptorSetLayoutBinding colorInputBindings[] = {
					{ 0, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment },		// color input attachment
			};
			colorInputLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(colorInputBindings), colorInputBindings));

			vk::DescriptorSetLayoutBinding perPolyBindings[] = {
					{ 0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// texture 0
					{ 1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// texture 1 (for 2-volume mode)
			};
			perPolyLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(perPolyBindings), perPolyBindings));

			vk::PushConstantRange pushConstant(vk::ShaderStageFlagBits::eFragment, 0, sizeof(OITDescriptorSets::PushConstants));
			vk::DescriptorSetLayout layouts[] = { *perFrameLayout, *perPolyLayout, *colorInputLayout, oitBuffers->GetDescriptorSetLayout() };
			pipelineLayout = GetContext()->GetDevice().createPipelineLayoutUnique(
					vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), ARRAY_SIZE(layouts), layouts, 1, &pushConstant));
			pipelines.Init(4096, this);
			general.Init(1024, this);
		}

		// (made once for the render passes: a new size keeps them)
		if (!prepared)
			Prepare();
	}

	OITPsoChoice GetPipeline(u32 listType, bool autosort, const PolyParam& pp, Pass pass)
	{
		return Choose(Key(listType, autosort, pp, pass));
	}
	// the dynamic states of a general pipeline's draw, after it is bound
	void SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const;

	vk::Pipeline GetModifierVolumePipeline(ModVolMode mode, int cullMode) const
	{
		return vk::Pipeline(fixedSlots[FIXED_MODVOL + ModVolIndex(mode, cullMode)].pipeline);
	}
	vk::Pipeline GetTrModifierVolumePipeline(ModVolMode mode, int cullMode) const
	{
		return vk::Pipeline(fixedSlots[FIXED_TRMODVOL + ModVolIndex(mode, cullMode)].pipeline);
	}
	// @perSample: SSAA's for a render pass that is not the last (OITShaderManager::GetFinalShader())
	vk::Pipeline GetFinalPipeline(bool perSample = false) const
	{
		return vk::Pipeline(fixedSlots[perSample ? FIXED_FINAL_PER_SAMPLE : FIXED_FINAL].pipeline);
	}
	int GetFinalSubpass() const { return renderPasses->GetFinalSubpass(); }
	u32 GetSamples() const { return renderPasses->GetSamples(); }
	vk::Pipeline GetClearPipeline() const
	{
		return vk::Pipeline(fixedSlots[FIXED_CLEAR].pipeline);
	}
	vk::PipelineLayout GetPipelineLayout() const { return *pipelineLayout; }
	vk::DescriptorSetLayout GetPerFrameDSLayout() const { return *perFrameLayout; }
	vk::DescriptorSetLayout GetPerPolyDSLayout() const { return *perPolyLayout; }
	vk::DescriptorSetLayout GetColorInputDSLayout() const { return *colorInputLayout; }

	vk::RenderPass GetRenderPass(bool initial, bool last, bool depthWrites = false, bool resumed = false) { return renderPasses->GetRenderPass(initial, last, depthWrites, resumed); }

	/* Everything made dropped: what the worker has of it first. (Render
	 * passes made again, another count of samples.) */
	void Reset();

	VkPipeline BuildPipeline(u64 key, vk::PipelineCache cache) override;

	/* The key's fields. Bits 0 to 13 are the shader's (pso_key.h); 56 to 59
	 * say what kind of pipeline it is, 60 to 63 are PsoKeyLog's. */
	enum : u64
	{
		KEY_GOURAUD = 14,
		KEY_RATE = 15,		// 2 bits: 0 without SSAA, 1 at each sample, 2 once a pixel
		KEY_PASS = 17,		// 2 bits: Pass
		KEY_SUBPASS = 19,	// 2 bits
		KEY_CULL = 21,		// 2 bits: none, front, back
		KEY_ZWRITE = 23,
		KEY_DEPTH_OP = 24,	// 3 bits: vk::CompareOp
		KEY_STENCIL = 27,	// 2 bits: none, the shadow bit set, cleared
		KEY_BLEND = 29,
		KEY_COLOR_WRITE = 30,
		KEY_SRC = 31,		// 3 bits: the PVR's
		KEY_DST = 34,		// 3 bits
		KEY_KIND = 56,		// 4 bits: Kind
	};
	enum Kind { KIND_POLY, KIND_GENERAL, KIND_FIXED };
	static constexpr u64 MODULE_BITS = (1ull << KEY_GOURAUD) | (3ull << KEY_RATE) | (3ull << KEY_PASS) | (3ull << KEY_SUBPASS);
	static constexpr u64 DEPTH_STENCIL_BITS = (3ull << KEY_CULL) | (1ull << KEY_ZWRITE) | (7ull << KEY_DEPTH_OP) | (3ull << KEY_STENCIL);
	static constexpr u64 BLEND_BITS = (1ull << KEY_BLEND) | (1ull << KEY_COLOR_WRITE) | (7ull << KEY_SRC) | (7ull << KEY_DST);

private:
	u64 Key(u32 listType, bool autosort, const PolyParam& pp, Pass pass) const;
	OITPsoChoice Choose(u64 key);
	u64 GeneralKey(u64 key) const
	{
		u64 fixed = MODULE_BITS;
		const pso_ext::Functions& dynamic = GetContext()->DynamicState();
		if (!dynamic.depthStencil)
			fixed |= DEPTH_STENCIL_BITS;
		if (!dynamic.blend)
			fixed |= BLEND_BITS;
		return (key & fixed) | ((u64)KIND_GENERAL << KEY_KIND);
	}
	void Prepare();
	VkPipeline BuildPoly(u64 key, bool general, vk::PipelineCache cache);
	VkPipeline CreateModVolPipeline(ModVolMode mode, int cullMode, vk::PipelineCache cache);
	VkPipeline CreateTrModVolPipeline(ModVolMode mode, int cullMode, vk::PipelineCache cache);
	VkPipeline CreateFinalPipeline(bool perSample, vk::PipelineCache cache);
	VkPipeline CreateClearPipeline(vk::PipelineCache cache);

	// the fixed pipelines' slots: the modifier volumes' by mode and cull mode, the last subpass's, the lists'
	enum { FIXED_MODVOL = 0, FIXED_TRMODVOL = 12, FIXED_FINAL = 24, FIXED_FINAL_PER_SAMPLE, FIXED_CLEAR, FIXED_COUNT };
	// (Xor to Exclusion; cull modes 0 and 1 are both none)
	static u32 ModVolIndex(ModVolMode mode, int cullMode) { return (u32)mode * 3 + (cullMode < 2 ? 0 : cullMode - 1); }

	vk::PipelineVertexInputStateCreateInfo GetMainVertexInputStateCreateInfo(bool full = true) const
	{
		// Vertex input state
		static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
		{
				{ 0, sizeof(Vertex) },
		};
		static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
		{
				vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, x)),	// pos
				vk::VertexInputAttributeDescription(1, 0, vk::Format::eR8G8B8A8Uint, offsetof(Vertex, col)),	// base color
				vk::VertexInputAttributeDescription(2, 0, vk::Format::eR8G8B8A8Uint, offsetof(Vertex, vtx_spc)), // offset color
				vk::VertexInputAttributeDescription(3, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, u)),		// tex coord
				vk::VertexInputAttributeDescription(4, 0, vk::Format::eR8G8B8A8Uint, offsetof(Vertex, col1)),	// base1 color
				vk::VertexInputAttributeDescription(5, 0, vk::Format::eR8G8B8A8Uint, offsetof(Vertex, spc1)),	// offset1 color
				vk::VertexInputAttributeDescription(6, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, u1)),		// tex1 coord
		};
		static const vk::VertexInputAttributeDescription vertexInputLightAttributeDescriptions[] =
		{
				vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, x)),	// pos
		};
		return vk::PipelineVertexInputStateCreateInfo(
				vk::PipelineVertexInputStateCreateFlags(),
				ARRAY_SIZE(vertexBindingDescriptions),
				vertexBindingDescriptions,
				full ? ARRAY_SIZE(vertexInputAttributeDescriptions) : ARRAY_SIZE(vertexInputLightAttributeDescriptions),
				full ? vertexInputAttributeDescriptions : vertexInputLightAttributeDescriptions);
	}

	PsoTable pipelines;		// the polygons' own
	PsoTable general;		// the general ones
	PsoSlot fixedSlots[FIXED_COUNT] = {};
	bool prepared = false;
	// what the pipelines are made for (one of a kind: all of them work with any)
	vk::RenderPass renderPass;
	u64 lastKey = ~0ull;
	OITPsoChoice lastChoice = {};

	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSetLayout perFrameLayout;
	vk::UniqueDescriptorSetLayout colorInputLayout;
	vk::UniqueDescriptorSetLayout perPolyLayout;
	RenderPasses ownRenderPasses;

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }

	RenderPasses *renderPasses;
	OITShaderManager *shaderManager = nullptr;
	// PsoKeyLog's: the screen's, the textures'
	u32 who = 3;
};

class RttOITPipelineManager : public OITPipelineManager
{
public:
	RttOITPipelineManager() { renderPasses = &rttRenderPasses; who = 4; }
	void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1) override
	{
		this->oitBuffers = oitBuffers;
		// (what was made for the render passes goes before they do)
		Reset();
		rttRenderPasses.Reset();
		OITPipelineManager::Init(shaderManager, oitBuffers, samples);
	}

private:
	RttRenderPasses rttRenderPasses;
	OITBuffers *oitBuffers = nullptr;
};
