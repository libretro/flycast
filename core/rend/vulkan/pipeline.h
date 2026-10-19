/*
 *  Created on: Oct 3, 2019

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
#include "vulkan.h"
#include "shaders.h"
#include "texture.h"
#include "utils.h"
#include "hw/pvr/ta_ctx.h"
#include "vulkan_context.h"
#include "ssaa.h"
#include "pso.h"

class DescriptorSets
{
public:
	void Init(SamplerManager* samplerManager, vk::PipelineLayout pipelineLayout, vk::DescriptorSetLayout perFrameLayout, vk::DescriptorSetLayout perPolyLayout)
	{
		this->samplerManager = samplerManager;
		this->pipelineLayout = pipelineLayout;
		this->perFrameLayout = perFrameLayout;
		this->perPolyLayout = perPolyLayout;

	}
	// (the vertex uniforms are a buffer of their own: BaseDrawer::WriteVertexUniforms())
	void UpdateUniforms(vk::Buffer vertexUniformBuffer, vk::Buffer buffer, u32 fragmentUniformOffset, vk::ImageView fogImageView, vk::ImageView paletteImageView)
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
			/* What an untextured polygon has as its texture: the general
			 * pipelines read one only for a textured polygon, but have it in
			 * their shader (pso.h) */
			if (!noTextureDescSet)
				noTextureDescSet = std::move(GetContext()->GetDevice().allocateDescriptorSetsUnique(
						vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), 1, &perPolyLayout)).front());
			if (noTextureView != fogImageView)
			{
				noTextureView = fogImageView;
				writeDescriptorSets.push_back(vk::WriteDescriptorSet(*noTextureDescSet, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
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
			writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perFrameDescSet, 3, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));
		}
		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
	}

	void SetTexture(u64 textureId, TSP tsp)
	{
		auto& inFlight = perPolyDescSetsInFlight;
		std::pair<u64, u32> index = std::make_pair(textureId, tsp.full & SamplerManager::TSP_Mask);
		if (inFlight.find(index) != inFlight.end())
			return;

		if (perPolyDescSets.empty())
		{
			std::vector<vk::DescriptorSetLayout> layouts(10, perPolyLayout);
			perPolyDescSets = GetContext()->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(GetContext()->GetDescriptorPool(), layouts.size(), &layouts[0]));
		}
		Texture *texture = reinterpret_cast<Texture *>(textureId);
		vk::DescriptorImageInfo imageInfo(samplerManager->GetSampler(tsp), texture->GetReadOnlyImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);

		std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perPolyDescSets.back(), 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr));

		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
		inFlight[index] = std::move(perPolyDescSets.back());
		perPolyDescSets.pop_back();
	}

	void BindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &perFrameDescSet.get(), 0, nullptr);
		// (a texture's replaces it; until one does, an untextured polygon has this)
		if (noTextureDescSet)
			cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, 1, &noTextureDescSet.get(), 0, nullptr);
	}

	void BindPerPolyDescriptorSets(vk::CommandBuffer cmdBuffer, u64 textureId, TSP tsp)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 1, 1,
				&perPolyDescSetsInFlight[std::make_pair(textureId, tsp.full & SamplerManager::TSP_Mask)].get(), 0, nullptr);
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
	vk::PipelineLayout pipelineLayout;

	vk::UniqueDescriptorSet perFrameDescSet;
	vk::UniqueDescriptorSet noTextureDescSet;
	vk::ImageView noTextureView;
	std::vector<vk::UniqueDescriptorSet> perPolyDescSets;
	std::map<std::pair<u64, u32>, vk::UniqueDescriptorSet> perPolyDescSetsInFlight;

	SamplerManager* samplerManager;
};

/* What a draw is made with: its pipeline, and whether that is the general
 * one (pso.h), which is then told the key's bits with the push constants
 * and its dynamic states with SetDynamicState() */
struct PsoChoice
{
	VkPipeline pipeline;
	u64 key;
	bool general;
};

/* The per-triangle renderer's pipelines (pso.h). A polygon's is kept under
 * a key of everything it is made of: its shader's bits (pso_key.h), the
 * module it takes, and its depth, stencil, cull, topology and blend states
 * as they are made, so two polygons that differ in nothing a pipeline has
 * share one. */
class PipelineManager : public PsoBuilder
{
public:
	~PipelineManager() override { Reset(); }

	/* @samples: of the render pass's polygon subpass (SSAA; 1 without).
	 * The general pipelines and the modifier volumes' are made here. */
	void Init(ShaderManager *shaderManager, vk::RenderPass renderPass, u32 samples = 1)
	{
		this->shaderManager = shaderManager;
		shaderManager->Init();

		if (!perFrameLayout)
		{
			// Descriptor set and pipeline layout
			vk::DescriptorSetLayoutBinding perFrameBindings[] = {
					{ 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex },			// vertex uniforms
					{ 1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment },		// fragment uniforms
					{ 2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// fog texture
					{ 3, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// palette texture
			};
			vk::DescriptorSetLayoutBinding perPolyBindings[] = {
					{ 0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment },// texture
			};
			perFrameLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(perFrameBindings), perFrameBindings));
			perPolyLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(perPolyBindings), perPolyBindings));
			vk::DescriptorSetLayout layouts[] = { *perFrameLayout, *perPolyLayout };
			// (six floats, then the general pipelines' key: pso_key.h)
			vk::PushConstantRange pushConstant(vk::ShaderStageFlagBits::eFragment, 0, TRI_PUSH_KEY_OFFSET + 4);
			pipelineLayout = GetContext()->GetDevice().createPipelineLayoutUnique(
					vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), ARRAY_SIZE(layouts), layouts, 1, &pushConstant));
			pipelines.Init(4096, this);
			general.Init(1024, this);
		}

		if (this->renderPass != renderPass || this->samples != samples)
		{
			Reset();
			this->renderPass = renderPass;
			this->samples = samples;
			Prepare();
		}
	}

	// What a polygon is being drawn for: itself, or a second time for the part
	// of it that is in a modifier volume (see rend/shadows.h), in which case
	// which of two polygons at the same depth is left showing matters.
	enum class ShadowPass { None, LaterWins, EarlierWins };

	PsoChoice GetPipeline(u32 listType, bool sortTriangles, const PolyParam& pp, ShadowPass shadowPass = ShadowPass::None)
	{
		return Choose(Key(listType, sortTriangles, pp, shadowPass));
	}
	// the dynamic states of a general pipeline's draw, after it is bound
	void SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const;

	vk::Pipeline GetModifierVolumePipeline(ModVolMode mode, int cullMode) const
	{
		return vk::Pipeline(modVolSlots[ModVolIndex(mode, cullMode)].pipeline);
	}

	/* The depth prepass of a sorted translucent list (rend/sorter.h): the
	 * pipeline a polygon is drawn with there. They are all made with the
	 * render pass, none in the middle of a frame: textured or not, flat or
	 * Gouraud, each culling, and each SSAA rate. */
	vk::Pipeline GetPrepassPipeline(const PolyParam& pp) const
	{
		const bool sampleRate = samples > 1 && !SsaaSingleRate(pp);
		return vk::Pipeline(prepassSlots[pp.pcw.Texture][pp.pcw.Gouraud][pp.isp.CullMode == 3 ? 2 : pp.isp.CullMode == 2 ? 1 : 0][sampleRate].pipeline);
	}

	/* Everything made dropped: what the worker has of it first. (A new
	 * render pass or count of samples.) */
	void Reset();

	vk::PipelineLayout GetPipelineLayout() const { return *pipelineLayout; }
	vk::DescriptorSetLayout GetPerFrameDSLayout() const { return *perFrameLayout; }
	vk::DescriptorSetLayout GetPerPolyDSLayout() const { return *perPolyLayout; }
	vk::RenderPass GetRenderPass() const { return renderPass; }

	VkPipeline BuildPipeline(u64 key, vk::PipelineCache cache) override;

	/* The key's fields. Bits 0 to 14 are the shader's (pso_key.h); 56 to 59
	 * say what kind of pipeline it is, 60 to 63 are PsoKeyLog's. */
	enum : u64
	{
		KEY_GOURAUD = 15,
		KEY_RATE = 16,		// 2 bits: 0 without SSAA, 1 at each sample, 2 once a pixel
		KEY_CULL = 18,		// 2 bits: none, front, back
		KEY_LIST = 20,		// a triangle list, not strips
		KEY_ZWRITE = 21,
		KEY_DEPTH_OP = 22,	// 3 bits: vk::CompareOp
		KEY_STENCIL = 25,	// 3 bits: Stencil
		KEY_BLEND = 28,
		KEY_SRC = 29,		// 3 bits: the PVR's
		KEY_DST = 32,		// 3 bits
		KEY_KIND = 56,		// 4 bits: Kind
	};
	enum Stencil { STENCIL_NONE, STENCIL_SHADOW_SET, STENCIL_SHADOW_CLEAR, STENCIL_LATER_WINS, STENCIL_EARLIER_WINS };
	enum Kind { KIND_POLY, KIND_GENERAL, KIND_MODVOL, KIND_PREPASS };
	static constexpr u64 MODULE_BITS = (1ull << KEY_GOURAUD) | (3ull << KEY_RATE);
	static constexpr u64 DEPTH_STENCIL_BITS = (3ull << KEY_CULL) | (1ull << KEY_LIST) | (1ull << KEY_ZWRITE)
			| (7ull << KEY_DEPTH_OP) | (7ull << KEY_STENCIL);
	static constexpr u64 BLEND_BITS = (1ull << KEY_BLEND) | (7ull << KEY_SRC) | (7ull << KEY_DST);

private:
	u64 Key(u32 listType, bool sortTriangles, const PolyParam& pp, ShadowPass shadowPass) const;
	PsoChoice Choose(u64 key);
	// the key of the general pipeline that draws for @key
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
	VkPipeline BuildModVol(ModVolMode mode, int cullMode, vk::PipelineCache cache);
	VkPipeline BuildPrepass(int texture, int gouraud, int cull, int rate, vk::PipelineCache cache);
	// cull modes 0 and 1 are both none
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
	// the modifier volumes': by mode and cull mode (ModVolIndex()), made with the general ones
	PsoSlot modVolSlots[(int)ModVolMode::Depth * 3 + 3] = {};
	// the depth prepass's: [textured][Gouraud][no culling, front, back][SSAA: shaded at each sample]
	PsoSlot prepassSlots[2][2][3][2] = {};
	u64 lastKey = ~0ull;
	PsoChoice lastChoice = {};

	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSetLayout perFrameLayout;
	vk::UniqueDescriptorSetLayout perPolyLayout;

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }

	vk::RenderPass renderPass;
	u32 samples = 1;
	ShaderManager *shaderManager = nullptr;
	// PsoKeyLog's: the screen's, the textures'
	u32 who = 1;
};

class RttPipelineManager : public PipelineManager
{
public:
	RttPipelineManager() { who = 2; }

	/* @samples: SSAA's. Then the polygons are drawn into attachments of
	 * that many samples, and a second subpass resolves them into the
	 * texture: see MakeSsaaRenderPass(). */
	void Init(ShaderManager *shaderManager, u32 samples = 1)
	{
		if (samples > 1)
		{
			rttRenderPass = MakeSsaaRenderPass(samples);
			PipelineManager::Init(shaderManager, *rttRenderPass, samples);
			return;
		}
		// RTT render pass
	    vk::AttachmentDescription attachmentDescriptions[] = {
	    		vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Unorm, vk::SampleCountFlagBits::e1,
	    				vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eColorAttachmentOptimal,
						vk::ImageLayout::eShaderReadOnlyOptimal),
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetDepthFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal),
	    };
	    vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	    vk::AttachmentReference depthReference(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
	    vk::SubpassDescription subpass(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, 0, nullptr, 1, &colorReference, nullptr, &depthReference);
	    vk::SubpassDependency dependencies[] {
	    	vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
	    			vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
					vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead),
			// one depth attachment for every render to a texture: the last one done with it before this one clears it
			vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0,
					vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
					vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
					vk::AccessFlagBits::eDepthStencilAttachmentWrite,
					vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite),
	    };

	    rttRenderPass = GetContext()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(), 2, attachmentDescriptions,
	    		1, &subpass, ARRAY_SIZE(dependencies), dependencies));

		PipelineManager::Init(shaderManager, *rttRenderPass);
	}

private:
	vk::UniqueRenderPass MakeSsaaRenderPass(u32 samples);

	vk::UniqueRenderPass rttRenderPass;
};

/* The render pass of SSAA's frames, the screen's and the textures':
 * attachment 0 the picture (@colorFormat, a single sample), 1 and 2 the
 * colour and depth attachments of @samples the polygons are drawn into.
 * Subpass 0 draws, subpass 1 resolves 1 into 0 (SsaaResolve). Only the
 * picture is stored; it is all written by the resolve, so not loaded. */
vk::UniqueRenderPass MakeSsaaRenderPass(vk::Format colorFormat, u32 samples, vk::ImageLayout pictureLayout);
