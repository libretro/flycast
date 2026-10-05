/*
	Created on: Apr 12, 2020
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
#include <cstdint>
#include <vector>
#include <retro_atomic.h>
#include "net_platform.h"
#include "miniupnp.h"

class NaomiNetwork
{
public:
	NaomiNetwork() {
#ifdef _WIN32
		server_ip.S_un.S_addr = INADDR_NONE;
#else
		server_ip.s_addr = INADDR_NONE;
#endif
	}
	~NaomiNetwork() { terminate(); }
	bool init();
	bool startNetwork();
	void pipeSlaves();
	bool receive(u8 *data, u32 size);
	void send(u8 *data, u32 size);
	/* The sockets belong to the thread that runs startNetwork(), receive()
	 * and send(). Another thread stops it with shutdown(), which only
	 * raises a flag that thread looks at wherever it waits, and closes the
	 * sockets with closeSockets() or terminate() once that thread is gone. */
	void shutdown();
	void closeSockets();
	void terminate();

	/* The thread that owns the sockets never sleeps for a fixed time and
	 * never spins. Wherever it has to wait it waits in select() for the
	 * sockets it is waiting on, together with a socket shutdown() writes
	 * to, so a wait ends the moment there is something to do or the
	 * network is told to stop.
	 *
	 * armWake() makes that socket; call it before the thread is started. */
	bool armWake();
	/* Until one of the ring's sockets has something to read. */
	void waitForData();
	/* For @usec, or until shutdown(). */
	void waitStop(int64_t usec);
	int slotCount() const { return slot_count; }
	int slotId() const { return slot_id; }
	u16 packetNumber() const { return packet_number; }
	bool hasToken() const { return got_token; }

private:
	bool createServerSocket();
	bool createBeaconSocket();
	void processBeacon();
	bool findServer();
	sock_t createAndBind(int protocol);
	bool isMaster() const { return slot_id == 0; }

	struct in_addr server_ip;
	std::string server_name;
	// server stuff
	sock_t server_sock = INVALID_SOCKET;
	sock_t beacon_sock = INVALID_SOCKET;
	std::vector<sock_t> slaves;
	// client stuff
	sock_t client_sock = INVALID_SOCKET;
	// common stuff
	int slot_count = 0;
	int slot_id = 0;
	bool got_token = false;
	u16 packet_number = 0;
	bool stopping() { return retro_atomic_load_acquire_int(&network_stopping) != 0; }
	// >0: one of socks is readable; 0: timed out (usec < 0: no timeout) or told to stop
	int waitReadable(const sock_t *socks, int count, int64_t usec);

	retro_atomic_int_t network_stopping;
	sock_t wake_sock = INVALID_SOCKET;
   MiniUPnP miniupnp;

	static const uint16_t SERVER_PORT = 37391;
};
