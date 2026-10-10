/*
	In case you wonder, the extern "C" stuff are for the assembly code on beagleboard/pandora
*/
#include <memory>
#include <map>
#include "types.h"
#include "decoder.h"
#pragma once

typedef void (*DynarecCodeEntryPtr)();
typedef std::shared_ptr<RuntimeBlockInfo> RuntimeBlockInfoPtr;

#define CODE_SIZE   (16*1024*1024)

/* For the blocks whose code keeps being rewritten (smc_hotspots). A build
 * with NO_MMU has them as well - it used to have no room here at all, and
 * would have been given a block to write into it all the same. */
#define TEMP_CODE_SIZE (1024*1024)

/* The room there has to be in a code cache before a block is compiled into
 * it: more than any one block comes to. A block is up to 511 operations,
 * and with the MMU on a load or store can be over a hundred bytes of code.
 * The largest seen here is 10 KB (Sega Rally 2); it was 16 KB that was
 * asked for, and upstream went to 32 KB after Tomb Raider: The Last
 * Revelation, another Windows CE game, ran past it. 64 KB of 16 MB is
 * nothing to give for not having to wonder. */
#define CODE_MARGIN (64*1024)

extern u8* CodeCache;

struct RuntimeBlockInfo_Core
{
	u32 addr;
	DynarecCodeEntryPtr code;
	u32 lookups;
};

struct RuntimeBlockInfo: RuntimeBlockInfo_Core
{
	bool Setup(u32 pc,fpscr_t fpu_cfg);
	const char* hash();

	u32 vaddr;

	u32 host_code_size;	   /* in bytes */
	u32 sh4_code_size;      /* in bytes */

	u32 runs;
	s32 staging_runs;

	fpscr_t fpu_cfg;
	u32 guest_cycles;
	u32 guest_opcodes;
	u32 host_opcodes;
	bool has_fpu_op;
	u32 blockcheck_failures;
	bool temp_block;
	// with the MMU on: may go straight on into another block (rdv_MmuMayGoOn())
	bool mmu_go_on;

	u32 BranchBlock; /* if not 0xFFFFFFFF then jump target */
	u32 NextBlock;   /* if not 0xFFFFFFFF then next block (by position) */

	/* 0 if not available */
	RuntimeBlockInfo* pBranchBlock;
	RuntimeBlockInfo* pNextBlock; 

	u32 relink_offset;
	u32 relink_data;
	u32 csc_RetCache; /* only for stats for now */

	BlockEndType BlockType;
	bool has_jcond;

   std::vector<shil_opcode> oplist;

	bool contains_code(const u8* ptr)
	{
		return ((size_t)(ptr-(u8*)code)) < host_code_size;
	}

	virtual ~RuntimeBlockInfo();

	virtual u32 Relink()=0;
	virtual void Relocate(void* dst)=0;
	
	//predecessors references
   std::vector<RuntimeBlockInfoPtr> pre_refs;

   void AddRef(const RuntimeBlockInfoPtr& other);
	void RemRef(const RuntimeBlockInfoPtr& other);

	void Discard();
	void UpdateRefs();
	void SetProtectedFlags();

	u32 memops;
	u32 linkedmemops;
	std::map<void*, u32> memory_accesses;	// key is host pc when access is made, value is opcode id
	bool read_only;
};


extern "C" {
__attribute__((used)) DynarecCodeEntryPtr DYNACALL bm_GetCodeByVAddr(u32 addr);
/* What bm_GetCodeByVAddr() keeps of where it found the code for an address
 * with the MMU on is to go: an address may be another page now, or a
 * block is gone. */
#if FEAT_SHREC != DYNAREC_NONE
void bm_ForgetVaddrs();
#else
static inline void bm_ForgetVaddrs() {}
#endif
}

RuntimeBlockInfoPtr bm_GetBlock2(void* dynarec_code);
RuntimeBlockInfoPtr bm_GetStaleBlock(void* dynarec_code);
RuntimeBlockInfoPtr DYNACALL bm_GetBlock(u32 addr);

void bm_AddBlock(RuntimeBlockInfo* blk);
void bm_DiscardBlock(RuntimeBlockInfo* block);
void bm_Reset();
// @nothing_since: no block has been compiled since the last time
void bm_ResetCache(bool nothing_since = false);
void bm_ResetTempCache(bool full);
void bm_Periodical_1s();

void bm_Init();
void bm_Term();

void bm_vmem_pagefill(void** ptr,u32 size_bytes);
bool bm_RamWriteAccess(void *p);
void bm_RamWriteAccess(u32 addr);
