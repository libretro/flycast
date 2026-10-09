/*
	Tiny cute block manager. Doesn't keep block graphs or anything fancy ...
	Its based on a simple hashed-lists idea
*/

#include <algorithm>
#include <set>
#include <map>
#include "blockmanager.h"
#include "wait_site.h"
#include "ngen.h"

#include "../sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/mem/vmem32.h"
#include "hw/sh4/sh4_sched.h"


#if defined(__unix__) && defined(DYNA_OPROF)
#include <opagent.h>
op_agent_t          oprofHandle;
#endif

#if FEAT_SHREC != DYNAREC_NONE


typedef std::vector<RuntimeBlockInfoPtr> bm_List;
typedef std::set<RuntimeBlockInfoPtr> bm_Set;
typedef std::map<void*, RuntimeBlockInfoPtr> bm_Map;

static bm_Set all_temp_blocks;
static bm_List del_blocks;

bool unprotected_pages[RAM_SIZE_MAX/PAGE_SIZE];
static std::set<RuntimeBlockInfo*> blocks_per_page[RAM_SIZE_MAX/PAGE_SIZE];

static bm_Map blkmap;
// Stats
u32 protected_blocks;
u32 unprotected_blocks;

#define FPCA(x) ((DynarecCodeEntryPtr&)sh4rcb.fpcb[(x>>1)&FPCB_MASK])

// addr must be a physical address
// This returns an executable address
static DynarecCodeEntryPtr DYNACALL bm_GetCode(u32 addr)
{
	DynarecCodeEntryPtr rv = FPCA(addr);

	return rv;
}

// addr must be a virtual address
// This returns an executable address
DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr)
{
#ifndef NO_MMU
	if (!mmu_enabled())
#endif
		return bm_GetCode(addr);
#ifndef NO_MMU
	else
	{
		if (addr & 1)
		{
			switch (addr)
			{
#ifdef USE_WINCE_HACK
			case 0xfffffde7: // GetTickCount
				// This should make this syscall faster
				r[0] = sh4_sched_now64() * 1000 / SH4_MAIN_CLOCK;
				next_pc = pr;
				break;

			case 0xfffffd05: // QueryPerformanceCounter(u64 *)
				{
					u32 paddr;
					if (mmu_data_translation<MMU_TT_DWRITE, u64>(r[4], paddr) == MMU_ERROR_NONE)
					{
						_vmem_WriteMem64(paddr, sh4_sched_now64() >> 4);
						r[0] = 1;
						next_pc = pr;
					}
					else
					{
						Do_Exception(addr, 0xE0, 0x100);
					}
				}
				break;
#endif

			default:
				Do_Exception(addr, 0xE0, 0x100);
				break;
			}
			addr = next_pc;
		}

		u32 paddr;
		u32 rv = mmu_instruction_translation(addr, paddr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(addr, rv, MMU_TT_IREAD);
			mmu_instruction_translation(next_pc, paddr);
		}

		return bm_GetCode(paddr);
	}
#endif
}

// addr must be a physical address
// This returns an executable address
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr)
{
	DynarecCodeEntryPtr cde = bm_GetCode(addr);  // Returns RX ptr

	if (cde == ngen_FailedToFindBlock)
		return NULL;
	else
		return bm_GetBlock2((void*)cde);  // Returns RX ptr
}

// This takes a RX address and returns the info block ptr (RW space)
RuntimeBlockInfoPtr bm_GetBlock2(void* dynarec_code)
{
	if (blkmap.empty())
		return NULL;

	void *dynarecrw = CC_RX2RW(dynarec_code);
	// Returns a block who's code addr is bigger than dynarec_code (or end)
	auto iter = blkmap.upper_bound(dynarecrw);
	if (iter == blkmap.begin())
		return NULL;
	iter--;  // Need to go back to find the potential candidate

	// However it might be out of bounds, check for that
	if ((u8*)iter->second->code + iter->second->host_code_size < (u8*)dynarecrw)
		return NULL;

	verify(iter->second->contains_code((u8*)dynarecrw));
	return iter->second;
}

static void bm_CleanupDeletedBlocks()
{
	del_blocks.clear();
}

// Takes RX pointer and returns a RW pointer
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code)
{
   void *dynarecrw = CC_RX2RW(dynarec_code);
	if (del_blocks.empty())
		return NULL;
	// Start from the end to get the youngest one
	auto it = del_blocks.end();
	do
	{
		it--;
		if ((*it)->contains_code((u8*)dynarecrw))
			return *it;
	} while (it != del_blocks.begin());

	return NULL;
}

void bm_AddBlock(RuntimeBlockInfo* blk)
{
	RuntimeBlockInfoPtr block(blk);
	if (block->temp_block)
		all_temp_blocks.insert(block);
	auto iter = blkmap.find((void*)blk->code);
	if (iter != blkmap.end()) {
		INFO_LOG(DYNAREC, "DUP: %08X %p %08X %p", iter->second->addr, iter->second->code, block->addr, block->code);
		verify(false);
	}
	blkmap[(void*)block->code] = block;

	verify((void*)bm_GetCode(block->addr) == (void*)ngen_FailedToFindBlock);
	FPCA(block->addr) = (DynarecCodeEntryPtr)CC_RW2RX(block->code);

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		char fname[512];

		sprintf(fname,"sh4:%08X,c:%d,s:%d,h:%d", block->addr, block->guest_cycles, block->guest_opcodes, block->host_opcodes);

		if (op_write_native_code(oprofHandle, fname, (uint64_t)block->code, (void*)block->code, block->host_code_size) != 0)
		{
			INFO_LOG(DYNAREC, "op_write_native_code error");
		}
	}
#endif

}

void bm_DiscardBlock(RuntimeBlockInfo* block)
{
	// Remove from block map
	auto it = blkmap.find((void*)block->code);
	verify(it != blkmap.end());
	RuntimeBlockInfoPtr block_ptr = it->second;

	blkmap.erase(it);

	/* It jumps to its neighbours no longer, and they are told: a block
	 * that goes has those that are linked to it linked again (Discard(),
	 * below), and this one must not be among them when its neighbour goes.
	 * It was left there. Its code would then be patched - "unlinked" - long
	 * after it was gone: no harm where its code is still its own, in the
	 * main cache, which is only ever emptied whole; but the temporary cache
	 * is filled again from the start, and the patch landed in the code of
	 * whatever block was there by then. (And the neighbour kept it from
	 * being freed for as long as the neighbour lived.) */
	if (block_ptr->pNextBlock != NULL)
		block_ptr->pNextBlock->RemRef(block_ptr);
	if (block_ptr->pBranchBlock != NULL)
		block_ptr->pBranchBlock->RemRef(block_ptr);
	block_ptr->pNextBlock = NULL;
	block_ptr->pBranchBlock = NULL;
	block_ptr->Relink();

	// Remove from jump table
	verify((void*)bm_GetCode(block_ptr->addr) == CC_RW2RX((void*)block_ptr->code));
	FPCA(block_ptr->addr) = ngen_FailedToFindBlock;

	if (block_ptr->temp_block)
		all_temp_blocks.erase(block_ptr);

	del_blocks.push_back(block_ptr);
	block_ptr->Discard();
}

void bm_Periodical_1s()
{
	bm_CleanupDeletedBlocks();
}


void bm_vmem_pagefill(void** ptr, u32 size_bytes)
{
	for (size_t i = 0; i < size_bytes / sizeof(ptr[0]); i++)
	{
		ptr[i]=(void*)ngen_FailedToFindBlock;
	}
}

void bm_Reset()
{
	bm_CleanupDeletedBlocks();
	protected_blocks = 0;
	unprotected_blocks = 0;

	if (_nvmem_enabled())
	{
		// Windows cannot lock/unlock a region spanning more than one VirtualAlloc or MapViewOfFile
		// so we have to unlock each region individually
		// No need for this mess in 4GB mode since windows doesn't use it
		/* (P0 and P3 are not this file's while the host maps the MMU's
		 * pages there: nothing in them was protected from here, and to
		 * unprotect an address that nothing is mapped at is to have the
		 * host put memory there.) */
		const bool mmu_has_them = vmem32_enabled();

		if (mmu_has_them)
			;
		else if (RAM_SIZE == 16 * 1024 * 1024)
		{
			mem_region_unlock(virt_ram_base + 0x0C000000, RAM_SIZE);
			mem_region_unlock(virt_ram_base + 0x0D000000, RAM_SIZE);
			mem_region_unlock(virt_ram_base + 0x0E000000, RAM_SIZE);
			mem_region_unlock(virt_ram_base + 0x0F000000, RAM_SIZE);
		}
		else
		{
			mem_region_unlock(virt_ram_base + 0x0C000000, RAM_SIZE);
			mem_region_unlock(virt_ram_base + 0x0E000000, RAM_SIZE);
		}
		if (_nvmem_4gb_space())
		{
			mem_region_unlock(virt_ram_base + 0x8C000000, 0x90000000 - 0x8C000000);
			mem_region_unlock(virt_ram_base + 0xAC000000, 0xB0000000 - 0xAC000000);
			if (!mmu_has_them)
				mem_region_unlock(virt_ram_base + 0xCC000000, 0xD0000000 - 0xCC000000);
		}
	}
	else
	{
		mem_region_unlock(&mem_b[0], RAM_SIZE);
	}
}

/* A page of main memory is to be written to or not, at every address the
 * host has it at. Main memory answers all over area 3 - at 0x0C000000 and
 * at 0x0D000000, 0x0E000000 and 0x0F000000 as well (twice over where there
 * are 32 megabytes of it) - and that in each of the SH4's regions, and
 * where the recompiled code reads and writes memory without asking, every
 * one of those is there for it. Only the first of each region was
 * protected ("TODO wraps"): code written over at another was not found to
 * have been, and what had been compiled from it went on being run. */
static void bm_ProtectPage(u32 addr, bool protect)
{
	// P0, P1, P2, P3
	static const u32 regions[] = { 0x0C000000, 0x8C000000, 0xAC000000, 0xCC000000 };
	u32 count, i, mirror;

	addr = addr & (RAM_MASK - PAGE_MASK);
	if (!_nvmem_enabled())
	{
		if (protect)
			mem_region_lock(&mem_b[addr], PAGE_SIZE);
		else
			mem_region_unlock(&mem_b[addr], PAGE_SIZE);
		return;
	}
	count = _nvmem_4gb_space() ? ARRAY_SIZE(regions) : 1;
	for (i = 0; i < count; i++)
	{
		/* with the MMU on, P0 and P3 are what it says they are: what the
		 * host has mapped there is vmem32's business, which asks before
		 * it maps (bm_IsRamPageProtected()) */
		if ((i == 0 || i == 3) && mmu_enabled() && _nvmem_4gb_space())
			continue;
		for (mirror = 0; mirror < 0x04000000; mirror += RAM_SIZE)
		{
			u8 *page = virt_ram_base + regions[i] + mirror + addr;

			if (protect)
				mem_region_lock(page, PAGE_SIZE);
			else
				mem_region_unlock(page, PAGE_SIZE);
		}
	}
}

static void bm_LockPage(u32 addr)
{
	bm_ProtectPage(addr, true);
}

static void bm_UnlockPage(u32 addr)
{
	bm_ProtectPage(addr, false);
}

void bm_ResetCache()
{
	ngen_ResetBlocks();
	sh4_wait_sites_reset();
	_vmem_bm_reset();

	for (const auto& it : blkmap)
	{
		RuntimeBlockInfoPtr block = it.second;
		block->relink_data = 0;
		block->pNextBlock = 0;
		block->pBranchBlock = 0;
		// needed for the transition to full mmu. Could perhaps limit it to the current block.
		block->Relink();
		// Avoid circular references
		block->Discard();
		del_blocks.push_back(block);
	}

	blkmap.clear();
	// blkmap includes temp blocks as well
	all_temp_blocks.clear();

	for (auto& block_list : blocks_per_page)
		block_list.clear();

	/* Every page may be protected again, when next something is compiled
	 * from it - but not while the MMU's pages are mapped by the host
	 * (vmem32): a page that has been written to is mapped to be written
	 * there, at however many addresses, and stays so. Protected again, it
	 * was protected everywhere but at the addresses the game uses, and
	 * code written over through those went on being run as it had been.
	 * Such a page stays as it is: its blocks look at their code before
	 * they run. */
	if (!vmem32_enabled())
		memset(unprotected_pages, 0, sizeof(unprotected_pages));

#ifdef DYNA_OPROF
	if (oprofHandle)
	{
		for (int i=0;i<del_blocks.size();i++)
		{
			if (op_unload_native_code(oprofHandle, (uint64_t)del_blocks[i]->code) != 0)
			{
				INFO_LOG(DYNAREC, "op_unload_native_code error");
			}
		}
	}
#endif
}

/* The temporary cache is emptied: because it is full (@full false: its
 * blocks go, the others stay), or with everything else (@full true: after
 * bm_ResetCache(), which has dealt with every block there is).
 *
 * When only the temporary blocks go they go as any block does, one by
 * one: unlinked from the blocks they jump to and from the blocks that
 * jump to them. They used to be taken out of the tables and no more. A
 * block of the main cache that was linked to one went on jumping to where
 * it had been, which is where the next blocks to be compiled are put: it
 * ran another block's code, or the middle of one. That was of no
 * consequence while the temporary cache was only used with the MMU on,
 * when nothing is linked; since 355f8028 it is used without, for code
 * that a game keeps rewriting. */
void bm_ResetTempCache(bool full)
{
	if (!full)
	{
		// (bm_DiscardBlock() takes them out of all_temp_blocks as it goes)
		const bm_List blocks(all_temp_blocks.begin(), all_temp_blocks.end());

		for (const auto& block : blocks)
			bm_DiscardBlock(block.get());
	}
	del_blocks.insert(del_blocks.begin(),all_temp_blocks.begin(),all_temp_blocks.end());
	all_temp_blocks.clear();
}

void bm_Init()
{

#ifdef DYNA_OPROF
	oprofHandle=op_open_agent();
	if (oprofHandle==0)
		INFO_LOG(DYNAREC, "bm: Failed to open oprofile");
	else
		INFO_LOG(DYNAREC, "bm: Oprofile integration enabled !");
#endif
	bm_Reset();
}

void bm_Term()
{
#ifdef DYNA_OPROF
	if (oprofHandle) op_close_agent(oprofHandle);
	
	oprofHandle=0;
#endif
	bm_Reset();
}

#if 0
void sh4_jitsym(FILE* out)
{
	for (auto& it : blkmap)
	{
		RuntimeBlockInfoPtr& block = it.second;
		fprintf(out, "%p %d %08X\n", block->code, block->host_code_size, block->addr);
	}
}

u32 GetLookup(RuntimeBlockInfo* elem)
{
	return elem->lookups;
}

bool UDgreater ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs > elem2->runs;
}

bool UDgreater2 ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs*elem1->host_opcodes > elem2->runs*elem2->host_opcodes;
}

bool UDgreater3 ( RuntimeBlockInfo* elem1, RuntimeBlockInfo* elem2 )
{
	return elem1->runs*elem1->host_opcodes/elem1->guest_cycles > elem2->runs*elem2->host_opcodes/elem2->guest_cycles;
}

void bm_PrintTopBlocks()
{
	double total_lups=0;
	double total_runs=0;
	double total_cycles=0;
	double total_hops=0;
	double total_sops=0;

	for (size_t i=0;i<all_blocks.size();i++)
	{
		total_lups+=GetLookup(all_blocks[i]);
		total_cycles+=all_blocks[i]->runs*all_blocks[i]->guest_cycles;
		total_hops+=all_blocks[i]->runs*all_blocks[i]->host_opcodes;
		total_sops+=all_blocks[i]->runs*all_blocks[i]->guest_opcodes;
		total_runs+=all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, "Total lookups:  %.0fKRuns, %.0fKLuops, Total cycles: %.0fMhz, Total Hops: %.0fMips, Total Sops: %.0fMips!",total_runs/1000,total_lups/1000,total_cycles/1000/1000,total_hops/1000/1000,total_sops/1000/1000);
	total_hops/=100;
	total_cycles/=100;
	total_runs/=100;

	double sel_hops=0;
	for (size_t i=0;i<(all_blocks.size()/100);i++)
	{
		INFO_LOG(DYNAREC, "Block %08X: %p, r: %d (c: %d, s: %d, h: %d) (r: %.2f%%, c: %.2f%%, h: %.2f%%)",
			all_blocks[i]->addr, all_blocks[i]->code,all_blocks[i]->runs,
			all_blocks[i]->guest_cycles,all_blocks[i]->guest_opcodes,all_blocks[i]->host_opcodes,

			all_blocks[i]->runs/total_runs,
			all_blocks[i]->guest_cycles*all_blocks[i]->runs/total_cycles,
			all_blocks[i]->host_opcodes*all_blocks[i]->runs/total_hops);
		
		sel_hops+=all_blocks[i]->host_opcodes*all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, " >-< %.2f%% covered in top 1%% blocks",sel_hops/total_hops);

	size_t i;
	for (i=all_blocks.size()/100;sel_hops/total_hops<50;i++)
	{
		INFO_LOG(DYNAREC, "Block %08X: %p, r: %d (c: %d, s: %d, h: %d) (r: %.2f%%, c: %.2f%%, h: %.2f%%)",
			all_blocks[i]->addr, all_blocks[i]->code,all_blocks[i]->runs,
			all_blocks[i]->guest_cycles,all_blocks[i]->guest_opcodes,all_blocks[i]->host_opcodes,

			all_blocks[i]->runs/total_runs,
			all_blocks[i]->guest_cycles*all_blocks[i]->runs/total_cycles,
			all_blocks[i]->host_opcodes*all_blocks[i]->runs/total_hops);
		
		sel_hops+=all_blocks[i]->host_opcodes*all_blocks[i]->runs;
	}

	INFO_LOG(DYNAREC, " >-< %.2f%% covered in top %.2f%% blocks",sel_hops/total_hops,i*100.0/all_blocks.size());

}

void bm_Sort()
{
	INFO_LOG(DYNAREC, "!!!!!!!!!!!!!!!!!!! BLK REPORT !!!!!!!!!!!!!!!!!!!!");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on Runs ! ----     ");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "<><><><><><><><><><><><><><><><><><><><><><><><><>");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on hops ! ----     ");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater2);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "<><><><><><><><><><><><><><><><><><><><><><><><><>");

	INFO_LOG(DYNAREC, "     ---- Blocks: Sorted based on wefs ! ----     ");
	std::sort(all_blocks.begin(),all_blocks.end(),UDgreater3);
	bm_PrintTopBlocks();

	INFO_LOG(DYNAREC, "^^^^^^^^^^^^^^^^^^^ END REPORT ^^^^^^^^^^^^^^^^^^^");

	for (size_t i=0;i<all_blocks.size();i++)
	{
		all_blocks[i]->runs=0;
	}
}
#endif

RuntimeBlockInfo::~RuntimeBlockInfo()
{
	if (sh4_code_size != 0)
	{
		if (read_only)
			protected_blocks--;
		else
			unprotected_blocks--;
	}
}

void RuntimeBlockInfo::AddRef(const RuntimeBlockInfoPtr& other)
{ 
	pre_refs.push_back(other); 
}

void RuntimeBlockInfo::RemRef(const RuntimeBlockInfoPtr& other)
{ 
	bm_List::iterator it = std::find(pre_refs.begin(), pre_refs.end(), other);
	if (it != pre_refs.end())
		pre_refs.erase(it);
}

void RuntimeBlockInfo::Discard()
{
	// Update references
	for (RuntimeBlockInfoPtr& ref : pre_refs)
	{
		if (ref->NextBlock == vaddr)
			ref->pNextBlock = NULL;
		if (ref->BranchBlock == vaddr)
			ref->pBranchBlock = NULL;
		ref->relink_data = 0;
		ref->Relink();
	}
	pre_refs.clear();

	if (read_only)
	{
		// Remove this block from the per-page block lists
		for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + this->sh4_code_size; addr += PAGE_SIZE)
		{
         auto& block_list = blocks_per_page[(addr & RAM_MASK) / PAGE_SIZE];
			block_list.erase(this);
		}
	}
}

void RuntimeBlockInfo::SetProtectedFlags()
{
#ifndef TARGET_NO_EXCEPTIONS
	// Don't write protect rom and BIOS/IP.BIN (Grandia II)
	if (!IsOnRam(addr) || (addr & 0x1FFF0000) == 0x0c000000)
#endif
	{
		this->read_only = false;
		unprotected_blocks++;
		return;
	}
	for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + sh4_code_size; addr += PAGE_SIZE)
	{
		if (unprotected_pages[(addr & RAM_MASK) / PAGE_SIZE])
		{
			this->read_only = false;
			unprotected_blocks++;
			return;
		}
	}
	this->read_only = true;
	protected_blocks++;
	for (u32 addr = this->addr & ~PAGE_MASK; addr < this->addr + sh4_code_size; addr += PAGE_SIZE)
	{
		auto& block_list = blocks_per_page[(addr & RAM_MASK) / PAGE_SIZE];
		if (block_list.empty())
			bm_LockPage(addr);
		block_list.insert(this);
	}
}

void bm_RamWriteAccess(u32 addr)
{
	addr &= RAM_MASK;
	if (unprotected_pages[addr / PAGE_SIZE])
	{
		ERROR_LOG(DYNAREC, "Page %08x already unprotected", addr);
		die("Fatal error");
	}
	unprotected_pages[addr / PAGE_SIZE] = true;
	bm_UnlockPage(addr);
   std::set<RuntimeBlockInfo*>& block_list = blocks_per_page[addr / PAGE_SIZE];
   std::vector<RuntimeBlockInfo*> list_copy;
	list_copy.insert(list_copy.begin(), block_list.begin(), block_list.end());
	if (!list_copy.empty())
		DEBUG_LOG(DYNAREC, "bm_RamWriteAccess write access to %08x pc %08x", addr, next_pc);
	for (auto& block : list_copy)
	{
		bm_DiscardBlock(block);
	}
	verify(block_list.empty());
}

bool bm_RamWriteAccess(void *p)
{
	if (_nvmem_enabled())
	{
		if (_nvmem_4gb_space())
		{
			if ((u8 *)p < virt_ram_base || (u8 *)p >= virt_ram_base + 0x100000000L)
				return false;
		}
		else
		{
			if ((u8 *)p < virt_ram_base || (u8 *)p >= virt_ram_base + 0x20000000)
				return false;
		}
		u32 addr = (u8*)p - virt_ram_base;
		if (mmu_enabled() && _nvmem_4gb_space() && ((addr & 0x80000000) == 0 || (addr >> 29) == 6))
			// If mmu enabled, let vmem32 manage user space, and P3
			// shouldn't be necessary since it's called first
			return false;
		if (!IsOnRam(addr) || ((addr >> 29) > 0 && (addr >> 29) < 4))	// system RAM is not mapped to 20, 40 and 60 because of laziness
			return false;
		bm_RamWriteAccess(addr);
	}
	else
	{
		if ((u8 *)p < &mem_b[0] || (u8 *)p >= &mem_b[RAM_SIZE])
			return false;
		bm_RamWriteAccess((u32)((u8 *)p - &mem_b[0]));
	}

	return true;
}

bool print_stats;
#if 0
void fprint_hex(FILE* d,const char* init,u8* ptr, u32& ofs, u32 limit)
{
	int base=ofs;
	int cnt=0;
	while(ofs<limit)
	{
		if (cnt==32)
		{
			fputs("\n",d);
			cnt=0;
		}

		if (cnt==0)
			fprintf(d,"%s:%d:",init,ofs-base);

		fprintf(d," %02X",ptr[ofs++]);
		cnt++;
	}
	fputs("\n",d);
}



void print_blocks()
{
	FILE* f=0;

	if (print_stats)
	{
		f=fopen(get_writable_data_path("/blkmap.lst").c_str(),"w");
		print_stats=false;

		INFO_LOG(DYNAREC, "Writing blocks to %p", f);
	}

	for (auto it : blkmap)
	{
		RuntimeBlockInfoPtr blk = it.second;
		if (f)
		{
			fprintf(f,"block: %p\n",blk.get());
			fprintf(f,"vaddr: %08X\n",blk->vaddr);
			fprintf(f,"paddr: %08X\n",blk->addr);
			fprintf(f,"hash: %s\n",blk->hash());
			fprintf(f,"hash_rloc: %s\n",blk->hash());
			fprintf(f,"code: %p\n",blk->code);
			fprintf(f,"runs: %d\n",blk->runs);
			fprintf(f,"BlockType: %d\n",blk->BlockType);
			fprintf(f,"NextBlock: %08X\n",blk->NextBlock);
			fprintf(f,"BranchBlock: %08X\n",blk->BranchBlock);
			fprintf(f,"pNextBlock: %p\n",blk->pNextBlock);
			fprintf(f,"pBranchBlock: %p\n",blk->pBranchBlock);
			fprintf(f,"guest_cycles: %d\n",blk->guest_cycles);
			fprintf(f,"guest_opcodes: %d\n",blk->guest_opcodes);
			fprintf(f,"host_opcodes: %d\n",blk->host_opcodes);
			fprintf(f,"il_opcodes: %zd\n",blk->oplist.size());

			u32 hcode=0;
			s32 gcode=-1;
			u8* pucode=(u8*)blk->code;

			size_t j=0;
			
			fprintf(f,"{\n");
			for (;j<blk->oplist.size();j++)
			{
				shil_opcode* op = &blk->oplist[j];
				//fprint_hex(f,"//h:",pucode,hcode,op->host_offs);

				if (gcode!=op->guest_offs)
				{
					gcode=op->guest_offs;
					u32 rpc=blk->vaddr+gcode;
#ifndef NO_MMU
					try {
#endif
						u16 op=IReadMem16(rpc);

					char temp[128];
						OpDesc[op]->Disassemble(temp,rpc,op);

						fprintf(f,"//g: %04X %s\n", op, temp);
#ifndef NO_MMU
					} catch (SH4ThrownException& ex) {
						fprintf(f,"//g: ???? (page fault)\n");
					}
#endif
				}

            std::string s=op->dissasm();
				fprintf(f,"//il:%d:%d:%s\n",op->guest_offs,op->host_offs,s.c_str());
			}
			
			//fprint_hex(f,"//h:",pucode,hcode,blk->host_code_size);

			fprintf(f,"}\n");
		}

		blk->runs=0;
	}

	if (f) fclose(f);
}
#endif
#endif

