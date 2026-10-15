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
#include "hw/pvr/ta_ctx.h"

class OITDescriptorSets
{
public:
	// std140 alignment required
	struct VertexShaderUniforms
	{
		float normal_matrix[16];
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
	};

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
	void UpdateUniforms(vk::Buffer buffer, u32 vertexUniformOffset, u32 fragmentUniformOffset, vk::ImageView fogImageView,
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
		bufferInfos.push_back(vk::DescriptorBufferInfo(buffer, vertexUniformOffset, sizeof(VertexShaderUniforms)));
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

		if (textureId1 != (u64)-1)
		{
			Texture *texture1 = reinterpret_cast<Texture *>(textureId1);
			vk::DescriptorImageInfo imageInfo1(samplerManager->GetSampler(tsp1), texture1->GetReadOnlyImageView(), vk::ImageLayout::eShaderReadOnlyOptimal);

			writeDescriptorSets.push_back(vk::WriteDescriptorSet(*perPolyDescSets.back(), 1, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo1, nullptr, nullptr));
		}
		GetContext()->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
		perPolyDescSetsInFlight[index] = std::move(perPolyDescSets.back());
		perPolyDescSets.pop_back();
	}

	void BindPerFrameDescriptorSets(vk::CommandBuffer cmdBuffer)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, 1, &perFrameDescSet.get(), 0, nullptr);
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
	std::array<vk::UniqueDescriptorSet, 2> colorInputDescSets;
	std::vector<vk::UniqueDescriptorSet> perPolyDescSets;
	std::map<std::tuple<u64, u32, u64, u32>, vk::UniqueDescriptorSet> perPolyDescSetsInFlight;

	SamplerManager* samplerManager;
};

class OITPipelineManager
{
public:
	OITPipelineManager() : renderPasses(&ownRenderPasses) {}
	virtual ~OITPipelineManager() = default;

	/* @samples: SSAA's (1: off), which the shader manager has been given too */
	virtual void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1)
	{
		this->shaderManager = shaderManager;
		shaderManager->Init();
		if (samples != renderPasses->GetSamples())
		{
			// everything was made for the render passes of the other count
			renderPasses->SetSamples(samples);
			trModVolPipelines.clear();
			finalPipeline.reset();
			finalPerSamplePipeline.reset();
			clearPipeline.reset();
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
		}

		pipelines.clear();
		modVolPipelines.clear();
		// (SSAA's are made now, not when first drawn)
		if (samples > 1)
		{
			GetFinalPipeline(false);
			GetFinalPipeline(true);
			GetClearPipeline();
		}
	}

	vk::Pipeline GetPipeline(u32 listType, bool autosort, const PolyParam& pp, Pass pass)
	{
		/* SSAA: shaded at each sample, but for what is drawn at a single
		 * rate; and a translucent fragment put in the lists once a pixel,
		 * with the samples it covers, so the lists are no longer for it */
		const u32 samples = renderPasses->GetSamples();
		const bool sampleRate = samples > 1 && pass != Pass::OIT && !SsaaSingleRate(pp);
		const u32 pipehash = hash(listType, autosort, &pp, pass) | ((u32)sampleRate << 30);
		const auto &pipeline = pipelines.find(pipehash);
		if (pipeline != pipelines.end())
			return pipeline->second.get();

		CreatePipeline(listType, autosort, pp, pass, sampleRate);
		// (the other rate is made with it: see the per-triangle renderer's)
		if (samples > 1 && pass != Pass::OIT && pipelines.find(pipehash ^ (1u << 30)) == pipelines.end())
			CreatePipeline(listType, autosort, pp, pass, !sampleRate);

		return *pipelines[pipehash];
	}

	vk::Pipeline GetModifierVolumePipeline(ModVolMode mode, int cullMode)
	{
		u32 pipehash = hash(mode, cullMode);
		const auto &pipeline = modVolPipelines.find(pipehash);
		if (pipeline != modVolPipelines.end())
			return pipeline->second.get();
		CreateModVolPipeline(mode, cullMode);

		return *modVolPipelines[pipehash];
	}
	vk::Pipeline GetTrModifierVolumePipeline(ModVolMode mode, int cullMode)
	{
		u32 pipehash = hash(mode, cullMode);
		const auto &pipeline = trModVolPipelines.find(pipehash);
		if (pipeline != trModVolPipelines.end())
			return pipeline->second.get();
		CreateTrModVolPipeline(mode, cullMode);

		return *trModVolPipelines[pipehash];
	}
	// @perSample: SSAA's for a render pass that is not the last (OITShaderManager::GetFinalShader())
	vk::Pipeline GetFinalPipeline(bool perSample = false)
	{
		vk::UniquePipeline& pipeline = perSample ? finalPerSamplePipeline : finalPipeline;
		if (!pipeline)
			CreateFinalPipeline(perSample);
		return *pipeline;
	}
	int GetFinalSubpass() const { return renderPasses->GetFinalSubpass(); }
	u32 GetSamples() const { return renderPasses->GetSamples(); }
	vk::Pipeline GetClearPipeline()
	{
		if (!clearPipeline)
			CreateClearPipeline();
		return *clearPipeline;
	}
	vk::PipelineLayout GetPipelineLayout() const { return *pipelineLayout; }
	vk::DescriptorSetLayout GetPerFrameDSLayout() const { return *perFrameLayout; }
	vk::DescriptorSetLayout GetPerPolyDSLayout() const { return *perPolyLayout; }
	vk::DescriptorSetLayout GetColorInputDSLayout() const { return *colorInputLayout; }

	vk::RenderPass GetRenderPass(bool initial, bool last, bool depthWrites = false, bool resumed = false) { return renderPasses->GetRenderPass(initial, last, depthWrites, resumed); }

private:
	void CreateModVolPipeline(ModVolMode mode, int cullMode);
	void CreateTrModVolPipeline(ModVolMode mode, int cullMode);

	u32 hash(u32 listType, bool autosort, const PolyParam *pp, Pass pass) const
	{
		u32 hash = pp->pcw.Gouraud | (pp->pcw.Offset << 1) | (pp->pcw.Texture << 2) | (pp->pcw.Shadow << 3)
			| (((pp->tileclip >> 28) == 3) << 4);
		hash |= ((listType >> 1) << 5);
		if (pp->tcw1.full != (u32)-1 || pp->tsp1.full != (u32)-1)
		{
			// Two-volume mode
			hash |= (1 << 31) | (pp->tsp.ColorClamp << 11);
		}
		else
      {
         bool ignoreTexAlpha = pp->tsp.IgnoreTexA || pp->tcw.PixelFmt == Pixel565;
         hash |= (pp->tsp.ShadInstr << 7) | (ignoreTexAlpha << 9) | (pp->tsp.UseAlpha << 10)
            | (pp->tsp.ColorClamp << 11) | (pp->tsp.FogCtrl << 12)
            | (pp->tsp.SrcInstr << 14) | (pp->tsp.DstInstr << 17);
      }
		hash |= (pp->isp.ZWriteDis << 20) | (pp->isp.CullMode << 21) | ((autosort ? 6 : pp->isp.DepthMode) << 23);
		hash |= ((u32)BaseTextureCacheData::IsGpuHandledPaletted(pp->tsp, pp->tcw) << 26) | ((u32)pass << 27);
		// a bump map has a shader of its own: see the per-triangle renderer's hash
		hash |= (u32)(pp->tcw.PixelFmt == PixelBumpMap) << 29;

		return hash;
	}
	u32 hash(ModVolMode mode, int cullMode) const
	{
		return ((int)mode << 2) | cullMode;
	}

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

	void CreatePipeline(u32 listType, bool autosort, const PolyParam& pp, Pass pass, bool sampleRate);
	void CreateFinalPipeline(bool perSample);
	void CreateClearPipeline();

	std::map<u32, vk::UniquePipeline> pipelines;
	std::map<u32, vk::UniquePipeline> modVolPipelines;
	std::map<u32, vk::UniquePipeline> trModVolPipelines;
	vk::UniquePipeline finalPipeline;
	vk::UniquePipeline finalPerSamplePipeline;
	vk::UniquePipeline clearPipeline;

	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSetLayout perFrameLayout;
	vk::UniqueDescriptorSetLayout colorInputLayout;
	vk::UniqueDescriptorSetLayout perPolyLayout;
	RenderPasses ownRenderPasses;

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }

	RenderPasses *renderPasses;
	OITShaderManager *shaderManager = nullptr;
};

class RttOITPipelineManager : public OITPipelineManager
{
public:
	RttOITPipelineManager() { renderPasses = &rttRenderPasses; }
	void Init(OITShaderManager *shaderManager, OITBuffers *oitBuffers, u32 samples = 1) override
	{
		this->oitBuffers = oitBuffers;
		rttRenderPasses.Reset();
		OITPipelineManager::Init(shaderManager, oitBuffers, samples);
	}

private:
	RttRenderPasses rttRenderPasses;
	OITBuffers *oitBuffers = nullptr;
};
