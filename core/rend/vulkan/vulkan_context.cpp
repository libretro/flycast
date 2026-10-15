#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Oct 2, 2019

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
#include "vulkan_context.h"
#include "rend/ssaa_pattern.h"
#include <streams/file_stream.h>
#include "hw/pvr/Renderer_if.h"
#include "compiler.h"

VulkanContext *VulkanContext::contextInstance;

static const char *PipelineCacheFileName = "vulkan_pipeline.cache";

const VkApplicationInfo* VkGetApplicationInfo()
{
	// apiVersion is the maximum vulkan version supported by the app
	static vk::ApplicationInfo applicationInfo("Flycast", 1, "Flycast", 1, VK_API_VERSION_1_1);
	return &(VkApplicationInfo&)applicationInfo;
}

bool VkCreateDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                         VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                         const char** required_device_extensions,
                         unsigned num_required_device_extensions,
                         const char** required_device_layers, unsigned num_required_device_layers,
                         const VkPhysicalDeviceFeatures* required_features)
{
	vulkan_symbol_wrapper_init(get_instance_proc_addr);
	vulkan_symbol_wrapper_load_global_symbols();
	vulkan_symbol_wrapper_load_core_symbols(instance);
	VULKAN_SYMBOL_WRAPPER_LOAD_INSTANCE_EXTENSION_SYMBOL(instance, vkGetPhysicalDeviceSurfaceSupportKHR);

	vk::PhysicalDevice physicalDevice(gpu);
	if (gpu == VK_NULL_HANDLE)
	{
		// Choose a discrete gpu if there's one, otherwise just pick the first one
		vk::Instance vkinstance(instance);
		const auto devices = vkinstance.enumeratePhysicalDevices();
		for (const auto& phyDev : devices)
		{
			vk::PhysicalDeviceProperties props;
			phyDev.getProperties(&props);
			if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
			{
				physicalDevice = phyDev;
				break;
			}
		}
		if (!physicalDevice)
			physicalDevice = vkinstance.enumeratePhysicalDevices().front();
	}
	context->gpu = (VkPhysicalDevice)physicalDevice;
	std::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

	// get the first index into queueFamilyProperties which supports graphics and compute
	context->queue_family_index = (u32)std::distance(queueFamilyProperties.begin(),
			std::find_if(queueFamilyProperties.begin(), queueFamilyProperties.end(),
					[](vk::QueueFamilyProperties const& qfp) { return (qfp.queueFlags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute))
							== (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute); }));
	verify(context->queue_family_index < queueFamilyProperties.size());

	if (surface != VK_NULL_HANDLE)
	{
		// determine a queue family index that supports present
		// first check if the queue_family_index is good enough
		vk::SurfaceKHR vksurface(surface);
		context->presentation_queue_family_index = physicalDevice.getSurfaceSupportKHR(context->queue_family_index, vksurface) ? context->queue_family_index : queueFamilyProperties.size();
		if (context->presentation_queue_family_index == queueFamilyProperties.size())
		{
			// the queue_family_index doesn't support present -> look for an other family index that supports both graphics, compute and present
			for (size_t i = 0; i < queueFamilyProperties.size(); i++)
			{
				if ((queueFamilyProperties[i].queueFlags & (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)) == (vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute)
						&& physicalDevice.getSurfaceSupportKHR((u32)i, vksurface))
				{
					context->queue_family_index = (u32)i;
					context->presentation_queue_family_index = (u32)i;
					break;
				}
			}
			if (context->presentation_queue_family_index == queueFamilyProperties.size())
			{
				// there's nothing like a single family index that supports both graphics/compute and present -> look for an other family index that supports present
				DEBUG_LOG(RENDERER, "Using separate Graphics and Present queue families");
				for (size_t i = 0; i < queueFamilyProperties.size(); i++)
				{
					if (physicalDevice.getSurfaceSupportKHR((u32)i, vksurface))
					{
						context->presentation_queue_family_index = (u32)i;
						break;
					}
				}
			}
		}
		if (context->queue_family_index == queueFamilyProperties.size() || context->presentation_queue_family_index == queueFamilyProperties.size())
		{
			ERROR_LOG(RENDERER, "Could not find a queue for graphics or present");
			return false;
		}
		if (context->queue_family_index == context->presentation_queue_family_index)
			DEBUG_LOG(RENDERER, "Using Graphics+Present queue family");
		else
			DEBUG_LOG(RENDERER, "Using distinct Graphics and Present queue families");
	}

	vk::PhysicalDeviceFeatures supportedFeatures;
	physicalDevice.getFeatures(&supportedFeatures);
	bool fragmentStoresAndAtomics = supportedFeatures.fragmentStoresAndAtomics;
	VulkanContext::Instance()->samplerAnisotropy = supportedFeatures.samplerAnisotropy;
	VulkanContext::Instance()->storageImageExtendedFormats = supportedFeatures.shaderStorageImageExtendedFormats;
	VulkanContext::Instance()->multiDrawIndirect = supportedFeatures.multiDrawIndirect;
	VulkanContext::Instance()->drawIndirectFirstInstance = supportedFeatures.drawIndirectFirstInstance;
	VulkanContext::Instance()->drawIndirectCount = false;
	// (SSAA's shading per sample: see VulkanContext::SsaaSamples())
	VulkanContext::Instance()->sampleRateShading = supportedFeatures.sampleRateShading;

	// Enable VK_KHR_dedicated_allocation if available
	bool getMemReq2Supported = false;
	VulkanContext::Instance()->dedicatedAllocationSupported = false;
	std::vector<const char *> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	for (int i = 0; i < num_required_device_extensions; i++)
		deviceExtensions.push_back(required_device_extensions[i]);
	for (const auto& property : physicalDevice.enumerateDeviceExtensionProperties())
	{
		if (!strcmp(property.extensionName, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
		{
			deviceExtensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
			getMemReq2Supported = true;
		}
		else if (!strcmp(property.extensionName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))
		{
			deviceExtensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
			VulkanContext::Instance()->dedicatedAllocationSupported = true;
		}
		else if (!strcmp(property.extensionName, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME))
		{
			// (GPU-side TA processing's translucent draws: see TaDecoder)
			deviceExtensions.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
			VulkanContext::Instance()->drawIndirectCount = true;
		}
	}
	VulkanContext::Instance()->dedicatedAllocationSupported &= getMemReq2Supported;

	// create a Device
	float queuePriority = 1.0f;
	vk::DeviceQueueCreateInfo deviceQueueCreateInfos[] = {
			vk::DeviceQueueCreateInfo(vk::DeviceQueueCreateFlags(), context->queue_family_index, 1, &queuePriority),
			vk::DeviceQueueCreateInfo(vk::DeviceQueueCreateFlags(), context->presentation_queue_family_index, 1, &queuePriority),
	};
	vk::PhysicalDeviceFeatures features(*required_features);
	if (fragmentStoresAndAtomics)
		features.fragmentStoresAndAtomics = true;
	if (VulkanContext::Instance()->samplerAnisotropy)
		features.samplerAnisotropy = true;
	if (VulkanContext::Instance()->storageImageExtendedFormats)
		features.shaderStorageImageExtendedFormats = true;
	if (VulkanContext::Instance()->multiDrawIndirect)
		features.multiDrawIndirect = true;
	if (VulkanContext::Instance()->drawIndirectFirstInstance)
		features.drawIndirectFirstInstance = true;
	if (VulkanContext::Instance()->sampleRateShading)
		features.sampleRateShading = true;
	vk::Device device = physicalDevice.createDevice(vk::DeviceCreateInfo(vk::DeviceCreateFlags(),
			context->queue_family_index == context->presentation_queue_family_index ? 1 : 2, deviceQueueCreateInfos,
			num_required_device_layers, required_device_layers, deviceExtensions.size(), &deviceExtensions[0], &features));
	context->device = (VkDevice)device;
	vulkan_symbol_wrapper_load_core_device_symbols(context->device);

	// Queues
	context->queue = (VkQueue)device.getQueue(context->queue_family_index, 0);
	context->presentation_queue = (VkQueue)device.getQueue(context->presentation_queue_family_index, 0);

	return true;
}

bool VulkanContext::Init(retro_hw_render_interface_vulkan *retro_render_if)
{
	if (retro_render_if->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN
			|| retro_render_if->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
		return false;
	this->retro_render_if = retro_render_if;
	swapChainSize = 1;
	for (u32 m = retro_render_if->get_sync_index_mask(retro_render_if->handle); m >>= 1; )
		swapChainSize++;

	instance = vk::Instance(retro_render_if->instance);
	physicalDevice = vk::PhysicalDevice(retro_render_if->gpu);
	device = vk::Device(retro_render_if->device);
	queue = vk::Queue(retro_render_if->queue);

	vk::PhysicalDeviceProperties *properties;
	static vk::PhysicalDeviceProperties props;
	physicalDevice.getProperties(&props);
	
	NOTICE_LOG(RENDERER, "GPU Supports Vulkan API: %u.%u.%u",
						  VK_VERSION_MAJOR(props.apiVersion),
						  VK_VERSION_MINOR(props.apiVersion),
						  VK_VERSION_PATCH(props.apiVersion));
	if (VK_VERSION_MINOR(props.apiVersion) >= 1 && ::vkGetPhysicalDeviceFormatProperties2 != nullptr)
	{
		NOTICE_LOG(RENDERER, "GPU Supports vkGetPhysicalDeviceProperties2");
		static vk::PhysicalDeviceProperties2 properties2;
		vk::PhysicalDeviceMaintenance3Properties properties3;
		properties2.pNext = &properties3;
		physicalDevice.getProperties2(&properties2);
		properties = &properties2.properties;
		maxMemoryAllocationSize = properties3.maxMemoryAllocationSize;
	}
	else
	{
		properties = &props;
		maxMemoryAllocationSize = 0xFFFFFFFFu;
	}
	uniformBufferAlignment = properties->limits.minUniformBufferOffsetAlignment;
	storageBufferAlignment = properties->limits.minStorageBufferOffsetAlignment;
	maxStorageBufferRange = properties->limits.maxStorageBufferRange;
	maxSamplerAnisotropy =  properties->limits.maxSamplerAnisotropy;
	vendorID = properties->vendorID;
	apiVersion = props.apiVersion;
	maxDrawIndirectCount = properties->limits.maxDrawIndirectCount;
	cmdDrawIndexedIndirectCount = drawIndirectCount
			? (PFN_vkCmdDrawIndexedIndirectCountKHR)device.getProcAddr("vkCmdDrawIndexedIndirectCountKHR") : nullptr;
	QueryVramBudget();
	// what SSAA can have: the sample counts colour, depth and stencil attachments can all have, and memory that is only there if used
	ssaaSampleCounts = (u32)(properties->limits.framebufferColorSampleCounts & properties->limits.framebufferDepthSampleCounts
			& properties->limits.framebufferStencilSampleCounts);
	lazyMemory = false;
	{
		const vk::PhysicalDeviceMemoryProperties memory = physicalDevice.getMemoryProperties();
		for (u32 i = 0; i < memory.memoryTypeCount; i++)
			if (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eLazilyAllocated)
				lazyMemory = true;
	}

	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(vk::Format::eR5G5B5A1UnormPack16);
	if ((formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc))
		optimalTilingSupported1555 = true;
	else
		NOTICE_LOG(RENDERER, "eR5G5B5A1UnormPack16 not supported for optimal tiling");
	formatProperties = physicalDevice.getFormatProperties(vk::Format::eR5G6B5UnormPack16);
	if ((formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc))
		optimalTilingSupported565 = true;
	else
		NOTICE_LOG(RENDERER, "eR5G6B5UnormPack16 not supported for optimal tiling");
	formatProperties = physicalDevice.getFormatProperties(vk::Format::eR4G4B4A4UnormPack16);
	if ((formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst)
			&& (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc))
		optimalTilingSupported4444 = true;
	else
		NOTICE_LOG(RENDERER, "eR4G4B4A4UnormPack16 not supported for optimal tiling");

	ShaderCompiler::Init();

   // Descriptor pool
	vk::DescriptorPoolSize pool_sizes[] =
	{
		{ vk::DescriptorType::eSampler, 2 },
      { vk::DescriptorType::eCombinedImageSampler, 4000 },
      { vk::DescriptorType::eSampledImage, 2 },
      { vk::DescriptorType::eStorageImage, 12 },
      { vk::DescriptorType::eUniformTexelBuffer, 2 },
		{ vk::DescriptorType::eStorageTexelBuffer, 2 },
      { vk::DescriptorType::eUniformBuffer, 36 },
      { vk::DescriptorType::eStorageBuffer, 36 },
      { vk::DescriptorType::eUniformBufferDynamic, 2 },
      { vk::DescriptorType::eStorageBufferDynamic, 2 },
      { vk::DescriptorType::eInputAttachment, 36 }
   };
	descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
   		10000, ARRAY_SIZE(pool_sizes), pool_sizes));

   std::string cachePath = get_writable_data_path(PipelineCacheFileName);
   RFILE *f = filestream_open(cachePath.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (f == nullptr)
   	pipelineCache = device.createPipelineCacheUnique(vk::PipelineCacheCreateInfo());
   else
   {
   	size_t cacheSize = (size_t)filestream_get_size(f);
   	u8 *cacheData = new u8[cacheSize];
   	if ((size_t)filestream_read(f, cacheData, cacheSize) != cacheSize)
   		cacheSize = 0;
   	filestream_close(f);
		pipelineCache = device.createPipelineCacheUnique(vk::PipelineCacheCreateInfo(vk::PipelineCacheCreateFlags(), cacheSize, cacheData));
		delete [] cacheData;
		INFO_LOG(RENDERER, "Vulkan pipeline cache loaded from %s: %zd bytes", cachePath.c_str(), cacheSize);
   }
   heap.Init(physicalDevice, device);
   FindDepthFormat();

	retro_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	retro_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	retro_image.create_info.pNext = nullptr;
	retro_image.create_info.format = (VkFormat)colorFormat;
	retro_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	retro_image.create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	retro_image.create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	retro_image.create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	retro_image.create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	retro_image.create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	retro_image.create_info.subresourceRange.baseArrayLayer = 0;
	retro_image.create_info.subresourceRange.layerCount = 1;
	retro_image.create_info.subresourceRange.baseMipLevel = 0;
	retro_image.create_info.subresourceRange.levelCount = 1;
	retro_image.create_info.flags = 0;

	return true;
}

/* The budget of the largest device-local heap, by VK_EXT_memory_budget
 * when the device has it (a physical device query: the extension does not
 * have to be enabled on the device the frontend made), and half that heap
 * when it has not */
void VulkanContext::QueryVramBudget()
{
	bool haveBudget = false;
	if (::vkGetPhysicalDeviceMemoryProperties2 != nullptr)
		for (const auto& property : physicalDevice.enumerateDeviceExtensionProperties())
			if (!strcmp(property.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
			{
				haveBudget = true;
				break;
			}

	vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget;
	vk::PhysicalDeviceMemoryProperties memory;
	if (haveBudget)
	{
		vk::PhysicalDeviceMemoryProperties2 memory2;
		memory2.pNext = &budget;
		physicalDevice.getMemoryProperties2(&memory2);
		memory = memory2.memoryProperties;
	}
	else
		memory = physicalDevice.getMemoryProperties();

	u32 heap = VK_MAX_MEMORY_HEAPS;
	for (u32 i = 0; i < memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
		if ((memory.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
				&& (heap == VK_MAX_MEMORY_HEAPS || memory.memoryHeaps[i].size > memory.memoryHeaps[heap].size))
			heap = i;
	if (heap == VK_MAX_MEMORY_HEAPS)
		heap = 0;	// (there is always a heap, and one at least is device-local)

	vramBudget = VramBudget();
	if (haveBudget && budget.heapBudget[heap] != 0)
	{
		vramBudget.budget_kib = vram_kib(budget.heapBudget[heap]);
		vramBudget.usage_kib = vram_kib(budget.heapUsage[heap]);
		vramBudget.exact = true;
	}
	else
		vramBudget.budget_kib = vram_kib(memory.memoryHeaps[heap].size / 2);
	vram_budget_log("Vulkan", vramBudget);
}

u32 VulkanContext::SsaaSamples(u32 requested, u32 perSampleKib, u32 fixedKib, bool transient, const char *what) const
{
	if (requested <= 1)
		return 1;
	/* Shaded per sample, or it is not supersampling: a device the frontend
	 * made has no sample rate shading the core knows of */
	if (!sampleRateShading)
	{
		WARN_LOG(RENDERER, "%s: SSAA off: the device was made without sample rate shading", what);
		return 1;
	}
	const u32 device = ssaa_device_samples(requested, ssaaSampleCounts);
	/* Attachments that are never stored take no memory where the device
	 * has memory that is only backed when used (tilers) */
	const bool free = transient && lazyMemory;
	const u32 samples = ssaa_clamp_samples(device, vramBudget.available_kib(), fixedKib, free ? 0 : perSampleKib);
	NOTICE_LOG(RENDERER, "%s: SSAA %u samples a pixel (asked %u, the device can draw %u; %u MB a sample%s, %u MB of the budget left)",
			what, samples, requested, device, perSampleKib >> 10, free ? " if backed" : "", vramBudget.available_kib() >> 10);
	return samples;
}

u32 VulkanContext::GetDepthBytes() const
{
	return depthFormat == vk::Format::eD32SfloatS8Uint ? 8 : 4;
}

vk::Format VulkanContext::FindDepthFormat()
{
	const vk::Format depthFormats[] = { vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint, vk::Format::eD16UnormS8Uint };
	vk::ImageTiling tiling;
	depthFormat = vk::Format::eUndefined;
	for (int i = 0; i < ARRAY_SIZE(depthFormats); i++)
	{
		vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(depthFormats[i]);

		if (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
		{
			tiling = vk::ImageTiling::eOptimal;
			depthFormat = depthFormats[i];
			break;
		}
	}
	if (depthFormat == vk::Format::eUndefined)
	{
		// Try to find a linear format
		for (int i = 0; i < ARRAY_SIZE(depthFormats); i++)
		{
			vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(depthFormats[i]);

			if (formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
			{
				tiling = vk::ImageTiling::eLinear;
				depthFormat = depthFormats[i];
				break;
			}
		}
		if (depthFormat == vk::Format::eUndefined)
			die("No supported depth/stencil format found");
	}
	NOTICE_LOG(RENDERER, "Using depth format %s tiling %s", vk::to_string(depthFormat).c_str(), vk::to_string(tiling).c_str());

	return depthFormat;
}

void VulkanContext::PresentFrame(vk::Image image, vk::ImageView imageView, vk::Offset2D extent)
{
	retro_image.image_view = (VkImageView)imageView;
	retro_image.create_info.image = (VkImage)image;
	retro_render_if->set_image(retro_render_if->handle, &retro_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
	// once a frame: memory left empty by a change of size goes back to the driver
	heap.Frame();
}

void VulkanContext::Term()
{
	if (device)
	{
		device.waitIdle();
		if (pipelineCache)
		{
			std::vector<u8> cacheData = device.getPipelineCacheData(*pipelineCache);
			if (!cacheData.empty())
			{
				std::string cachePath = get_writable_data_path(PipelineCacheFileName);
				RFILE *f = filestream_open(cachePath.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
				if (f != nullptr)
				{
					(void)filestream_write(f, &cacheData[0], cacheData.size());
					filestream_close(f);
				}
			}
		}
	}
	ShaderCompiler::Term();
	descriptorPool.reset();
	heap.Term();
	pipelineCache.reset();
}
