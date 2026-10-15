/*
    Created on: Nov 12, 2019

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
#include <algorithm>
#include <memory>
#include "../buffer.h"
#include "../texture.h"
#include "../commandpool.h"
#include "rend/oit_size.h"

class OITBuffers
{
public:
	/* The fragments are spread over this many storage buffers when the
	 * device cannot bind them as one (the shaders' PixelBuffer[]) */
	static constexpr u32 Bindings = 4;
	/* Render passes of a frame whose fragments are counted (OITDrawer::Draw()) */
	static constexpr u32 Counts = 64;
	// Frame slots counted: the command pool's, which are the swap chain's
	static constexpr u32 CountSlots = 8;

	/* The fragment buffer for a picture of @width x @height: rend/oit_size.h,
	 * no more than the device can bind. One storage buffer is no larger
	 * than the device can allocate in one piece, nor than a shader can be
	 * given as one (Mesa's software Vulkan stops at 128 MB, and a buffer
	 * bound past the limit drew nothing at all). */
	static vk::DeviceSize PlannedSize(int width, int height)
	{
		return oit_buffer_bytes((u32)std::max(width, 0), (u32)std::max(height, 0),
				VulkanContext::Instance()->GetVramBudget().available_kib(), (u64)BindingLimit() * Bindings);
	}

	/* The screen's size: the fragment buffer is made for it, at the next
	 * Init(). Renders to a texture larger than the screen are drawn in
	 * more bands rather than the buffer made again while a game runs. */
	void SetScreenSize(int width, int height)
	{
		plannedSize = PlannedSize(width, height);
	}

	void Init(int width, int height, CommandPool *commandPool)
	{
		const VulkanContext *context = VulkanContext::Instance();
		if (!descSetLayout)
		{
			// Descriptor set and pipeline layout
			vk::DescriptorSetLayoutBinding descSetLayoutBindings[] = {
					{ 0, vk::DescriptorType::eStorageBuffer, Bindings, vk::ShaderStageFlagBits::eFragment },	// pixel buffer
					{ 1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eFragment },		// pixel counter
					{ 2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eFragment },		// a-buffer pointers
			};

			descSetLayout = context->GetDevice().createDescriptorSetLayoutUnique(
					vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), ARRAY_SIZE(descSetLayoutBindings), descSetLayoutBindings));
		}
		// (the renderer's set-up: the layout only, the buffers are for the screen's size)
		if (width <= 0 || height <= 0)
			return;
		bool remake = false;
		if (plannedSize == 0)
			plannedSize = PlannedSize(width, height);
		if (pixelBuffers[0] == nullptr || plannedSize != bufferSize)
		{
			MakePixelBuffers(commandPool);
			remake = true;
		}
		if (!counts)
		{
			// (read by the CPU once a frame's fence has passed: see Poll())
			counts = std::unique_ptr<BufferData>(new BufferData(CountSlots * Counts * sizeof(u32), vk::BufferUsageFlagBits::eTransferDst));
			std::fill(std::begin(written), std::end(written), 0);
		}
		if (width > maxWidth || height > maxHeight || !abufferPointerAttachment)
		{
			maxWidth = std::max(maxWidth, width);
			maxHeight = std::max(maxHeight, height);
			/* Frames already handed to the GPU may still be using the image and
			 * the descriptor set that names it. Neither is touched: the image is
			 * retired, to be destroyed once those frames are done, and so is the
			 * descriptor set, with a new one written in its place below.
			 *
			 * This used to wait for the frontend's frames, which are not the
			 * ones in question - the core submits its own - and then rewrote
			 * the descriptor set those were using. */
			if (abufferPointerAttachment)
				commandPool->DeferDelete(std::move(abufferPointerAttachment));
			abufferPointerAttachment = std::unique_ptr<FramebufferAttachment>(
					new FramebufferAttachment(context->GetPhysicalDevice(), context->GetDevice()));
			// (cleared when it is first used: see OnNewFrame)
			abufferPointerAttachment->Init(maxWidth, maxHeight, vk::Format::eR32Uint,
					vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst);
			abufferPointerTransitionNeeded = true;
			firstFrameAfterInit = true;
			remake = true;
		}
		if (!remake)
			return;

		if (descSet)
			commandPool->DeferDelete(std::unique_ptr<vk::UniqueDescriptorSet>(new vk::UniqueDescriptorSet(std::move(descSet))));
		descSet = std::move(context->GetDevice().allocateDescriptorSetsUnique(
				vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), 1, &descSetLayout.get())).front());
		std::vector<vk::WriteDescriptorSet> writeDescriptorSets;
		// (the buffers not needed are given the first again: the shaders never go to them)
		vk::DescriptorBufferInfo pixelBufferInfo[Bindings];
		for (u32 i = 0; i < Bindings; i++)
			pixelBufferInfo[i] = vk::DescriptorBufferInfo(*pixelBuffers[i < bindings ? i : 0]->buffer, 0, bindingSize);
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*descSet, 0, 0, Bindings, vk::DescriptorType::eStorageBuffer, nullptr, pixelBufferInfo, nullptr));
		vk::DescriptorBufferInfo pixelCounterBufferInfo(*pixelCounter->buffer, 0, 2 * sizeof(u32));
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*descSet, 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &pixelCounterBufferInfo, nullptr));
		vk::DescriptorImageInfo pointerImageInfo(vk::Sampler(), abufferPointerAttachment->GetImageView(), vk::ImageLayout::eGeneral);
		writeDescriptorSets.push_back(vk::WriteDescriptorSet(*descSet, 2, 0, 1, vk::DescriptorType::eStorageImage, &pointerImageInfo, nullptr, nullptr));
		context->GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
	}

	void BindDescriptorSet(vk::CommandBuffer cmdBuffer, vk::PipelineLayout pipelineLayout, u32 firstSet)
	{
		cmdBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout, firstSet, 1, &descSet.get(), 0, nullptr);
	}

	void OnNewFrame(vk::CommandBuffer commandBuffer)
	{
		if (abufferPointerTransitionNeeded)
		{
			abufferPointerTransitionNeeded = false;

			vk::ImageSubresourceRange imageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
			// (made ready for the clear below, which is what uses it next)
			vk::ImageMemoryBarrier imageMemoryBarrier(vk::AccessFlags(), vk::AccessFlagBits::eTransferWrite,
					vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					abufferPointerAttachment->GetImage(), imageSubresourceRange);
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
					imageMemoryBarrier);
			/* The lists start empty. The image is new and holds nothing
			 * that means anything; every frame leaves the lists empty for
			 * the next, and this does it for the first. */
			const vk::ClearColorValue endOfList(std::array<uint32_t, 4>{ 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu });
			commandBuffer.clearColorImage(abufferPointerAttachment->GetImage(), vk::ImageLayout::eGeneral, endOfList, imageSubresourceRange);
			vk::ImageMemoryBarrier cleared(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
					vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					abufferPointerAttachment->GetImage(), imageSubresourceRange);
			commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr,
					cleared);
		}
		else
		{
			firstFrameAfterInit = false;
		}
	}

	void ResetPixelCounter(vk::CommandBuffer commandBuffer)
	{
    	/* The render passes and frames before this must be done counting... */
    	vk::BufferMemoryBarrier barrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferWrite,
    			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *pixelCounter->buffer, 0, VK_WHOLE_SIZE);
    	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
    			{}, nullptr, barrier, nullptr);
    	// (the count back at nothing, and after it how many fragments there is room for)
    	vk::BufferCopy copy(0, 0, 2 * sizeof(u32));
    	commandBuffer.copyBuffer(*pixelCounterReset->buffer, *pixelCounter->buffer, 1, &copy);
    	/* ...and the next render pass's shaders must see it back at nothing. (From upstream.) */
    	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
    			{}, nullptr, barrier, nullptr);
	}

	/* How many fragments the render pass just ended asked for, those that
	 * found no room too (the shaders count them all), copied out for the
	 * CPU to read when this frame slot comes round again: see Poll().
	 * Nothing waits for it. */
	void CountPass(vk::CommandBuffer commandBuffer)
	{
		if (countSlot >= CountSlots || written[countSlot] >= Counts)
			return;
		vk::BufferMemoryBarrier barrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *pixelCounter->buffer, 0, VK_WHOLE_SIZE);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
				{}, nullptr, barrier, nullptr);
		const vk::DeviceSize offset = (countSlot * Counts + written[countSlot]) * sizeof(u32);
		vk::BufferCopy copy(0, offset, sizeof(u32));
		commandBuffer.copyBuffer(*pixelCounter->buffer, *counts->buffer, 1, &copy);
		// (and the counter is not reset before it has been read)
		vk::BufferMemoryBarrier toHost(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *counts->buffer, offset, sizeof(u32));
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eHost,
				{}, nullptr, toHost, nullptr);
		written[countSlot]++;
	}

	/* For a frame about to be drawn in frame slot @slot, whose fence the
	 * command pool has waited for: what the last frame drawn in that slot
	 * counted, if @owner drew it - the most fragments one of its render
	 * passes asked for, and the bands it was drawn in. A frame two or three
	 * back, then: never this one, which nothing waits for. */
	bool Poll(int slot, const void *owner, u32& fragments, u32& bands)
	{
		countSlot = slot >= 0 && slot < (int)CountSlots ? (u32)slot : CountSlots;
		if (countSlot == CountSlots)
			return false;
		bool counted = false;
		fragments = 0;
		bands = countBands[countSlot];
		if (countOwner[countSlot] == owner && written[countSlot] != 0)
		{
			const u32 *count = (const u32 *)counts->MapMemory() + countSlot * Counts;
			for (u32 i = 0; i < written[countSlot]; i++)
				fragments = std::max(fragments, count[i]);
			counted = true;
		}
		written[countSlot] = 0;
		countOwner[countSlot] = owner;
		return counted;
	}
	// ...and the bands this one is drawn in
	void SetBands(u32 bands)
	{
		if (countSlot < CountSlots)
			countBands[countSlot] = bands;
	}

	// How many fragments there is room for
	u32 GetCapacity() const { return capacity; }
	vk::DeviceSize GetSize() const { return bufferSize; }

	void Term()
	{
		for (auto& buffer : pixelBuffers)
			buffer.reset();
		bufferSize = 0;
		pixelCounter.reset();
		pixelCounterReset.reset();
		counts.reset();
		abufferPointerAttachment.reset();
		descSet.reset();
		maxWidth = maxHeight = 0;
	}

	vk::DescriptorSetLayout GetDescriptorSetLayout() const { return *descSetLayout; }
	bool isFirstFrameAfterInit() const { return firstFrameAfterInit; }

private:
	static vk::DeviceSize BindingLimit()
	{
		const VulkanContext *context = VulkanContext::Instance();
		return std::min<vk::DeviceSize>(context->GetMaxMemoryAllocationSize(), context->GetMaxStorageBufferRange())
				& ~(vk::DeviceSize)(OIT_FRAGMENT_BYTES - 1);
	}

	/* The fragment buffer, of plannedSize: in as few storage buffers as
	 * the device allows, all of one size. The ones it replaces are retired
	 * like the pointer image, and so is the counter's reset value, which
	 * says how many fragments there is room for. */
	void MakePixelBuffers(CommandPool *commandPool)
	{
		const vk::DeviceSize limit = BindingLimit();
		bindings = (u32)std::min<vk::DeviceSize>((plannedSize + limit - 1) / limit, Bindings);
		bindingSize = (plannedSize / bindings) & ~(vk::DeviceSize)(OIT_FRAGMENT_BYTES - 1);
		bufferSize = plannedSize;
		capacity = (u32)std::min<vk::DeviceSize>(bindingSize / OIT_FRAGMENT_BYTES * bindings, 0xffffffffu);
		for (u32 i = 0; i < Bindings; i++)
		{
			if (pixelBuffers[i])
				commandPool->DeferDelete(std::move(pixelBuffers[i]));
			if (i < bindings)
				pixelBuffers[i] = std::unique_ptr<BufferData>(new BufferData(bindingSize,
						vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal));
		}
		if (!pixelCounter)
			pixelCounter = std::unique_ptr<BufferData>(new BufferData(2 * sizeof(u32),
					vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
					vk::MemoryPropertyFlagBits::eDeviceLocal));
		if (pixelCounterReset)
			commandPool->DeferDelete(std::move(pixelCounterReset));
		pixelCounterReset = std::unique_ptr<BufferData>(new BufferData(2 * sizeof(u32), vk::BufferUsageFlagBits::eTransferSrc));
		const u32 reset[2] = { 0, capacity };
		pixelCounterReset->upload(sizeof(reset), reset);
		NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: fragment buffer of %u MB in %u storage buffer%s (room for %u fragments)",
				(u32)(bufferSize >> 20), bindings, bindings > 1 ? "s" : "", capacity);
	}

	vk::UniqueDescriptorSet descSet;
	vk::UniqueDescriptorSetLayout descSetLayout;

	std::unique_ptr<BufferData> pixelBuffers[Bindings];
	vk::DeviceSize plannedSize = 0;
	vk::DeviceSize bufferSize = 0;
	vk::DeviceSize bindingSize = 0;
	u32 bindings = 0;
	u32 capacity = 0;
	std::unique_ptr<BufferData> pixelCounter;
	std::unique_ptr<BufferData> pixelCounterReset;
	std::unique_ptr<FramebufferAttachment> abufferPointerAttachment;
	bool abufferPointerTransitionNeeded = false;
	bool firstFrameAfterInit = false;
	int maxWidth = 0;
	int maxHeight = 0;

	// the counts copied out, Counts for each frame slot; CPU side
	std::unique_ptr<BufferData> counts;
	u32 written[CountSlots] = {};
	const void *countOwner[CountSlots] = {};
	u32 countBands[CountSlots] = {};
	u32 countSlot = CountSlots;
};
