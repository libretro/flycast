/* See pso.h. */
#include "pso.h"
#include <algorithm>
#include <cstring>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

VkPsoWorker VkPsoWorker::instance;

void PsoTable::Init(u32 capacity, PsoBuilder *owner)
{
	if (!slots)
	{
		u32 n = 16;
		while (n < capacity)
			n <<= 1;
		slots = new PsoSlot[n];
		mask = n - 1;
		for (u32 i = 0; i < n; i++)
		{
			slots[i].owner = nullptr;
			slots[i].pipeline = VK_NULL_HANDLE;
			slots[i].next = nullptr;
			retro_atomic_int_init(&slots[i].state, PSO_EMPTY);
		}
	}
	this->owner = owner;
}

PsoSlot *PsoTable::Insert(u64 key)
{
	// kept under three quarters full: Find() always meets an empty slot
	if (used >= (mask + 1) / 4 * 3)
		return nullptr;
	u32 i = Hash(key);
	while (slots[i].owner)
		i = (i + 1) & mask;
	PsoSlot& s = slots[i];
	s.key = key;
	s.owner = owner;
	s.pipeline = VK_NULL_HANDLE;
	s.gen = owner->Generation();
	retro_atomic_store_relaxed_int(&s.state, PSO_EMPTY);
	used++;
	return &s;
}

void PsoTable::Clear(vk::Device device)
{
	if (slots == nullptr)
		return;
	for (u32 i = 0; i <= mask; i++)
	{
		PsoSlot& s = slots[i];
		if (!s.owner)
			continue;
		if (s.pipeline != VK_NULL_HANDLE)
			device.destroyPipeline(vk::Pipeline(s.pipeline));
		s.pipeline = VK_NULL_HANDLE;
		s.owner = nullptr;
		retro_atomic_store_relaxed_int(&s.state, PSO_EMPTY);
	}
	used = 0;
}

void PsoKeyLog::Init()
{
	if (!keys)
		keys = new u64[CAPACITY];
	count = 0;
}

void PsoKeyLog::Term()
{
	delete [] keys;
	keys = nullptr;
	count = 0;
}

const u64 *PsoKeyLog::Keys(u32& n)
{
	if (keys)
	{
		std::sort(keys, keys + count);
		count = (u32)(std::unique(keys, keys + count) - keys);
	}
	n = count;
	return keys;
}

void PsoKeyLog::Load(const u64 *from, u32 n)
{
	Init();
	count = std::min<u32>(n, CAPACITY);
	memcpy(keys, from, count * sizeof(u64));
}

VkPsoWorker::VkPsoWorker()
{
	retro_atomic_int_init(&stop, 0);
	retro_atomic_int_init(&requested, 0);
	retro_atomic_int_init(&answered, 0);
	retro_atomic_int_init(&built, 0);
	retro_atomic_int_init(&batchActive, 0);
	retro_atomic_int_init(&batchNext, 0);
	retro_atomic_int_init(&batchDone, 0);
	retro_atomic_int_init(&batchInside, 0);
}

static VkPipelineCache MakeCache(vk::Device device, const void *data, size_t size, bool externallySynced)
{
	VkPipelineCacheCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.flags = externallySynced ? pso_ext::PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED : 0;
	info.initialDataSize = size;
	info.pInitialData = data;
	VkPipelineCache cache = VK_NULL_HANDLE;
	if (vkCreatePipelineCache((VkDevice)device, &info, nullptr, &cache) != VK_SUCCESS && size != 0)
	{
		// (data the device will not take: an empty cache)
		info.initialDataSize = 0;
		info.pInitialData = nullptr;
		vkCreatePipelineCache((VkDevice)device, &info, nullptr, &cache);
	}
	return cache;
}

void VkPsoWorker::Start(vk::Device device, const void *initialData, size_t size, bool externallySynced)
{
	this->device = device;
	cache = MakeCache(device, initialData, size, externallySynced);
	retro_atomic_store_release_int(&stop, 0);
	retro_atomic_store_relaxed_int(&requested, 0);
	retro_atomic_store_relaxed_int(&answered, 0);
	syncBuilt = 0;
#ifndef TARGET_NO_THREADS
	thread = sthread_create(Entry, this);
	if (thread == nullptr)
		WARN_LOG(RENDERER, "Vulkan: no pipeline thread; polygons are drawn with the general pipelines");
#endif
}

void VkPsoWorker::Stop(vk::PipelineCache into)
{
#ifndef TARGET_NO_THREADS
	if (thread != nullptr)
	{
		retro_atomic_fetch_add_seq_cst_int(&stop, 1);
		work.Notify();
		sthread_join(thread);
		thread = nullptr;
	}
#endif
	if (cache != VK_NULL_HANDLE)
	{
		MergeInto(into);
		vkDestroyPipelineCache((VkDevice)device, cache, nullptr);
		cache = VK_NULL_HANDLE;
	}
	NOTICE_LOG(RENDERER, "Vulkan pipelines: %u made by the pipeline thread, %u by a draw that had to wait",
			Built(), syncBuilt);
}

void VkPsoWorker::MergeInto(vk::PipelineCache into)
{
	if (cache != VK_NULL_HANDLE && into)
	{
		VkPipelineCache src = cache;
		vkMergePipelineCaches((VkDevice)device, (VkPipelineCache)into, 1, &src);
	}
}

void VkPsoWorker::Ask(PsoSlot *slot, List& list)
{
	slot->gen = slot->owner->Generation();
	retro_atomic_store_relaxed_int(&slot->state, PSO_QUEUED);
	retro_atomic_store_release_int(&requested, retro_atomic_load_relaxed_int(&requested) + 1);
	list.Push(slot);
#ifndef TARGET_NO_THREADS
	work.Notify();
#endif
}

void VkPsoWorker::Build(PsoSlot *slot, vk::PipelineCache cache)
{
	slot->pipeline = slot->owner->BuildPipeline(slot->key, cache);
	retro_atomic_store_release_int(&slot->state, slot->pipeline != VK_NULL_HANDLE ? PSO_READY : PSO_FAILED);
}

void VkPsoWorker::BuildSync(PsoSlot *slot, vk::PipelineCache cache)
{
	instance.syncBuilt++;
	Build(slot, cache);
}

bool VkPsoWorker::AllAnswered() const
{
	return retro_atomic_load_acquire_int(&answered) == retro_atomic_load_relaxed_int(&requested);
}

bool VkPsoWorker::BatchDone() const
{
	return (u32)retro_atomic_load_acquire_int(&batchDone) == batchSize;
}

bool VkPsoWorker::BatchLeft() const
{
	return retro_atomic_load_seq_cst_int(&batchInside) == 0;
}

void VkPsoWorker::Wait(bool (VkPsoWorker::*done)() const)
{
#ifndef TARGET_NO_THREADS
	for (;;)
	{
		if ((this->*done)())
			return;
		int key = idle.Prepare();
		if ((this->*done)())
		{
			idle.Cancel();
			return;
		}
		idle.Commit(key);
	}
#endif
}

void VkPsoWorker::WaitIdle()
{
	if (thread != nullptr)
		Wait(&VkPsoWorker::AllAnswered);
}

void VkPsoWorker::BuildNow(PsoSlot *const *slots, u32 n, vk::PipelineCache cache)
{
	if (n == 0)
		return;
	for (u32 i = 0; i < n; i++)
		retro_atomic_store_relaxed_int(&slots[i]->state, PSO_QUEUED);
	batch = slots;
	batchSize = n;
	retro_atomic_store_relaxed_int(&batchNext, 0);
	retro_atomic_store_relaxed_int(&batchDone, 0);
	retro_atomic_fetch_add_seq_cst_int(&batchActive, 1);
#ifndef TARGET_NO_THREADS
	if (thread != nullptr)
		work.Notify();
#endif
	for (;;)
	{
		const u32 i = (u32)retro_atomic_fetch_add_int(&batchNext, 1);
		if (i >= n)
			break;
		Build(slots[i], cache);
		retro_atomic_fetch_add_int(&batchDone, 1);
	}
	Wait(&VkPsoWorker::BatchDone);
	retro_atomic_fetch_add_seq_cst_int(&batchActive, -1);
	// (the worker may still be looking at the batch: not to be changed before it has gone)
	Wait(&VkPsoWorker::BatchLeft);
	batch = nullptr;
	batchSize = 0;
}

// the worker's part of BuildNow()'s slots: whether it made any
bool VkPsoWorker::HelpBatch()
{
	if (retro_atomic_load_acquire_int(&batchActive) == 0)
		return false;
	retro_atomic_fetch_add_seq_cst_int(&batchInside, 1);
	bool any = false;
	if (retro_atomic_load_seq_cst_int(&batchActive) != 0)
	{
		const u32 n = batchSize;
		for (;;)
		{
			const u32 i = (u32)retro_atomic_fetch_add_int(&batchNext, 1);
			if (i >= n)
				break;
			Build(batch[i], vk::PipelineCache(cache));
			retro_atomic_fetch_add_int(&batchDone, 1);
			any = true;
#ifndef TARGET_NO_THREADS
			idle.Notify();
#endif
		}
	}
	retro_atomic_fetch_add_seq_cst_int(&batchInside, -1);
#ifndef TARGET_NO_THREADS
	idle.Notify();
#endif
	return any;
}

void VkPsoWorker::Loop()
{
#ifndef TARGET_NO_THREADS
	sthread_setname("vk pipelines");
	/* Below the threads a frame waits for: what it makes is never waited
	 * for (pso.h), and where there are fewer processors than busy threads
	 * the emulation and the drawing go first */
#if defined(__linux__)
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
	// what is to be made, oldest first: the draws' before the rest
	PsoSlot *urgentHead = nullptr, *urgentTail = nullptr;
	PsoSlot *bgHead = nullptr, *bgTail = nullptr;
	auto append = [](PsoSlot *list, PsoSlot *&head, PsoSlot *&tail) {
		if (list == nullptr)
			return;
		if (tail != nullptr)
			tail->next = list;
		else
			head = list;
		tail = list;
		while (tail->next != nullptr)
			tail = tail->next;
	};
	for (;;)
	{
		if (HelpBatch())
			continue;
		append(urgent.TakeAll(), urgentHead, urgentTail);
		append(background.TakeAll(), bgHead, bgTail);
		PsoSlot *slot = urgentHead;
		if (slot != nullptr)
		{
			urgentHead = slot->next;
			if (urgentHead == nullptr)
				urgentTail = nullptr;
		}
		else if ((slot = bgHead) != nullptr)
		{
			bgHead = slot->next;
			if (bgHead == nullptr)
				bgTail = nullptr;
		}
		if (slot == nullptr)
		{
			if (retro_atomic_load_acquire_int(&stop) != 0)
				return;
			const int key = work.Prepare();
			PsoSlot *u = urgent.TakeAll();
			PsoSlot *b = background.TakeAll();
			if (u != nullptr || b != nullptr || retro_atomic_load_acquire_int(&stop) != 0
					|| retro_atomic_load_acquire_int(&batchActive) != 0)
			{
				work.Cancel();
				append(u, urgentHead, urgentTail);
				append(b, bgHead, bgTail);
			}
			else
				work.Commit(key);
			continue;
		}
		slot->next = nullptr;
		// (asked for before its manager started again: dropped)
		if (slot->gen == slot->owner->Generation() && retro_atomic_load_acquire_int(&stop) == 0)
		{
			Build(slot, vk::PipelineCache(cache));
			retro_atomic_fetch_add_int(&built, 1);
		}
		else
			retro_atomic_store_release_int(&slot->state, PSO_FAILED);
		retro_atomic_fetch_add_int(&answered, 1);
		idle.Notify();
	}
#endif
}
