/*
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
#include "heap.h"
#include "vulkan_context.h"

static const VkMemoryPropertyFlags HOST_MEMORY =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

void VulkanHeap::Init(vk::PhysicalDevice physicalDevice, vk::Device device)
{
	verify(!ready);

	const vk::PhysicalDeviceMemoryProperties props = physicalDevice.getMemoryProperties();

	/* One block is 64 MB, or an eighth of the device's memory where that
	 * is less. */
	VkDeviceSize largest = 0;
	for (u32 i = 0; i < props.memoryHeapCount; i++)
		if ((VkDeviceSize)props.memoryHeaps[i].size > largest)
			largest = props.memoryHeaps[i].size;
	VkDeviceSize blockSize = 64u * 1024u * 1024u;
	if (largest / 8 < blockSize)
		blockSize = std::max<VkDeviceSize>((largest / 8) & ~(VkDeviceSize)0xFFFFF, 4u * 1024u * 1024u);

	vk_heap_fns_t fns;
	fns.allocate_memory = vkAllocateMemory;
	fns.free_memory = vkFreeMemory;
	fns.map_memory = vkMapMemory;
	fns.unmap_memory = vkUnmapMemory;

	if (!vk_heap_init(&heap, (VkDevice)device, (const VkPhysicalDeviceMemoryProperties *)&props, &fns, blockSize, 0))
		throwResultException(vk::Result::eErrorInitializationFailed, "vk_heap_init failed");
	this->device = device;
	ready = true;

	/* The first block for textures and render targets and the first one
	 * for the buffers the CPU fills are taken now, so that a frame gets
	 * its memory as an offset and not from the driver. */
	vk_heap_reserve(&heap, ~0u, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1);
	vk_heap_reserve(&heap, ~0u, HOST_MEMORY, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1);

	NOTICE_LOG(RENDERER, "Vulkan heap: %u MB blocks, %u MB reserved", (u32)(blockSize >> 20), (u32)(heap.bytes_reserved >> 20));
}

void VulkanHeap::Term()
{
	if (ready)
	{
		vk_heap_shutdown(&heap);
		ready = false;
	}
}

Allocation VulkanHeap::Allocate(const vk::MemoryRequirements& requirements, VkMemoryPropertyFlags required,
		VkMemoryPropertyFlags preferred, bool linear)
{
	vk_heap_alloc_t alloc;
	if (!vk_heap_alloc(&heap, (const VkMemoryRequirements *)&requirements, required, preferred, linear, &alloc))
		throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_heap_alloc failed");
	return Allocation(&heap, alloc);
}

Allocation VulkanHeap::AllocateForImage(vk::Image image, bool linear, bool hostVisible, bool transient)
{
	const vk::MemoryRequirements requirements = device.getImageMemoryRequirements(image);
	vk_heap_alloc_t alloc;

	if (hostVisible)
	{
		if (!vk_heap_alloc(&heap, (const VkMemoryRequirements *)&requirements, HOST_MEMORY,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, linear, &alloc))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_heap_alloc failed");
	}
	/* A transient attachment takes memory the device only backs if it has
	 * to, where there is such a thing. */
	else if (!transient
			|| !vk_heap_alloc(&heap, (const VkMemoryRequirements *)&requirements,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0, linear, &alloc))
	{
		if (!vk_heap_alloc(&heap, (const VkMemoryRequirements *)&requirements, 0,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, linear, &alloc))
			throwResultException(vk::Result::eErrorOutOfDeviceMemory, "vk_heap_alloc failed");
	}

	Allocation allocation(&heap, alloc);
	device.bindImageMemory(image, vk::DeviceMemory(alloc.memory), alloc.offset);
	return allocation;
}

Allocation VulkanHeap::AllocateForBuffer(vk::Buffer buffer, bool hostVisible, bool readBack)
{
	const vk::MemoryRequirements requirements = device.getBufferMemoryRequirements(buffer);
	VkMemoryPropertyFlags required = 0;
	VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	if (hostVisible)
	{
		required = HOST_MEMORY;
		/* What the CPU reads back wants the host's cache between it and
		 * the memory; device memory seen through the bus is write-combined
		 * and reading it is slow. What the CPU only writes goes as close
		 * to the device as there is. */
		if (readBack)
			preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	}

	Allocation allocation = Allocate(requirements, required, preferred, true);
	device.bindBufferMemory(buffer, vk::DeviceMemory(allocation.alloc.memory), allocation.alloc.offset);
	return allocation;
}
