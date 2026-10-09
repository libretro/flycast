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
#include "quad.h"
#include "vulkan_context.h"
#include "commandpool.h"

static VulkanContext *GetContext()
{
	return VulkanContext::Instance();
}

vk::PipelineVertexInputStateCreateInfo GetQuadInputStateCreateInfo(bool uv)
{
	// Vertex input state
	static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
	{
			{ 0, sizeof(QuadVertex) },
	};
	static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
	{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(QuadVertex, pos)),	// pos
			vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32Sfloat, offsetof(QuadVertex, uv)),		// tex coord
	};
	return vk::PipelineVertexInputStateCreateInfo(
			vk::PipelineVertexInputStateCreateFlags(),
			ARRAY_SIZE(vertexBindingDescriptions),
			vertexBindingDescriptions,
			ARRAY_SIZE(vertexInputAttributeDescriptions) - (uv ? 0 : 1),
			vertexInputAttributeDescriptions);
}

void QuadPipeline::CreatePipeline(bool alpha)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetQuadInputStateCreateInfo(true);

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(), vk::PrimitiveTopology::eTriangleStrip);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo;
	pipelineRasterizationStateCreateInfo.lineWidth = 1.0;
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
			true,								// blendEnable
			alpha ? vk::BlendFactor::eSrcAlpha : vk::BlendFactor::eConstantAlpha,	// srcColorBlendFactor
			alpha ? vk::BlendFactor::eOneMinusSrcAlpha : vk::BlendFactor::eOneMinusConstantAlpha, // dstColorBlendFactor
			vk::BlendOp::eAdd,					// colorBlendOp
			alpha ? vk::BlendFactor::eSrcAlpha : vk::BlendFactor::eConstantAlpha,	// srcAlphaBlendFactor
					alpha ? vk::BlendFactor::eOneMinusSrcAlpha : vk::BlendFactor::eOneMinusConstantAlpha, // dstAlphaBlendFactor
			vk::BlendOp::eAdd,					// alphaBlendOp
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
						| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
	);
	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  1,                                          // attachmentCount
	  &pipelineColorBlendAttachmentState,         // pAttachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eBlendConstants };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), ARRAY_SIZE(dynamicStates),
			dynamicStates);

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, shaderManager->GetQuadVertexShader(), "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, shaderManager->GetQuadFragmentShader(), "main" },
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo
	(
	  vk::PipelineCreateFlags(),                  // flags
	  2,                                          // stageCount
	  stages,                                     // pStages
	  &pipelineVertexInputStateCreateInfo,        // pVertexInputState
	  &pipelineInputAssemblyStateCreateInfo,      // pInputAssemblyState
	  nullptr,                                    // pTessellationState
	  &pipelineViewportStateCreateInfo,           // pViewportState
	  &pipelineRasterizationStateCreateInfo,      // pRasterizationState
	  &pipelineMultisampleStateCreateInfo,        // pMultisampleState
	  &pipelineDepthStencilStateCreateInfo,       // pDepthStencilState
	  &pipelineColorBlendStateCreateInfo,         // pColorBlendState
	  &pipelineDynamicStateCreateInfo,            // pDynamicState
	  *pipelineLayout,                            // layout
	  renderPass,                                 // renderPass
	  subpass                                     // subpass
	);

	(alpha ? alphaPipeline : pipeline) = GetContext()->GetDevice().createGraphicsPipelineUnique(GetContext()->GetPipelineCache(), graphicsPipelineCreateInfo);
}

void QuadPipeline::Init(ShaderManager *shaderManager, vk::RenderPass renderPass, int subpass)
{
	this->shaderManager = shaderManager;

	if (!pipelineLayout)
	{
		vk::DescriptorSetLayoutBinding bindings[] = {
				{ 0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment }, // texture
		};
		descSetLayout = GetContext()->GetDevice().createDescriptorSetLayoutUnique(
				vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(bindings), bindings));
		pipelineLayout = GetContext()->GetDevice().createPipelineLayoutUnique(
				vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &descSetLayout.get()));
	}
	if (!linearSampler)
	{
		/* What is drawn with this one is a whole picture at another size:
		 * the screen as it was before the renderer changed size, or before
		 * the frontend took the context away, under a render that covers
		 * part of it. Past its edge there is nothing: the edge is repeated.
		 * With a black border there instead, a picture made larger had its
		 * outermost pixels mixed with black - a dark line around the screen
		 * that stayed for as long as the game did not draw there. */
		linearSampler = GetContext()->GetDevice().createSamplerUnique(
				vk::SamplerCreateInfo(vk::SamplerCreateFlags(),
						vk::Filter::eLinear, vk::Filter::eLinear,
						vk::SamplerMipmapMode::eLinear,
						vk::SamplerAddressMode::eClampToEdge,
						vk::SamplerAddressMode::eClampToEdge,
						vk::SamplerAddressMode::eClampToEdge, 0.0f, false,
						16.0f, false, vk::CompareOp::eNever, 0.0f, 0.0f,
						vk::BorderColor::eFloatOpaqueBlack));
	}
	if (!nearestSampler)
	{
		nearestSampler = GetContext()->GetDevice().createSamplerUnique(
				vk::SamplerCreateInfo(vk::SamplerCreateFlags(),
						vk::Filter::eNearest, vk::Filter::eNearest,
						vk::SamplerMipmapMode::eNearest,
						vk::SamplerAddressMode::eClampToBorder,
						vk::SamplerAddressMode::eClampToBorder,
						vk::SamplerAddressMode::eClampToBorder, 0.0f, false,
						16.0f, false, vk::CompareOp::eNever, 0.0f, 0.0f,
						vk::BorderColor::eFloatOpaqueBlack));
	}
	if (this->renderPass != renderPass || this->subpass != subpass)
	{
		this->renderPass = renderPass;
		this->subpass = subpass;
		pipeline.reset();
	}
}

void QuadDrawer::Init(QuadPipeline *pipeline)
{
	this->pipeline = pipeline;
	buffer = std::unique_ptr<QuadBuffer>(new QuadBuffer());
	slots.clear();
	slots.resize(VulkanContext::Instance()->GetSwapChainSize());
}

void QuadDrawer::Draw(vk::CommandBuffer commandBuffer, vk::ImageView imageView, QuadVertex vertices[], bool nearestFilter, CommandPool *commandPool)
{
	VulkanContext *context = GetContext();
	Slot& slot = slots[context->GetCurrentImageIndex()];
	const vk::Sampler sampler = nearestFilter ? pipeline->GetNearestSampler() : pipeline->GetLinearSampler();

	/* The set says which image to draw and how to sample it, and that is the
	 * same from one frame to the next, so it is written when it changes and
	 * not every time. Writing it every time was also not allowed: the frame
	 * before can still be on its way through the GPU using this very set -
	 * the frontend's frame index, which picks the set, does not move when the
	 * game renders twice in one run - and a set in use must not be written.
	 * When it does change, the old set is left as it is for the frames that
	 * use it and a new one takes its place.
	 *
	 * The old one goes to the command pool, which destroys what it is given
	 * when it comes round to this frame's place again - having waited, on
	 * the way, for the fence of every frame submitted before then. (It was
	 * kept here for a number of draws instead, and a number of draws says
	 * nothing about what the GPU has finished: a game that renders several
	 * times to one of the frontend's frames gets through them at once.) */
	if (!slot.set || slot.view != imageView || slot.sampler != sampler)
	{
		if (slot.set)
			commandPool->DeferDelete(std::unique_ptr<vk::UniqueDescriptorSet>(
					new vk::UniqueDescriptorSet(std::move(slot.set))));
		vk::DescriptorSetLayout layout = pipeline->GetDescSetLayout();
		slot.set = std::move(context->GetDevice().allocateDescriptorSetsUnique(
				vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), 1, &layout)).front());
		slot.view = imageView;
		slot.sampler = sampler;
		vk::DescriptorImageInfo imageInfo(sampler, imageView, vk::ImageLayout::eShaderReadOnlyOptimal);
		vk::WriteDescriptorSet write(*slot.set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo, nullptr, nullptr);
		context->GetDevice().updateDescriptorSets(1, &write, 0, nullptr);
	}
	commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->GetPipelineLayout(), 0, 1, &slot.set.get(), 0, nullptr);

	buffer->Update(vertices);
	buffer->Bind(commandBuffer);
	buffer->Draw(commandBuffer);
}
