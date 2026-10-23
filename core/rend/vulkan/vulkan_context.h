/*
    Created on: Nov 29, 2019

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
#include "vulkan.h"
#include "heap.h"
#include "quad.h"
#include "rend/TexCache.h"
#include "rend/vram_budget.h"
#include "libretro_vulkan.h"
#include "pso.h"

extern int screen_width, screen_height;

#define VENDOR_AMD 0x1022
#define VENDOR_ARM 0x13B5
#define VENDOR_INTEL 0x8086
#define VENDOR_NVIDIA 0x10DE
#define VENDOR_QUALCOMM 0x5143

class VulkanContext
{
public:
	VulkanContext() { verify(contextInstance == nullptr); contextInstance = this; }
	~VulkanContext() { verify(contextInstance == this); contextInstance = nullptr; }

	bool Init(retro_hw_render_interface_vulkan *render_if);
	void Term();

	u32 GetGraphicsQueueFamilyIndex() const { return retro_render_if->queue_index; }
	void SetWindowSize(u32 width, u32 height) { this->width = screen_width = width; this->height = screen_height = height; }
	void PresentFrame(vk::Image image, vk::ImageView imageView, vk::Offset2D extent);

	vk::PhysicalDevice GetPhysicalDevice() const { return physicalDevice; }
	vk::Device GetDevice() const { return device; }
	vk::PipelineCache GetPipelineCache() const { return pipelineCache; }
	vk::DescriptorPool GetDescriptorPool() const { return *descriptorPool; }
	vk::Extent2D GetViewPort() const { return { width, height }; }
	/* How many frames the renderer keeps in its rings (command pools,
	 * buffers, attachments), as the frontend's swap chain was when the
	 * context was made. The frontend can make its chain longer later (when
	 * fast-forward changes how it presents): the rings were made at this
	 * size and stay at it. What is kept per frontend frame index
	 * (GetCurrentImageIndex) grows with the index instead. */
	int GetSwapChainSize() const { return swapChainSize; }
	int GetCurrentImageIndex() const { return retro_render_if->get_sync_index(retro_render_if->handle); }
	// FIXME that's not quite correct
	void WaitIdle() const { retro_render_if->wait_sync_index(retro_render_if->handle); }
	void SubmitCommandBuffers(u32 bufferCount, vk::CommandBuffer *buffers, vk::Fence fence) {
		retro_render_if->lock_queue(retro_render_if->handle);
		queue.submit(vk::SubmitInfo(0, nullptr, nullptr, bufferCount, buffers, 0, nullptr), fence);
		retro_render_if->unlock_queue(retro_render_if->handle);
	}
	vk::DeviceSize GetUniformBufferAlignment() const { return uniformBufferAlignment; }
	vk::DeviceSize GetStorageBufferAlignment() const { return storageBufferAlignment; }
	bool IsFormatSupported(TextureType textureType)
	{
		switch (textureType)
		{
		case TextureType::_4444:
			return optimalTilingSupported4444;
		case TextureType::_565:
			return optimalTilingSupported565;
		case TextureType::_5551:
			return optimalTilingSupported1555;
		default:
			return true;
		}
	}
	std::string GetDriverName() const { vk::PhysicalDeviceProperties props; physicalDevice.getProperties(&props); return props.deviceName; }
	std::string GetDriverVersion() const {
		vk::PhysicalDeviceProperties props;
		physicalDevice.getProperties(&props);

		return std::to_string(VK_VERSION_MAJOR(props.driverVersion))
			+ "." + std::to_string(VK_VERSION_MINOR(props.driverVersion))
			+ "." + std::to_string(VK_VERSION_PATCH(props.driverVersion));
	}
	vk::Format GetColorFormat() const { return colorFormat; }
	vk::Format GetDepthFormat() const { return depthFormat; }
	static VulkanContext *Instance() { return contextInstance; }
	bool SupportsSamplerAnisotropy() const { return samplerAnisotropy; }
	bool SupportsShaderInt64() const { return shaderInt64; }
	bool SupportsSampledImageArrayIndexing() const { return sampledImageArrayIndexing; }
	bool SupportsNonUniformTextureIndexing() const { return nonUniformTextureIndexing; }
	bool SupportsSubgroupBallotCompute() const { return subgroupBallotCompute; }
	bool SupportsStorageImageExtendedFormats() const { return storageImageExtendedFormats; }
	u32 GetApiVersion() const { return apiVersion; }
	bool SupportsDedicatedAllocation() const { return dedicatedAllocationSupported; }
	VulkanHeap& GetHeap() { return heap; }
	vk::DeviceSize GetMaxMemoryAllocationSize() const { return maxMemoryAllocationSize; }
	u32 GetMaxStorageBufferRange() const { return maxStorageBufferRange; }
	f32 GetMaxSamplerAnisotropy() const { return samplerAnisotropy ? maxSamplerAnisotropy : 1.f; }
	u32 GetVendorID() const { return vendorID; }
	// what video memory there was to use when the context was set up
	const VramBudget& GetVramBudget() const { return vramBudget; }
	/* Indirect draws, as the device was made with them: several draws in
	 * one (the most a call can take), firstInstance in their arguments, and
	 * the count read from a buffer (vkCmdDrawIndexedIndirectCountKHR(),
	 * null without it) */
	bool SupportsMultiDrawIndirect() const { return multiDrawIndirect; }
	u32 GetMaxDrawIndirectCount() const { return multiDrawIndirect ? maxDrawIndirectCount : 1; }
	bool SupportsDrawIndirectFirstInstance() const { return drawIndirectFirstInstance; }
	PFN_vkCmdDrawIndexedIndirectCountKHR GetDrawIndexedIndirectCount() const { return cmdDrawIndexedIndirectCount; }
	/* SSAA: the samples a pixel gets of the @requested, at most what the
	 * device can draw with, and what fits in the video memory budget with
	 * @perSampleKib for each sample after @fixedKib of other pools - none
	 * where the attachments are @transient and the device has memory that
	 * is only backed when used; 1 for none. One line in the log, for @what. */
	u32 SsaaSamples(u32 requested, u32 perSampleKib, u32 fixedKib, bool transient, const char *what) const;
	// bytes a pixel of the depth and stencil attachments, at most
	u32 GetDepthBytes() const;
	bool HasLazyMemory() const { return lazyMemory; }
	/* The video memory budget read again, and logged: what is in use now */
	void LogVramUsage() { QueryVramBudget(); }
	/* The dynamic states the general pipelines are drawn with, as the
	 * device was made with them (pso.h) */
	const pso_ext::Functions& DynamicState() const { return dynamicState; }
	// the keys of the pipelines made, kept with the pipeline cache
	PsoKeyLog& PsoKeys() { return psoKeys; }
	/* The pipeline cache written to the system directory, with the
	 * worker's merged in, if pipelines were made since it last was */
	void SavePipelineCache();

private:
	vk::Format FindDepthFormat();
	void QueryVramBudget();

	VulkanHeap heap;
	int swapChainSize = 1;
	u32 width = 0;
	u32 height = 0;

	vk::DeviceSize uniformBufferAlignment = 0;
	vk::DeviceSize storageBufferAlignment = 0;
	u32 maxStorageBufferRange = 0;
	vk::DeviceSize maxMemoryAllocationSize = 0;
	bool optimalTilingSupported565 = false;
	bool optimalTilingSupported1555 = false;
	bool optimalTilingSupported4444 = false;
public:
	bool samplerAnisotropy = false;
	/* 16- and 8-bit storage images (R16_UINT, R8_UINT), for texture decoding:
	 * enabled on the device when the core made it and the GPU has them */
	bool storageImageExtendedFormats = false;
	f32 maxSamplerAnisotropy = 0.f;
	bool dedicatedAllocationSupported = false;
	/* enabled on the device when the core made it and the GPU has them;
	 * a device the frontend made has none of them */
	bool multiDrawIndirect = false;
	bool drawIndirectFirstInstance = false;
	bool drawIndirectCount = false;		// VK_KHR_draw_indirect_count
	bool sampleRateShading = false;
	bool postDepthCoverage = false;		// VK_EXT_post_depth_coverage: SSAA's per-pixel lists (vulkan/shaders/oit_sample_depth.glsl)
	bool shaderInt64 = false;		// enabled on the device when it has it
	bool sampledImageArrayIndexing = false;	// ...and this, shaderSampledImageArrayDynamicIndexing
	bool nonUniformTextureIndexing = false;	// ...and shaderSampledImageArrayNonUniformIndexing (VK_EXT_descriptor_indexing)
	bool subgroupBallotCompute = false;	// Vulkan 1.1's subgroup ballot in compute shaders
	/* VK_EXT_extended_dynamic_state and _state3 (pso.h), and
	 * VK_EXT_pipeline_creation_cache_control: caches without a lock */
	bool extendedDynamicState = false;
	bool extendedDynamicState3 = false;
	bool pipelineCacheControl = false;
private:
	void LoadPipelineCache();
	pso_ext::Functions dynamicState = {};
	PsoKeyLog psoKeys;
	u32 savedBuilt = 0;
	u32 maxDrawIndirectCount = 1;
	PFN_vkCmdDrawIndexedIndirectCountKHR cmdDrawIndexedIndirectCount = nullptr;
	u32 vendorID = 0;
	u32 apiVersion = 0;	// the device's
	VramBudget vramBudget;
	u32 ssaaSampleCounts = 1;	// VkSampleCountFlags
	bool lazyMemory = false;
	vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm;

	vk::UniqueDescriptorPool descriptorPool;

	vk::Format depthFormat = vk::Format::eUndefined;

	vk::PipelineCache pipelineCache;

	retro_hw_render_interface_vulkan *retro_render_if = nullptr;
	vk::Instance instance;
	vk::PhysicalDevice physicalDevice;
	vk::Device device;
	vk::Queue queue;

	retro_vulkan_image retro_image;

	static VulkanContext *contextInstance;
};

const VkApplicationInfo* VkGetApplicationInfo();
bool VkCreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                         VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         const char** required_device_extensions,
                         unsigned num_required_device_extensions,
                         const char** required_device_layers, unsigned num_required_device_layers,
                         const VkPhysicalDeviceFeatures* required_features);
bool VkCreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                         VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         retro_vulkan_create_device_wrapper_t create_device_wrapper, void *opaque);
