#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include "pipeline.h"
#include "hw/pvr/Renderer_if.h"
#include "quad.h"

static vk::CullModeFlags CullMode(u32 cull)
{
	return cull == 2 ? vk::CullModeFlagBits::eBack : cull == 1 ? vk::CullModeFlagBits::eFront : vk::CullModeFlagBits::eNone;
}

// a key's stencil state (PipelineManager::Stencil)
static vk::StencilOpState StencilState(u32 stencil)
{
	switch (stencil)
	{
	case PipelineManager::STENCIL_SHADOW_SET:
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0x80);
	case PipelineManager::STENCIL_SHADOW_CLEAR:
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eAlways, 0, 0x80, 0);
	/* Drawn again for its shadow: where this polygon is the one showing
	 * (same depth), takes shadows (stencil bit 7) and is in a volume
	 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
	 * marked may be drawn over is what tells the two kinds apart. */
	case PipelineManager::STENCIL_LATER_WINS:
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eReplace, vk::StencilOp::eKeep, vk::CompareOp::eEqual, 0x81, 0x40, 0xC1);
	case PipelineManager::STENCIL_EARLIER_WINS:
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eInvert, vk::StencilOp::eKeep, vk::CompareOp::eEqual, 0xC1, 0x40, 0x81);
	default:
		return vk::StencilOpState(vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::StencilOp::eKeep, vk::CompareOp::eNever);
	}
}

u64 PipelineManager::Key(u32 listType, bool sortTriangles, const PolyParam& pp, ShadowPass shadowPass) const
{
	// the shader
	FragmentShaderParams params = {};
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
	params.trilinear = pp.pcw.Texture && pp.tsp.FilterMode > 1 && listType != ListType_Punch_Through && pp.tcw.MipMapped == 1;
	params.useAlpha = pp.tsp.UseAlpha;
	params.palette = BaseTextureCacheData::IsGpuHandledPaletted(pp.tsp, pp.tcw);
	params.shadowed = shadowPass != ShadowPass::None;
	u64 key = (u32)params.key() | ((u64)pp.pcw.Gouraud << KEY_GOURAUD);
	// (SSAA: shaded at each sample, but for what is drawn at a single rate)
	if (samples > 1)
		key |= (u64)(SsaaSingleRate(pp) ? 2 : 1) << KEY_RATE;

	// the fixed functions
	key |= (u64)(pp.isp.CullMode == 3 ? 2 : pp.isp.CullMode == 2 ? 1 : 0) << KEY_CULL;
	if (sortTriangles && !settings.pvr.Emulation.AlphaSortMode)
		key |= 1ull << KEY_LIST;

	vk::CompareOp depthOp;
	if (listType == ListType_Punch_Through || sortTriangles)
		depthOp = vk::CompareOp::eGreaterOrEqual;
	else
		depthOp = depthOps[pp.isp.DepthMode];
	bool depthWriteEnable;
	// Sorted translucent polygons, per triangle or per strip, do not write
	// depth: a strip drawn in sorted order would hide parts of the ones after it.
	if (sortTriangles)
		// FIXME temporary work-around for intel driver bug
		depthWriteEnable = GetContext()->GetVendorID() == VENDOR_INTEL;
	else
	{
		// Z Write Disable seems to be ignored for punch-through.
		// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
		if (listType == ListType_Punch_Through)
			depthWriteEnable = true;
		else
			depthWriteEnable = !pp.isp.ZWriteDis;
	}
	const bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	u32 stencil = STENCIL_NONE;
	if (shadowPass != ShadowPass::None)
	{
		depthOp = vk::CompareOp::eEqual;
		depthWriteEnable = false;
		if (shadowed)
			stencil = shadowPass == ShadowPass::LaterWins ? STENCIL_LATER_WINS : STENCIL_EARLIER_WINS;
	}
	else if (shadowed)
		stencil = pp.pcw.Shadow != 0 ? STENCIL_SHADOW_SET : STENCIL_SHADOW_CLEAR;
	key |= ((u64)depthWriteEnable << KEY_ZWRITE) | ((u64)depthOp << KEY_DEPTH_OP) | ((u64)stencil << KEY_STENCIL);

	// Apparently punch-through polys support blending, or at least some combinations
	// An opaque polygon is blended by its instructions too when it is in a
	// list continuation; the ones of the first render pass have had theirs
	// set to source x 1 and destination x 0 when the list was read. That
	// pair is no blending at all, and is left as that so it costs nothing.
	if (shadowPass == ShadowPass::None
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| pp.tsp.SrcInstr != 1 || pp.tsp.DstInstr != 0))
		key |= (1ull << KEY_BLEND) | ((u64)pp.tsp.SrcInstr << KEY_SRC) | ((u64)pp.tsp.DstInstr << KEY_DST);

	return key;
}

PsoChoice PipelineManager::Choose(u64 key)
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

void PipelineManager::SetDynamicState(vk::CommandBuffer cmdBuffer, u64 key) const
{
	const pso_ext::Functions& f = GetContext()->DynamicState();
	const VkCommandBuffer cmd = (VkCommandBuffer)cmdBuffer;
	if (f.depthStencil)
	{
		f.CmdSetCullMode(cmd, (VkCullModeFlags)CullMode((key >> KEY_CULL) & 3));
		f.CmdSetPrimitiveTopology(cmd, ((key >> KEY_LIST) & 1) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		f.CmdSetDepthTestEnable(cmd, VK_TRUE);
		f.CmdSetDepthWriteEnable(cmd, (VkBool32)((key >> KEY_ZWRITE) & 1));
		f.CmdSetDepthCompareOp(cmd, (VkCompareOp)((key >> KEY_DEPTH_OP) & 7));
		const u32 stencil = (key >> KEY_STENCIL) & 7;
		const vk::StencilOpState state = StencilState(stencil);
		f.CmdSetStencilTestEnable(cmd, stencil != STENCIL_NONE);
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
		const VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		f.CmdSetColorWriteMask(cmd, 0, 1, &writeMask);
	}
}

void PipelineManager::Reset()
{
	// (nothing of it is to be made any more; what is being made, finished)
	NewGeneration();
	VkPsoWorker::Instance().WaitIdle();
	if (GetContext() != nullptr && GetContext()->GetDevice())
	{
		vk::Device device = GetContext()->GetDevice();
		pipelines.Clear(device);
		general.Clear(device);
		for (PsoSlot& slot : modVolSlots)
		{
			if (slot.pipeline != VK_NULL_HANDLE)
				device.destroyPipeline(vk::Pipeline(slot.pipeline));
			slot.pipeline = VK_NULL_HANDLE;
			slot.owner = nullptr;
		}
		PsoSlot *prepass = &prepassSlots[0][0][0][0];
		for (u32 i = 0; i < sizeof(prepassSlots) / sizeof(PsoSlot); i++)
		{
			if (prepass[i].pipeline != VK_NULL_HANDLE)
				device.destroyPipeline(vk::Pipeline(prepass[i].pipeline));
			prepass[i].pipeline = VK_NULL_HANDLE;
			prepass[i].owner = nullptr;
		}
	}
	lastKey = ~0ull;
}

void PipelineManager::Prepare()
{
	VkPsoWorker& worker = VkPsoWorker::Instance();
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
	// the general pipelines: of each module, and of each fixed state the device must have made with them
	for (u64 gouraud = 0; gouraud < 2; gouraud++)
		for (u64 rate = samples > 1 ? 1 : 0; rate <= (samples > 1 ? 2u : 0u); rate++)
			addGeneral((gouraud << KEY_GOURAUD) | (rate << KEY_RATE));
	GetContext()->PsoKeys().ForEach(who, [&](u64 key) { if (fits(key)) addGeneral(key); });
	// the modifier volumes'
	for (int mode = 0; mode <= (int)ModVolMode::Depth; mode++)
		for (int cull = 0; cull < 3; cull++)
		{
			if ((ModVolMode)mode == ModVolMode::Final && cull != 0)
				continue;
			PsoSlot& slot = modVolSlots[mode * 3 + cull];
			slot.key = ((u64)KIND_MODVOL << KEY_KIND) | ((u64)mode << 2) | (u64)(cull == 0 ? 0 : cull + 1);
			slot.owner = this;
			slot.pipeline = VK_NULL_HANDLE;
			now.push_back(&slot);
		}
	// the depth prepass's (rend/sorter.h)
	for (int texture = 0; texture < 2; texture++)
		for (int gouraud = 0; gouraud < 2; gouraud++)
			for (int cull = 0; cull < 3; cull++)
				for (int rate = 0; rate < (samples > 1 ? 2 : 1); rate++)
				{
					PsoSlot& slot = prepassSlots[texture][gouraud][cull][rate];
					slot.key = ((u64)KIND_PREPASS << KEY_KIND) | (u64)((texture << 4) | (gouraud << 3) | (cull << 1) | rate);
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
}

VkPipeline PipelineManager::BuildPipeline(u64 key, vk::PipelineCache cache)
{
	switch ((key >> KEY_KIND) & 15)
	{
	case KIND_POLY:
		return BuildPoly(key, false, cache);
	case KIND_GENERAL:
		return BuildPoly(key, true, cache);
	case KIND_MODVOL:
		return BuildModVol((ModVolMode)((key >> 2) & 7), (int)(key & 3), cache);
	case KIND_PREPASS:
		return BuildPrepass((int)((key >> 4) & 1), (int)((key >> 3) & 1), (int)((key >> 1) & 3), (int)(key & 1), cache);
	default:
		return VK_NULL_HANDLE;
	}
}

VkPipeline PipelineManager::BuildModVol(ModVolMode mode, int cullMode, vk::PipelineCache cache)
{
	// Vertex input state
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo;

	if (mode == ModVolMode::Final || mode == ModVolMode::Depth)
	{
		pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo(false);
		pipelineInputAssemblyStateCreateInfo = vk::PipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
				mode == ModVolMode::Final ? vk::PrimitiveTopology::eTriangleStrip : vk::PrimitiveTopology::eTriangleList);
	}
	else
	{
		static const vk::VertexInputBindingDescription vertexBindingDescriptions[] =
		{
				{ 0, sizeof(float) * 3 },
		};
		static const vk::VertexInputAttributeDescription vertexInputAttributeDescriptions[] =
		{
				vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, 0),	// pos
		};
		pipelineVertexInputStateCreateInfo = vk::PipelineVertexInputStateCreateInfo(
				vk::PipelineVertexInputStateCreateFlags(),
				ARRAY_SIZE(vertexBindingDescriptions),
				vertexBindingDescriptions,
				ARRAY_SIZE(vertexInputAttributeDescriptions),
				vertexInputAttributeDescriptions);
		pipelineInputAssemblyStateCreateInfo = vk::PipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
				vk::PrimitiveTopology::eTriangleList);
	}

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
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(samples, false);

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
	case ModVolMode::Final:
		// takes shadows, in a volume, and not drawn again for it (bit 6);
		// clears the volume bits and that mark
		stencilOpState = vk::StencilOpState(vk::StencilOp::eZero, vk::StencilOp::eZero, vk::StencilOp::eZero, vk::CompareOp::eEqual, 0xC1, 0x43, 0x81);
		break;
	case ModVolMode::Depth:
		break;
	}
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  mode == ModVolMode::Xor || mode == ModVolMode::Or || mode == ModVolMode::Depth, // depthTestEnable
	  mode == ModVolMode::Depth,                  // depthWriteEnable
	  mode == ModVolMode::Depth ? vk::CompareOp::eGreaterOrEqual : vk::CompareOp::eGreater, // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  mode != ModVolMode::Depth,                  // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags(
			mode != ModVolMode::Final ? (vk::ColorComponentFlagBits)0
					: vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(
		mode == ModVolMode::Final,          // blendEnable
		vk::BlendFactor::eSrcAlpha,         // srcColorBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha, // dstColorBlendFactor
		vk::BlendOp::eAdd,                  // colorBlendOp
		vk::BlendFactor::eSrcAlpha,         // srcAlphaBlendFactor
		vk::BlendFactor::eOneMinusSrcAlpha, // dstAlphaBlendFactor
		vk::BlendOp::eAdd,                  // alphaBlendOp
		colorComponentFlags                 // colorWriteMask
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
	  renderPass                                  // renderPass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

/* A polygon's pipeline, all of it from @key; @general: the one that draws
 * for the keys that differ from it only in what the draw sets (pso.h) */
VkPipeline PipelineManager::BuildPoly(u64 key, bool general, vk::PipelineCache cache)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo();

	// Input assembly state
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			((key >> KEY_LIST) & 1) ? vk::PrimitiveTopology::eTriangleList : vk::PrimitiveTopology::eTriangleStrip);

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
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(samples, rate == 1);

	// Depth and stencil
	const u32 stencil = (key >> KEY_STENCIL) & 7;
	const vk::StencilOpState stencilOpState = StencilState(stencil);
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo
	(
	  vk::PipelineDepthStencilStateCreateFlags(), // flags
	  true,                                       // depthTestEnable
	  (key >> KEY_ZWRITE) & 1,                    // depthWriteEnable
	  (vk::CompareOp)((key >> KEY_DEPTH_OP) & 7), // depthCompareOp
	  false,                                      // depthBoundTestEnable
	  stencil != STENCIL_NONE,                    // stencilTestEnable
	  stencilOpState,                             // front
	  stencilOpState                              // back
	);

	// Color flags and blending
	vk::ColorComponentFlags colorComponentFlags(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
	if ((key >> KEY_BLEND) & 1)
	{
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
		  colorComponentFlags         // colorWriteMask
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
	vk::ShaderModule vertex_module = shaderManager->GetVertexShader(VertexShaderParams{ gouraud });
	FragmentShaderParams params = {};
	params.gouraud = gouraud;
	params.sampleRate = (int)rate;
	vk::ShaderModule fragment_module = shaderManager->GetFragmentShader(params);
	const PsoSpecialization specialization(general ? -1 : (s32)(key & ((1u << TRI_SC_BITS) - 1)), samples);

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
	  renderPass                                  // renderPass
	);

	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

/* A pipeline of the depth prepass of a sorted translucent list (rend/sorter.h):
 * depth only, written where the depth test passes (the colour pass's test) */
VkPipeline PipelineManager::BuildPrepass(int texture, int gouraud, int cull, int rate, vk::PipelineCache cache)
{
	vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo = GetMainVertexInputStateCreateInfo();
	vk::PipelineInputAssemblyStateCreateInfo pipelineInputAssemblyStateCreateInfo(vk::PipelineInputAssemblyStateCreateFlags(),
			vk::PrimitiveTopology::eTriangleList);
	vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo(vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr);
	vk::PipelineDepthStencilStateCreateInfo pipelineDepthStencilStateCreateInfo(vk::PipelineDepthStencilStateCreateFlags(),
			true, true, vk::CompareOp::eGreaterOrEqual, false, false);
	vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState(false, vk::BlendFactor::eZero, vk::BlendFactor::eZero,
			vk::BlendOp::eAdd, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::ColorComponentFlags());
	vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo(vk::PipelineColorBlendStateCreateFlags(), false,
			vk::LogicOp::eNoOp, 1, &pipelineColorBlendAttachmentState, { { 1.0f, 1.0f, 1.0f, 1.0f } });
	vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo(vk::PipelineDynamicStateCreateFlags(), 2, dynamicStates);
	vk::PipelineRasterizationStateCreateInfo pipelineRasterizationStateCreateInfo(vk::PipelineRasterizationStateCreateFlags(),
			false, false, vk::PolygonMode::eFill, CullMode(cull), vk::FrontFace::eCounterClockwise, false, 0.0f, 0.0f, 0.0f, 1.0f);
	vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo = SsaaMultisample(samples, rate == 1);

	PrepassShaderParams params = {};
	params.texture = texture == 1;
	params.gouraud = gouraud == 1;
	params.sampleRate = samples > 1 ? (rate == 1 ? 1 : 2) : 0;
	params.samples = samples;
	const PsoSpecialization specialization(0, samples);
	vk::PipelineShaderStageCreateInfo stages[] = {
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eVertex,
					shaderManager->GetVertexShader(VertexShaderParams{ gouraud == 1 }), "main" },
			{ vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eFragment,
					shaderManager->GetPrepassShader(params), "main", &specialization.info },
	};
	vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo(vk::PipelineCreateFlags(), 2, stages,
			&pipelineVertexInputStateCreateInfo, &pipelineInputAssemblyStateCreateInfo, nullptr,
			&pipelineViewportStateCreateInfo, &pipelineRasterizationStateCreateInfo, &pipelineMultisampleStateCreateInfo,
			&pipelineDepthStencilStateCreateInfo, &pipelineColorBlendStateCreateInfo, &pipelineDynamicStateCreateInfo,
			*pipelineLayout, renderPass);
	return PsoCreate(GetContext()->GetDevice(), cache, graphicsPipelineCreateInfo);
}

vk::UniqueRenderPass MakeSsaaRenderPass(vk::Format colorFormat, u32 samples, vk::ImageLayout pictureLayout)
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::SampleCountFlagBits n = (vk::SampleCountFlagBits)samples;
	vk::AttachmentDescription attachmentDescriptions[] = {
			// the picture
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), colorFormat, vk::SampleCountFlagBits::e1,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
					vk::ImageLayout::eUndefined, pictureLayout),
			// the samples' colour and depth
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), colorFormat, n,
					vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
					vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), context->GetDepthFormat(), n,
					vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
					vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
					vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal),
	};
	vk::AttachmentReference pictureReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::AttachmentReference colorReference(1, vk::ImageLayout::eColorAttachmentOptimal);
	vk::AttachmentReference depthReference(2, vk::ImageLayout::eDepthStencilAttachmentOptimal);
	vk::AttachmentReference samplesInput(1, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::SubpassDescription subpasses[] = {
			vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, 0, nullptr, 1, &colorReference, nullptr, &depthReference),
			vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics, 1, &samplesInput, 1, &pictureReference, nullptr, nullptr),
	};
	const vk::PipelineStageFlags fbStages = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
			| vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput;
	const vk::AccessFlags fbWrites = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
	const vk::AccessFlags fbAll = fbWrites | vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentRead
			| vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead;
	vk::SubpassDependency dependencies[] = {
			/* The sample attachments are the same every frame: the one before
			 * has to be done with them, the resolve's reading included, before
			 * this one clears them; and the picture is read where it was
			 * shown or sampled */
			vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0, fbStages, fbStages, fbWrites | vk::AccessFlagBits::eShaderRead, fbAll),
			vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 1, fbStages, fbStages, fbWrites | vk::AccessFlagBits::eShaderRead, fbAll),
			// the samples drawn, then resolved
			vk::SubpassDependency(0, 1, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
					vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eInputAttachmentRead, vk::DependencyFlagBits::eByRegion),
			// the picture, to be shown or sampled
			vk::SubpassDependency(1, VK_SUBPASS_EXTERNAL, fbStages, fbStages | vk::PipelineStageFlagBits::eTransfer,
					vk::AccessFlagBits::eColorAttachmentWrite, fbAll | vk::AccessFlagBits::eTransferRead),
	};
	return context->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
			ARRAY_SIZE(attachmentDescriptions), attachmentDescriptions, ARRAY_SIZE(subpasses), subpasses,
			ARRAY_SIZE(dependencies), dependencies));
}

vk::UniqueRenderPass RttPipelineManager::MakeSsaaRenderPass(u32 samples)
{
	return ::MakeSsaaRenderPass(vk::Format::eR8G8B8A8Unorm, samples, vk::ImageLayout::eShaderReadOnlyOptimal);
}
