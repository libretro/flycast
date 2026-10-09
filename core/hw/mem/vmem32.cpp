/*
    Created on: Apr 11, 2019

	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <unordered_set>
#include "build.h"
#include "vmem32.h"
#include "_vmem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */
#include <unistd.h>
#include <errno.h>
#ifdef _ANDROID
#include <linux/ashmem.h>
#endif
#endif

#ifndef MAP_NOSYNC
#define MAP_NOSYNC       0
#endif

#include "types.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/mmu.h"

extern bool VramLockedWriteOffset(size_t offset);
#include "rend/rtt_watch.h"

#ifdef _WIN32
extern HANDLE mem_handle;
#else
#ifdef HAVE_LIBNX
extern uintptr_t vmem_fd;
#else
extern int vmem_fd;
#endif // HAVE_LIBNX
#endif

#define VMEM32_ERROR_NOT_MAPPED 0x100

static const u64 VMEM32_SIZE = 0x100000000L;
static const u64 USER_SPACE = 0x80000000L;
static const u64 AREA7_ADDRESS = 0x7C000000L;
static const u64 P3_START = 0xC0000000L;
static const u64 P3_END = 0xE0000000L;

static std::unordered_set<u32> vram_mapped_pages;
/* Main memory: a bit for each 4K of the SH4's addresses, set where a page
 * that may be written to has been mapped. (It was kept by where the page
 * is in main memory, which is another thing: see vmem32_map_mmu().) */
static u8 sram_mapped_pages[VMEM32_SIZE / PAGE_SIZE / 8];

static inline bool sram_page_mapped(u32 address)
{
	return (sram_mapped_pages[address >> 15] & (1 << ((address >> 12) & 7))) != 0;
}

static inline void sram_page_set_mapped(u32 address)
{
	sram_mapped_pages[address >> 15] |= 1 << ((address >> 12) & 7);
}

bool vmem32_inited;
// something is mapped in P3: the MMU's pages, or what is there while it is off
static bool p3_mapped = true;

// stats
//u64 vmem32_page_faults;
//u64 vmem32_flush;

static void* vmem32_map_buffer(u32 dst, u32 addrsz, u32 offset, u32 size, bool write)
{
	void* ptr;
	void* rv;

	//printf("MAP32 %08X w/ %d\n",dst,offset);
	u32 map_times = addrsz / size;
#ifdef _WIN32
	rv = MapViewOfFileEx(mem_handle, FILE_MAP_READ | (write ? FILE_MAP_WRITE : 0), 0, offset, size, &virt_ram_base[dst]);
	if (rv == NULL)
		return NULL;

	for (u32 i = 1; i < map_times; i++)
	{
		dst += size;
		ptr = MapViewOfFileEx(mem_handle, FILE_MAP_READ | (write ? FILE_MAP_WRITE : 0), 0, offset, size, &virt_ram_base[dst]);
		if (ptr == NULL)
			return NULL;
	}
#else
	u32 prot = PROT_READ | (write ? PROT_WRITE : 0);
	rv = mmap(&virt_ram_base[dst], size, prot, MAP_SHARED | MAP_NOSYNC | MAP_FIXED, vmem_fd, offset);
	if (MAP_FAILED == rv)
	{
		ERROR_LOG(VMEM, "MAP1 failed %d", errno);
		return NULL;
	}

	for (u32 i = 1; i < map_times; i++)
	{
		dst += size;
		ptr = mmap(&virt_ram_base[dst], size, prot , MAP_SHARED | MAP_NOSYNC | MAP_FIXED, vmem_fd, offset);
		if (MAP_FAILED == ptr)
		{
			ERROR_LOG(VMEM, "MAP2 failed %d", errno);
			return NULL;
		}
	}
#endif
	return rv;
}

static void vmem32_unmap_buffer(u32 start, u64 end)
{
#ifdef _WIN32
	UnmapViewOfFile(&virt_ram_base[start]);
#else
	mmap(&virt_ram_base[start], end - start, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
}

static void vmem32_protect_buffer(u32 start, u32 size)
{
	verify((start & PAGE_MASK) == 0);
#ifdef _WIN32
	DWORD old;
	VirtualProtect(virt_ram_base + start, size, PAGE_READONLY, &old);
#else
	mprotect(&virt_ram_base[start], size, PROT_READ);
#endif
}

static void vmem32_unprotect_buffer(u32 start, u32 size)
{
	verify((start & PAGE_MASK) == 0);
#ifdef _WIN32
	DWORD old;
	VirtualProtect(virt_ram_base + start, size, PAGE_READWRITE, &old);
#else
	mprotect(&virt_ram_base[start], size, PROT_READ | PROT_WRITE);
#endif
}

static const u32 page_sizes[] = { 1024, 4 * 1024, 64 * 1024, 1024 * 1024 };

static u32 vmem32_paddr_to_offset(u32 address)
{
	u32 low_addr = address & 0x1FFFFFFF;
	switch ((address >> 26) & 7)
	{
	case 0:	// area 0
		// Aica ram
		if (low_addr >= 0x00800000 && low_addr < 0x00800000 + 0x00800000)
		{
			return ((low_addr - 0x00800000) & (ARAM_SIZE - 1)) + MAP_ARAM_START_OFFSET;
		}
		else if (low_addr >= 0x02800000 && low_addr < 0x02800000 + 0x00800000)
		{
			return low_addr - 0x02800000 + MAP_ARAM_START_OFFSET;
		}
		break;
	case 1:	// area 1
		// Vram
		if (low_addr >= 0x04000000 && low_addr < 0x04000000 + 0x01000000)
		{
			return ((low_addr - 0x04000000) & (VRAM_SIZE - 1)) + MAP_VRAM_START_OFFSET;
		}
		else if (low_addr >= 0x06000000 && low_addr < 0x06000000 + 0x01000000)
		{
			return ((low_addr - 0x06000000) & (VRAM_SIZE - 1)) + MAP_VRAM_START_OFFSET;
		}
		break;
	case 3:	// area 3
		// System ram
		if (low_addr >= 0x0C000000 && low_addr < 0x0C000000 + 0x04000000)
		{
			return ((low_addr - 0x0C000000) & (RAM_SIZE - 1)) + MAP_RAM_START_OFFSET;
		}
		break;
	//case 4:
		// TODO vram?
		//break;
	default:
		break;
	}
	// Unmapped address
	return -1;
}

static u32 vmem32_map_mmu(u32 address, bool write)
{
#ifndef NO_MMU
	u32 pa;
	const TLB_Entry *entry;
	u32 rc = mmu_full_lookup<false>(address, &entry, pa);
	if (rc == MMU_ERROR_NONE)
	{
		if (address >= P3_START)
			p3_mapped = true;
		//0X  & User mode-> protection violation
		//if ((entry->Data.PR >> 1) == 0 && p_sh4rcb->cntx.sr.MD == 0)
		//	return MMU_ERROR_PROTECTED;

		/* A write to a page that may not be written is an exception. The
		 * test was commented out, and the page was mapped read-only all the
		 * same: so the write faulted, was "handled" by mapping the page
		 * read-only again, and faulted again, for ever.
		 *
		 * The other test that was here, for the first write to a page whose
		 * dirty bit is clear, stays out: no path of this MMU raises that
		 * exception, upstream's neither, and the Windows CE games have only
		 * ever been run without it. */
		if (write && (entry->Data.PR & 1) == 0)
			return MMU_ERROR_PROTECTED;
		u32 page_size = page_sizes[entry->Data.SZ1 * 2 + entry->Data.SZ0];
		if (page_size == 1024)
			return VMEM32_ERROR_NOT_MAPPED;

		u32 vpn = (entry->Address.VPN << 10) & ~(page_size - 1);
		u32 ppn = (entry->Data.PPN << 10) & ~(page_size - 1);
		u32 offset = vmem32_paddr_to_offset(ppn);
		if (offset == -1)
			return VMEM32_ERROR_NOT_MAPPED;

		bool allow_write = (entry->Data.PR & 1) != 0;
		if (offset >= MAP_VRAM_START_OFFSET && offset < MAP_VRAM_START_OFFSET + VRAM_SIZE)
		{
			// Check vram protected regions
			u32 start = offset - MAP_VRAM_START_OFFSET;
			// (and nothing rendered to a texture is still to arrive in it: rend/rtt_watch.h)
			rtt_watch_touch(start, page_size);
			if (!vram_mapped_pages.insert(vpn).second)
			{
				// page has been mapped already: vram locked write
				vmem32_unprotect_buffer(address & ~PAGE_MASK, PAGE_SIZE);
				u32 addr_offset = start + (address & (page_size - 1));
				VramLockedWriteOffset(addr_offset);

				return MMU_ERROR_NONE;
			}
			verify(vmem32_map_buffer(vpn, page_size, offset, page_size, allow_write) != NULL);
			// Protect what is protected in the main vram mappings
			u32 end = std::min(start + page_size, VRAM_SIZE);
			for (u32 vram_addr = start; vram_addr < end; vram_addr += PAGE_SIZE)
				if (_vmem_vram_page_protected(vram_addr / PAGE_SIZE))
					vmem32_protect_buffer(vpn + (vram_addr - start), PAGE_SIZE);
		}
		else if (offset >= MAP_RAM_START_OFFSET && offset < MAP_RAM_START_OFFSET + RAM_SIZE)
		{
#if FEAT_SHREC != DYNAREC_NONE
			/* Main memory, which the recompiler may have compiled code
			 * from. A page it counts on hearing of a write to
			 * (bm_IsRamPageProtected()) is mapped so that it cannot be
			 * written, and the write that then faults is where it hears.
			 *
			 * The recompiler's pages are of 4K and so are the host's; the
			 * SH4's may be of 64K or a megabyte. All of the SH4's page is
			 * mapped at once, and each 4K of it then protected or not as
			 * the recompiler has it. Only the first 4K was asked about, and
			 * only the first was reported written whichever was: code in
			 * the rest of a large page could be written over unnoticed.
			 *
			 * What has been mapped is kept by the address it was mapped
			 * at. It was kept by the page of main memory, so that a page
			 * read at one address and then written at another - the same
			 * memory in two of Windows CE's slots, say - was taken to be
			 * mapped at the second: the fault was answered by making
			 * writable what was there, which is no memory at all, and the
			 * write and every one after it at that address went into
			 * nothing, where reads then found them and nothing else. */
			const u32 start = offset - MAP_RAM_START_OFFSET;
			const u32 host_page = address & ~PAGE_MASK;

			if (allow_write && sram_page_mapped(host_page))
			{
				// mapped, and it faults: a write to a page that is protected
				const u32 sub = host_page & (page_size - 1);

				vmem32_unprotect_buffer(host_page, PAGE_SIZE);
				if (bm_IsRamPageProtected(start + sub))
					bm_RamWriteAccess(ppn + sub);
			}
			else
			{
				u32 sub, run = 0, protect = 0;

				for (sub = 0; sub < page_size; sub += PAGE_SIZE)
					if (bm_IsRamPageProtected(start + sub))
						protect += PAGE_SIZE;
				// (all of it protected, which a 4K page is or is not: mapped so at once)
				verify(vmem32_map_buffer(vpn, page_size, offset, page_size, allow_write && protect != page_size) != NULL);
				if (allow_write)
				{
					for (sub = 0; sub < page_size; sub += PAGE_SIZE)
						sram_page_set_mapped(vpn + sub);
					if (protect != 0 && protect != page_size)
					{
						for (sub = 0; sub < page_size; sub += PAGE_SIZE)
						{
							if (bm_IsRamPageProtected(start + sub))
								run += PAGE_SIZE;
							else if (run != 0)
							{
								vmem32_protect_buffer(vpn + sub - run, run);
								run = 0;
							}
						}
						if (run != 0)
							vmem32_protect_buffer(vpn + page_size - run, run);
					}
				}
			}
#else
			verify(vmem32_map_buffer(vpn, page_size, offset, page_size, allow_write) != NULL);
#endif
		}
		else
			// Not vram or system ram
			verify(vmem32_map_buffer(vpn, page_size, offset, page_size, allow_write) != NULL);

		return MMU_ERROR_NONE;
	}
#else
	u32 rc = MMU_ERROR_PROTECTED;
#endif
	return rc;
}

static u32 vmem32_map_address(u32 address, bool write)
{
	u32 area = address >> 29;
	switch (area)
	{
	case 3:	// P0/U0
		if (address >= AREA7_ADDRESS)
			// area 7: unmapped
			return VMEM32_ERROR_NOT_MAPPED;
		/* no break */
	case 0:
	case 1:
	case 2:
	case 6:	// P3
		return vmem32_map_mmu(address, write);

	default:
		break;
	}
	return VMEM32_ERROR_NOT_MAPPED;
}

#if !defined(NO_MMU) && defined(HOST_64BIT_CPU)
bool vmem32_handle_signal(void *fault_addr, bool write, u32 exception_pc)
{
	if (!vmem32_inited || (u8*)fault_addr < virt_ram_base || (u8*)fault_addr >= virt_ram_base + VMEM32_SIZE)
		return false;
	//vmem32_page_faults++;
	u32 guest_addr = (u8*)fault_addr - virt_ram_base;
	u32 rv = vmem32_map_address(guest_addr, write);
	DEBUG_LOG(VMEM, "vmem32_handle_signal handled signal %s @ %p -> %08x rv=%d", write ? "W" : "R", fault_addr, guest_addr, rv);
	if (rv == MMU_ERROR_NONE)
		return true;
	if (rv == VMEM32_ERROR_NOT_MAPPED)
		return false;
#if HOST_CPU == CPU_ARM64
	p_sh4rcb->cntx.pc = exception_pc;
#else
	p_sh4rcb->cntx.pc = p_sh4rcb->cntx.exception_pc;
#endif
	DoMMUException(guest_addr, rv, write ? MMU_TT_DWRITE : MMU_TT_DREAD);
	ngen_HandleException();
	// not reached
	return true;
}
#endif

/* Everything the MMU's pages were mapped as goes: P0, and P3, which the
 * MMU translates as well. (P3 used to be left - "TODO flush P3?" - and
 * with the memory it has while the MMU is off still mapped there: an
 * address in it that the program had mapped was read and written at the
 * wrong place if that was one of those, and at the place it had first
 * been mapped to ever after if it was not.) */
void vmem32_flush_mmu()
{
	//vmem32_flush++;
	vram_mapped_pages.clear();
	memset(sram_mapped_pages, 0, USER_SPACE / PAGE_SIZE / 8);
	vmem32_unmap_buffer(0, USER_SPACE);
	// (P3 if there is anything in it: this is done at every change of address space)
	if (p3_mapped)
	{
		memset(sram_mapped_pages + P3_START / PAGE_SIZE / 8, 0, (P3_END - P3_START) / PAGE_SIZE / 8);
		vmem32_unmap_buffer(P3_START, P3_END);
		p3_mapped = false;
	}
}

void vmem32_forget(u32 va, u32 size)
{
	// (a page of 1K is not mapped at all)
	if (!vmem32_inited || size < PAGE_SIZE)
		return;
	if (!(va < USER_SPACE || (va >= P3_START && va < P3_END)))
		return;
	vram_mapped_pages.erase(va);
	for (u32 sub = 0; sub < size; sub += PAGE_SIZE)
		sram_mapped_pages[(va + sub) >> 15] &= ~(1 << (((va + sub) >> 12) & 7));
	vmem32_unmap_buffer(va, (u64)va + size);
}

/* Not used any more: never on.
 *
 * What this file does is have the host's MMU do the SH4's, for a Windows CE
 * game: each page the game touches is mapped on the host where the game has
 * it, the first time it is touched, and recompiled code then reads and
 * writes with no translating at all. But a mapping is for one address
 * space, and all of them go whenever the game changes to another - which
 * Sega Rally 2 does seventeen times a frame, between six of them. It had
 * 1.2 million pages mapped in a minute that way, 87 in 100 of them a page
 * it had mapped before for the same address space, and each one a fault
 * for the kernel to hand back and a page to map in answer: of the 27
 * seconds of processor that minute took on x86-64 Linux, 10 were the
 * kernel's.
 *
 * The other way, which is what Windows and the 32-bit hosts have always
 * had, is a table of where each page is (mmu.h), looked up by recompiled
 * code before each access. The same minute that way: under 16 seconds, a
 * fifth of one of them the kernel's - and no more of the game's own time
 * either, for all that every access is a few instructions longer. So that
 * is what all hosts do now.
 *
 * (It was already switched off for seventeen Windows CE games, in
 * rom_luts.h, which have had the table all along.) */
bool vmem32_init()
{
	return false;
}

void vmem32_term()
{
	if (vmem32_inited)
	{
		vmem32_inited = false;
		vmem32_flush_mmu();
	}
}
