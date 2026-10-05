/*
	Created on: Mar 15, 2020
	Copyright 2020 flyinghead
	This file is part of flycast.
    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include <atomic>
#include <memory>
#include <thread>
#include "lockfree.h"
#include "network/naomi_network.h"

class NaomiM3Comm
{
public:
	~NaomiM3Comm();
	u32 ReadMem(u32 address, u32 size);
	void WriteMem(u32 address, u32 data, u32 size);
	bool DmaStart(u32 addr, u32 data);

	void closeNetwork();

private:
	void initNetwork();
	void connectNetwork();
	void receiveNetwork();
	void sendNetwork();
	void connectedState(bool success);
	void startThread();
	void syncNetwork();
	void publishSlot();

	u16 comm_ctrl = 0xC000;
	u16 comm_offset = 0;
	u16 comm_status0 = 0;
	u16 comm_status1 = 0;
	u8 m68k_ram[128 * 1024];
	u8 comm_ram[128 * 1024];

	int slot_count = 0;
	int slot_id = 0;
	std::atomic<bool> network_stopping{ false };
	std::unique_ptr<std::thread> thread;
	NaomiNetwork network;

	/* What the emulation thread and the network thread tell each other.
	 *
	 * comm_ram and m68k_ram belong to the emulation thread; the network
	 * thread never touches them. It keeps its own picture of the ring
	 * (net_ring: this board's slot, then the packet last received) and the
	 * two exchange whole pieces of it through triple buffers, so neither
	 * waits for the other and nothing is ever seen half written:
	 *
	 *   rx  the packet the network thread received last, copied into
	 *       comm_ram by the emulation thread when the game next looks;
	 *   tx  this board's slot as the game last wrote it, picked up by the
	 *       network thread when it next has the token.
	 *
	 * The rest is single words. */
	enum { PACKET_MAX = sizeof(comm_ram) - 0x100, SLOT_MAX = 0x10000 };
	enum { NET_DOWN = 0, NET_UP, NET_SEEN };

	cTripleBuffer rx;
	u8 rx_buf[3][PACKET_MAX];
	cTripleBuffer tx;
	u8 tx_buf[3][SLOT_MAX];
	u8 net_ring[sizeof(comm_ram)];		// network thread only
	int net_thread_slot_count = 0;		// network thread only
	int net_thread_slot_id = 0;		// network thread only
	u16 seen_packet_number = 0;		// emulation thread only

	retro_atomic_int_t net_slot_size;	// set by the game (m68k ram 0x204)
	retro_atomic_int_t net_state;		// NET_UP once connected, NET_SEEN once the game side knows
	retro_atomic_int_t net_slot_count;
	retro_atomic_int_t net_slot_id;
	retro_atomic_int_t net_packet_number;
};
