#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
	Created on: Nov 24, 2019

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
/* The allocator is created VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED: the
 * renderer is its only user, on one thread, so VMA never takes the mutexes
 * it keeps. Give it ones that do nothing rather than std::mutex and the
 * platform's read-write lock. */
class VmaNoMutex
{
public:
	void Lock() {}
	void Unlock() {}
};
#define VMA_MUTEX VmaNoMutex

class VmaNoRWMutex
{
public:
	void LockRead() {}
	void UnlockRead() {}
	void LockWrite() {}
	void UnlockWrite() {}
};
#define VMA_RW_MUTEX VmaNoRWMutex

#define VMA_IMPLEMENTATION
#include "vulkan.h"
#include "vmallocator.h"
#include "vulkan_context.h"

#if HOST_CPU == CPU_ARM
__attribute__((pcs("aapcs-vfp")))
#endif
static void vmaAllocateDeviceMemoryCallback(
    VmaAllocator      allocator,
    uint32_t          memoryType,
    VkDeviceMemory    memory,
    VkDeviceSize      size)
{
	DEBUG_LOG(RENDERER, "VMAAllocator: %llu bytes allocated (type %d)", (unsigned long long)size, memoryType);
}

#if HOST_CPU == CPU_ARM
__attribute__((pcs("aapcs-vfp")))
#endif
static void vmaFreeDeviceMemoryCallback(
    VmaAllocator      allocator,
    uint32_t          memoryType,
    VkDeviceMemory    memory,
    VkDeviceSize      size)
{
	DEBUG_LOG(RENDERER, "VMAAllocator: %llu bytes freed (type %d)", (unsigned long long)size, memoryType);
}

static const VmaDeviceMemoryCallbacks memoryCallbacks = { vmaAllocateDeviceMemoryCallback, vmaFreeDeviceMemoryCallback };

void VMAllocator::Init(vk::PhysicalDevice physicalDevice, vk::Device device)
{
	verify(allocator == VK_NULL_HANDLE);
	VmaAllocatorCreateInfo allocatorInfo = { VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT };
	if (VulkanContext::Instance()->SupportsDedicatedAllocation())
		allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;

	allocatorInfo.physicalDevice = (VkPhysicalDevice)physicalDevice;
	allocatorInfo.device = (VkDevice)device;
#if !defined(NDEBUG) || defined(DEBUGFAST)
	allocatorInfo.pDeviceMemoryCallbacks = &memoryCallbacks;
#endif
	VkResult rc = vmaCreateAllocator(&allocatorInfo, &allocator);
	if (rc != VK_SUCCESS)
		throwResultException((vk::Result)rc, "vmaCreateAllocator failed");
}
