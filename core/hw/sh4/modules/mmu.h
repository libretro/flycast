#pragma once
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/mem/_vmem.h"

//Translation Types
//Opcode read
#define MMU_TT_IREAD 0
//Data write
#define MMU_TT_DWRITE 1
//Data write
#define MMU_TT_DREAD 2

//Return Values
//Translation was successful
#define MMU_ERROR_NONE	   0
//TLB miss
#define MMU_ERROR_TLB_MISS 1
//TLB Multihit
#define MMU_ERROR_TLB_MHIT 2
//Mem is read/write protected (depends on translation type)
#define MMU_ERROR_PROTECTED 3
//Mem is write protected , firstwrite
#define MMU_ERROR_FIRSTWRITE 4
//data-Opcode read/write missasligned
#define MMU_ERROR_BADADDR 5
//Can't Execute
#define MMU_ERROR_EXECPROT 6

struct TLB_Entry
{
	CCN_PTEH_type Address;
	CCN_PTEL_type Data;
	CCN_PTEA_type Assistance;
};

extern TLB_Entry UTLB[64];
extern TLB_Entry ITLB[4];
extern u32 sq_remap[64];
extern const u32 fast_reg_lut[8];

//These are working only for SQ remaps on ndce
bool UTLB_Sync(u32 entry);
void ITLB_Sync(u32 entry);

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data);
// the page @va is in has been taken out of the TLB
void mmu_forget(u32 va);
// the TLB's entry of that number has been written to
void mmu_utlb_written(u32 entry);
void mmu_set_state();
void mmu_flush_table();
void mmu_raise_exception(u32 mmu_error, u32 address, u32 am);

/* A program that brings a TLB of its own making, and is not Windows CE:
 * it has the MMU, done the strict way, from the moment it turns address
 * translation on until the machine is reset. See fastmmu.cpp. */
extern bool mmu_strict;
// AT has come on or gone off: is this such a program?
void mmu_detect_strict();
// what the strict way keeps of its lookups is to go
void mmu_strict_changed();

static INLINE bool mmu_enabled()
{
#ifndef NO_MMU
	return mmu_strict || (settings.dreamcast.FullMMU && CCN_MMUCR.AT == 1);
#else
	return false;
#endif
}

template<bool internal = false>
u32 mmu_full_lookup(u32 va, const TLB_Entry **entry, u32& rv);

/* Translations kept by, for the recompilers, which have to translate every
 * address themselves.
 *
 * (The 64-bit ones on Linux and Android used to have the host's MMU do
 * it: each page a Windows CE game touched was mapped on the host where
 * the game has it, and code read and wrote with no translating at all.
 * But such a mapping is for one address space, and all of it went
 * whenever the game changed to another - seventeen times a frame in Sega
 * Rally 2, which had 1.2 million pages mapped a minute that way, 87 in 100
 * of them a page it had mapped before for the same address space. Of the
 * 27 seconds of processor that minute took on x86-64 Linux, 10 were the
 * kernel's; with what follows it was under 16, and the kernel's a fifth of
 * one.)
 *
 * Two tables, one for reads and one for writes, each with an entry for
 * every 4K page of the SH4's address space: what to add to an address in
 * the page to be at it on the host, or 0. Recompiled code looks an address
 * up in the one for what it is doing, and on a hit goes straight to
 * memory; on a miss it calls the
 * translation, which fills the entry in if the page is one of main memory -
 * the only kind for which a host address is all an access needs.
 *
 * A write's entry is only filled by a write that was allowed, so a page
 * that may not be written, or has not been written yet and has its first
 * write to report, still gets its exception however often it has been read.
 * (Upstream's table, which this is after, has one entry for both.)
 *
 * What it does not know is who is asking: an entry made in privileged mode
 * is found in user mode too.
 *
 * It is emptied whenever what an address means can have changed: the TLB
 * flushed, another address space, the MMU or its single-space mode switched,
 * a TLB entry written through its memory-mapped array, a state loaded. A
 * page whose TLB entry is loaded or invalidated is forgotten on its own.
 * Emptying costs by how much of the tables has been used since the last
 * time, which is little: the parts that were are kept note of. */
#if !defined(NO_MMU) && (HOST_CPU == CPU_ARM || HOST_CPU == CPU_ARM64 || HOST_CPU == CPU_X64)
#define MMU_HOST_PAGE_LUT 1
extern uintptr_t mmu_read_lut[0x100000];
extern uintptr_t mmu_write_lut[0x100000];
void mmu_lut_flush();
void mmu_lut_forget(u32 va, u32 size);
// @va has just been translated to @pa, for a write if @write
void mmu_lut_fill(u32 va, u32 pa, bool write);
#else
static INLINE void mmu_lut_flush() {}
static INLINE void mmu_lut_forget(u32 va, u32 size) {}
#endif
u32 mmu_instruction_lookup(u32 va, const TLB_Entry **entry, u32& rv);
template<u32 translation_type>
u32 mmu_full_SQ(u32 va, u32& rv);

#ifdef FAST_MMU
static INLINE u32 mmu_instruction_translation(u32 va, u32& rv)
{
	if (va & 1)
		return MMU_ERROR_BADADDR;
	// (a strict program keeps all this while it has AT off: nothing is translated then)
	if (fast_reg_lut[va >> 29] != 0 || CCN_MMUCR.AT == 0)
	{
		rv = va;
		return MMU_ERROR_NONE;
	}

	const TLB_Entry *tlb_entry;
	return mmu_full_lookup(va, &tlb_entry, rv);
}
#else
u32 mmu_instruction_translation(u32 va, u32& rv);
#endif

template<u32 translation_type, typename T>
extern u32 mmu_data_translation(u32 va, u32& rv);

/* 64 bits are read and written as two times 32 by the SH4, each at its own
 * address: where the second is in another page than the first - at the
 * last four bytes of a page, be it one of 1K or more - it is somewhere
 * else altogether, and need not be there at all. Whoever translates one
 * address and takes eight bytes from where it is has to do such an access
 * as the two that it is. (None did, and such an access read and wrote four
 * bytes of whatever comes next in memory.) */
static INLINE bool mmu_in_two_pages(u32 va)
{
	return (va & 0x3FF) == 0x3FC;
}
void DoMMUException(u32 addr, u32 mmu_error, u32 access_type);

template<u32 translation_type>
bool mmu_is_translated(u32 va, u32 size)
{
   if (va & (size - 1))
      return true;

   if (translation_type == MMU_TT_DWRITE)
   {
      if ((va & 0xFC000000) == 0xE0000000)
         //SQ writes are not translated, only write backs are.
         return false;
   }
   if ((va & 0xFC000000) == 0x7C000000)
		// On-chip RAM area isn't translated
      return false;

   if (fast_reg_lut[va >> 29] != 0)
      return false;

   return true;
}

#if defined(NO_MMU)
	bool inline mmu_TranslateSQW(u32 addr, u32* mapped) {
		*mapped = sq_remap[(addr>>20)&0x3F] | (addr & 0xFFFE0);
		return true;
	}
	void inline mmu_flush_table() {}
#else
	template<typename T> T DYNACALL mmu_ReadMem(u32 adr);
	u16 DYNACALL mmu_IReadMem16(u32 addr);

	template<typename T> void DYNACALL mmu_WriteMem(u32 adr, T data);
	
	bool mmu_TranslateSQW(u32 adr, u32 *out);

	template<typename T>
	T DYNACALL mmu_ReadMemNoEx(u32 adr, u32 *exception_occurred)
	{
		if (sizeof(T) == 8 && mmu_in_two_pages(adr))
		{
			const u32 low = mmu_ReadMemNoEx<u32>(adr, exception_occurred);
			if (*exception_occurred)
				return 0;
			const u32 high = mmu_ReadMemNoEx<u32>(adr + 4, exception_occurred);
			if (*exception_occurred)
				return 0;
			return (T)(((u64)high << 32) | low);
		}

		u32 addr;
		u32 rv = mmu_data_translation<MMU_TT_DREAD, T>(adr, addr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(adr, rv, MMU_TT_DREAD);
			*exception_occurred = 1;
			return 0;
		}
		else
		{
			*exception_occurred = 0;
			// the next read of the page need not come here
			mmu_lut_fill(adr, addr, false);
			return _vmem_readt<T, T>(addr);
		}
	}

	template<typename T>
	u32 DYNACALL mmu_WriteMemNoEx(u32 adr, T data)
	{
		if (sizeof(T) == 8 && mmu_in_two_pages(adr))
		{
			if (mmu_WriteMemNoEx<u32>(adr, (u32)data))
				return 1;
			return mmu_WriteMemNoEx<u32>(adr + 4, (u32)((u64)data >> 32));
		}

		u32 addr;
		u32 rv = mmu_data_translation<MMU_TT_DWRITE, T>(adr, addr);
		if (rv != MMU_ERROR_NONE)
		{
			DoMMUException(adr, rv, MMU_TT_DWRITE);
			return 1;
		}
		mmu_lut_fill(adr, addr, true);
		_vmem_writet<T>(addr, data);
		return 0;
	}
#endif
