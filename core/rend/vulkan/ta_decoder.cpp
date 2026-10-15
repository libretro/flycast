/* GPU-side TA processing on Vulkan: see ta_decoder.h */
#include "ta_decoder.h"
#include "commandpool.h"
#include "vulkan_context.h"
#include "rend/ta_gpu.h"

#include <algorithm>
#include <chrono>

extern int screen_height;

/* The kernels, by TAG_K_*, as SPIR-V made from rend/ta_gpu.h when the core
 * was built: tools/rend/kernels_spv.sh, which checks that they still are
 * its. Nothing is compiled when the core runs. */
#include "rend/ta_gpu_spv.inc"
static_assert(sizeof(ta_gpu_spv) / sizeof(ta_gpu_spv[0]) == TAG_KERNELS + 1, "a kernel for each TAG_K_*");

// (the drawers draw from the records and commands: BaseDrawer::DrawIndices())
static_assert(TAG_C_WORDS * 4 == sizeof(VkDrawIndexedIndirectCommand), "a record is an indexed indirect draw");

// The buffers, in the kernels' bindings' order (set 1)
enum { B_TAB, B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_COUNT };

struct TaDecoder::Resources
{
	std::unique_ptr<BufferData> pool;	// the TA contexts' data
	std::unique_ptr<BufferData> buffers[B_COUNT];
	u32 *tables = nullptr;			// mapped: a piece for each frame
	u32 tableWords = 0;
	u32 slice = 0, slices = 0;
	TaGpuLayout layout;
	vk::UniqueDescriptorSetLayout setLayouts[2];
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipelines[TAG_KERNELS + 1];
	vk::UniqueDescriptorPool descriptorPool;
	std::vector<vk::DescriptorSet> sliceSets, tableSets;
	vk::UniqueQueryPool queries;		// two timestamps a frame, if the queue has them
	double period = 0;			// ns a tick
	u64 mask = 0;
	/* (TA_GPU_SORT) room for the sorted translucent indices of a frame,
	 * for each slot of the texture pool, whose command buffers the drawers
	 * draw with: a slot's are written by the CPU once BeginFrame() has
	 * waited for the slot's fence, which the draws of the frame that last
	 * read them were submitted with */
	std::unique_ptr<BufferData> sorted;
	u8 *sortedMap = nullptr;
	vk::DeviceSize sortSlot = 0;		// bytes; 0: none
};

TaDecoder::TaDecoder() {}
TaDecoder::~TaDecoder() {}

bool TaDecoder::Init(CommandPool& pool)
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::Device device = context->GetDevice();
	const vk::PhysicalDevice physicalDevice = context->GetPhysicalDevice();

#ifdef MSB_FIRST
	// (the kernels read the blocks little-endian)
	failed = true;
	return false;
#endif
	frames = pool.GetSize();
	TaGpuSizes sz;
	ta_gpu_sizes(&sz);
	std::unique_ptr<Resources> r(new Resources());
	const u32 strips = sz.list[0] + sz.list[1] + sz.list[2];
	u32 work[9];
	const u32 workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	/* The contexts: every frame in flight, one being drawn, and the few the
	 * game writes to at a time and renders to textures with */
	r->slice = ta_data_size();
	/* ...and spares, for a game that writes to more at once, as many as a
	 * thirty-second of the memory budget left allows, up to four more: a
	 * frame that finds them all taken is drawn from the CPU's parse, in a
	 * context of its own memory (tactx_gpu_pool_attach() makes one) */
	const u64 budget = (u64)context->GetVramBudget().available_kib() * 1024 / 32;
	const u32 spares = (u32)std::min<u64>(budget / r->slice, 4);
	r->slices = std::min(frames + 4 + (int)spares, 16);
	r->tableWords = (sz.table_words + 63) & ~63u;
	// the translucent draw records: each group's count and room for a draw a vertex
	const u32 trdSlots = sz.verts + sz.headers;

	const u64 most = std::min<u64>(context->GetMaxStorageBufferRange(), context->GetMaxMemoryAllocationSize());
	if ((u64)r->slice > most || (u64)sz.verts * TAG_V_WORDS * 4 > most || (u64)workWords * 4 > most
			|| (u64)trdSlots * TAG_C_WORDS * 4 > most)
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (buffers larger than the device has)");
		failed = true;
		return false;
	}
	/* A translucent draw group is one indirect draw of all its strips: with
	 * their count from the GPU (VK_KHR_draw_indirect_count), or of the
	 * group's whole room, whose records past the count draw nothing */
	draw.drawCount = context->GetDrawIndexedIndirectCount();
	if (context->GetMaxDrawIndirectCount() < sz.verts || (draw.drawCount == nullptr && !context->SupportsMultiDrawIndirect()))
	{
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (no indirect draws of several at once)");
		failed = true;
		return false;
	}
	instance = context->SupportsDrawIndirectFirstInstance();
	try {
		r->pool = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->slice * r->slices,
				vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst));
		/* The SH4 reads the control words back and states copy the data:
		 * memory without the host's cache in front of it is too slow for that */
		if (!(r->pool->allocation.PropertyFlags() & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
		{
			INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (no host-cached memory)");
			failed = true;
			return false;
		}
		r->buffers[B_TAB] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->tableWords * 4 * frames,
				vk::BufferUsageFlagBits::eStorageBuffer));
		r->tables = (u32 *)r->buffers[B_TAB]->MapMemory();
		const vk::MemoryPropertyFlags local = vk::MemoryPropertyFlagBits::eDeviceLocal;
		const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer;
		r->buffers[B_VTX] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)sz.verts * TAG_V_WORDS * 4,
				storage | vk::BufferUsageFlagBits::eVertexBuffer, local));
		r->buffers[B_MOD] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)sz.modtrig * 9 * 4 + 4,
				storage | vk::BufferUsageFlagBits::eVertexBuffer, local));
		r->buffers[B_IDX] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)sz.idx * 4,
				storage | vk::BufferUsageFlagBits::eIndexBuffer, local));
		r->buffers[B_WORK] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)workWords * 4,
				storage | vk::BufferUsageFlagBits::eTransferDst, local));
		r->buffers[B_REC] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)strips * TAG_C_WORDS * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer, local));
		// (and a second command for each, for shadows: TAG_T_SHD_CMD)
		r->buffers[B_CMD] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)sz.headers * 2 * TAG_C_WORDS * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer, local));
		r->buffers[B_FRM] = std::unique_ptr<BufferData>(new BufferData(TAG_O_WORDS * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst, local));
		r->buffers[B_TRD] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)trdSlots * TAG_C_WORDS * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst, local));
		r->buffers[B_TRP] = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)sz.list[2] * TAG_P_WORDS * 4, storage, local));
		/* The sorted indices: three a triangle, for as many triangles as a
		 * frame can have indices, and as many again for the depth prepass
		 * (rend/sorter.h), whose triangles are some of those. In
		 * host-visible memory, which the CPU writes them to once (they are
		 * its sort's) and the GPU reads once. */
		if (sortWanted)
		{
			const vk::DeviceSize slot = ((vk::DeviceSize)sz.idx * 2 * 3 * 4 + 255) & ~(vk::DeviceSize)255;
			r->sorted = std::unique_ptr<BufferData>(new BufferData(slot * frames, vk::BufferUsageFlagBits::eIndexBuffer));
			r->sortedMap = (u8 *)r->sorted->MapMemory();
			r->sortSlot = slot;
		}
	} catch (const vk::SystemError& e) {
		INFO_LOG(RENDERER, "Vulkan: TA processing stays on the CPU (%s)", e.what());
		failed = true;
		return false;
	}

	r->layout.words[0] = 0;		// (each slice is bound from its start)
	for (int i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	vk::DescriptorSetLayoutBinding bindings[B_COUNT];
	for (int i = 0; i < B_COUNT; i++)
		bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute);
	r->setLayouts[0] = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), 1, bindings));
	r->setLayouts[1] = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), B_COUNT, bindings));
	const vk::DescriptorSetLayout layouts[] = { *r->setLayouts[0], *r->setLayouts[1] };
	r->pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 2, layouts));

	// every kernel, now: none is made while a game runs
	for (int k = 1; k <= TAG_KERNELS; k++)
	{
		vk::UniqueShaderModule module = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(),
				ta_gpu_spv_size[k], ta_gpu_spv[k]));
		const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main");
		r->pipelines[k] = device.createComputePipelineUnique(context->GetPipelineCache(),
				vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *r->pipelineLayout));
	}

	const vk::DescriptorPoolSize poolSize(vk::DescriptorType::eStorageBuffer, r->slices + B_COUNT * frames);
	r->descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), r->slices + frames, 1, &poolSize));
	std::vector<vk::DescriptorSetLayout> sliceLayouts(r->slices, layouts[0]), tableLayouts(frames, layouts[1]);
	r->sliceSets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*r->descriptorPool, r->slices, sliceLayouts.data()));
	r->tableSets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*r->descriptorPool, frames, tableLayouts.data()));
	std::vector<vk::DescriptorBufferInfo> infos;
	std::vector<vk::WriteDescriptorSet> writes;
	infos.reserve(r->slices + B_COUNT * frames);
	for (u32 i = 0; i < r->slices; i++)
	{
		infos.push_back(vk::DescriptorBufferInfo(*r->pool->buffer, (vk::DeviceSize)i * r->slice, r->slice));
		writes.push_back(vk::WriteDescriptorSet(r->sliceSets[i], 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &infos.back()));
	}
	for (int f = 0; f < frames; f++)
		for (int b = 0; b < B_COUNT; b++)
		{
			if (b == B_TAB)
				infos.push_back(vk::DescriptorBufferInfo(*r->buffers[b]->buffer, (vk::DeviceSize)f * r->tableWords * 4, r->tableWords * 4));
			else
				infos.push_back(vk::DescriptorBufferInfo(*r->buffers[b]->buffer, 0, VK_WHOLE_SIZE));
			writes.push_back(vk::WriteDescriptorSet(r->tableSets[f], b, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &infos.back()));
		}
	device.updateDescriptorSets(writes, nullptr);

	// GPU time, read when a frame is known to be done: never waited for
	const std::vector<vk::QueueFamilyProperties> families = physicalDevice.getQueueFamilyProperties();
	const u32 family = context->GetGraphicsQueueFamilyIndex();
	const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
	if (bits != 0)
	{
		r->queries = device.createQueryPoolUnique(vk::QueryPoolCreateInfo(vk::QueryPoolCreateFlags(), vk::QueryType::eTimestamp, 2 * frames));
		r->period = physicalDevice.getProperties().limits.timestampPeriod;
		r->mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
	}

	if (!tactx_gpu_pool_attach((u8 *)r->pool->MapMemory(), r->slice, r->slices))
	{
		failed = true;
		return false;
	}
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		failed = true;
		return false;
	}
	// (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever)
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	slotSerial.assign(frames, 0);
	slotTimed.assign(frames, false);
	draw.vertices = *r->buffers[B_VTX]->buffer;
	draw.indices = *r->buffers[B_IDX]->buffer;
	draw.modtrig = *r->buffers[B_MOD]->buffer;
	draw.commands = *r->buffers[B_CMD]->buffer;
	draw.trDraws = *r->buffers[B_TRD]->buffer;
	draw.trParams = *r->buffers[B_TRP]->buffer;
	draw.trParamsSize = (vk::DeviceSize)sz.list[2] * TAG_P_WORDS * 4;
	draw.shadowCommands = (vk::DeviceSize)sz.headers * TAG_C_WORDS * 4;
	draw.sorted = r->sorted ? *r->sorted->buffer : vk::Buffer();
	draw.sortedOffset = 0;
	draw.sortedMap = nullptr;
	sortable = r->sortSlot != 0;
	res = std::move(r);
	ready = true;
	NOTICE_LOG(RENDERER, "Vulkan: TA processing on the GPU (%u frames of %u MB in host-cached memory, %u KB of tables a frame; "
			"translucent groups drawn %s, %s strip numbers%s)",
			res->slices, res->slice >> 20, res->tableWords / 256, draw.drawCount != nullptr ? "with their count" : "whole",
			instance ? "with" : "without", res->sortSlot != 0 ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

void TaDecoder::Log(const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "Vulkan: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, %llu might run out of room); "
			"GPU time mean %.3f ms, max %.3f ms; pool: %u frames found it full, %u contexts allocated; "
			"%llu with a sorted pass (the sort mean %.1f us, max %.1f us), %llu with shadows the GPU decides",
			when, (unsigned long long)statFrames, (unsigned long long)statSkipped,
			(unsigned long long)statWhy[TA_GPU_WHY_TABLE], (unsigned long long)statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)statWhy[TA_GPU_WHY_SORT], (unsigned long long)statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)statWhy[TA_GPU_WHY_ROOM],
			statGpuTimed ? statGpuMs / statGpuTimed : 0.0, statGpuMaxMs, shortFrames, allocated,
			(unsigned long long)statSorted, statSorted ? statSortUs / statSorted : 0.0, statSortMaxUs,
			(unsigned long long)statShadowed);
}

void TaDecoder::Term()
{
	if (!ready)
		return;
	Log("");
	// (the renderer is done with the GPU)
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	res.reset();
	ready = false;
	sortable = false;
	prepared = false;
}

/* Every frame up to the newest whose slot's fence has signalled is done:
 * a fence is signalled once all that was submitted before it is */
void TaDecoder::PollDone(CommandPool& pool)
{
	for (int s = 0; s < frames; s++)
		if (s != index && (s32)(slotSerial[s] - done) > 0 && pool.IsDone(s))
			done = slotSerial[s];
	tactx_gpu_done(done);
}

void TaDecoder::Setup(CommandPool& pool, bool wanted)
{
	if (ready && wanted && frames == pool.GetSize())
		return;
	if (ready)
	{
		/* Turned off while a game runs, or another size of the texture
		 * pool: the frames in the pool go on in memory of their own, and
		 * the buffers go once the frames that may still read them are done */
		ta_set_gpu_processing(false);
		tactx_gpu_pool_detach();
		pool.DeferDelete(std::move(res));
		ready = false;
		sortable = false;
		prepared = false;
	}
	if (!wanted)
	{
		failed = false;
		return;
	}
	if (failed)
		return;
	const auto start = std::chrono::steady_clock::now();
	if (Init(pool))
		NOTICE_LOG(RENDERER, "Vulkan: TA processing set up in %.1f ms",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void TaDecoder::BeginFrame(CommandPool& pool)
{
	/* (made in Setup(), never here: a frame the decoder is not ready for
	 * is parsed on the CPU) */
	if (!ready)
		return;
	if (frames != pool.GetSize())
	{
		Setup(pool, false);
		return;
	}
	prepared = false;
	index = pool.GetIndex();
	if (++serial == 0)
		serial = 1;
	// BeginFrame() waited for this slot's fence: its last frame is done
	if ((s32)(slotSerial[index] - done) > 0)
		done = slotSerial[index];
	if (slotTimed[index])
	{
		u64 ts[2];
		if (VulkanContext::Instance()->GetDevice().getQueryPoolResults(*res->queries, 2 * index, 2, sizeof(ts), ts, sizeof(u64),
				vk::QueryResultFlagBits::e64) == vk::Result::eSuccess)
		{
			const double ms = (double)((ts[1] - ts[0]) & res->mask) * res->period / 1e6;
			statGpuMs += ms;
			statGpuMaxMs = std::max(statGpuMaxMs, ms);
			statGpuTimed++;
		}
	}
	slotSerial[index] = serial;
	slotTimed[index] = false;
	PollDone(pool);
	if (sortable)
	{
		draw.sortedOffset = (vk::DeviceSize)index * res->sortSlot;
		draw.sortedMap = (u32 *)(res->sortedMap + draw.sortedOffset);
	}
}

bool TaDecoder::Prepare(TA_context *ctx, u32 want, bool *rv)
{
	prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer)
	{
		// (a frame that found the pool full is in memory the GPU cannot read)
		statSkipped++;
		statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	flags = want;
	prepared = ta_gpu_prepare(ctx, &res->layout, res->tables + (size_t)index * res->tableWords, res->tableWords, &frame, flags);
	if (!prepared)
	{
		statSkipped++;
		statWhy[frame.why < TA_GPU_WHY_COUNT ? frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = frame.rv;
	if (ta_gpu_sorted_indices() != 0)
	{
		statSorted++;
		const double us = ta_gpu_sorted_ns() / 1e3;
		statSortUs += us;
		statSortMaxUs = std::max(statSortMaxUs, us);
	}
	if (frame.shadows != 0)
		statShadowed++;
	// (nothing to draw: nothing to decode)
	if (!frame.rv)
		prepared = false;
	return true;
}

void TaDecoder::Record(TA_context *ctx, CommandPool& pool)
{
	if (!prepared)
		return;
	prepared = false;
	Resources& r = *res;
	const vk::CommandBuffer cmd = pool.Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	if (r.queries)
	{
		cmd.resetQueryPool(*r.queries, 2 * index, 2);
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *r.queries, 2 * index);
	}
	/* The outputs are one copy: whatever read the last frame's (its draws,
	 * from the next stage on, or its own kernels) is done before they are
	 * written again */
	const vk::MemoryBarrier before(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite
			| vk::AccessFlagBits::eVertexAttributeRead | vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eIndirectCommandRead,
			vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
	// (the per-pixel renderer reads the translucent polygon parameters in its fragment shaders)
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eVertexInput
			| vk::PipelineStageFlagBits::eDrawIndirect | vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
			vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eComputeShader, {}, before, nullptr, nullptr);
	const vk::Buffer frm = *r.buffers[B_FRM]->buffer;
	cmd.fillBuffer(frm, 0, TAG_O_WORDS * 4, 0);
	cmd.fillBuffer(frm, TAG_O_ZMAX * 4, 4, 0x3F800000u);	// fZ_max starts at 1
	if (frame.slots != 0)
		cmd.fillBuffer(*r.buffers[B_WORK]->buffer, (vk::DeviceSize)r.layout.words[9] * 4, (vk::DeviceSize)frame.slots * 4, 0);
	// the frame's translucent draw records: counts and room
	if (frame.trd != 0)
		cmd.fillBuffer(*r.buffers[B_TRD]->buffer, 0, (vk::DeviceSize)frame.trd * TAG_C_WORDS * 4, 0);
	const vk::MemoryBarrier filled(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader, {}, filled, nullptr, nullptr);

	const vk::DescriptorSet sets[] = { r.sliceSets[ctx->pool], r.tableSets[index] };
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipelineLayout, 0, 2, sets, 0, nullptr);
	const vk::MemoryBarrier step(vk::AccessFlagBits::eShaderWrite,
			vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eIndirectCommandRead);
	const vk::PipelineStageFlags next = vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect;
	auto dispatch = [&](int k, u32 groups) {
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[k]);
		if (groups != 0)
			cmd.dispatch(groups, 1, 1);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, next, {}, step, nullptr, nullptr);
	};
	auto indirect = [&](int k, u32 at) {
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[k]);
		cmd.dispatchIndirect(frm, (vk::DeviceSize)at * 4);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, next, {}, step, nullptr, nullptr);
	};
	const u32 scanGroups = (frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (frame.verts + frame.modtrig + 63) / 64);
	dispatch(TAG_K_COUNT, scanGroups);
	dispatch(TAG_K_COUNTSUM, 1);
	dispatch(TAG_K_ENDS, scanGroups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (frame.slots + 63) / 64);
	// (a workgroup for each group named, up to the most a dispatch can have: then each takes several)
	if (frame.shadows != 0)
		dispatch(TAG_K_SHADOW, std::min(frame.shadows, 65535u));
	if (frame.trd != 0)
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);

	// for whatever draws from them
	const vk::MemoryBarrier after(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eVertexAttributeRead
			| vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eShaderRead
			| vk::AccessFlagBits::eUniformRead);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eVertexInput
			| vk::PipelineStageFlagBits::eDrawIndirect | vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader
			| vk::PipelineStageFlagBits::eComputeShader,
			{}, after, nullptr, nullptr);
	if (r.queries)
	{
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *r.queries, 2 * index + 1);
		slotTimed[index] = true;
	}
	cmd.end();
	// the context's slice is read until this frame is done
	ctx->gpu_serial = serial;
	if (++statFrames % 600 == 0)
		Log(" so far");
}
