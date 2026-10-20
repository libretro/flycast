/* The video memory a DXGI adapter has for a renderer to use
 * (rend/vram_budget.h): its budget for its local memory, by
 * QueryVideoMemoryInfo() (DXGI 1.4, Windows 10); before that, its dedicated
 * memory, or half its shared memory for one that has none. The Direct3D 11
 * and Direct3D 12 renderers both ask their adapter this. Windows only. */
#ifndef REND_DXGI_BUDGET_H
#define REND_DXGI_BUDGET_H

#include "rend/d3d_system.h"
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include "rend/vram_budget.h"

static inline VramBudget dxgi_vram_budget(IDXGIAdapter *adapter, const DXGI_ADAPTER_DESC *desc)
{
	VramBudget budget;
	IDXGIAdapter3 *adapter3 = nullptr;
	if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&adapter3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info;
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget != 0)
		{
			budget.budget_kib = vram_kib(info.Budget);
			budget.usage_kib = vram_kib(info.CurrentUsage);
			budget.exact = true;
		}
		adapter3->Release();
	}
	if (!budget.exact && desc != nullptr)
		budget.budget_kib = vram_kib(desc->DedicatedVideoMemory != 0 ? (u64)desc->DedicatedVideoMemory
				: (u64)desc->SharedSystemMemory / 2);
	return budget;
}

#endif
