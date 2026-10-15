/* Textures made on the GPU from their bytes in video memory: see
 * tex_decoder.h */
#include "tex_decoder.h"
#include "commandpool.h"
#include "vulkan_context.h"

#include <algorithm>

/* The kernels, by TEXD_OUT8, 16, 32, as SPIR-V made from rend/texdecode.h
 * when the core was built: tools/rend/kernels_spv.sh, which checks that
 * they still are its. Nothing is compiled when the core runs. */
#include "rend/texdecode_spv.inc"

/* An image of @format the kernel writes to through a @view view: optimal,
 * sampled, written and copied to and from */
static bool Storable(vk::PhysicalDevice physicalDevice, vk::Format format, vk::Format view, bool extended)
{
	const vk::FormatProperties viewProps = physicalDevice.getFormatProperties(view);
	if (!(viewProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage))
		return false;
	const vk::FormatProperties props = physicalDevice.getFormatProperties(format);
	const vk::FormatFeatureFlags sampled = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
	if ((props.optimalTilingFeatures & sampled) != sampled)
		return false;
	VkImageFormatProperties ifp;
	VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (extended ? VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0);
	return vkGetPhysicalDeviceImageFormatProperties((VkPhysicalDevice)physicalDevice, (VkFormat)format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			flags, &ifp) == VK_SUCCESS;
}

/* A format a planar texture's texels can be copied into as they are */
static bool Copyable(vk::PhysicalDevice physicalDevice, vk::Format format)
{
	const vk::FormatProperties props = physicalDevice.getFormatProperties(format);
	const vk::FormatFeatureFlags needed = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eSampledImageFilterLinear
			| vk::FormatFeatureFlagBits::eTransferDst;
	return (props.optimalTilingFeatures & needed) == needed;
}

void TexDecoder::Init()
{
	VulkanContext *context = VulkanContext::Instance();
	const int swapChain = context->GetSwapChainSize();

	if (ready && frames == swapChain)
		return;
	Term();

	const vk::PhysicalDevice physicalDevice = context->GetPhysicalDevice();
	const vk::Device device = context->GetDevice();
	// the image formats the kernel writes through views of another format need Vulkan 1.1
	const bool v11 = VK_VERSION_MAJOR(context->GetApiVersion()) > 1 || VK_VERSION_MINOR(context->GetApiVersion()) >= 1;
	const bool extended = v11 && context->SupportsStorageImageExtendedFormats();
#ifdef MSB_FIRST
	// the kernel reads the bytes little-endian
	if (true)
#else
	if (!v11)
#endif
	{
		INFO_LOG(RENDERER, "Vulkan: textures are converted on the CPU");
		return;
	}
	takes32 = Storable(physicalDevice, vk::Format::eR8G8B8A8Unorm, vk::Format::eR32Uint, false);
	takes16[0] = extended && Storable(physicalDevice, vk::Format::eR5G5B5A1UnormPack16, vk::Format::eR16Uint, true);
	takes16[1] = extended && Storable(physicalDevice, vk::Format::eR5G6B5UnormPack16, vk::Format::eR16Uint, true);
	takes16[2] = extended && Storable(physicalDevice, vk::Format::eR4G4B4A4UnormPack16, vk::Format::eR16Uint, true);
	takes8 = extended && Storable(physicalDevice, vk::Format::eR8Unorm, vk::Format::eR8Uint, true);
	native[0] = Copyable(physicalDevice, vk::Format::eA1R5G5B5UnormPack16) ? vk::Format::eA1R5G5B5UnormPack16 : vk::Format::eUndefined;
	native[1] = Copyable(physicalDevice, vk::Format::eR5G6B5UnormPack16) ? vk::Format::eR5G6B5UnormPack16 : vk::Format::eUndefined;
	native[2] = Copyable(physicalDevice, vk::Format::eB4G4R4A4UnormPack16) ? vk::Format::eB4G4R4A4UnormPack16 : vk::Format::eUndefined;

	/* The ring: what a frame's textures take as they are in video memory -
	 * a few hundred KB in most frames, megabytes when a scene is loaded -
	 * a share of the video memory there is, between 2 and 16 MB a frame */
	const VramBudget& budget = context->GetVramBudget();
	u64 bytes = (u64)budget.available_kib() * 1024 / 256;
	bytes = std::max<u64>(bytes, 2 * 1024 * 1024);
	bytes = std::min<u64>(bytes, 16 * 1024 * 1024);
	u64 most = std::min<u64>(context->GetMaxStorageBufferRange(), context->GetMaxMemoryAllocationSize());
	if (bytes * swapChain > most)
		bytes = most / swapChain;
	ringFrame = (u32)bytes & ~15u;
	ring = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)ringFrame * swapChain,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc));
	ringMapped = (u8 *)ring->MapMemory();

	const vk::DescriptorSetLayoutBinding ringBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute);
	ringLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), 1, &ringBinding));
	const vk::DescriptorSetLayoutBinding imageBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute);
	imageLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), 1, &imageBinding));
	const vk::DescriptorSetLayout layouts[] = { *ringLayout, *imageLayout };
	const vk::PushConstantRange pushRange(vk::ShaderStageFlagBits::eCompute, 0, sizeof(Level::pc));
	pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 2, layouts, 1, &pushRange));

	// every kernel there is, now: none is made while a game runs
	const bool wanted[3] = { takes8, takes16[0] || takes16[1] || takes16[2], takes32 };
	for (u32 out = 0; out < 3; out++)
	{
		if (!wanted[out])
			continue;
		vk::UniqueShaderModule module = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(),
				texdecode_spv_size[out], texdecode_spv[out]));
		const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main");
		pipelines[out] = device.createComputePipelineUnique(context->GetPipelineCache(),
				vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *pipelineLayout));
	}

	const vk::DescriptorPoolSize ringSize(vk::DescriptorType::eStorageBuffer, 1);
	ringPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), 1, 1, &ringSize));
	ringSet = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*ringPool, 1, &layouts[0])).front();
	const vk::DescriptorBufferInfo ringInfo(*ring->buffer, 0, VK_WHOLE_SIZE);
	device.updateDescriptorSets(vk::WriteDescriptorSet(ringSet, 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &ringInfo), nullptr);

	const vk::DescriptorPoolSize imageSize(vk::DescriptorType::eStorageImage, MAX_DISPATCHES);
	for (int i = 0; i < swapChain; i++)
		pools.push_back(device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), MAX_DISPATCHES, 1, &imageSize)));

	jobs.resize(MAX_JOBS);
	dispatches.resize(MAX_DISPATCHES);
	copies.resize(MAX_JOBS);
	setLayouts.assign(MAX_DISPATCHES, *imageLayout);
	sets.resize(MAX_DISPATCHES);
	imageInfos.resize(MAX_DISPATCHES);
	writes.resize(MAX_DISPATCHES);
	barriers.resize(MAX_JOBS);

	frames = swapChain;
	index = 0;
	jobCount = dispatchCount = copyCount = 0;
	ringUsed = 0;
	ready = true;
	INFO_LOG(RENDERER, "Vulkan: textures are made on the GPU (%u KB of ring a frame; 32-bit %d, 16-bit %d%d%d, 8-bit %d; copied as they are %d%d%d)",
			ringFrame / 1024, takes32, takes16[0], takes16[1], takes16[2], takes8,
			native[0] != vk::Format::eUndefined, native[1] != vk::Format::eUndefined, native[2] != vk::Format::eUndefined);
}

void TexDecoder::Term()
{
	ready = false;
	jobCount = dispatchCount = copyCount = 0;
	pools.clear();
	ringSet = vk::DescriptorSet();
	ringPool.reset();
	for (auto& p : pipelines)
		p.reset();
	pipelineLayout.reset();
	imageLayout.reset();
	ringLayout.reset();
	ringMapped = nullptr;
	ring.reset();
	frames = 0;
	takes32 = takes8 = false;
	takes16[0] = takes16[1] = takes16[2] = false;
	native[0] = native[1] = native[2] = vk::Format::eUndefined;
}

bool TexDecoder::Takes(vk::Format format) const
{
	switch (format)
	{
	case vk::Format::eR8G8B8A8Unorm: return takes32;
	case vk::Format::eR5G5B5A1UnormPack16: return takes16[0];
	case vk::Format::eR5G6B5UnormPack16: return takes16[1];
	case vk::Format::eR4G4B4A4UnormPack16: return takes16[2];
	case vk::Format::eR8Unorm: return takes8;
	default: return false;
	}
}

vk::Format TexDecoder::NativeFormat(TextureType type) const
{
	switch (type)
	{
	case TextureType::_5551: return native[0];
	case TextureType::_565: return native[1];
	case TextureType::_4444: return native[2];
	default: return vk::Format::eUndefined;
	}
}

bool TexDecoder::Room(u32 bytes, u32 count) const
{
	if (!ready || flushed || jobCount >= MAX_JOBS || dispatchCount + count > MAX_DISPATCHES)
		return false;
	return ringUsed + ((bytes + 15) & ~15u) <= ringFrame;
}

void TexDecoder::BeginFrame(int frame)
{
	if (!ready)
		return;
	index = frame % frames;
	serial++;
	flushed = false;
	jobCount = dispatchCount = copyCount = 0;
	ringUsed = 0;
	// the frame that last had this place is done (its fence): so are its sets
	VulkanContext::Instance()->GetDevice().resetDescriptorPool(*pools[index]);
}

u8 *TexDecoder::Reserve(u32 bytes, u32 *offset)
{
	bytes = (bytes + 15) & ~15u;
	if (!ready || ringUsed + bytes > ringFrame)
	{
		statFull++;
		return nullptr;
	}
	*offset = (u32)index * ringFrame + ringUsed;
	ringUsed += bytes;
	statBytes += bytes;
	return ringMapped + *offset;
}

int TexDecoder::AddDecode(vk::Image image, bool fresh, u32 mipLevels, u32 out, const Level *levels, u32 count)
{
	Job& job = jobs[jobCount];
	job.image = image;
	job.first = dispatchCount;
	job.count = count;
	job.levels = mipLevels;
	job.out = out;
	job.fresh = fresh;
	job.copy = false;
	job.cancelled = false;
	std::copy(levels, levels + count, dispatches.begin() + dispatchCount);
	dispatchCount += count;
	statTextures++;
	statDispatches += count;
	return (int)jobCount++;
}

int TexDecoder::AddCopy(vk::Image image, bool fresh, u32 offset, u32 rowLength, u32 width, u32 height)
{
	Job& job = jobs[jobCount];
	job.image = image;
	job.first = copyCount;
	job.count = 0;
	job.levels = 1;
	job.out = TEXD_OUT16;
	job.fresh = fresh;
	job.copy = true;
	job.cancelled = false;
	Copy& copy = copies[copyCount++];
	copy.offset = offset;
	copy.rowLength = rowLength;
	copy.width = width;
	copy.height = height;
	statTextures++;
	statCopies++;
	return (int)jobCount++;
}

bool TexDecoder::Cancel(int job)
{
	if (job < 0 || (u32)job >= jobCount || jobs[job].cancelled)
		return false;
	jobs[job].cancelled = true;
	return jobs[job].fresh;
}

void TexDecoder::Flush(CommandPool& pool)
{
	if (!ready)
		return;
	/* Nothing more until the next frame: what was not recorded here would
	 * not be done. And what textures know of their jobs is out of date. */
	flushed = true;
	serial++;
	u32 live = 0, liveDispatches = 0;
	for (u32 i = 0; i < jobCount; i++)
		if (!jobs[i].cancelled)
		{
			live++;
			liveDispatches += jobs[i].count;
		}
	if (live == 0)
	{
		jobCount = dispatchCount = copyCount = 0;
		return;
	}
	const vk::Device device = VulkanContext::Instance()->GetDevice();

	// a storage image set for each dispatch, all in one go
	u32 n = 0;
	if (liveDispatches > 0)
	{
		const vk::DescriptorSetAllocateInfo allocInfo(*pools[index], liveDispatches, setLayouts.data());
		if (device.allocateDescriptorSets(&allocInfo, sets.data()) != vk::Result::eSuccess)
		{
			// (the pool has a set for every dispatch there can be)
			ERROR_LOG(RENDERER, "Vulkan: no descriptor sets for %u texture levels", liveDispatches);
			jobCount = dispatchCount = copyCount = 0;
			return;
		}
		for (u32 i = 0; i < jobCount; i++)
		{
			const Job& job = jobs[i];
			if (job.cancelled)
				continue;
			for (u32 d = job.first; d < job.first + job.count; d++)
			{
				imageInfos[n] = vk::DescriptorImageInfo(vk::Sampler(), dispatches[d].view, vk::ImageLayout::eGeneral);
				writes[n] = vk::WriteDescriptorSet(sets[n], 0, 0, 1, vk::DescriptorType::eStorageImage, &imageInfos[n]);
				n++;
			}
		}
		device.updateDescriptorSets(n, writes.data(), 0, nullptr);
	}

	const vk::CommandBuffer cmd = pool.Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	/* Every image from what it was - sampled by an earlier frame, or never
	 * written - to what it is written as. Nothing in them is kept. */
	u32 b = 0;
	for (u32 i = 0; i < jobCount; i++)
	{
		const Job& job = jobs[i];
		if (job.cancelled)
			continue;
		barriers[b++] = vk::ImageMemoryBarrier(vk::AccessFlags(),
				job.copy ? vk::AccessFlagBits::eTransferWrite : vk::AccessFlagBits::eShaderWrite,
				job.fresh ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
				job.copy ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eGeneral,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, job.image,
				vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, job.levels, 0, 1));
	}
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
			vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr, b, barriers.data());

	for (u32 i = 0; i < jobCount; i++)
	{
		const Job& job = jobs[i];
		if (job.cancelled || !job.copy)
			continue;
		const Copy& c = copies[job.first];
		const vk::BufferImageCopy region(c.offset, c.rowLength, c.height,
				vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
				vk::Offset3D(0, 0, 0), vk::Extent3D(c.width, c.height, 1));
		cmd.copyBufferToImage(*ring->buffer, job.image, vk::ImageLayout::eTransferDstOptimal, region);
	}

	if (n > 0)
	{
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, ringSet, nullptr);
		// a kernel at a time, in the order the sets were written
		for (u32 out = 0; out < 3; out++)
		{
			bool bound = false;
			u32 s = 0;
			for (u32 i = 0; i < jobCount; i++)
			{
				const Job& job = jobs[i];
				if (job.cancelled)
					continue;
				if (job.out != out)
				{
					s += job.count;
					continue;
				}
				if (!bound && job.count > 0)
				{
					cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[out]);
					bound = true;
				}
				for (u32 d = job.first; d < job.first + job.count; d++, s++)
				{
					const Level& lv = dispatches[d];
					cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 1, sets[s], nullptr);
					cmd.pushConstants(*pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(lv.pc), lv.pc);
					cmd.dispatch(((lv.pc[3] & 0xFFFF) + 7) / 8, ((lv.pc[3] >> 16) + 7) / 8, 1);
				}
			}
		}
	}

	// and to be sampled by this frame's draws
	for (u32 i = 0; i < b; i++)
	{
		vk::ImageMemoryBarrier& barrier = barriers[i];
		barrier.srcAccessMask = barrier.dstAccessMask;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		barrier.oldLayout = barrier.newLayout;
		barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer,
			vk::PipelineStageFlagBits::eFragmentShader, {}, 0, nullptr, 0, nullptr, b, barriers.data());
	cmd.end();

	jobCount = dispatchCount = copyCount = 0;
}
