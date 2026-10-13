/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "bba.h"
#include "rtl8139c.h"
#include "hw/holly/holly_intc.h"
#include "network/picoppp.h"
#include <libretro.h>
#include <features/features_cpu.h>

static RTL8139State *rtl8139device;

// 1400 - 1600 GAPSPCI bridge registers
// 1600 - 1700 standard PCI config
// 1700 - 1800 rtl8139c I/O ports
// 840000 - 847FFF RAM
#define	GAPSPCI_REGS			0x001400
#define	GAPSPCI_REGS_SIZE		   0x200
#define GAPSPCI_PCI_CONFIG		0x001600
#define GAPSPCI_PCI_CONFIG_SIZE	   0x100
#define GAPSPCI_RTL_REGS		0x001700
#define GAPSPCI_RTL_REGS_SIZE	   0x100
#define GAPSPCI_RAM_BASE		0x840000
#define GAPSPCI_RAM_BASE_MASK	0xFF0000
#define GAPSPCI_RAM_SIZE		  0x8000
#define GAPSPCI_RAM_MASK (GAPSPCI_RAM_SIZE - 1)

// GAPSPCI registers
#define GAPS_INT_ENABLE 0x14
#define GAPS_RESET      0x18
#define GAPS_DMA_BASE   0x28
#define GAPS_DMA_OFFSET 0x2c

static u8 GAPS_ram[GAPSPCI_RAM_SIZE];
static u8 GAPS_regs[GAPSPCI_REGS_SIZE];
static u32 dmaOffset;
static bool interruptPending;
static const u32 MAX_ETH_FRAME_SIZE = 1514;

extern std::vector<sched_list> sch_list;

/* The network session the adapter is on: the network thread (picoTCP or
 * DCNet) that is up while the game uses the network, started when the
 * game sends its first frame and stopped when it resets the adapter.
 *
 * As with the modem's line (see modem.cpp), a save state says which
 * session its adapter is on, by a number no other session has, and how
 * many frames it has received and sent on it. A state loaded on the
 * session that is up - run-ahead, rewind - is given again the frames it
 * received since it was made, at the same times, from eth_log, and the
 * frames it sends again are not sent twice. A state of another session
 * has the session that is up stopped: the game's next frame starts one. */
#define ETH_LOG_FRAMES 128
static u32 eth_session;		// the session the adapter is on, 0 for none (in the state)
static u32 eth_rx_pos;		// frames received from it (in the state)
static u32 eth_tx_pos;		// frames sent to it (in the state)
static u32 live_session;	// the session up, 0 for none
static u32 eth_rx_taken;	// frames taken from the live session
static u32 eth_tx_sent;		// frames sent to it
static u64 eth_looked;		// the last time the live session itself was read
static u64 live_session_made;	// when the live session was started (emulated time)
static u8 eth_log[ETH_LOG_FRAMES][NET_ETH_FRAME_MAX];	// the last frames taken from it
static u16 eth_log_size[ETH_LOG_FRAMES];
static u64 eth_log_time[ETH_LOG_FRAMES];		// and when
static int deliver_schid = -1;

static bool session_is_live()
{
	return live_session != 0 && eth_session == live_session;
}

static void session_stop()
{
	if (live_session != 0)
	{
		stop_pico();
		live_session = 0;
	}
}

static bool session_start()
{
	if (live_session != 0 && eth_session == 0 && live_session_made == sh4_sched_now64()
			&& eth_rx_taken <= ETH_LOG_FRAMES)
	{
		/* Started at this very moment, by the machine of a state loaded
		 * since (run-ahead): it is this one's, and what came from it is
		 * given again as it came */
		eth_session = live_session;
		eth_rx_pos = 0;
		eth_tx_pos = 0;
		sh4_sched_request(deliver_schid, SH4_MAIN_CLOCK / 1000);
		return true;
	}
	session_stop();
	if (!start_pico())
	{
		WARN_LOG(NETWORK, "BBA: the network cannot be started");
		return false;
	}
	// The time is the session's number: no other session, of this
	// process or of another, was started at this microsecond
	live_session = (u32)cpu_features_get_time_usec();
	if (live_session == 0)
		live_session = 1;
	live_session_made = sh4_sched_now64();
	eth_session = live_session;
	eth_rx_pos = eth_rx_taken = 0;
	eth_tx_pos = eth_tx_sent = 0;
	eth_looked = 0;
	sh4_sched_request(deliver_schid, SH4_MAIN_CLOCK / 1000);
	return true;
}

static int bba_recv_frame(const u8 *data, u32 len);

/* Once every millisecond of emulated time while the adapter is on a
 * session: the frames that have come from the network are given to it, as
 * many as it has room for. One it has no room for is offered again. */
static int deliver_sched(int tag, int cycles, int jitter)
{
	if (!session_is_live())
	{
		eth_session = 0;
		return 0;
	}
	const u64 now = sh4_sched_now64();
	for (;;)
	{
		const u32 slot = eth_rx_pos % ETH_LOG_FRAMES;
		if (eth_rx_pos != eth_rx_taken)
		{
			// Again: at the time it was taken the first time
			if (eth_log_time[slot] > now)
				break;
		}
		else
		{
			// The session had nothing more at this time when it was read
			if (now <= eth_looked)
				break;
			const u32 size = net_eth_take(eth_log[slot]);
			if (size == 0)
				break;
			eth_log_size[slot] = (u16)size;
			eth_log_time[slot] = now;
			eth_rx_taken++;
		}
		if (!bba_recv_frame(eth_log[slot], eth_log_size[slot]))
			break;
		eth_rx_pos++;
	}
	if (eth_rx_pos == eth_rx_taken && now > eth_looked)
		eth_looked = now;
	return SH4_MAIN_CLOCK / 1000;
}

static void setInterrupt()
{
	if (interruptPending && GAPS_regs[GAPS_INT_ENABLE] != 0)
		asic_RaiseInterrupt(holly_EXP_PCI);
	else
		asic_CancelInterrupt(holly_EXP_PCI);
}

void pci_set_irq(PCIDevice *pci_dev, int level)
{
	interruptPending = level != 0;
	setInterrupt();
}

void bba_Init()
{
	deliver_schid = sh4_sched_register(0, &deliver_sched);
	eth_session = 0;
	eth_rx_pos = 0;
	eth_tx_pos = 0;
	NICConf nicConf = { 0xc, 0xa, 0xf, 0xe, 0, 0 };
	rtl8139device = rtl8139_init(&nicConf);
	pci_rtl8139_realize(PCI_DEVICE(rtl8139device));

	memset(&GAPS_regs[0], 0, sizeof(GAPS_regs));
	memcpy(&GAPS_regs[0], "GAPSPCI_BRIDGE_2", 0x10);
	memcpy(&GAPS_regs[0x1c], "SEGA", 4);
}

void bba_Term()
{
	if (rtl8139device != nullptr)
	{
		session_stop();
		eth_session = 0;
		if (sh4_sched_is_scheduled(deliver_schid))
			sh4_sched_request(deliver_schid, -1);
		rtl8139_destroy(rtl8139device);
		rtl8139device = nullptr;
	}
}

void bba_Reset(bool hard)
{
	if (hard)
	{
		bba_Term();
		bba_Init();
	}
}

u32 bba_ReadMem(u32 addr, u32 sz)
{
	u32 data = 0;
	if ((addr & GAPSPCI_RAM_BASE_MASK) == GAPSPCI_RAM_BASE)
	{
		if (addr & 0x8000)
			// G2 DMA access
			addr += dmaOffset;
		addr &= GAPSPCI_RAM_MASK;
		if (addr + sz > GAPSPCI_RAM_SIZE)
		{
			// wrap around
			memcpy(&data, &GAPS_ram[addr], GAPSPCI_RAM_SIZE - addr);
			memcpy((u8 *)&data + (GAPSPCI_RAM_SIZE - addr), &GAPS_ram[0], sz - (GAPSPCI_RAM_SIZE - addr));
		}
		else
			memcpy(&data, &GAPS_ram[addr], sz);

		return data;
	}
	DEBUG_LOG(NETWORK, "bba_ReadMem<%d> %06x", sz, addr);

	switch (addr & 0xFFFF00)
	{
	case GAPSPCI_REGS:
	case GAPSPCI_REGS + 0x100:
		addr &= GAPSPCI_REGS_SIZE - 1;
		memcpy(&data, &GAPS_regs[addr], sz);
		if (addr == GAPS_RESET)
			data &= 0xFF;
		break;

	case GAPSPCI_PCI_CONFIG:
		memcpy(&data, &PCI_DEVICE(rtl8139device)->config[addr & (GAPSPCI_PCI_CONFIG_SIZE - 1)], sz);
		DEBUG_LOG(NETWORK, "pcidev->config(r%d) %02x %x", sz, addr & (GAPSPCI_PCI_CONFIG_SIZE - 1), data);
		break;

	case GAPSPCI_RTL_REGS:
		return rtl8139_ioport_read(rtl8139device, addr & (GAPSPCI_RTL_REGS_SIZE - 1), sz);

	default:
		INFO_LOG(NETWORK, "bba_ReadMem<%d> address %x unknown", sz, addr);
		data = -1;
		break;
	}

	return data;
}

void bba_WriteMem(u32 addr, u32 data, u32 sz)
{
	if ((addr & GAPSPCI_RAM_BASE_MASK) == GAPSPCI_RAM_BASE)
	{
		if (addr & 0x8000)
			// G2 DMA access
			addr += dmaOffset;
		addr &= GAPSPCI_RAM_MASK;
		if (addr + sz > GAPSPCI_RAM_SIZE)
		{
			// wrap around
			memcpy(&GAPS_ram[addr], &data, GAPSPCI_RAM_SIZE - addr);
			memcpy(&GAPS_ram[0], (u8 *)&data + (GAPSPCI_RAM_SIZE - addr), sz - (GAPSPCI_RAM_SIZE - addr));
		}
		else
			memcpy(&GAPS_ram[addr], &data, sz);

		return;
	}
	DEBUG_LOG(NETWORK, "bba_WriteMem<%d> %06x = %x", sz, addr, data);

	switch (addr & 0xFFFF00)
	{
	case GAPSPCI_REGS:
	case GAPSPCI_REGS + 0x100:
		addr &= GAPSPCI_REGS_SIZE - 1;
		memcpy(&GAPS_regs[addr], &data, sz);
		switch(addr)
		{
		case GAPS_INT_ENABLE:
			setInterrupt();
			break;

		case GAPS_RESET:
			if (data & 1)
			{
				INFO_LOG(NETWORK, "BBA: GAPS reset");
				rtl8139_reset(rtl8139device);
				// A new session is started by the next frame the game sends
				session_stop();
				eth_session = 0;
			}
			break;

		case GAPS_DMA_OFFSET:
			dmaOffset = data & GAPSPCI_RAM_MASK;
			break;
		}
		break;

	case GAPSPCI_PCI_CONFIG:
		DEBUG_LOG(NETWORK, "pcidev->config(w%d) %02x %x", sz, addr & (GAPSPCI_PCI_CONFIG_SIZE - 1), data);
		//pci_default_write_config(pcidev, addr & 0xFF, data, sz);
		break;

	case GAPSPCI_RTL_REGS:
		rtl8139_ioport_write(rtl8139device, addr & (GAPSPCI_RTL_REGS_SIZE - 1), data, sz);
		break;

	default:
		INFO_LOG(NETWORK, "bba_WriteMem<%d> address %x unknown (data %x)", sz, addr, data);
		break;
	}
}

ssize_t qemu_send_packet(RTL8139State *s, const uint8_t *buf, int size)
{
	if (!session_is_live() && !session_start())
		return size;
	if (eth_tx_pos == eth_tx_sent)
	{
		pico_receive_eth_frame(buf, size);
		eth_tx_sent++;
	}
	eth_tx_pos++;

	return size;
}

// 0 if the adapter has no room for the frame now
static int bba_recv_frame(const u8 *data, u32 len)
{
	// (what no Ethernet frame is, is dropped)
	if (len < 6 || len > MAX_ETH_FRAME_SIZE)
		return 1;
	if (!rtl8139_can_receive(rtl8139device))
		return 0;
	rtl8139_receive(rtl8139device, data, len);

	return 1;
}

// The address and the length are the guest's (a transmit descriptor, the
// receive ring's place and size). A transfer that reaches the end of the
// adapter's memory goes on at its start, as the reads and writes above do.
void pci_dma_read(PCIDevice *dev, dma_addr_t addr, void *buf, dma_addr_t len)
{
	while (len != 0)
	{
		addr &= GAPSPCI_RAM_MASK;
		const dma_addr_t chunk = len < GAPSPCI_RAM_SIZE - addr ? len : GAPSPCI_RAM_SIZE - addr;
		memcpy(buf, &GAPS_ram[addr], chunk);
		buf = (u8 *)buf + chunk;
		addr += chunk;
		len -= chunk;
	}
}

void pci_dma_write(PCIDevice *dev, dma_addr_t addr, const void *buf, dma_addr_t len)
{
	while (len != 0)
	{
		addr &= GAPSPCI_RAM_MASK;
		const dma_addr_t chunk = len < GAPSPCI_RAM_SIZE - addr ? len : GAPSPCI_RAM_SIZE - addr;
		memcpy(&GAPS_ram[addr], buf, chunk);
		buf = (const u8 *)buf + chunk;
		addr += chunk;
		len -= chunk;
	}
}

void bba_Serialize(void **data, unsigned int *total_size)
{
	LIBRETRO_S(GAPS_regs);
	LIBRETRO_S(GAPS_ram);
	LIBRETRO_S(dmaOffset);
	LIBRETRO_S(interruptPending);
	rtl8139_serialize(rtl8139device, data, total_size);
}

void bba_Unserialize(void **data, unsigned int *total_size)
{
	LIBRETRO_US(GAPS_regs);
	LIBRETRO_US(GAPS_ram);
	LIBRETRO_US(dmaOffset);
	LIBRETRO_US(interruptPending);
	// returns true if the receiver is enabled and the network stack must be started
	if (rtl8139_unserialize(rtl8139device, data, total_size))
		start_pico();
}

void bba_serialize_v24(void **data, unsigned int *total_size)
{
	u8 pending = interruptPending;
	s32 deliver_start = sch_list[deliver_schid].start;
	s32 deliver_end = sch_list[deliver_schid].end;

	LIBRETRO_SA(GAPS_regs, GAPSPCI_REGS_SIZE);
	LIBRETRO_SA(GAPS_ram, GAPSPCI_RAM_SIZE);
	LIBRETRO_S(dmaOffset);
	LIBRETRO_S(pending);
	rtl8139_serialize_v24(rtl8139device, data, total_size);
	LIBRETRO_S(deliver_start);
	LIBRETRO_S(deliver_end);
	LIBRETRO_S(eth_session);
	LIBRETRO_S(eth_rx_pos);
	LIBRETRO_S(eth_tx_pos);
}

bool bba_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 pending = 0;
	s32 deliver_start = -1;
	s32 deliver_end = -1;

	LIBRETRO_USA(GAPS_regs, GAPSPCI_REGS_SIZE);
	LIBRETRO_USA(GAPS_ram, GAPSPCI_RAM_SIZE);
	LIBRETRO_US(dmaOffset);
	LIBRETRO_US(pending);
	rtl8139_unserialize_v24(rtl8139device, data, total_size);
	LIBRETRO_US(deliver_start);
	LIBRETRO_US(deliver_end);
	LIBRETRO_US(eth_session);
	LIBRETRO_US(eth_rx_pos);
	LIBRETRO_US(eth_tx_pos);

	dmaOffset &= GAPSPCI_RAM_MASK;
	interruptPending = pending != 0;
	sch_list[deliver_schid].start = deliver_start;
	sch_list[deliver_schid].end = deliver_end;
	/* The session that is up is not the state's, or the state is from too
	 * long ago on it for its frames to be given again: the state's adapter
	 * is on none, and the game's next frame starts one */
	if (live_session != 0
			&& (eth_session != live_session
				|| eth_rx_taken - eth_rx_pos > ETH_LOG_FRAMES
				|| eth_tx_sent - eth_tx_pos > 0x40000000)
			// (but a session started after the state's time, which its
			// machine may start again at the same time: see session_start())
			&& !(eth_session == 0 && live_session_made > sh4_sched_now64() && eth_rx_taken <= ETH_LOG_FRAMES))
		session_stop();
	if (!session_is_live())
		eth_session = 0;
	return true;
}


#define POLYNOMIAL_BE 0x04c11db6

uint32_t net_crc32(const uint8_t *p, int len)
{
    uint32_t crc;
    int carry, i, j;
    uint8_t b;

    crc = 0xffffffff;
    for (i = 0; i < len; i++) {
        b = *p++;
        for (j = 0; j < 8; j++) {
            carry = ((crc & 0x80000000L) ? 1 : 0) ^ (b & 0x01);
            crc <<= 1;
            b >>= 1;
            if (carry) {
                crc = ((crc ^ POLYNOMIAL_BE) | carry);
            }
        }
    }

    return crc;
}

/*
 * QEMU PCI bus manager
 *
 * Copyright (c) 2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
static inline bool is_power_of_2(uint64_t value)
{
    if (!value) {
        return false;
    }

    return !(value & (value - 1));
}

int pci_bar(PCIDevice *d, int reg)
{
    uint8_t type;

    if (reg != PCI_ROM_SLOT)
        return PCI_BASE_ADDRESS_0 + reg * 4;

    type = d->config[PCI_HEADER_TYPE] & ~PCI_HEADER_TYPE_MULTI_FUNCTION;
    return type == PCI_HEADER_TYPE_BRIDGE ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS;
}

void pci_register_bar(PCIDevice *pci_dev, int region_num,
                      uint8_t type, MemoryRegion *memory)
{
    PCIIORegion *r;
    uint32_t addr; /* offset in pci config space */
    uint64_t wmask;
    pcibus_t size = memory->size;

    verify(region_num >= 0);
    verify(region_num < PCI_NUM_REGIONS);
    verify(is_power_of_2(size));

    r = &pci_dev->io_regions[region_num];
    r->addr = PCI_BAR_UNMAPPED;
    r->size = size;
    r->type = type;

    wmask = ~(size - 1);
    if (region_num == PCI_ROM_SLOT) {
        /* ROM enable bit is writable */
        wmask |= PCI_ROM_ADDRESS_ENABLE;
    }

    addr = pci_bar(pci_dev, region_num);
    pci_set_long(pci_dev->config + addr, type);

    pci_set_long(pci_dev->wmask + addr, wmask & 0xffffffff);
    pci_set_long(pci_dev->cmask + addr, 0xffffffff);
}
