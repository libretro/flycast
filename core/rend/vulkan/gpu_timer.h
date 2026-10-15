/* The GPU time of the screen's renders, for the log: a timestamp before a
 * frame's commands and one after, read back when the frame's slot of the
 * command pool comes round again - its fence has been waited for by then,
 * so the read never waits, and nothing is read before that. Nothing is
 * allocated after Init(): the times are kept in room made there.
 *
 * At Term(), one line: how many frames, and their mean, 99th percentile
 * and longest GPU time. */
#pragma once
#include <algorithm>
#include <vector>
#include "vulkan_context.h"

class GpuFrameTimer
{
public:
	void Init(int slots, const char *what)
	{
		Term();
		this->what = what;
		VulkanContext *context = VulkanContext::Instance();
		const std::vector<vk::QueueFamilyProperties> families = context->GetPhysicalDevice().getQueueFamilyProperties();
		const u32 family = context->GetGraphicsQueueFamilyIndex();
		const u32 bits = family < families.size() ? families[family].timestampValidBits : 0;
		if (bits == 0 || slots <= 0)
			return;
		queries = context->GetDevice().createQueryPoolUnique(vk::QueryPoolCreateInfo(vk::QueryPoolCreateFlags(),
				vk::QueryType::eTimestamp, 2 * slots));
		period = context->GetPhysicalDevice().getProperties().limits.timestampPeriod;
		mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
		written.assign(slots, false);
		times.clear();
		times.reserve(MaxTimes);
	}

	/* Before the first command of the frame in @slot of the command pool,
	 * outside a render pass: the time the slot's last frame took is taken */
	void Begin(vk::CommandBuffer cmd, int slot)
	{
		if (!queries || slot < 0 || slot >= (int)written.size())
			return;
		if (written[slot])
		{
			u64 ts[2];
			if (VulkanContext::Instance()->GetDevice().getQueryPoolResults(*queries, 2 * slot, 2, sizeof(ts), ts, sizeof(u64),
					vk::QueryResultFlagBits::e64) == vk::Result::eSuccess && times.size() < MaxTimes)
				times.push_back((float)((double)((ts[1] - ts[0]) & mask) * period / 1e6));
		}
		cmd.resetQueryPool(*queries, 2 * slot, 2);
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *queries, 2 * slot);
		written[slot] = true;
	}
	// ...and after its last
	void End(vk::CommandBuffer cmd, int slot)
	{
		if (!queries || slot < 0 || slot >= (int)written.size())
			return;
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *queries, 2 * slot + 1);
	}

	void Term()
	{
		if (!times.empty())
		{
			std::vector<float> sorted(times);
			std::sort(sorted.begin(), sorted.end());
			double sum = 0;
			for (float t : sorted)
				sum += t;
			NOTICE_LOG(RENDERER, "Vulkan %s: GPU time of %u frames: mean %.3f ms, 99th percentile %.3f ms, longest %.3f ms", what,
					(u32)sorted.size(), sum / sorted.size(), sorted[(sorted.size() * 99) / 100 < sorted.size() ? (sorted.size() * 99) / 100 : sorted.size() - 1],
					sorted.back());
		}
		times.clear();
		written.clear();
		queries.reset();
	}

private:
	static constexpr size_t MaxTimes = 1 << 16;
	vk::UniqueQueryPool queries;
	double period = 0;	// ns a tick
	u64 mask = 0;
	std::vector<bool> written;
	std::vector<float> times;	// ms
	const char *what = "";
};
