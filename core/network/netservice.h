/*
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
#pragma once
#include "types.h"

/* The device in the Dreamcast's expansion port, through which online games
 * reach the network: the modem it came with, the broadband adapter, or
 * nothing. */
enum
{
	NET_DEVICE_NONE = 0,
	NET_DEVICE_MODEM = 1,
	NET_DEVICE_BBA = 2,
};
// The device of the running machine (emulation thread)
extern u8 net_device;
// The device the core option asks for: the machine gets it when it starts and at a hard reset
extern u8 net_device_option;
// Whether the network is reached through the DCNet service rather than the built-in stack
extern bool net_dcnet_option;

// The port is filled when the machine starts, and emptied when it stops (holly/sb.cpp)
void net_device_attach(void);
void net_device_reset(bool hard);
void net_device_detach(void);

/* The network devices in a save state, from version 24 on: which device the
 * machine has, and what that device holds beyond what older states have. */
void net_serialize_v24(void **data, unsigned int *total_size);
bool net_unserialize_v24(void **data, unsigned int *total_size);
void net_state_before_v24(void);
