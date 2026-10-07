#include "types.h"
#include <algorithm>
#include <string.h>

#include "maple_if.h"

#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "types.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/maple/maple_helper.h"
#include "maple_cfg.h"

enum MaplePattern
{
	MP_Start,
	MP_SDCKBOccupy = 2,
	MP_Reset,
	MP_SDCKBOccupyCancel,
	MP_NOP = 7
};

maple_device* MapleDevices[4][6];

int maple_sched;

/*
	Maple host controller
	Direct processing, async interrupt handling
	Device code is on maple_devs.cpp/h, config&management is on maple_cfg.cpp/h

	This code is missing many of the hardware details, like proper trigger handling,
	DMA continuation on suspect, etc ...
*/

static void maple_DoDma();
static void maple_handle_reconnect();

//really hackish
//misses delay , and stop/start implementation
//ddt/etc are just hacked for wince to work
//now with proper maple delayed DMA maybe its time to look into it ?
bool maple_ddt_pending_reset;

/* What the devices have answered in the transfer under way.
 *
 * An answer takes time to come in over the bus, and the game is told with an
 * interrupt when the whole transfer is over. Until then its memory has to be
 * as it was: a game that looks before the interrupt must not find the answer
 * there already. So the answers wait here, each as the address it goes to,
 * its length in bytes and its words, and are put in memory by
 * maple_dma_done(). An address of 0 is an answer with nowhere to go: the
 * overrun interrupt, raised then too.
 *
 * In the save state, from V18. */
u32 maple_out[MAPLE_OUT_WORDS];
u32 maple_out_used;

static void maple_out_write(u32 dest, const u32 *data, u32 bytes)
{
	if (dest == 0)
	{
		asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
		return;
	}
	u32 *p = (u32 *)GetMemPtr(dest, bytes);
	if (p != NULL)
		memcpy(p, data, bytes);
}

static void maple_out_put(u32 dest, const u32 *data, u32 bytes)
{
	const u32 words = (bytes + 3) / 4;

	if (maple_out_used + 2 + words > MAPLE_OUT_WORDS)
	{
		// more answers than there is room to keep: this one arrives at once
		maple_out_write(dest, data, bytes);
		return;
	}
	maple_out[maple_out_used++] = dest;
	maple_out[maple_out_used++] = bytes;
	memcpy(&maple_out[maple_out_used], data, bytes);
	maple_out_used += words;
}

// The transfer is over: the answers go where the game asked, and it is told
void maple_dma_done()
{
	u32 at = 0;

	while (at + 2 <= maple_out_used)
	{
		const u32 dest = maple_out[at], bytes = maple_out[at + 1];
		const u32 words = (bytes + 3) / 4;
		if (at + 2 + words > maple_out_used)
			break;
		maple_out_write(dest, &maple_out[at + 2], bytes);
		at += 2 + words;
	}
	maple_out_used = 0;
	SB_MDST = 0;
	asic_RaiseInterrupt(holly_MAPLE_DMA);
}
void maple_vblank()
{
	if (SB_MDEN & 1)
	{
		if (SB_MDTSEL & 1)
		{
			if (maple_ddt_pending_reset)
			{
				DEBUG_LOG(MAPLE, "DDT vblank ; reset pending");
			}
			else
			{
				DEBUG_LOG(MAPLE, "DDT vblank");
				SB_MDST = 1;
				maple_DoDma();
				// if trigger reset is manual, mark it as pending
				if ((SB_MSYS >> 12) & 1)
					maple_ddt_pending_reset = true;
			}
		}
		else
		{
			maple_ddt_pending_reset = false;
		}
	}
	maple_handle_reconnect();
}

static void maple_SB_MSHTCL_Write(u32 addr, u32 data)
{
	if (data&1)
		maple_ddt_pending_reset=false;
}

static void maple_SB_MDST_Write(u32 addr, u32 data)
{
	if (data & 0x1)
	{
		if (SB_MDEN &1)
		{
			SB_MDST=1;
			maple_DoDma();
		}
	}
}

static void maple_SB_MDEN_Write(u32 addr, u32 data)
{
	SB_MDEN=data&1;
}

#ifdef STRICT_MODE
static bool check_mdapro(u32 addr)
{
	u32 area = (addr >> 26) & 7;
	u32 bottom = ((((SB_MDAPRO >> 8) & 0x7f) << 20) | 0x08000000);
	u32 top = (((SB_MDAPRO & 0x7f) << 20) | 0x080fffe0);

	if (area != 3 || addr < bottom || addr > top)
	{
		INFO_LOG(MAPLE, "MAPLE ERROR : Invalid address: %08x. SB_MDAPRO: %x %x", addr, (SB_MDAPRO >> 8) & 0x7f, SB_MDAPRO & 0x7f);
		return false;
	}
	return true;
}

static void maple_SB_MDSTAR_Write(u32 addr, u32 data)
{
	SB_MDSTAR = data & 0x1fffffe0;
	if (!check_mdapro(SB_MDSTAR))
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
}
#endif

static bool IsOnSh4Ram(u32 addr)
{
	if (((addr>>26)&0x7)==3)
	{
		if ((((addr>>29) &0x7)!=7))
			return true;
	}

	return false;
}

static void maple_DoDma(void)
{
	verify(SB_MDEN &1)
	verify(SB_MDST &1)

	DEBUG_LOG(MAPLE, "Maple: DoMapleDma SB_MDSTAR=%x", SB_MDSTAR);
	u32 addr = SB_MDSTAR;
#ifdef STRICT_MODE
	if (!check_mdapro(addr))
	{
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
		SB_MDST = 0;
		return;
	}
#endif
	const bool swap_msb = (SB_MMSEL == 0);
	u32 xfer_in = 0, xfer_out = 0;		// bytes from the console, and from the devices
	bool last   = false;
   bool occupy = false;
	while (last != true)
	{
		u32 header_1 = ReadMem32_nommu(addr);
		u32 header_2 = ReadMem32_nommu(addr + 4) &0x1FFFFFE0;

		last = (header_1 >> 31) == 1;//is last transfer ?
		u32 plen = (header_1 & 0xFF )+1;//transfer length (32-bit unit)
		u32 maple_op=(header_1>>8)&7;	// Pattern selection: 0 - START, 2 - SDCKB occupy permission, 3 - RESET, 4 - SDCKB occupy cancel, 7 - NOP
		//this is kinda wrong .. but meh
		//really need to properly process the commands at some point
		switch (maple_op)
		{
		case MP_Start:
		{
#ifdef STRICT_MODE
			if (!check_mdapro(header_2) || !check_mdapro(addr + 8 + plen * sizeof(u32) - 1))
			{
				asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
				SB_MDST = 0;
				return;
			}
#else
			if (!IsOnSh4Ram(header_2))
			{
				// nowhere for the answer to go: the overrun interrupt, when the transfer is over
				INFO_LOG(MAPLE, "MAPLE ERROR : DESTINATION NOT ON SH4 RAM 0x%X", header_2);
				header_2 = 0;
			}
#endif

			u32* p_data =(u32*) GetMemPtr(addr + 8,(plen)*sizeof(u32));
			if (p_data == NULL)
			{
				INFO_LOG(MAPLE, "MAPLE ERROR : INVALID SB_MDSTAR value 0x%X", addr);
				SB_MDST=0;
				maple_out_used = 0;
				return;
			}

			const u32 frame_header = swap_msb ? SWAP32(p_data[0]) : p_data[0];

			//Command code 
			u32 command = frame_header & 0xFF;
			//Recipient address 
			u32 reci = (frame_header >> 8) & 0xFF;//0-5;
			//Sender address 
			//u32 send=(p_data[0] >> 16) & 0xFF;
			//Number of additional words in frame 
			u32 inlen = (frame_header >> 24) & 0xFF;

			u32 port=maple_GetPort(reci);
			u32 bus=maple_GetBusId(reci);

			if (MapleDevices[bus][5] && MapleDevices[bus][port])
			{
				static u32 maple_out_buf[1024 / 4];
				if (swap_msb)
				{
					static u32 maple_in_buf[1024 / 4];
					maple_in_buf[0] = frame_header;
					// every word of the frame: plen of them, the header included
					for (u32 i = 1; i < plen; i++)
						maple_in_buf[i] = SWAP32(p_data[i]);
					p_data = maple_in_buf;
				}
				u32 outlen = MapleDevices[bus][port]->RawDma(&p_data[0], inlen * 4 + 4, maple_out_buf);
				// each frame also has its start, parity and stop on the wire
				xfer_in += plen * 4 + 3;
				xfer_out += outlen + 3;
#ifdef STRICT_MODE
				if (!check_mdapro(header_2 + outlen - 1))
				{
					// TODO: This isn't correct (with SB_MMSEL=1) since the interrupt
					// should be raised before the memory is written to
					asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
					SB_MDST = 0;
					maple_out_used = 0;
					return;
				}
#endif
				if (swap_msb)
					for (u32 i = 0; i < outlen / 4; i++)
						maple_out_buf[i] = SWAP32(maple_out_buf[i]);
				maple_out_put(header_2, maple_out_buf, outlen);
			}
			else
			{
				if (port != 5 && command != 1)
					INFO_LOG(MAPLE, "MAPLE: Unknown device bus %d port %d cmd %d", bus, port, command);
				static const u32 nobody = 0xFFFFFFFF;
				maple_out_put(header_2, &nobody, 4);
			}

			//goto next command
			addr += 2 * 4 + plen * 4;
		}
		break;

		case MP_SDCKBOccupy:
		{
			u32 bus = (header_1 >> 16) & 3;
			if (MapleDevices[bus][5])
			{
				occupy = MapleDevices[bus][5]->get_lightgun_pos();
				xfer_in++;
			}

			addr += 1 * 4;
		}
		break;

		case MP_SDCKBOccupyCancel:
			addr += 1 * 4;
			break;

		case MP_Reset:
			addr += 1 * 4;
			xfer_in++;
			break;

		case MP_NOP:
			addr += 1 * 4;
			break;

		default:
			INFO_LOG(MAPLE, "MAPLE: Unknown maple_op == %d length %d", maple_op, plen * 4);
			addr += 1 * 4;
		}
	}

	/* How long the transfer takes. The console sends at 2 Mbit/s; devices
	 * answer more slowly, 724 to 738 kbit/s as measured on the wire with a
	 * protocol analyser (the captures are in OrangeFox86's
	 * DreamcastControllerUsbPico, under measurements). These are upstream's
	 * figures, which it came to through Silent Scope, which reads its
	 * memory card at boot and fails if the answer comes back too soon. */
	if (!occupy)
	{
		u64 cycles = (u64)SH4_MAIN_CLOCK * xfer_in / (2000000 / 8)
			+ (u64)SH4_MAIN_CLOCK * xfer_out / (740000 / 8);
		sh4_sched_request(maple_sched, (int)std::min(cycles, (u64)SH4_MAIN_CLOCK));
	}
}

static int maple_schd(int tag, int c, int j)
{
	if (SB_MDEN&1)
		maple_dma_done();
	else
	{
		INFO_LOG(MAPLE, "WARNING: MAPLE DMA ABORT");
		SB_MDST=0; //I really wonder what this means, can the DMA be continued ?
		maple_out_used = 0;
	}

	return 0;
}

void maple_SB_MDAPRO_Write(u32 addr, u32 data)
{
	if ((data >> 16) == 0x6155)
		SB_MDAPRO = data & 0x00007f7f;
}

//Init registers :)
void maple_Init()
{
	sb_rio_register(SB_MDST_addr,RIO_WF,0,&maple_SB_MDST_Write);
	sb_rio_register(SB_MDEN_addr,RIO_WF,0,&maple_SB_MDEN_Write);
	sb_rio_register(SB_MSHTCL_addr,RIO_WF,0,&maple_SB_MSHTCL_Write);
	sb_rio_register(SB_MDAPRO_addr, RIO_WO_FUNC, nullptr, &maple_SB_MDAPRO_Write);
#ifdef STRICT_MODE
	sb_rio_register(SB_MDSTAR_addr, RIO_WF, 0, &maple_SB_MDSTAR_Write);
	sb_rio_register(SB_MDSTAR_addr, RIO_WF, nullptr, &maple_SB_MDSTAR_Write);
#endif

	maple_sched=sh4_sched_register(0,&maple_schd);
}

void maple_Reset(bool hard)
{
	maple_ddt_pending_reset=false;
	maple_out_used = 0;
	SB_MDTSEL = 0x00000000;
	SB_MDEN   = 0x00000000;
	SB_MDST   = 0x00000000;
	SB_MSYS   = 0x3A980000;
	SB_MSHTCL = 0x00000000;
	SB_MDAPRO = 0x00007F00;
	SB_MMSEL  = 0x00000001;
}

void maple_Term()
{
	
}

void maple_FlushSaves()
{
	int bus, port;
	for (bus = 0; bus < 4; bus++)
		for (port = 0; port < 6; port++)
			if (MapleDevices[bus][port])
				MapleDevices[bus][port]->FlushSave();
}

static u64 reconnect_time;

void maple_ReconnectDevices()
{
	mcfg_DestroyDevices();
	reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

static void maple_handle_reconnect()
{
	if (reconnect_time != 0 && reconnect_time <= sh4_sched_now64())
	{
		reconnect_time = 0;
		mcfg_CreateDevices();
	}
}
