#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Nov 10, 2019

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
#include "oit_renderpass.h"

vk::UniqueRenderPass RenderPasses::MakeRenderPass(bool initial, bool last, bool depthWrites, bool resumed)
{
	if (samples > 1)
		return MakeSsaaRenderPass(initial, last, depthWrites, resumed);
    vk::AttachmentDescription attachmentDescriptions[] = {
    		// Swap chain image
    		GetAttachment0Description(initial, last),
			// OP+PT color attachment
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetColorFormat(), vk::SampleCountFlagBits::e1,
					initial ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
					last ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
					initial ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal),
			// OP+PT depth attachment
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetDepthFormat(), vk::SampleCountFlagBits::e1,
					initial ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
					last ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
					vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
					initial ? vk::ImageLayout::eUndefined : vk::ImageLayout::eDepthStencilReadOnlyOptimal, vk::ImageLayout::eDepthStencilReadOnlyOptimal),
    };
    if (resumed)
    {
    	attachmentDescriptions[0].loadOp = vk::AttachmentLoadOp::eLoad;
    	attachmentDescriptions[0].initialLayout = attachmentDescriptions[0].finalLayout;
    }
    vk::AttachmentReference swapChainReference(0, vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference colorReference(1, vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference depthReference(2, vk::ImageLayout::eDepthStencilAttachmentOptimal);

    vk::AttachmentReference depthReadOnlyRef(2, DepthInputLayout(depthWrites));
    vk::AttachmentReference colorInput(1, vk::ImageLayout::eShaderReadOnlyOptimal);

    vk::SubpassDescription subpasses[] = {
    	// Depth and modvol pass	FIXME subpass 0 shouldn't reference the color attachment
    	vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
    			0, nullptr,
				1, &colorReference,
				nullptr,
				&depthReference),
    	// Color pass
    	vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
    			1, &depthReadOnlyRef,
				1, &colorReference,
				nullptr,
				&depthReadOnlyRef),
    	// Final pass
    	vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
    			1, &colorInput,
				1, &swapChainReference,
				nullptr,
				&depthReference),	// depth-only Tr pass when continuation
    };

    std::vector<vk::SubpassDependency> dependencies = GetSubpassDependencies();
    dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eFragmentShader,
    		vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
    		vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead,
			vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
			vk::DependencyFlagBits::eByRegion);
    dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 1, vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::AccessFlagBits::eInputAttachmentRead, vk::AccessFlagBits::eColorAttachmentWrite, vk::DependencyFlagBits::eByRegion);
    /* The colour and depth attachments the polygons are drawn into, and the
     * per-pixel lists, are the same ones for every render pass and every
     * frame: the one before may still be reading or writing them when this
     * one clears or loads them and writes the lists again. Nothing said it
     * had to be done first. Most drivers work that out for themselves; the
     * ones that do not show one frame's leavings in the next. (From
     * upstream, which names MoltenVK on macOS 15.)
     * These two attachments are first used in subpasses 0 and 1. */
    const vk::PipelineStageFlags oitStages = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
    		| vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput;
    for (u32 subpass = 0; subpass < 2; subpass++)
    	dependencies.emplace_back(VK_SUBPASS_EXTERNAL, subpass, oitStages, oitStages,
    			vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eShaderWrite,
    			vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
    				| vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    				| vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
    /* The picture's attachment is first used in subpass 2. With several
     * render passes it was the polygons' colour attachment of the one
     * before, written and then read as an input. */
    dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 2,
    		vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::AccessFlagBits::eColorAttachmentWrite,
    		vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
    dependencies.emplace_back(0, 1, vk::PipelineStageFlagBits::eLateFragmentTests, vk::PipelineStageFlagBits::eFragmentShader,
    		vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
			vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead, vk::DependencyFlagBits::eByRegion);
    // carries the dependency above on to where the colour attachment changes hands for subpass 2
    dependencies.emplace_back(0, 1, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite,
			vk::DependencyFlagBits::eByRegion);
    // the depth attachment is stored at the end of subpass 2, and changes layout after that - not before
    dependencies.emplace_back(2, VK_SUBPASS_EXTERNAL,
    		vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests, oitStages,
    		vk::AccessFlagBits::eDepthStencilAttachmentWrite,
    		vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    			| vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead);
    // (the colour attachment is stored at the end of subpass 2)
    dependencies.emplace_back(1, 2, vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
    		vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite,
    		vk::DependencyFlagBits::eByRegion);
    // This dependency is only needed if the render pass isn't the last: it's needed for the depth-only Tr pass
    // Unfortunately we want all render passes to be compatible, and that means all attachments must be identical
    dependencies.emplace_back(1, 2, vk::PipelineStageFlagBits::eFragmentShader,
    		vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
    		vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead,
			vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
			vk::DependencyFlagBits::eByRegion);
    dependencies.emplace_back(2, 2, vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eFragmentShader,
    		vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
			vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
			vk::DependencyFlagBits::eByRegion);

    return GetContext()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
    		ARRAY_SIZE(attachmentDescriptions), attachmentDescriptions,
    		ARRAY_SIZE(subpasses), subpasses,
			dependencies.size(), dependencies.data()));
}


/* SSAA: attachment 0 is the picture, of one sample; 1 and 2 the colour
 * and depth of the samples, one image each for all the render passes of a
 * frame. The first render pass clears them; one that is not the last
 * stores them for the next, the last leaves them: with one render pass
 * they are never stored, and a tiler never backs them. The picture is all
 * written by the last subpass of the last render pass, so it is never
 * loaded, and stored by the last only. The colour samples are read and
 * written in subpass 2 (each put together where it is) and read in 3:
 * they are in the general layout there. */
vk::UniqueRenderPass RenderPasses::MakeSsaaRenderPass(bool initial, bool last, bool depthWrites, bool resumed)
{
	const vk::SampleCountFlagBits n = (vk::SampleCountFlagBits)samples;
	const vk::ImageLayout depthLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	vk::AttachmentDescription attachmentDescriptions[] = {
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetColorFormat(), vk::SampleCountFlagBits::e1,
					vk::AttachmentLoadOp::eDontCare, last ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
					vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetColorFormat(), n,
					initial ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
					last ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
					vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
					initial ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral),
			vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetDepthFormat(), n,
					initial ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
					last ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
					vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
					initial ? vk::ImageLayout::eUndefined : depthLayout, depthLayout),
	};
	if (resumed)
	{
		attachmentDescriptions[0].loadOp = vk::AttachmentLoadOp::eLoad;
		attachmentDescriptions[0].initialLayout = attachmentDescriptions[0].finalLayout;
	}
	vk::AttachmentReference pictureReference(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::AttachmentReference colorReference(1, vk::ImageLayout::eColorAttachmentOptimal);
	vk::AttachmentReference depthReference(2, depthLayout);
	vk::AttachmentReference depthReadOnlyRef(2, DepthInputLayout(depthWrites));
	vk::AttachmentReference colorGeneral(1, vk::ImageLayout::eGeneral);

	vk::SubpassDescription subpasses[] = {
		// depth and stencil, modifier volumes
		vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				0, nullptr, 1, &colorReference, nullptr, &depthReference),
		// colour, and the translucent lists
		vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				1, &depthReadOnlyRef, 1, &colorReference, nullptr, &depthReadOnlyRef),
		// translucent modifier volumes; each sample put together and the translucent depth, before another render pass
		vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				1, &colorGeneral, 1, &colorGeneral, nullptr, &depthReference),
		// the picture put together and resolved
		vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
				1, &colorGeneral, 1, &pictureReference, nullptr, nullptr),
	};

	const vk::PipelineStageFlags fbStages = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
			| vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput;
	const vk::AccessFlags writes = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite
			| vk::AccessFlagBits::eShaderWrite;
	const vk::AccessFlags all = writes | vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentRead
			| vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead;
	std::vector<vk::SubpassDependency> dependencies;
	/* The samples, the lists and the picture's image are the same ones for
	 * every render pass and every frame: what came before is done with
	 * them before any subpass uses them */
	for (u32 subpass = 0; subpass < 4; subpass++)
		dependencies.emplace_back(VK_SUBPASS_EXTERNAL, subpass, fbStages, fbStages, writes | vk::AccessFlagBits::eShaderRead, all);
	// each subpass reads and writes what the one before wrote, where it is
	for (u32 subpass = 0; subpass < 3; subpass++)
		dependencies.emplace_back(subpass, subpass + 1, fbStages, fbStages, writes, all, vk::DependencyFlagBits::eByRegion);
	// (the barriers between the passes over the lists in subpasses 2 and 3)
	for (u32 subpass = 2; subpass < 4; subpass++)
		dependencies.emplace_back(subpass, subpass, vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eFragmentShader,
				vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
				vk::DependencyFlagBits::eByRegion);
	// what the render pass leaves - the picture, to be shown or sampled; the samples, for the next render pass
	for (u32 subpass = 2; subpass < 4; subpass++)
		dependencies.emplace_back(subpass, VK_SUBPASS_EXTERNAL, fbStages, fbStages | vk::PipelineStageFlagBits::eTransfer,
				writes, all | vk::AccessFlagBits::eTransferRead);

	return GetContext()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
			ARRAY_SIZE(attachmentDescriptions), attachmentDescriptions,
			ARRAY_SIZE(subpasses), subpasses,
			dependencies.size(), dependencies.data()));
}
