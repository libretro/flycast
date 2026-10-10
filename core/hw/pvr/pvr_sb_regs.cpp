/*
	PVR-SB handling
	DMA hacks are here
*/

#include "types.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/modules/dmac.h"
#include "hw/sh4/sh4_mem.h"
#include "pvr_sb_regs.h"
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "ta.h"

void RegWrite_SB_C2DST(u32 addr, u32 data)
{
	if(data & 1)
	{
		SB_C2DST=1;
		DMAC_Ch2St();
	}
}

/* PVR-DMA */
void do_pvr_dma(void)
{
	u32 chcr   = DMAC_CHCR(0).full;
	u32 dmaor  = DMAC_DMAOR.full;
	u32 dmatcr = DMAC_DMATCR(0);

	u32 src    = SB_PDSTAR; // System RAM address
	u32 dst    = SB_PDSTAP; // VRAM address
	u32 len    = SB_PDLEN;

	if((dmaor &DMAOR_MASK) != 0x8201)
	{
		INFO_LOG(PVR, "DMAC: DMAOR has invalid settings (%X) !", dmaor);
		return;
	}

	if (len & 0x1F)
	{
		INFO_LOG(PVR, "DMAC: SB_C2DLEN has invalid size (%X) !", len);
		return;
	}

	if (SB_PDDIR)
	{
		//PVR -> System
      WriteMemBlock_nommu_dma(src, dst, len);
	}
	else
	{
		//System -> PVR
      WriteMemBlock_nommu_dma(dst,src,len);
	}

	DMAC_SAR(0)        = (src + len);
	DMAC_CHCR(0).TE = 1;
	DMAC_DMATCR(0)     = 0x00000000;

	SB_PDST            = 0x00000000;

	//TODO : *CHECKME* is that ok here ? the docs don't say here it's used [PVR-DMA , bit 11]
	asic_RaiseInterrupt(holly_PVR_DMA);
}

void RegWrite_SB_PDST(u32 addr, u32 data)
{
	if (data & 1)
	{
		SB_PDST=1;
		do_pvr_dma();
	}
}

u32 calculate_start_link_addr(void)
{
	u32 rv;
	/* The table is read in main RAM and nowhere else, however far the
	 * list's ends have moved the index on (the table starts on a multiple
	 * of 32, so an entry never straddles the end). */
	u32 base = SB_SDSTAW & (RAM_MASK - 31);

	if (SB_SDWLT==0) /* 16b width */
		rv=*(u16*)&mem_b.data[(base + SB_SDDIV * 2) & RAM_MASK];
	else /* 32b width */
		rv=*(u32*)&mem_b.data[(base + SB_SDDIV * 4) & RAM_MASK];

	SB_SDDIV++; //next index

	return rv;
}

void pvr_do_sort_dma(void)
{
	SB_SDDIV           = 0; //index is 0 now :)
	u32 link_addr      = calculate_start_link_addr();
	u32 link_base_addr = SB_SDBAAW & ~31;
	/* What one transfer may take, in 32-byte blocks: one for each link
	 * followed and one for each block it sends. Main RAM holds RAM_SIZE / 32
	 * blocks, which a list that sends nothing twice cannot exceed; twice
	 * that is allowed, at least twice what the tile accelerator's buffer
	 * takes before it is full and drops the rest. A list that goes round in
	 * a circle, or a block count that is no count at all, used never to
	 * come back from the register write that starts the transfer. */
	u32 budget         = RAM_SIZE / 16;

	while (link_addr != 2)
	{
		if (SB_SDLAS==1)
			link_addr   *= 32;

		/* Parameters are 32 bytes and start on a multiple of that, whatever
		 * the low bits of an unshifted link address say: the tile accelerator
		 * copies them as whole aligned blocks. */
		u32 ea          = (link_base_addr+link_addr) & (RAM_MASK - 31);
		u32* ea_ptr     = (u32*)&mem_b.data[ea];
		u32 count       = ea_ptr[0x18>>2];
		link_addr       = ea_ptr[0x1C>>2];//Next link

		if (count >= budget)
		{
			WARN_LOG(PVR, "Sort-DMA: list without end at %08x (%u blocks), transfer ended", ea, count);
			break;
		}
		budget         -= count + 1;

		/* transfer global param: it is read in main RAM only, and one that
		 * runs off the end goes on at the start, as the address does */
		while (count != 0)
		{
			u32 blocks   = (RAM_SIZE - ea) / 32;
			if (blocks > count)
				blocks    = count;
			ta_vtx_data((u32*)&mem_b.data[ea], blocks);
			count       -= blocks;
			ea           = 0;
		}
		if (link_addr == 1)
			link_addr    = calculate_start_link_addr();
	}

	// End of DMA :)
	SB_SDST = 0;
	SB_SDSTAW += 32;
	asic_RaiseInterrupt(holly_PVR_SortDMA);
}

// Auto sort DMA :|
void RegWrite_SB_SDST(u32 addr, u32 data)
{
	if(data & 1)
		pvr_do_sort_dma();
}

//Init/Term , global
void pvr_sb_Init(void)
{
	//0x005F7C18    SB_PDST RW  PVR-DMA start
	sb_rio_register(SB_PDST_addr,RIO_WF,0,&RegWrite_SB_PDST);

	//0x005F6808    SB_C2DST RW  ch2-DMA start 
	sb_rio_register(SB_C2DST_addr,RIO_WF,0,&RegWrite_SB_C2DST);

	//0x005F6820    SB_SDST RW  Sort-DMA start
	sb_rio_register(SB_SDST_addr,RIO_WF,0,&RegWrite_SB_SDST);
}

void pvr_sb_Term(void)
{
}

//Reset -> Reset - Initialise
void pvr_sb_Reset(bool hard)
{
}
