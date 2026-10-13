/*
	Created on: Sep 15, 2018

	Copyright 2018 flyinghead

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

#pragma once
#include "types.h"

/* The network thread, for the modem and the broadband adapter: picoTCP, or
 * the DCNet service. Called on the emulation thread. */
bool start_pico();
// The thread is told to stop: it is not waited for
void stop_pico();
// The thread is stopped and waited for (the machine stops)
void net_thread_join();
void write_pico(u8 b);
int read_pico();

// The longest frame that crosses between the adapter and the network
#define NET_ETH_FRAME_MAX 1600
// A frame the adapter has sent
void pico_receive_eth_frame(const u8 *frame, u32 size);
/* The next frame from the network for the adapter, into @frame
 * (NET_ETH_FRAME_MAX bytes): its size, 0 if there is none */
u32 net_eth_take(u8 *frame);
