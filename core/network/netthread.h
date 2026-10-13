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
#include "net_platform.h"

/* The network thread's side of what it shares with the emulation thread
 * (picoppp.cpp): rings that take no lock, one writer and one reader each,
 * and a socket the emulation thread wakes it with. For what runs on that
 * thread: picoTCP, or DCNet. */

// False once the thread is to stop
bool netthread_running();
// To be read before looking for work, and given to netthread_wait()
int netthread_kicks();
// The bytes the modem has sent
size_t netthread_modem_take(u8 *buf, size_t len);
/* Bytes for the modem: how many there was room for. When there was not
 * room for all, the thread is woken once the modem has read some. */
size_t netthread_modem_give(const u8 *buf, size_t len);
// The next frame the adapter has sent, into @frame (NET_ETH_FRAME_MAX bytes): its size, 0 for none
u32 netthread_eth_take(u8 *frame);
/* A frame for the adapter. False if there is no room for it: the thread is
 * woken once the adapter has taken some. */
bool netthread_eth_give(const u8 *frame, u32 size);
/* Waits for one of the sockets in @readable or @writable, for something
 * from the emulation thread, or for @timeout_ms. @kicks: netthread_kicks()
 * from before the thread last looked for work. */
void netthread_wait(int kicks, fd_set *readable, fd_set *writable, int max_fd, int timeout_ms);
// The IPv4 address of @host (network order), 0 if it has none
u32 netthread_resolve(const char *host);

// DCNet, in place of the stack (dcnet.cpp)
void dcnet_run(bool bba);
