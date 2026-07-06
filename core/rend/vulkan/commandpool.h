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
#include "vulkan_context.h"
#include <memory>

class CommandPool
{
public:
	void Init()
	{
		size_t size = VulkanContext::Instance()->GetSwapChainSize();

		if (commandPools.size() > size)
		{
			commandPools.resize(size);
			fences.resize(size);
		}
		else
		{
			while (commandPools.size() < size)
			{
				commandPools.push_back(VulkanContext::Instance()->GetDevice().createCommandPoolUnique(
						vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, VulkanContext::Instance()->GetGraphicsQueueFamilyIndex())));
				fences.push_back(VulkanContext::Instance()->GetDevice().createFenceUnique(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled)));
			}
		}
		if (freeBuffers.size() != size)
			freeBuffers.resize(size);
		if (inFlightBuffers.size() != size)
			inFlightBuffers.resize(size);
		if (deferredDeletes.size() != size)
			deferredDeletes.resize(size);
	}

	void Term()
	{
		deferredDeletes.clear();
		freeBuffers.clear();
		inFlightBuffers.clear();
		fences.clear();
		commandPools.clear();
	}

	void EndFrame()
	{
		std::vector<vk::CommandBuffer> commandBuffers = vk::uniqueToRaw(inFlightBuffers[index]);
		VulkanContext::Instance()->SubmitCommandBuffers(commandBuffers.size(), commandBuffers.data(), *fences[index]);
	}

	void BeginFrame()
	{
		index = (index + 1) % VulkanContext::Instance()->GetSwapChainSize();
		VulkanContext::Instance()->GetDevice().waitForFences(1, &fences[index].get(), true, UINT64_MAX);
		VulkanContext::Instance()->GetDevice().resetFences(1, &fences[index].get());
		/* The fence above has signaled, so the GPU is done with everything that was
		   submitted for this slot. Anything retired against it is now safe to destroy. */
		deferredDeletes[index].clear();
		std::vector<vk::UniqueCommandBuffer>& inFlight = inFlightBuffers[index];
		std::vector<vk::UniqueCommandBuffer>& freeBuf = freeBuffers[index];
		std::move(inFlight.begin(), inFlight.end(), std::back_inserter(freeBuf));
		inFlight.clear();
		VulkanContext::Instance()->GetDevice().resetCommandPool(*commandPools[index], vk::CommandPoolResetFlagBits::eReleaseResources);
	}

	vk::CommandBuffer Allocate()
	{
		if (freeBuffers[index].empty())
		{
			inFlightBuffers[index].emplace_back(std::move(
					VulkanContext::Instance()->GetDevice().allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(*commandPools[index], vk::CommandBufferLevel::ePrimary, 1))
					.front()));
		}
		else
		{
			inFlightBuffers[index].emplace_back(std::move(freeBuffers[index].back()));
			freeBuffers[index].pop_back();
		}
		return *inFlightBuffers[index].back();
	}

	vk::Fence GetCurrentFence()
	{
		return *fences[index];
	}

	int GetIndex() const
	{
		return index;
	}

	/* Retire a GPU resource so it is destroyed once the current frame's fence has
	   signaled, rather than stalling the whole device with WaitIdle. Used when an
	   attachment must be recreated (grown) while previous frames may still reference
	   the old one. The retired object is freed in BeginFrame when this slot recurs. */
	template<typename T>
	void DeferDelete(std::unique_ptr<T> obj)
	{
		if (obj)
			deferredDeletes[index].emplace_back(obj.release(), [](T *p) { delete p; });
	}

private:
	int index = 0;
	std::vector<std::vector<vk::UniqueCommandBuffer>> freeBuffers;
	std::vector<std::vector<vk::UniqueCommandBuffer>> inFlightBuffers;
	std::vector<vk::UniqueCommandPool> commandPools;
	std::vector<vk::UniqueFence> fences;
	std::vector<std::vector<std::shared_ptr<void>>> deferredDeletes;
};
