/* The raster renderers' pipelines, made without a frame waiting for one.
 *
 * A polygon's pipeline is kept under a key: everything the pipeline is made
 * of, worked out from the polygon (PipelineManager::Key(),
 * OITPipelineManager::Key()). The first time a key is drawn its pipeline is
 * asked of the worker thread below, and the polygon is drawn at once with
 * the renderer's general pipeline for it instead: the same shader with its
 * bits read from the push constants (pso_key.h) and, where the device has
 * them (VK_EXT_extended_dynamic_state and _state3), the depth, stencil,
 * cull, topology and blend states set with the draw. It draws the same
 * pixels. The general pipelines, the modifier volumes' and the other fixed
 * ones are made when the renderer starts, by this thread and the worker
 * together, so no draw ever waits for one.
 *
 * A device without the dynamic states has a general pipeline for each of
 * those states it is drawn with; one not made yet (a state not seen in the
 * pipelines kept from earlier runs, PsoKeyLog) is made there and then -
 * the one case a draw waits, and only once for each such state.
 *
 * Nothing here takes a lock: a slot's state is an atomic the worker
 * publishes the pipeline with; the worker's requests are an intrusive list
 * (cMpscList), and it sleeps on an eventcount. The worker has a pipeline
 * cache of its own, merged into the context's when that is saved. Every
 * table is made when its renderer is; nothing is allocated in a frame. */
#pragma once
#include "vulkan.h"
#include "lockfree.h"
#include "stdclass.h"

class PsoBuilder;

enum PsoState { PSO_EMPTY, PSO_QUEUED, PSO_READY, PSO_FAILED };

struct PsoSlot
{
	u64 key;
	PsoBuilder *owner;
	retro_atomic_int_t state;	// PsoState: the worker publishes READY or FAILED
	int gen;					// the owner's generation it was asked for in
	VkPipeline pipeline;
	PsoSlot *next;				// the worker's lists
};

/* What makes the pipelines of its slots' keys: a renderer's pipeline
 * manager. BuildPipeline() is called from the worker thread too, and reads
 * only what the manager does not change while it has slots asked for
 * (Reset() waits for the worker first). */
class PsoBuilder
{
public:
	virtual ~PsoBuilder() = default;
	// VK_NULL_HANDLE if it could not be made
	virtual VkPipeline BuildPipeline(u64 key, vk::PipelineCache cache) = 0;
	int Generation() const { return retro_atomic_load_acquire_int(&generation); }

protected:
	PsoBuilder() { retro_atomic_int_init(&generation, 0); }
	// what has been asked for so far is not to be made any more
	void NewGeneration() { retro_atomic_fetch_add_int(&generation, 1); }

private:
	retro_atomic_int_t generation;
};

/* A manager's pipelines by key: open addressing, made once. Only the
 * thread that draws looks keys up and adds them. */
class PsoTable
{
public:
	~PsoTable() { delete [] slots; }
	void Init(u32 capacity, PsoBuilder *owner);
	PsoSlot *Find(u64 key) const
	{
		for (u32 i = Hash(key);; i = (i + 1) & mask)
		{
			PsoSlot& s = slots[i];
			if (!s.owner)
				return nullptr;
			if (s.key == key)
				return &s;
		}
	}
	// nullptr when the table is full
	PsoSlot *Insert(u64 key);
	/* The pipelines destroyed and the table emptied. Nothing of it may be
	 * with the worker (VkPsoWorker::WaitIdle()). */
	void Clear(vk::Device device);
	u32 Used() const { return used; }
	template<typename F>
	void ForEach(F f) const
	{
		for (u32 i = 0; i <= mask; i++)
			if (slots[i].owner)
				f(slots[i]);
	}

private:
	u32 Hash(u64 key) const { return (u32)((key * 0x9E3779B97F4A7C15ull) >> 40) & mask; }

	PsoSlot *slots = nullptr;
	PsoBuilder *owner = nullptr;
	u32 mask = 0;
	u32 used = 0;
};

// a graphics pipeline, or VK_NULL_HANDLE (and a line in the log) if the device could not make it
static inline VkPipeline PsoCreate(vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info)
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult res = vkCreateGraphicsPipelines((VkDevice)device, (VkPipelineCache)cache, 1,
			reinterpret_cast<const VkGraphicsPipelineCreateInfo *>(&info), nullptr, &pipeline);
	if (res != VK_SUCCESS)
	{
		WARN_LOG(RENDERER, "Vulkan: a pipeline could not be made (%d)", (int)res);
		return VK_NULL_HANDLE;
	}
	return pipeline;
}

/* (tests: every polygon drawn with the general pipelines, to see they draw
 * what the polygons' own ones do) */
#ifdef VK_PSO_TEST_GENERAL
static const bool PsoTestGeneral = true;
#else
static const bool PsoTestGeneral = false;
#endif

static inline bool PsoReady(const PsoSlot *slot)
{
	return slot != nullptr && retro_atomic_load_acquire_int(&slot->state) == PSO_READY;
}

/* The keys of the pipelines made, kept with the pipeline cache from one run
 * to the next (VulkanContext): at its start a renderer asks the worker for
 * all of them that fit how it draws now. @who: which manager's. */
class PsoKeyLog
{
public:
	enum { CAPACITY = 8192 };
	void Init();
	void Term();
	void Record(u32 who, u64 key)
	{
		if (count < CAPACITY)
			keys[count++] = ((u64)who << 60) | key;
	}
	template<typename F>
	void ForEach(u32 who, F f) const
	{
		for (u32 i = 0; i < count; i++)
			if ((keys[i] >> 60) == who)
				f(keys[i] & ~(15ull << 60));
	}
	// read from / written to the cache file: the keys, sorted, each once
	const u64 *Keys(u32& n);
	void Load(const u64 *keys, u32 n);

private:
	u64 *keys = nullptr;
	u32 count = 0;
};

/* The worker thread */
class VkPsoWorker
{
public:
	static VkPsoWorker& Instance() { return instance; }

	/* @initialData: the pipeline cache's, given to the worker's cache too;
	 * @externallySynced: its caches are made with
	 * VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT (no driver lock) */
	void Start(vk::Device device, const void *initialData, size_t size, bool externallySynced);
	// the thread joined; its cache merged into @into and destroyed
	void Stop(vk::PipelineCache into);
	/* Without it (a build without threads) nothing is asked of it: the
	 * polygons are drawn with the general pipelines */
	bool Running() const { return thread != nullptr; }

	// the drawing thread: @slot is a new one of its table; made soon
	void Request(PsoSlot *slot) { Ask(slot, urgent); }
	// made when nothing more pressing is asked
	void Prefetch(PsoSlot *slot) { Ask(slot, background); }
	/* @n slots made now, by this thread (with @cache) and the worker
	 * together; back when all are */
	void BuildNow(PsoSlot *const *slots, u32 n, vk::PipelineCache cache);
	// until all that was asked so far is made or dropped
	void WaitIdle();
	// the worker's cache merged into @into (the worker idle)
	void MergeInto(vk::PipelineCache into);

	/* One slot's pipeline made with @cache, and published. (All of the
	 * renderers' graphics pipelines are made here: one place to count.) */
	static void Build(PsoSlot *slot, vk::PipelineCache cache);
	/* A draw that cannot go without @slot's pipeline: made on this thread
	 * now. The slot is not one the worker has. */
	static void BuildSync(PsoSlot *slot, vk::PipelineCache cache);

	u32 Built() const { return (u32)retro_atomic_load_relaxed_int(&built); }
	u32 SyncBuilt() const { return syncBuilt; }

private:
	typedef cMpscList<PsoSlot, &PsoSlot::next> List;

	void Ask(PsoSlot *slot, List& list);
	static void Entry(void *p) { ((VkPsoWorker *)p)->Loop(); }
	void Loop();
	bool HelpBatch();
	void Wait(bool (VkPsoWorker::*done)() const);
	bool AllAnswered() const;
	bool BatchDone() const;
	bool BatchLeft() const;

	static VkPsoWorker instance;

	sthread_t *thread = nullptr;
	vk::Device device;
	VkPipelineCache cache = VK_NULL_HANDLE;
	List urgent;
	List background;
#ifndef TARGET_NO_THREADS
	cEventCount work;	// the worker sleeps here
	cEventCount idle;	// and who waits for it, here
#endif
	retro_atomic_int_t stop;
	retro_atomic_int_t requested;	// written by the drawing thread only
	retro_atomic_int_t answered;
	retro_atomic_int_t built;
	u32 syncBuilt = 0;

	// BuildNow()'s slots, shared out between the two threads
	PsoSlot *const *batch = nullptr;
	u32 batchSize = 0;
	retro_atomic_int_t batchActive;
	retro_atomic_int_t batchNext;
	retro_atomic_int_t batchDone;
	retro_atomic_int_t batchInside;

public:
	VkPsoWorker();
};

/* Device functions of VK_EXT_extended_dynamic_state and _state3 the general
 * pipelines are drawn with (VulkanContext has them when the device does);
 * the Vulkan headers here are older than both */
namespace pso_ext {
enum : int
{
	DYNAMIC_STATE_CULL_MODE = 1000267000,
	DYNAMIC_STATE_PRIMITIVE_TOPOLOGY = 1000267002,
	DYNAMIC_STATE_DEPTH_TEST_ENABLE = 1000267006,
	DYNAMIC_STATE_DEPTH_WRITE_ENABLE = 1000267007,
	DYNAMIC_STATE_DEPTH_COMPARE_OP = 1000267008,
	DYNAMIC_STATE_STENCIL_TEST_ENABLE = 1000267010,
	DYNAMIC_STATE_STENCIL_OP = 1000267011,
	DYNAMIC_STATE_COLOR_BLEND_ENABLE = 1000455010,
	DYNAMIC_STATE_COLOR_BLEND_EQUATION = 1000455011,
	DYNAMIC_STATE_COLOR_WRITE_MASK = 1000455012,
	STRUCTURE_TYPE_EDS_FEATURES = 1000267000,
	STRUCTURE_TYPE_EDS3_FEATURES = 1000455000,
	STRUCTURE_TYPE_CACHE_CONTROL_FEATURES = 1000297000,
	PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED = 1,
};
struct EdsFeatures
{
	int sType;
	void *pNext;
	VkBool32 extendedDynamicState;
};
struct Eds3Features
{
	int sType;
	void *pNext;
	VkBool32 tessellationDomainOrigin, depthClampEnable, polygonMode, rasterizationSamples, sampleMask,
		alphaToCoverageEnable, alphaToOneEnable, logicOpEnable, colorBlendEnable, colorBlendEquation,
		colorWriteMask, rasterizationStream, conservativeRasterizationMode, extraPrimitiveOverestimationSize,
		depthClipEnable, sampleLocationsEnable, colorBlendAdvanced, provokingVertexMode, lineRasterizationMode,
		lineStippleEnable, depthClipNegativeOneToOne, viewportWScalingEnable, viewportSwizzle,
		coverageToColorEnable, coverageToColorLocation, coverageModulationMode, coverageModulationTableEnable,
		coverageModulationTable, coverageReductionMode, representativeFragmentTestEnable, shadingRateImageEnable;
};
struct CacheControlFeatures
{
	int sType;
	void *pNext;
	VkBool32 pipelineCreationCacheControl;
};
struct ColorBlendEquation
{
	VkBlendFactor srcColorBlendFactor;
	VkBlendFactor dstColorBlendFactor;
	VkBlendOp colorBlendOp;
	VkBlendFactor srcAlphaBlendFactor;
	VkBlendFactor dstAlphaBlendFactor;
	VkBlendOp alphaBlendOp;
};
struct Functions
{
	void (VKAPI_PTR *CmdSetCullMode)(VkCommandBuffer, VkCullModeFlags);
	void (VKAPI_PTR *CmdSetPrimitiveTopology)(VkCommandBuffer, VkPrimitiveTopology);
	void (VKAPI_PTR *CmdSetDepthTestEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetDepthWriteEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetDepthCompareOp)(VkCommandBuffer, VkCompareOp);
	void (VKAPI_PTR *CmdSetStencilTestEnable)(VkCommandBuffer, VkBool32);
	void (VKAPI_PTR *CmdSetStencilOp)(VkCommandBuffer, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp);
	void (VKAPI_PTR *CmdSetColorBlendEnable)(VkCommandBuffer, uint32_t, uint32_t, const VkBool32 *);
	void (VKAPI_PTR *CmdSetColorBlendEquation)(VkCommandBuffer, uint32_t, uint32_t, const ColorBlendEquation *);
	void (VKAPI_PTR *CmdSetColorWriteMask)(VkCommandBuffer, uint32_t, uint32_t, const VkColorComponentFlags *);
	bool depthStencil;	// VK_EXT_extended_dynamic_state: cull, topology, depth, stencil
	bool blend;			// VK_EXT_extended_dynamic_state3: blend enable, equation, write mask
};
}
