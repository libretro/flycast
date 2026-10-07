#include "wait_site.h"
#include "shil.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <string.h>

#if FEAT_SHREC != DYNAREC_NONE

u32 sh4_write_gen;

// A block of sites at a time, never moved: recompiled code has their addresses
static std::vector<std::unique_ptr<WaitSite[]>> wait_sites;
static u32 wait_sites_used;
#define WAIT_SITES_PER_CHUNK 1024

WaitSite *sh4_wait_site()
{
	if (wait_sites_used == wait_sites.size() * WAIT_SITES_PER_CHUNK)
	{
		if (wait_sites.size() == 64)
			return nullptr;		// 65536 loops: the rest go unwatched
		wait_sites.emplace_back(new WaitSite[WAIT_SITES_PER_CHUNK]);
	}
	WaitSite *site = &wait_sites[wait_sites_used / WAIT_SITES_PER_CHUNK][wait_sites_used % WAIT_SITES_PER_CHUNK];
	wait_sites_used++;
	memset(site, 0, sizeof(*site));
	return site;
}

void sh4_wait_sites_reset()
{
	wait_sites_used = 0;
}

bool sh4_block_writes(const RuntimeBlockInfo *block)
{
	for (const shil_opcode& op : block->oplist)
		if (op.op == shop_writem || op.op == shop_ifb || op.op == shop_pref)
			return true;
	return false;
}

u32 sh4_wait_check(WaitSite *site)
{
	const u64 now = sh4_sched_now64();

	if (site->valid && site->slice == now)
	{
		if (site->gen == sh4_write_gen && !memcmp(site->regs, &Sh4cntx, sizeof(site->regs)))
		{
			site->backoff = 0;
			site->skip = 0;
			return 1;
		}
		// it wrote something, or its registers moved: a loop with work to do
		site->backoff = std::min(site->backoff * 2 + 1, 1023u);
		site->skip = site->backoff;
		site->valid = false;
		return 0;
	}
	// nothing from this time slice to compare with: take it, and look at the next pass
	memcpy(site->regs, &Sh4cntx, sizeof(site->regs));
	site->gen = sh4_write_gen;
	site->slice = now;
	site->valid = true;
	site->skip = 0;
	return 0;
}

#endif
