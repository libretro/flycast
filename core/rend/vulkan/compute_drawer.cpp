#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/* The compute renderer on Vulkan: see compute_drawer.h */
#include "compute_drawer.h"
#include "vulkan_context.h"
#include "ssaa.h"
#include "rend/TexCache.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/ta_gpu.h"

#include <algorithm>
#include <array>

/* The kernels, by PVRC_K_*, as SPIR-V made from rend/tile/pvr_compute.h
 * when the source was: tools/rend/kernels_spv.sh, which checks they still
 * are its. Nothing is compiled when the core runs. */
#include "compute_spv.inc"
static_assert(sizeof(compute_spv) / sizeof(compute_spv[0]) == PVRC_KERNELS + 1, "a kernel for each PVRC_K_*");
// (the walk's unsorted variant's pipeline, after the kernels')
static const int WalkUnsorted = PVRC_KERNELS + 1;

/* The bin pool's entries: a run for each tile a primitive touches, for a
 * frame's every primitive (4 MiB) */
static const u32 PoolEntries = 2u << 20;

// The bindings of the kernels' one set
enum { B_FRAME, B_REC, B_BIN, B_OUT, B_LAST, B_PAL, B_TEX, B_VTX, B_IDX, B_MOD, B_CMD, B_TRD, B_DFRM, B_COUNT };
// (the buffers a frame of the GPU's TA decode is read from: B_VTX to B_DFRM)
static const int GpuBindings = B_COUNT - B_VTX;

// what PVRC_K_GPUSRC reads of the decode's buffers
static_assert(PVRC_TAG_C_WORDS == TAG_C_WORDS && PVRC_TAG_C_COUNT == TAG_C_COUNT && PVRC_TAG_C_FIRST == TAG_C_FIRST
		&& PVRC_TAG_O_OVERRUN == TAG_O_OVERRUN && PVRC_TAG_O_ZSCALE == TAG_O_ZSCALE, "the decode's words, as the kernels read them");

/* ---- The Vulkan side ---- */

/* The textures a frame's polygons have, each given a slot: a table of a
 * fixed size, emptied by a new generation each frame */
struct SlotTable : public ComputeFrame::Slots
{
	static const u32 Size = 2 * PVRC_TEX_SLOTS;
	struct Entry
	{
		u64 texid;
		u32 tsp;
		u32 gen;
		u32 slot;
	};
	Entry entries[Size];
	u64 slotTex[PVRC_TEX_SLOTS];
	u32 slotTsp[PVRC_TEX_SLOTS];
	u32 gen = 0;
	u32 used = 0;

	SlotTable() { memset(entries, 0, sizeof(entries)); }
	void Reset()
	{
		if (++gen == 0)
		{
			memset(entries, 0, sizeof(entries));
			gen = 1;
		}
		used = 0;
	}
	u32 Slot(u64 texid, TSP tsp) override
	{
		const u32 mask = tsp.full & SamplerManager::TSP_Mask;
		u32 h = (u32)(texid >> 4) * 2654435761u ^ mask * 40503u;
		for (u32 i = 0; i < Size; i++)
		{
			Entry& e = entries[(h + i) & (Size - 1)];
			if (e.gen != gen)
			{
				if (used == PVRC_TEX_SLOTS)
					return PVRC_NONE;
				e.gen = gen;
				e.texid = texid;
				e.tsp = tsp.full;
				e.slot = used;
				slotTex[used] = texid;
				slotTsp[used] = tsp.full;
				return used++;
			}
			if (e.texid == texid && (e.tsp & SamplerManager::TSP_Mask) == mask)
				return e.slot;
		}
		return PVRC_NONE;
	}
};

struct ComputeDrawer::Resources
{
	std::unique_ptr<BufferData> ring;	// a piece for each slot of the pool
	u32 ringWords = 0;
	u32 *ringMap = nullptr;
	std::unique_ptr<BufferData> records;
	std::unique_ptr<BufferData> bins;
	std::unique_ptr<BufferData> stats;	// the counts, a piece a slot, read when it comes round
	u32 *statsMap = nullptr;
	std::vector<bool> statsWritten;
	u32 binWords = 0;
	u32 runs = 0, chunks = 0, tileLog2 = 5;
	u32 keep = 4;
	bool binsZero = false;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipelines[PVRC_KERNELS + 2];	// (and the walk's unsorted variant last: WalkUnsorted)
	// with SSAA, the kernels for renders to a texture, at one sample a pixel
	vk::UniquePipeline rttPipelines[PVRC_KERNELS + 2];
	vk::UniqueDescriptorPool descriptorPool;
	std::vector<vk::DescriptorSet> sets;
	// what each set's texture slots, picture and last picture have
	std::vector<std::vector<VkImageView>> setViews;
	std::vector<std::vector<VkSampler>> setSamplers;
	std::vector<u32> setUsed;
	// ...and the buffers each set's B_VTX to B_DFRM are (the ring's slot, or the decode's)
	std::vector<std::array<VkBuffer, GpuBindings>> setBuffers;
	SlotTable slots;
	VkImageView dummyView = VK_NULL_HANDLE;
	std::vector<vk::DescriptorImageInfo> imageInfos;
	std::vector<vk::WriteDescriptorSet> writes;
	std::vector<vk::DescriptorBufferInfo> bufferInfos;
	vk::UniqueSampler nearest;
	vk::UniqueSampler linear;	// (a picture of another size to start from)
	bool lastScaled = false;
	bool gpu = false;	// this frame's is the decode's
	bool rtt = false;	// ...a render to a texture
	// this frame's, between Prepare and Record
	u32 words = 0, prims = 0;
	u64 skipped = 0, dropped = 0, hiz = 0, clipped = 0;
	u32 peels = 0;
};

ComputeDrawer::ComputeDrawer() {}
ComputeDrawer::~ComputeDrawer() {}

const char *ComputeDrawer::DeviceLacks()
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::PhysicalDeviceLimits limits = context->GetPhysicalDevice().getProperties().limits;
	if (!context->SupportsShaderInt64())
		return "no 64-bit integers in shaders";
	if (!context->SupportsSampledImageArrayIndexing())
		return "no indexing of an array of textures";
	if (!context->SupportsNonUniformTextureIndexing() && !context->SupportsSubgroupBallotCompute())
		return "no non-uniform indexing of an array of textures, nor subgroup ballot";
	if (limits.maxComputeWorkGroupInvocations < PVRC_WG || limits.maxComputeWorkGroupSize[0] < PVRC_WG)
		return "workgroups too small";
	if (limits.maxPerStageDescriptorSamplers < PVRC_TEX_SLOTS + 2 || limits.maxPerStageDescriptorSampledImages < PVRC_TEX_SLOTS + 2
			|| limits.maxDescriptorSetSamplers < PVRC_TEX_SLOTS + 2 || limits.maxDescriptorSetSampledImages < PVRC_TEX_SLOTS + 2)
		return "too few texture bindings";
	if (limits.maxComputeSharedMemorySize < PVRC_SORT_MAX * 4)
		return "too little shared memory";
	if (limits.maxPerStageDescriptorStorageBuffers < 3 + GpuBindings || limits.maxDescriptorSetStorageBuffers < 3 + GpuBindings)
		return "too few storage buffers";
	return nullptr;
}

bool ComputeDrawer::Init(vk::Extent2D extent, u32 samples, CommandPool *pool, SamplerManager *samplerManager)
{
	Term();
	if (failed)
		return false;
	VulkanContext *context = VulkanContext::Instance();
	const vk::Device device = context->GetDevice();
	const vk::PhysicalDeviceLimits limits = context->GetPhysicalDevice().getProperties().limits;
	this->extent = extent;
	this->samples = samples > 4 ? 8 : samples > 2 ? 4 : samples > 1 ? 2 : 1;
	this->commandPool = pool;
	this->samplerManager = samplerManager;

	auto fail = [&](const char *why) {
		INFO_LOG(RENDERER, "Vulkan compute renderer: not on this device (%s); the raster renderer draws", why);
		failed = true;
		res.reset();
		return false;
	};
	const char *lacks = DeviceLacks();
	if (lacks != nullptr)
		return fail(lacks);
	std::unique_ptr<Resources> r(new Resources());
	r->keep = pvrc_fragments_kept(limits.maxComputeSharedMemorySize);
	r->tileLog2 = ComputeFrame::TileLog2(extent.width);
	const u32 tiles = ((extent.width + (1u << r->tileLog2) - 1) >> r->tileLog2) * ((extent.height + (1u << r->tileLog2) - 1) >> r->tileLog2);
	r->chunks = (ComputeFrame::MaxPrims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	// (room for the screen's tiles, and a texture rendered to's: a few words each)
	r->runs = r->chunks * std::max(tiles, ComputeFrame::MaxRttTiles());
	r->binWords = pvrc_bin_words(r->runs, PoolEntries);
	r->ringWords = (ComputeFrame::MaxWords() + 63) & ~63u;
	const int slots = pool->GetSize();
	const u64 most = std::min<u64>(context->GetMaxStorageBufferRange(), context->GetMaxMemoryAllocationSize());
	if ((u64)r->ringWords * 4 > most || (u64)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4 > most || (u64)r->binWords * 4 > most)
		return fail("buffers larger than the device has");
	const u64 deviceBytes = (u64)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4 + (u64)r->binWords * 4;
	if ((deviceBytes >> 10) > context->GetVramBudget().available_kib())
		return fail("not enough video memory");
	try {
		const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer;
		r->ring = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->ringWords * 4 * slots, storage));
		r->ringMap = (u32 *)r->ring->MapMemory();
		r->records = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4,
				storage, vk::MemoryPropertyFlagBits::eDeviceLocal));
		r->bins = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->binWords * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eDeviceLocal));
		r->stats = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)PVRC_STATS * 4 * slots, vk::BufferUsageFlagBits::eTransferDst));
		r->statsMap = (u32 *)r->stats->MapMemory();
		r->statsWritten.assign(slots, false);
	} catch (const vk::SystemError& e) {
		return fail(e.what());
	}

	vk::DescriptorSetLayoutBinding bindings[B_COUNT] = {
		{ B_FRAME, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_REC, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_BIN, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_OUT, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_LAST, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_PAL, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_TEX, vk::DescriptorType::eCombinedImageSampler, PVRC_TEX_SLOTS, vk::ShaderStageFlagBits::eCompute },
		{ B_VTX, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_IDX, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_MOD, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_CMD, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_TRD, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_DFRM, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
	};
	r->setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), B_COUNT, bindings));
	r->pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &r->setLayout.get()));

	/* every kernel, now, for this many samples, and for one when that is
	 * not one (renders to a texture): none is made while a game runs. The
	 * walk in its two variants (pvr_compute.h: PVRC_V_*), the one for the
	 * tiles without sorted translucent fragments keeping none */
	for (int set = 0; set < (this->samples > 1 ? 2 : 1); set++)
	{
		const u32 n = set == 0 ? this->samples : 1;
		for (int k = 1; k <= WalkUnsorted; k++)
		{
			const bool walk = k == PVRC_K_WALK || k == WalkUnsorted;
			const u32 spec[5] = { n, SSAA_LOG2(n), k == WalkUnsorted ? 1u : r->keep, 0,
					k == WalkUnsorted ? PVRC_V_UNSORTED : k == PVRC_K_WALK ? PVRC_V_SORTED : PVRC_V_ALL };
			const vk::SpecializationMapEntry entries[5] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 }, { 4, 16, 4 } };
			const vk::SpecializationInfo specInfo(5, entries, sizeof(spec), spec);
			// (the walk's textures taken in turns where the device cannot index them non-uniformly)
			const bool waterfall = walk && !context->SupportsNonUniformTextureIndexing();
			vk::UniqueShaderModule module = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(),
					waterfall ? sizeof(compute_spv_walk_waterfall) : compute_spv_size[walk ? PVRC_K_WALK : k],
					waterfall ? compute_spv_walk_waterfall : compute_spv[walk ? PVRC_K_WALK : k]));
			const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main", &specInfo);
			(set == 0 ? r->pipelines : r->rttPipelines)[k] = device.createComputePipelineUnique(context->GetPipelineCache(),
					vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *r->pipelineLayout));
		}
	}

	const vk::DescriptorPoolSize sizes[] = {
		{ vk::DescriptorType::eStorageBuffer, (3u + GpuBindings) * slots },
		{ vk::DescriptorType::eStorageImage, (u32)slots },
		{ vk::DescriptorType::eCombinedImageSampler, (PVRC_TEX_SLOTS + 2u) * slots },
	};
	r->descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), slots, 3, sizes));
	std::vector<vk::DescriptorSetLayout> layouts(slots, *r->setLayout);
	r->sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*r->descriptorPool, slots, layouts.data()));
	r->setViews.assign(slots, std::vector<VkImageView>(PVRC_TEX_SLOTS, VK_NULL_HANDLE));
	r->setSamplers.assign(slots, std::vector<VkSampler>(PVRC_TEX_SLOTS, VK_NULL_HANDLE));
	r->setUsed.assign(slots, PVRC_TEX_SLOTS);	// (all of them to be written the first time)
	r->imageInfos.reserve(PVRC_TEX_SLOTS + 8);
	r->writes.reserve(PVRC_TEX_SLOTS + 8 + GpuBindings);
	r->bufferInfos.reserve(GpuBindings);
	r->nearest = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge));
	r->linear = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge));
	std::vector<vk::DescriptorBufferInfo> buffers;
	buffers.reserve((3 + GpuBindings) * slots);
	std::vector<vk::WriteDescriptorSet> writes;
	for (int s = 0; s < slots; s++)
	{
		buffers.push_back(vk::DescriptorBufferInfo(*r->ring->buffer, (vk::DeviceSize)s * r->ringWords * 4, (vk::DeviceSize)r->ringWords * 4));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_FRAME, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		buffers.push_back(vk::DescriptorBufferInfo(*r->records->buffer, 0, VK_WHOLE_SIZE));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_REC, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		buffers.push_back(vk::DescriptorBufferInfo(*r->bins->buffer, 0, VK_WHOLE_SIZE));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_BIN, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		// (a frame of the parse's reads its vertices, indices and volumes from its own words; Prepare() changes them for the decode's)
		for (int b = B_VTX; b < B_COUNT; b++)
		{
			buffers.push_back(vk::DescriptorBufferInfo(*r->ring->buffer, (vk::DeviceSize)s * r->ringWords * 4, (vk::DeviceSize)r->ringWords * 4));
			writes.push_back(vk::WriteDescriptorSet(r->sets[s], b, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		}
	}
	{
		std::array<VkBuffer, GpuBindings> ring;
		ring.fill((VkBuffer)*r->ring->buffer);
		r->setBuffers.assign(slots, ring);
	}
	device.updateDescriptorSets(writes, nullptr);

	timer.Init(slots, "compute renderer");
	res = std::move(r);
	ready = true;
	NOTICE_LOG(RENDERER, "Vulkan compute renderer: %u x %u, %u sample%s a pixel, tiles of %u, %u translucent fragments a sample at a time; %u KiB of video memory, %u KiB of host memory",
			extent.width, extent.height, this->samples, this->samples > 1 ? "s" : "", 1u << res->tileLog2, res->keep,
			(u32)(deviceBytes >> 10), (u32)(((u64)res->ringWords * 4 * slots) >> 10));
	return true;
}

void ComputeDrawer::Term()
{
	if (res)
	{
		if (statFrames + statFallback != 0)
		{
			NOTICE_LOG(RENDERER, "Vulkan compute renderer: %llu frames drawn (%llu of them the GPU's TA decode's, %llu renders to a texture), %llu left to the raster renderer; %llu runs walked over their chunk, %llu triangles at no depth left out, %llu with a vertex behind the eye clipped, at most %u rounds of translucent fragments, %llu primitives passed over by the tiles' depths",
					(unsigned long long)statFrames, (unsigned long long)statGpuFrames, (unsigned long long)statRttFrames, (unsigned long long)statFallback, (unsigned long long)res->skipped,
					(unsigned long long)res->dropped, (unsigned long long)res->clipped, res->peels, (unsigned long long)res->hiz);
			NOTICE_LOG(RENDERER, "Vulkan compute renderer: fallbacks: %llu renders to a texture it cannot write, %llu with the VMUs or crosshairs drawn over, %llu starting from a picture it cannot take, %llu with more render passes than it takes, %llu with more textures than its slots, %llu too large, %llu the decode's without its buffers",
					(unsigned long long)statWhy[ComputeFrame::Why_Rtt], (unsigned long long)statWhy[ComputeFrame::Why_Osd],
					(unsigned long long)statWhy[ComputeFrame::Why_Picture], (unsigned long long)statWhy[ComputeFrame::Why_Passes],
					(unsigned long long)statWhy[ComputeFrame::Why_Textures], (unsigned long long)statWhy[ComputeFrame::Why_Room],
					(unsigned long long)statWhy[ComputeFrame::Why_Decode]);
		}
	}
	timer.Term();
	res.reset();
	ready = false;
	statFrames = statFallback = statGpuFrames = statRttFrames = 0;
	memset(statWhy, 0, sizeof(statWhy));
}

bool ComputeDrawer::CanDraw() const
{
	if (!ready || pvrrc.isRenderFramebuffer || (pvrrc.gpu && gpuDraw == nullptr))
		return false;
	return true;
}

bool ComputeDrawer::Prepare(bool last, bool scaled, const Texture *paletteTexture, vk::Extent2D rtt)
{
	Resources& r = *res;
	const int slot = commandPool->GetIndex();
	// the counts of the frame that had this slot before, done by now
	if (r.statsWritten[slot])
	{
		const u32 *st = r.statsMap + slot * PVRC_STATS;
		r.skipped += st[PVRC_ST_SKIPPED];
		r.dropped += st[PVRC_ST_DROPPED];
		r.clipped += st[PVRC_ST_CLIPPED];
		r.hiz += st[PVRC_ST_HIZ];
		r.peels = std::max(r.peels, st[PVRC_ST_PEELS]);
	}

	ComputeFrame::Layout layout;
	r.rtt = pvrrc.isRTT;
	layout.width = r.rtt ? rtt.width : extent.width;
	layout.height = r.rtt ? rtt.height : extent.height;
	layout.tileLog2 = r.rtt ? ComputeFrame::TileLog2(layout.width) : r.tileLog2;
	layout.pool = PoolEntries;
	layout.runs = r.runs;
	layout.chunks = r.chunks;
	layout.lastPicture = last && !r.rtt;
	layout.lastScaled = layout.lastPicture && scaled;
	layout.rttScale = r.rtt ? ComputeFrame::RttScale() : 0;
	r.lastScaled = layout.lastScaled;
	// (a texture rendered to larger than the bins are made for: none the PowerVR's registers ask for)
	const u32 tiles = ((layout.width + (1u << layout.tileLog2) - 1) >> layout.tileLog2) * ((layout.height + (1u << layout.tileLog2) - 1) >> layout.tileLog2);
	if (layout.width == 0 || layout.height == 0 || (u64)tiles * r.chunks > r.runs)
	{
		Fallback(ComputeFrame::Why_Room);
		return false;
	}
	r.slots.Reset();
	u32 *words = r.ringMap + (size_t)slot * r.ringWords;
	r.words = frame.Pack(layout, r.slots, words, r.ringWords, &r.prims);
	if (r.words == 0)
	{
		Fallback(frame.why);
		return false;
	}
	r.gpu = pvrrc.gpu;
	r.dummyView = (VkImageView)paletteTexture->GetImageView();

	/* (the buffers and textures the set has are written by Record(),
	 * after the texture a render to a texture goes to is taken, which the
	 * frame's textures can be: as the raster renderer has them) */
	return true;
}

void ComputeDrawer::Record(vk::Image image, vk::ImageView view, vk::ImageView last)
{
	Resources& r = *res;
	const int slot = commandPool->GetIndex();
	const u32 *words = r.ringMap + (size_t)slot * r.ringWords;
	/* The set's textures, picture and palette, where they changed */
	VulkanContext *context = VulkanContext::Instance();
	const vk::DescriptorSet set = r.sets[slot];
	r.imageInfos.clear();
	r.writes.clear();
	r.bufferInfos.clear();
	/* ...and what the frame's vertices, indices and volumes are read from:
	 * the ring's slot, or the decode's buffers (whose handles change only
	 * when it is made again) */
	{
		const VkBuffer ring = (VkBuffer)*r.ring->buffer;
		std::array<VkBuffer, GpuBindings> want;
		if (r.gpu)
			want = { (VkBuffer)gpuDraw->vertices, (VkBuffer)gpuDraw->indices, (VkBuffer)gpuDraw->modtrig,
					(VkBuffer)gpuDraw->commands, (VkBuffer)gpuDraw->trDraws, (VkBuffer)gpuDraw->frame };
		else
			want.fill(ring);
		std::array<VkBuffer, GpuBindings>& have = r.setBuffers[slot];
		for (int b = 0; b < GpuBindings; b++)
		{
			if (have[b] == want[b])
				continue;
			if (want[b] == ring)
				r.bufferInfos.push_back(vk::DescriptorBufferInfo(vk::Buffer(ring), (vk::DeviceSize)slot * r.ringWords * 4, (vk::DeviceSize)r.ringWords * 4));
			else
				r.bufferInfos.push_back(vk::DescriptorBufferInfo(vk::Buffer(want[b]), 0, VK_WHOLE_SIZE));
			r.writes.push_back(vk::WriteDescriptorSet(set, B_VTX + b, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &r.bufferInfos.back()));
			have[b] = want[b];
		}
	}
	const VkImageView dummyView = r.dummyView;
	const VkSampler dummySampler = (VkSampler)*r.nearest;
	auto write = [&](u32 binding, u32 element, vk::DescriptorType type, VkSampler sampler, VkImageView v, vk::ImageLayout l) {
		r.imageInfos.push_back(vk::DescriptorImageInfo(vk::Sampler(sampler), vk::ImageView(v), l));
		r.writes.push_back(vk::WriteDescriptorSet(set, binding, element, 1, type, &r.imageInfos.back()));
	};
	write(B_PAL, 0, vk::DescriptorType::eCombinedImageSampler, dummySampler, dummyView, vk::ImageLayout::eShaderReadOnlyOptimal);
	std::vector<VkImageView>& views = r.setViews[slot];
	std::vector<VkSampler>& samplers = r.setSamplers[slot];
	const u32 wasUsed = r.setUsed[slot];
	for (u32 i = 0; i < std::max(r.slots.used, wasUsed); i++)
	{
		VkImageView v = dummyView;
		VkSampler s = dummySampler;
		if (i < r.slots.used)
		{
			TSP tsp;
			tsp.full = r.slots.slotTsp[i];
			v = (VkImageView)reinterpret_cast<Texture *>(r.slots.slotTex[i])->GetReadOnlyImageView();
			s = (VkSampler)samplerManager->GetSampler(tsp);
		}
		/* (a slot this frame uses is written whatever it had: a view made in
		 * the place of one gone can have its handle) */
		if (i < r.slots.used || views[i] != v || samplers[i] != s)
		{
			write(B_TEX, i, vk::DescriptorType::eCombinedImageSampler, s, v, vk::ImageLayout::eShaderReadOnlyOptimal);
			views[i] = v;
			samplers[i] = s;
		}
	}
	r.setUsed[slot] = r.slots.used;
	// (the pictures and the palette every frame: three words)
	write(B_OUT, 0, vk::DescriptorType::eStorageImage, VK_NULL_HANDLE, (VkImageView)view, vk::ImageLayout::eGeneral);
	write(B_LAST, 0, vk::DescriptorType::eCombinedImageSampler, last && r.lastScaled ? (VkSampler)*r.linear : dummySampler,
			last ? (VkImageView)last : dummyView, vk::ImageLayout::eShaderReadOnlyOptimal);
	if (!r.writes.empty())
		context->GetDevice().updateDescriptorSets(r.writes, nullptr);

	/* The dispatches */
	vk::CommandBuffer cmd = commandPool->Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	timer.Begin(cmd, slot);
	const vk::Buffer bins = *r.bins->buffer;
	// (the counts and the counts for the log start at 0; the pool is 0 after the frame before's clear)
	// (after the frame before is done with them)
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
			vk::DependencyFlags(), vk::MemoryBarrier(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferRead,
					vk::AccessFlagBits::eTransferWrite), nullptr, nullptr);
	{
		/* (only this frame's runs: the bins have room for the largest
		 * texture rendered to's, and the kernels read no others) */
		const u32 used = (r.gpu ? r.chunks : words[PVRC_F_CHUNKS]) * words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
		if (!r.binsZero)
			cmd.fillBuffer(bins, 0, VK_WHOLE_SIZE, 0);
		else if (used >= r.runs)
			cmd.fillBuffer(bins, 0, (vk::DeviceSize)PVRC_B_POOL(r.runs) * 4, 0);
		else
		{
			if (used != 0)
			{
				cmd.fillBuffer(bins, (vk::DeviceSize)PVRC_B_COUNT(r.runs) * 4, (vk::DeviceSize)used * 4, 0);
				cmd.fillBuffer(bins, (vk::DeviceSize)PVRC_B_START(r.runs) * 4, (vk::DeviceSize)used * 4, 0);
				cmd.fillBuffer(bins, (vk::DeviceSize)PVRC_B_CURSOR(r.runs) * 4, (vk::DeviceSize)used * 4, 0);
			}
			cmd.fillBuffer(bins, (vk::DeviceSize)PVRC_B_STATS(r.runs) * 4, (vk::DeviceSize)(PVRC_B_POOL(r.runs) - PVRC_B_STATS(r.runs)) * 4, 0);
		}
		r.binsZero = true;
		/* (the walk's variants' dispatches across: the CPU's; the sort counts the tiles of each) */
		const u32 across = pvrc_walk_x(words[PVRC_F_TILE_LOG2], r.rtt ? 1 : samples);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(),
				vk::MemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite), nullptr, nullptr);
		cmd.fillBuffer(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_WALK_U_X) * 4, 4, across);
		cmd.fillBuffer(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_WALK_S_X) * 4, 4, across);
	}
	/* (and what the frame's textures were made with before: the texture
	 * decoder's and the uploads' barriers, and a render to texture's
	 * render pass, make them ready for fragment shaders, not these) */
	const vk::MemoryBarrier toCompute(vk::AccessFlagBits::eMemoryWrite,
			vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eIndirectCommandRead);
	const vk::ImageMemoryBarrier toGeneral(vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
			vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect, vk::DependencyFlags(), toCompute, nullptr, toGeneral);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipelineLayout, 0, 1, &set, 0, nullptr);
	// (a render to a texture is drawn at one sample a pixel)
	const vk::UniquePipeline *pipelines = r.rtt && samples > 1 ? r.rttPipelines : r.pipelines;
	const u32 n = r.rtt ? 1 : samples;
	auto step = [&](int kernel, u32 x, u32 y) {
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[kernel]);
		if (x != 0 && y != 0)
			cmd.dispatch(x, y, 1);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
				vk::DependencyFlags(), vk::MemoryBarrier(vk::AccessFlagBits::eShaderWrite,
						vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eIndirectCommandRead), nullptr, nullptr);
	};
	const u32 tiles = words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	const u32 groups = (r.prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP;
	if (r.gpu)
	{
		/* A frame of the decode's: its numbering, and the dispatches of
		 * the kernels whose sizes are the GPU's, from it */
		auto indirect = [&](int kernel, u32 at) {
			cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[kernel]);
			cmd.dispatchIndirect(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + at) * 4);
			cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
					vk::DependencyFlags(), vk::MemoryBarrier(vk::AccessFlagBits::eShaderWrite,
							vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eIndirectCommandRead), nullptr, nullptr);
		};
		step(PVRC_K_GPUSRC, 1, 1);
		indirect(PVRC_K_SETUP, PVRC_ST_SETUP_X);
		step(PVRC_K_SCAN, 1, 1);
		indirect(PVRC_K_SCATTER, PVRC_ST_SETUP_X);
		indirect(PVRC_K_SORT, PVRC_ST_SORT_X);
	}
	else
	{
		step(PVRC_K_SETUP, groups, 1);
		step(PVRC_K_SCAN, 1, 1);
		step(PVRC_K_SCATTER, groups, 1);
		step(PVRC_K_SORT, tiles, words[PVRC_F_CHUNKS]);
	}
	/* (the walk's two variants, each over the coarse tiles of its list,
	 * the sort's: nothing between them) */
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[WalkUnsorted]);
	cmd.dispatchIndirect(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_WALK_U_X) * 4);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[PVRC_K_WALK]);
	cmd.dispatchIndirect(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_WALK_S_X) * 4);
	step(PVRC_K_WALK, 0, 0);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[PVRC_K_CLEAR]);
	cmd.dispatchIndirect(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_CLEAR_X) * 4);
	// the counts, for when this slot comes round
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(),
			vk::MemoryBarrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead), nullptr, nullptr);
	cmd.copyBuffer(bins, *r.stats->buffer, vk::BufferCopy((vk::DeviceSize)PVRC_B_STATS(r.runs) * 4, (vk::DeviceSize)slot * PVRC_STATS * 4, PVRC_STATS * 4));
	r.statsWritten[slot] = true;
	/* the picture, for the frontend, as the raster renderer leaves it; a
	 * texture, for the frames that draw with it (either renderer) and the
	 * copy to video memory when the game touches it */
	const vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eShaderWrite,
			r.rtt ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead : vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eGeneral,
			vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
			r.rtt ? vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer
					: vk::PipelineStageFlagBits::eFragmentShader, vk::DependencyFlags(), nullptr, nullptr, toRead);
	timer.End(cmd, slot);
	cmd.end();
	statFrames++;
	if (r.gpu)
		statGpuFrames++;
	if (r.rtt)
		statRttFrames++;
}
