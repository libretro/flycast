/* What video memory a renderer may use: read once from the API when the
 * renderer starts, for its pools to be sized against at that time and never
 * grown later (ssaa_pattern.h, ssaa_clamp_samples()).
 *
 * - Vulkan: VK_EXT_memory_budget, the largest device-local heap's budget
 *   and what the process already uses of it; without it, half that heap.
 * - Direct3D (DXGI): IDXGIAdapter3::QueryVideoMemoryInfo(), the local
 *   segment's budget and usage; without it, the adapter's dedicated
 *   memory, or half its shared memory when it has none.
 * - OpenGL: GL_NVX_gpu_memory_info's available memory, or
 *   GL_ATI_meminfo's free texture memory; without either, a fixed figure
 *   (256 MB for OpenGL ES, 512 MB for desktop OpenGL).
 * In KiB. "exact" when the API said what the budget is. */
#ifndef REND_VRAM_BUDGET_H
#define REND_VRAM_BUDGET_H

#include "types.h"
#include "log/Log.h"

struct VramBudget
{
	u32 budget_kib = 0;	// what may be used, all together
	u32 usage_kib = 0;	// of it, used already when it was read
	bool exact = false;	// from the API; or an estimate

	// what is left for new pools
	u32 available_kib() const { return budget_kib > usage_kib ? budget_kib - usage_kib : 0; }
};

static inline u32 vram_kib(u64 bytes)
{
	const u64 kib = bytes >> 10;
	return kib > 0xffffffffull ? 0xffffffffu : (u32)kib;
}

static inline void vram_budget_log(const char *api, const VramBudget& budget)
{
	NOTICE_LOG(RENDERER, "%s: video memory budget %u MB, %u MB in use (%s)", api,
			budget.budget_kib >> 10, budget.usage_kib >> 10, budget.exact ? "reported" : "estimated");
}

#endif
