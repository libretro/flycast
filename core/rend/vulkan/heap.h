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
#pragma once
#include "vulkan.h"
#include "vk_heap.h"

/* The renderer's device memory. The allocator itself is vk_heap.c, in C;
 * this is what the renderer's classes hold on to. */

class VulkanHeap;

/* A piece of device memory that goes back to the heap when this does. */
class Allocation
{
public:
	Allocation() : heap(nullptr) {}
	Allocation(const Allocation& other) = delete;
	Allocation& operator=(const Allocation& other) = delete;

	Allocation(Allocation&& other) : heap(other.heap), alloc(other.alloc) {
		other.heap = nullptr;
	}

	Allocation& operator=(Allocation&& other) {
		std::swap(this->heap, other.heap);
		std::swap(this->alloc, other.alloc);
		return *this;
	}

	~Allocation() {
		if (heap != nullptr)
			vk_heap_free(heap, &alloc);
	}
	bool IsHostVisible() const {
		return heap != nullptr && alloc.mapped != nullptr;
	}
	// The memory, mapped for the life of the allocation. NULL if it is not host visible.
	void *MapMemory() const {
		return heap != nullptr ? alloc.mapped : nullptr;
	}

private:
	Allocation(vk_heap_t *heap, const vk_heap_alloc_t& alloc) : heap(heap), alloc(alloc) {}

	vk_heap_t *heap;
	vk_heap_alloc_t alloc;

	friend class VulkanHeap;
};

class VulkanHeap
{
public:
	void Init(vk::PhysicalDevice physicalDevice, vk::Device device);
	void Term();

	/* What was large has become small - the renderer has changed size -
	 * and what it had before goes back to the heap over the next frames,
	 * as the frames that used it are done with. TrimSoon() is said then;
	 * Frame(), called once a frame, gives the blocks that have come empty
	 * back to the driver when those frames are long past, all but the
	 * last of each kind. */
	void TrimSoon() { trimIn = 32; }
	void Frame()
	{
		if (trimIn != 0 && --trimIn == 0)
			TrimSpare();
	}

	/* Memory for an image, bound to it. With hostVisible it is memory the
	 * CPU writes the image into; otherwise it is on the device. linear is
	 * the image's tiling. transient is an attachment that never needs
	 * backing memory if the device can do without. */
	Allocation AllocateForImage(vk::Image image, bool linear, bool hostVisible, bool transient = false);

	/* Memory for a buffer, bound to it. With hostVisible the CPU writes
	 * it, or reads it back when readBack is set; otherwise it is on the
	 * device. */
	Allocation AllocateForBuffer(vk::Buffer buffer, bool hostVisible, bool readBack);

private:
	void TrimSpare();
	vk::Result AllocError() const;
	Allocation Allocate(const vk::MemoryRequirements& requirements, VkMemoryPropertyFlags required,
			VkMemoryPropertyFlags preferred, bool linear);

	vk_heap_t heap;
	vk::Device device;
	bool ready = false;
	u32 trimIn = 0;		// frames until the trim, 0 if none is due
};
