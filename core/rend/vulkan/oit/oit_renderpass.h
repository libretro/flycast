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
#pragma once
#include "../vulkan_context.h"

class RenderPasses
{
public:
	/* @depthWrites: the colour subpass may write depth. It reads the depth
	 * buffer as an input, so the buffer is normally in a read-only layout
	 * there, which forbids writing; translucent polygons that are not
	 * sorted are drawn in that subpass and do write. For a frame that has
	 * such polygons the buffer is in the general layout instead, which
	 * allows both. The two kinds of render pass differ in nothing else and
	 * a pipeline made for one works with the other.
	 * @resumed: a band of a frame drawn in bands (OITDrawer::Draw()) after
	 * the first. The picture's attachment holds the bands drawn before, and
	 * is loaded, not cleared and not taken from an undefined layout, which
	 * would let the device drop all of it. Again a pipeline made for one
	 * render pass works with the other. */
	vk::RenderPass GetRenderPass(bool initial, bool last, bool depthWrites = false, bool resumed = false)
	{
		size_t index = (initial ? 1 : 0) | (last ? 2 : 0) | (depthWrites ? 4 : 0) | (resumed ? 8 : 0);
		if (!renderPasses[index])
			renderPasses[index] = MakeRenderPass(initial, last, depthWrites, resumed);
		return *renderPasses[index];
	}
	/* Every kind of render pass made now, when the renderer starts or the
	 * picture changes size: the ones for drawing in bands (@resumed) are
	 * first needed while a game runs, and nothing is made then */
	void MakeAll()
	{
		for (size_t index = 0; index < renderPasses.size(); index++)
			GetRenderPass(index & 1, index & 2, index & 4, index & 8);
	}
	// The layout the depth buffer is in while the colour subpass reads it
	static vk::ImageLayout DepthInputLayout(bool depthWrites)
	{
		return depthWrites ? vk::ImageLayout::eGeneral : vk::ImageLayout::eDepthStencilReadOnlyOptimal;
	}
	void Reset()
	{
		for (auto& renderPass : renderPasses)
			renderPass.reset();
	}
	/* SSAA's samples a pixel (1: off): the render passes made for another
	 * count are dropped. See MakeSsaaRenderPass(). */
	void SetSamples(u32 samples)
	{
		if (samples != this->samples)
		{
			Reset();
			this->samples = samples;
		}
	}
	u32 GetSamples() const { return samples; }
	/* The subpasses with SSAA: 0 depth and stencil, 1 colour and the
	 * translucent lists, 2 the translucent modifier volumes (and, in a
	 * render pass that is not the last, each sample put together, and the
	 * translucent depth for the next), 3 the picture put together and
	 * resolved. Without, 2 does all that 2 and 3 do. */
	static constexpr int SsaaFinalSubpass = 3;
	int GetFinalSubpass() const { return samples > 1 ? SsaaFinalSubpass : 2; }
	virtual ~RenderPasses() = default;

protected:
	VulkanContext *GetContext() const { return VulkanContext::Instance(); }
	vk::UniqueRenderPass MakeRenderPass(bool initial, bool last, bool depthWrites, bool resumed);
	vk::UniqueRenderPass MakeSsaaRenderPass(bool initial, bool last, bool depthWrites, bool resumed);
	virtual vk::AttachmentDescription GetAttachment0Description(bool initial, bool last) const
	{
		return vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetColorFormat(), vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
	}
	virtual vk::Format GetColorFormat() const { return GetContext()->GetColorFormat(); }
	virtual std::vector<vk::SubpassDependency> GetSubpassDependencies() const
	{
		std::vector<vk::SubpassDependency> deps;
		deps.emplace_back(2, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
				vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead, vk::DependencyFlagBits::eByRegion);
		return deps;
	}

private:
	std::array<vk::UniqueRenderPass, 16> renderPasses;
	u32 samples = 1;
};

class RttRenderPasses : public RenderPasses
{
protected:
	virtual vk::AttachmentDescription GetAttachment0Description(bool initial, bool last) const override
	{
		return vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Unorm, vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eUndefined,
				vk::ImageLayout::eShaderReadOnlyOptimal);
	}
	virtual vk::Format GetColorFormat() const override { return vk::Format::eR8G8B8A8Unorm; }
	virtual std::vector<vk::SubpassDependency> GetSubpassDependencies() const override
	{
		std::vector<vk::SubpassDependency> deps;
		deps.emplace_back(2, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
				vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
		return deps;
	}
};
