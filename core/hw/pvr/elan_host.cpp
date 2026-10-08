/* The NAOMI 2's geometry processor, wired into the machine: where it is
 * in the SH4's memory, what it does to the tile accelerator, the holly
 * and video memory, and its place in a save state. The chip itself is
 * elan.c.
 *
 * Area 2 of a NAOMI 2:
 *   085f6800 - 085f7cff  the holly's registers, on both PowerVRs at once
 *   085f8000 - 085f9fff  the PowerVR's registers, on both at once
 *   08800000 - 088000ff  the chip's registers
 *   09000000             its command port
 *   0a000000 - 0bffffff  its memory
 */
#include "types.h"
#include "elan.h"
#include "elan_host.h"
#include "ta.h"
#include "ta_ctx.h"
#include "pvr_regs.h"
#include "pvr_mem.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"

u8 *elan_ram;
int elan_schid = -1;

static _vmem_handler reg_handler;
static _vmem_handler cmd_handler;

static u32 DYNACALL read_elanreg(u32 paddr)
{
	u32 addr = paddr & 0x01ffffff;

	switch (addr >> 16)
	{
	case 0x5f:
		if (addr >= 0x005f6800 && addr <= 0x005f7cff)
			return sb_ReadMem(addr, 4);
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
			return PvrReg(addr, u32);
		break;
	case 0x80:
		return elan_reg_read(addr);
	}
	return 0;
}

static void DYNACALL write_elanreg(u32 paddr, u32 data)
{
	u32 addr = paddr & 0x01ffffff;

	switch (addr >> 16)
	{
	case 0x5f:
		if (addr >= 0x005f6800 && addr <= 0x005f7cff)
		{
			sb_WriteMem(addr, data, 4);
			// and the second PowerVR's interrupt registers
			asic_WriteCLXB(addr, data);
		}
		else if (addr >= 0x005f8000 && addr <= 0x005f9fff)
			pvr_WriteReg(addr, data);
		break;
	case 0x80:
		elan_reg_write(addr, data);
		break;
	}
}

static void DYNACALL write_elancmd(u32 addr, u32 data)
{
	elan_cmd_write(addr, data);
}

static int elan_sched(int tag, int cycles, int jitter)
{
	elan_dma_done();
	return 0;
}

extern "C" void elan_host_ta(const uint32_t *blocks, unsigned count)
{
	if (((uintptr_t)blocks & 31) == 0)
		ta_vtx_data((u32 *)blocks, count);
	else
	{
		// the tile accelerator copies whole aligned blocks
		for (; count; count--, blocks += 8)
		{
			DECL_ALIGN(32) u32 block[8];
			memcpy(block, blocks, sizeof(block));
			ta_vtx_data32(block);
		}
	}
}

extern "C" uint32_t *elan_host_ta_room(unsigned blocks)
{
	return (uint32_t *)ta_vtx_room(blocks);
}

extern "C" int elan_host_ta_list(void)
{
	return ta_vtx_list();
}

extern "C" int elan_host_ta_half(void)
{
	return ta_vtx_half();
}

extern "C" void elan_host_list_end(unsigned bit)
{
	asic_RaiseInterruptBothCLX((HollyInterruptID)bit);
	TA_ITP_CURRENT += 32;
}

extern "C" bool elan_host_texture_dma(uint32_t dst, uint32_t src, uint32_t size, int from_eram)
{
	const u8 *from;

	dst &= VRAM_MASK;
	if (size > VRAM_SIZE - dst)
		return false;
	if (from_eram)
	{
		if (size > ELAN_RAM_SIZE - src)
			return false;
		from = elan_ram + src;
	}
	else
	{
		src = DMAC_SAR(2) & RAM_MASK;
		if (size > RAM_SIZE - src)
			return false;
		from = mem_b.data + src;
	}
	// (a write to video memory the texture cache is watching traps, and the
	// cache hears of it that way)
	memcpy(vram.data + dst, from, size);
	// The chip moves 64 bits at 100 MHz, in theory; Initial D 3 wants some
	// 50 MB/s of it to start (flycast)
	sh4_sched_request(elan_schid, 512);
	return true;
}

void elan_host_init()
{
	if (elan_schid == -1)
		elan_schid = sh4_sched_register(0, &elan_sched);
	if (settings.System == DC_PLATFORM_NAOMI2)
	{
		if (!elan_ram)
		{
			elan_ram = (u8 *)OS_aligned_malloc(4096, ELAN_RAM_SIZE);
			if (elan_ram)
				memset(elan_ram, 0, ELAN_RAM_SIZE);
		}
	}
	else if (elan_ram)
	{
		OS_aligned_free(elan_ram);
		elan_ram = NULL;
	}
	elan_init(elan_ram);
}

void elan_host_term()
{
	if (elan_ram)
		OS_aligned_free(elan_ram);
	elan_ram = NULL;
	elan_init(NULL);
}

void elan_host_reset(bool hard)
{
	if (hard)
	{
		if (elan_ram)
			memset(elan_ram, 0, ELAN_RAM_SIZE);
		elan_reset();
	}
}

void elan_host_map_init()
{
	reg_handler = _vmem_register_handler(0, 0, read_elanreg, 0, 0, write_elanreg);
	cmd_handler = _vmem_register_handler(0, 0, 0, 0, 0, write_elancmd);
}

void elan_host_map(u32 base)
{
	if (settings.System != DC_PLATFORM_NAOMI2 || !elan_ram)
		return;
	_vmem_map_handler(reg_handler, base | 0x08, base | 0x08);
	_vmem_map_handler(cmd_handler, base | 0x09, base | 0x09);
	_vmem_map_block(elan_ram, base | 0x0a, base | 0x0b, ELAN_RAM_MASK);
}
