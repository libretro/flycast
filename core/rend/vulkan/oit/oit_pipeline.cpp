#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include "oit_pipeline.h"
#include "../quad.h"

static vk::CullModeFlags CullMode(u32 cull)
{
	return cull == 2 ? vk::CullModeFlagBits::eBack : cull == 1 ? vk::CullModeFlagBits::eFront : vk::CullModeFlagBits::eNone;
}

// a key's stencil state: none, or the shadow bit set or cleared
static vk::StencilOpState StencilState(u32 stencil)
{
	if (stencil == 1)
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0x80);
	if (stencil == 2)
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0);
	return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::CompareOp::eNever);
}

u64 OITPipelineManager::Key(u32 listType, bool autosort, const PolyParam& pp, Pass pass) const
{
	// the shader
	OITShaderManager::FragmentShaderParams params = {};
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = pp.tcw.PixelFmt == PixelBumpMap;
	/* By the polygon alone. The clamping limits were looked at too, to leave
	 * the clamp out of the shader while they clamp nothing - but a pipeline
	 * is made once and kept under a key that has only the polygon's bit in
	 * it, and the limits change: a polygon first drawn while they clamped
	 * nothing was never clamped after. (Upstream: Red Dog.) */
	params.clamping = pp.tsp.ColorClamp;
	params.insideClipTest = (pp.tileclip >> 28) == 3;
	params.fog = pp.tsp.FogCtrl;
	params.ignoreTexAlpha = pp.tsp.IgnoreTexA || pp.tcw.PixelFmt == Pixel565;
	params.offset = pp.pcw.Offset;
	params.shaderInstr = pp.tsp.ShadInstr;
	params.texture = pp.pcw.Texture;
	params.useAlpha = pp.tsp.UseAlpha;
	params.twoVolume = pp.tsp1.full != (u32)-1 || pp.tcw1.full != (u32)-1;
	params.palette = BaseTextureCacheData::IsGpuHandledPaletted(pp.tsp, pp.tcw);
	u64 key = (u32)params.key() | ((u64)pp.pcw.Gouraud << KEY_GOURAUD) | ((u64)pass << KEY_PASS);
	/* SSAA: shaded at each sample, but for what is drawn at a single
	 * rate; and a translucent fragment put in the lists once a pixel,
	 * with the samples it covers, so the lists are no longer for it */
	if (renderPasses->GetSamples() > 1)
		key |= (u64)(pass != Pass::OIT && !SsaaSingleRate(pp) ? 1 : 2) << KEY_RATE;
	key |= (u64)(pass == Pass::Depth ? (listType == ListType_Translucent ? 2 : 0) : 1) << KEY_SUBPASS;

	// the fixed functions
	key |= (u64)(pp.isp.CullMode == 3 ? 2 : pp.isp.CullMode == 2 ? 1 : 0) << KEY_CULL;
	vk::CompareOp depthOp;
	if (pass == Pass::Color && !pp.isp.ZWriteDis && listType != ListType_Translucent)
		depthOp = vk::CompareOp::eEqual;
	else if (listType == ListType_Punch_Through || autosort)
		depthOp = vk::CompareOp::eGreaterOrEqual;
	else
		depthOp = depthOps[pp.isp.DepthMode];
	bool depthWriteEnable;
	/* Depth is written in the depth pass. In the colour pass the depth buffer
	 * is read-only - it is an input to the shaders there - and depth writes
	 * are not allowed. For opaque and punch-through polygons there is nothing
	 * to write anyway: they are drawn where their depth is the one the depth
	 * pass left, so a write would put the same value back.
	 *
	 * Translucent polygons that are not sorted are the exception. They are
	 * drawn in the colour pass only, in order, each tested against the depth
	 * the ones before it wrote, so they still write. So does everything on
	 * Intel, which is the driver bug workaround this condition came with. */
	const bool colourPassWrites = pass == Pass::Color
			&& (GetContext()->GetVendorID() == VENDOR_INTEL
				|| (!autosort && listType == ListType_Translucent));
	if (pass != Pass::Depth && !colourPassWrites)
		depthWriteEnable = false;
	// Z Write Disable seems to be ignored for punch-through.
	// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
	else if (listType == ListType_Punch_Through)
		depthWriteEnable = true;
	else
		depthWriteEnable = !pp.isp.ZWriteDis;
	const bool shadowed = pass == Pass::Depth && (listType == ListType_Opaque || listType == ListType_Punch_Through);
	key |= ((u64)depthWriteEnable << KEY_ZWRITE) | ((u64)depthOp << KEY_DEPTH_OP)
			| ((u64)(shadowed ? (pp.pcw.Shadow != 0 ? 1 : 2) : 0) << KEY_STENCIL);
	// Apparently punch-through polys support blending, or at least some combinations
	if (listType == ListType_Punch_Through || pass == Pass::Color)
		key |= (1ull << KEY_BLEND) | (1ull << KEY_COLOR_WRITE) | ((u64)pp.tsp.SrcInstr << KEY_SRC) | ((u64)pp.tsp.DstInstr << KEY_DST);

	return key;
}

OITPsoChoice OITPipelineManager::Choose(u64 key)
{
	if (key == lastKey)
		return lastChoice;
	VkPsoWorker& worker = VkPsoWorker::Instance();
	PsoSlot *slot = pipelines.Find(key);
	if (slot == nullptr && worker.Running() && !PsoTestGeneral)
	{
		// its own pipeline, asked for now; drawn with the general one until it is made
		slot = pipelines.Insert(key);
		if (slot != nullptr)
		{
			worker.Request(slot);
			GetContext()->PsoKeys().Record(who, key);
		}
	}
	if (PsoReady(slot))
	{
		lastKey = key;
		lastChoice = { slot->pipeline, key, false };
		return lastChoice;
	}
	const u64 generalKey = GeneralKey(key);
	PsoSlot *slotGeneral = general.Find(generalKey);
	if (slotGeneral == nullptr)
	{
		/* A device that cannot set this polygon's depth, stencil or blend
		 * states with the draw, and a general pipeline for them not made
		 * when the renderer started: made now, the draw waiting for it */
		slotGeneral = general.Insert(generalKey);
		if (slotGeneral != nullptr)
			VkPsoWorker::BuildSync(slotGeneral, GetContext()->GetPipelineCache());
	}
	if (PsoReady(slotGeneral))
		return { slotGeneral->pipeline, key, true };
	return { VK_NULL_HANDLE, key, false };
}

void OITPipelineManager::SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const
{
	const pso_ext::Functions& f = GetContext()->DynamicState();
	const VkCommandBuffer cmd = (VkCommandBuffer)cmdBuffer;
	if (f.depthStencil)
	{
		f.CmdSetCullMode(cmd, (VkCullModeFlags)CullMode((key >> KEY_CULL) & 3));
		f.CmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		f.CmdSetDepthTestEnable(cmd, VK_TRUE);
		f.CmdSetDepthWriteEnable(cmd, (VkBool32)((key >> KEY_ZWRITE) & 1));
		f.CmdSetDepthCompareOp(cmd, (VkCompareOp)((key >> KEY_DEPTH_OP) & 7));
		const u32 stencil = (key >> KEY_STENCIL) & 3;
		const vk::StencilOpState state = StencilState(stencil);
		f.CmdSetStencilTestEnable(cmd, stencil != 0);
		f.CmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)state.failOp, (VkStencilOp)state.passOp,
				(VkStencilOp)state.depthFailOp, (VkCompareOp)state.compareOp);
		cmdBuffer.setStencilCompareMask(vk::StencilFaceFlagBits::eVkStencilFrontAndBack, state.compareMask);
		cmdBuffer.setStencilWriteMask(vk::StencilFaceFlagBits::eVkStencilFrontAndBack, state.writeMask);
		cmdBuffer.setStencilReference(vk::StencilFaceFlagBits::eVkStencilFrontAndBack, state.reference);
	}
	if (f.blend)
	{
		const VkBool32 enable = (VkBool32)((key >> KEY_BLEND) & 1);
		f.CmdSetColorBlendEnable(cmd, 0, 1, &enable);
		pso_ext::ColorBlendEquation equation = {};
		equation.colorBlendOp = VK_BLEND_OP_ADD;
		equation.alphaBlendOp = VK_BLEND_OP_ADD;
		if (enable)
		{
			equation.srcColorBlendFactor = equation.srcAlphaBlendFactor = (VkBlendFactor)getBlendFactor((key >> KEY_SRC) & 7, true);
			equation.dstColorBlendFactor = equation.dstAlphaBlendFactor = (VkBlendFactor)getBlendFactor((key >> KEY_DST) & 7, false);
		}
		f.CmdSetColorBlendEquation(cmd, 0, 1, &equation);
		const VkColorComponentFlags writeMask = ((key >> KEY_COLOR_WRITE) & 1)
				? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT : 0;
		f.CmdSetColorWriteMask(cmd, 0, 1, &writeMask);
	}
}

void OITPipelineManager::Reset()
{
	// (nothing of it is to be made any more; what is being made, finished)
	NewGeneration();
	VkPsoWorker::Instance().WaitIdle();
	if (GetContext() != nullptr && GetContext()->GetDevice())
	{
		vk::Device device = GetContext()->GetDevice();
		pipelines.Clear(device);
		general.Clear(device);
		for (PsoSlot& slot : fixedSlots)
		{
			if (slot.pipeline != VK_NULL_HANDLE)
				device.destroyPipeline(vk::Pipeline(slot.pipeline));
			slot.pipeline = VK_NULL_HANDLE;
			slot.owner = nullptr;
		}
	}
	lastKey = ~0ull;
	prepared = false;
}

void OITPipelineManager::Prepare()
{
	VkPsoWorker& worker = VkPsoWorker::Instance();
	const u32 samples = renderPasses->GetSamples();
	/* (taken here: the render passes are made when first asked for, which
	 * is not something two threads may do at once) */
	renderPass = renderPasses->GetRenderPass(true, true);
	std::vector<PsoSlot *> now;
	auto addGeneral = [&](u64 key) {
		const u64 generalKey = GeneralKey(key);
		if (general.Find(generalKey) == nullptr)
		{
			PsoSlot *slot = general.Insert(generalKey);
			if (slot != nullptr)
				now.push_back(slot);
		}
	};
	auto fits = [&](u64 key) {
		const u32 rate = (key >> KEY_RATE) & 3;
		return ((key >> KEY_KIND) & 15) == KIND_POLY && (samples > 1 ? rate == 1 || rate == 2 : rate == 0);
	};
	/* The general pipelines: of each module and subpass, and of each fixed
	 * state the device must have made with them. The depth pass draws the
	 * opaque polygons in subpass 0, the translucent ones in 2; the colour
	 * pass and the lists are in 1. */
	for (u64 gouraud = 0; gouraud < 2; gouraud++)
		for (u64 pass = 0; pass < 3; pass++)
			for (u64 subpass = 0; subpass < 3; subpass++)
			{
				if ((pass == (u64)Pass::Depth) != (subpass != 1))
					continue;
				for (u64 rate = samples > 1 ? 1 : 0; rate <= (samples > 1 ? 2u : 0u); rate++)
				{
					if (pass == (u64)Pass::OIT && rate == 1)
						continue;
					addGeneral((gouraud << KEY_GOURAUD) | (rate << KEY_RATE) | (pass << KEY_PASS) | (subpass << KEY_SUBPASS));
				}
			}
	GetContext()->PsoKeys().ForEach(who, [&](u64 key) { if (fits(key)) addGeneral(key); });
	// the fixed ones
	for (u32 i = 0; i < FIXED_COUNT; i++)
	{
		if (i == FIXED_FINAL_PER_SAMPLE && samples <= 1)
			continue;
		PsoSlot& slot = fixedSlots[i];
		slot.key = ((u64)KIND_FIXED << KEY_KIND) | i;
		slot.owner = this;
		slot.pipeline = VK_NULL_HANDLE;
		now.push_back(&slot);
	}
	worker.BuildNow(now.data(), (u32)now.size(), GetContext()->GetPipelineCache());
	/* The polygons' own pipelines made in earlier runs: asked for now, made
	 * while nothing more pressing is */
	if (worker.Running())
		GetContext()->PsoKeys().ForEach(who, [&](u64 key) {
			if (fits(key) && pipelines.Find(key) == nullptr)
			{
				PsoSlot *slot = pipelines.Insert(key);
				if (slot != nullptr)
					worker.Prefetch(slot);
			}
		});
	prepared = true;
}

VkPipeline OITPipelineManager::BuildPipeline(u64 key, vk::PipelineCache cache)
{
	switch ((key >> KEY_KIND) & 15)
	{
	case KIND_POLY:
		return BuildPoly(key, false, cache);
	case KIND_GENERAL:
		return BuildPoly(key, true, cache);
	case KIND_FIXED:
		{
			const u32 i = (u32)(key & 0xff);
			// (the modifier volumes': Xor, Or, Inclusion, Exclusion, by cull mode none, front, back)
			static const int culls[3] = { 0, 2, 3 };
			if (i < FIXED_TRMODVOL)
				return CreateModVolPipeline((ModVolMode)(i / 3), culls[i % 3], cache);
			if (i < FIXED_FINAL)
				return CreateTrModVolPipeline((ModVolMode)((i - FIXED_TRMODVOL) / 3), culls[(i - FIXED_TRMODVOL) % 3], cache);
			if (i == FIXED_FINAL || i == FIXED_FINAL_PER_SAMPLE)
				return CreateFinalPipeline(i == FIXED_FINAL_PER_SAMPLE, cache);
			if (i == FIXED_CLEAR)
				return CreateClearPipeline(cache);
			return VK_NULL_HANDLE;
		}
	default:
		return VK_NULL_HANDLE;
	}
}

/* A polygon's pipeline, all of it from @key; @general: the one that draws
 * for the keys that differ from it only in what the draw sets (pso.h) */
VkPipeline OITPipelineManager::BuildPoly(u64 key, bool general, vk::PipelineCache cache)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo();

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleStrip);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  CullMode((key >> KEY_CULL) & 3),              // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	const u32 rate = (key >> KEY_RATE) & 3;
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(renderPasses->GetSamples(), rate == 1);

	// Depth and stencil
	const u32 stencil = (key >> KEY_STENCIL) & 3;
	const vk::StencilOpState stencilOpState = StencilState(stencil);
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  true,                                       // depthTestEnable
	  (key >> KEY_ZWRITE) & 1,                    // depthWriteEnable
	  (vk::CompareOp)((key >> KEY_DEPTH_OP) & 7), // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  stencil != 0,                               // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
	if ((key >> KEY_BLEND) & 1)
	{
		vk::ColorComponentFlags colorComponentFlags(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
		u32 src = (key >> KEY_SRC) & 7;
		u32 dst = (key >> KEY_DST) & 7;
		pipelineColorBlendAttachmentState =
		{
		  true,                          // blendEnable
		  getBlendFactor(src, true),     // srcColorBlendFactor
		  getBlendFactor(dst, false),    // dstColorBlendFactor
		  vk::BlendOp::eAdd,             // colorBlendOp
		  getBlendFactor(src, true),     // srcAlphaBlendFactor
		  getBlendFactor(dst, false),    // dstAlphaBlendFactor
		  vk::BlendOp::eAdd,             // alphaBlendOp
		  colorComponentFlags            // colorWriteMask
		};
	}
	else
	{
		pipelineColorBlendAttachmentState =
		{
		  false,                      // blendEnable
		  vk::BlendFactor::eZero,     // srcColorBlendFactor
		  vk::BlendFactor::eZero,     // dstColorBlendFactor
		  vk::BlendOp::eAdd,          // colorBlendOp
		  vk::BlendFactor::eZero,     // srcAlphaBlendFactor
		  vk::BlendFactor::eZero,     // dstAlphaBlendFactor
		  vk::BlendOp::eAdd,          // alphaBlendOp
		  vk::ColorComponentFlags()   // colorWriteMask
		};
	}

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  1,                                          // attachmentCount
	  &pipelineColorBlendAttachmentState,         // pAttachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	// the general pipeline's states that are set with each draw (SetDynamicState())
	vk::DynamicState dynamicStates[18] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	u32 dynamicCount = 2;
	if (general)
	{
		const pso_ext::Functions& dynamic = GetContext()->DynamicState();
		if (dynamic.depthStencil)
		{
			const int states[] = { pso_ext::DYNAMIC_STATE_CULL_MODE, pso_ext::DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
					pso_ext::DYNAMIC_STATE_DEPTH_TEST_ENABLE, pso_ext::DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
					pso_ext::DYNAMIC_STATE_DEPTH_COMPARE_OP, pso_ext::DYNAMIC_STATE_STENCIL_TEST_ENABLE,
					pso_ext::DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
					VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
			for (int state : states)
				dynamicStates[dynamicCount++] = (vk::DynamicState)state;
		}
		if (dynamic.blend)
		{
			const int states[] = { pso_ext::DYNAMIC_STATE_COLOR_BLEND_ENABLE, pso_ext::DYNAMIC_STATE_COLOR_BLEND_EQUATION,
					pso_ext::DYNAMIC_STATE_COLOR_WRITE_MASK };
			for (int state : states)
				dynamicStates[dynamicCount++] = (vk::DynamicState)state;
		}
	}
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), dynamicCount, dynamicStates);

	const bool gouraud = (key >> KEY_GOURAUD) & 1;
	vk::ShaderModule vertex_module = shaderManager->GetVertexShader(OITShaderManager::VertexShaderParams{ gouraud });
	OITShaderManager::FragmentShaderParams params = {};
	params.gouraud = gouraud;
	params.pass = (Pass)((key >> KEY_PASS) & 3);
	params.sampleRate = (int)rate;
	vk::ShaderModule fragment_module = shaderManager->GetFragmentShader(params);
	const PsoSpecialization specialization(general ? -1 : (s32)(key & ((1u << OIT_SC_BITS) - 1)), renderPasses->GetSamples());

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main", &specialization.info },
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
	  (u32)((key >> KEY_SUBPASS) & 3)             // subpass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

VkPipeline OITPipelineManager::CreateFinalPipeline(bool perSample, vk::PipelineCache cache)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetQuadInputStateCreateInfo(false);

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleStrip);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  vk::CullModeFlagBits::eNone,                  // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	// (SSAA: the last subpass is of single samples; the one before, which puts each sample together, of all of them)
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(perSample ? renderPasses->GetSamples() : 1, perSample);

	// Depth and stencil
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState =
	{
		false,                         // blendEnable
		vk::BlendFactor::eZero,        // srcColorBlendFactor
		vk::BlendFactor::eZero,        // dstColorBlendFactor
		vk::BlendOp::eAdd,             // colorBlendOp
		vk::BlendFactor::eZero,        // srcAlphaBlendFactor
		vk::BlendFactor::eZero,        // dstAlphaBlendFactor
		vk::BlendOp::eAdd,             // alphaBlendOp
		colorComponentFlags            // colorWriteMask
	};
	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  1,                                          // attachmentCount
	  &pipelineColorBlendAttachmentState,         // pAttachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);

	vk::ShaderModule vertex_module = shaderManager->GetFinalVertexShader();
	vk::ShaderModule fragment_module = shaderManager->GetFinalShader(perSample);
	const PsoSpecialization specialization(0, renderPasses->GetSamples());

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main", &specialization.info },
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
	  perSample ? 2 : GetFinalSubpass()          // subpass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);

}

VkPipeline OITPipelineManager::CreateClearPipeline(vk::PipelineCache cache)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetQuadInputStateCreateInfo(false);

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleStrip);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  vk::CullModeFlagBits::eNone,                  // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;

	// Depth and stencil
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
	  vk::PipelineColorBlendStateCreateFlags(),   // flags
	  false,                                      // logicOpEnable
	  vk::LogicOp::eNoOp,                         // logicOp
	  1,                                          // attachmentCount
	  &pipelineColorBlendAttachmentState,         // pAttachments
	  { { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);

	vk::ShaderModule vertex_module = shaderManager->GetFinalVertexShader();
	vk::ShaderModule fragment_module = shaderManager->GetClearShader();

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main" },
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
	  GetFinalSubpass()                           // subpass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

VkPipeline OITPipelineManager::CreateModVolPipeline(ModVolMode mode, int cullMode, vk::PipelineCache cache)
{
	verify(mode != ModVolMode::Final);

	static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
	{
			{ 0, sizeof(float) * 3 },
	};
	static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
	{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, 0),	// pos
	};
	// Vertex input state
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo(
			vk::PipelineVertexInputStateCreateFlags(),
			ARRAY_SIZE(vertexBindingDescriptions),
			vertexBindingDescriptions,
			ARRAY_SIZE(vertexInputAttributeDescriptions),
			vertexInputAttributeDescriptions);
	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleList);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  cullMode == 3 ? vk::CullModeFlagBits::eBack
  			  : cullMode == 2 ? vk::CullModeFlagBits::eFront
  			  : vk::CullModeFlagBits::eNone,        // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(renderPasses->GetSamples(), false);

	// Depth and stencil
	vk::StencilOpState stencilOpState;
	switch (mode)
	{
	case ModVolMode::Xor:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eInvert, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 2, 2);
		break;
	case ModVolMode::Or:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 2, 2, 2);
		break;
	case ModVolMode::Inclusion:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eReplace, vk::StencilOp::eZero, vk::CompareOp::eLessOrEqual, 3, 3, 1);
		break;
	case ModVolMode::Exclusion:
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eKeep, vk::StencilOp::eZero, vk::CompareOp::eEqual, 3, 3, 1);
		break;
	default:
		break;
	}
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  mode == ModVolMode::Xor || mode == ModVolMode::Or, // depthTestEnable
	  false,                                      // depthWriteEnable
	  vk::CompareOp::eGreater,                    // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  true,                                       // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
		vk::PipelineColorBlendStateCreateFlags(),   // flags
		false,                                      // logicOpEnable
		vk::LogicOp::eNoOp,                         // logicOp
		1,                                          // attachmentCount
		&pipelineColorBlendAttachmentState,         // pAttachments
		{ { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);

	vk::ShaderModule vertex_module = shaderManager->GetModVolVertexShader();
	vk::ShaderModule fragment_module = shaderManager->GetModVolShader();

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main" },
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
	  0                                           // subpass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

VkPipeline OITPipelineManager::CreateTrModVolPipeline(ModVolMode mode, int cullMode, vk::PipelineCache cache)
{
	verify(mode != ModVolMode::Final);

	static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
	{
			{ 0, sizeof(float) * 3 },
	};
	static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
	{
			vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, 0),	// pos
	};
	// Vertex input state
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo(
			vk::PipelineVertexInputStateCreateFlags(),
			ARRAY_SIZE(vertexBindingDescriptions),
			vertexBindingDescriptions,
			ARRAY_SIZE(vertexInputAttributeDescriptions),
			vertexInputAttributeDescriptions);
	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleList);

	// Viewport and scissor states
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);

	// Rasterization and multisample states
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo
	(
	  vk::PipelineRasterizationStateCreateFlags(),  // flags
	  false,                                        // depthClampEnable
	  false,                                        // rasterizerDiscardEnable
	  vk::PolygonMode::eFill,                       // polygonMode
	  cullMode == 3 ? vk::CullModeFlagBits::eBack
  			  : cullMode == 2 ? vk::CullModeFlagBits::eFront
  			  : vk::CullModeFlagBits::eNone,        // cullMode
	  vk::FrontFace::eCounterClockwise,             // frontFace
	  false,                                        // depthBiasEnable
	  0.0f,                                         // depthBiasConstantFactor
	  0.0f,                                         // depthBiasClamp
	  0.0f,                                         // depthBiasSlopeFactor
	  1.0f                                          // lineWidth
	);
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(renderPasses->GetSamples(), false);

	// Depth and stencil
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo;

	// Color flags and blending
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;

	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo
	(
		vk::PipelineColorBlendStateCreateFlags(),   // flags
		false,                                      // logicOpEnable
		vk::LogicOp::eNoOp,                         // logicOp
		1,                                          // attachmentCount
		&pipelineColorBlendAttachmentState,         // pAttachments
		{ { 1.0f, 1.0f, 1.0f, 1.0f } }              // blendConstants
	);

	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);

	vk::ShaderModule vertex_module = shaderManager->GetModVolVertexShader();
	vk::ShaderModule fragment_module = shaderManager->GetTrModVolShader(mode);

	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex, vertex_module, "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment, fragment_module, "main" },
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
      2                                           // subpass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}
