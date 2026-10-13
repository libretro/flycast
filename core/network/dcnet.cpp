/*
	Copyright 2025 flyinghead

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
/* DCNet: the Dreamcast's line goes over the Internet to a relay of the
 * community's servers, which answers the modem's PPP or the adapter's
 * Ethernet itself, so that nothing of the network is emulated here.
 *
 * It runs on the network thread in place of picoTCP, with the same rings
 * to the emulated modem and adapter, and waits the same way: in select(),
 * on its sockets and on the emulation thread's wake socket. Its sockets
 * do not block. */
#include "types.h"
#include "netthread.h"
#include "picoppp.h"
#include "reios/reios.h"
#include "stdclass.h"
#include <libretro.h>
#include <features/features_cpu.h>
#include <string>
#include <vector>
#include <algorithm>

#if !defined(_MSC_VER) && !defined(TARGET_NO_THREADS)

static const char DCNET_HOST[] = "dcnet.flyca.st";
static const u16 PPP_PORT = 7654;
static const u16 TAP_PORT = 7655;
static const u16 POWER_SMASH_PPP_PORT = 7656;
// Access points: the main server lists them, and the one that answers fastest is used
static const u16 AP_PORT = 7655;
static const u8 AP_MAGIC[4] = { 0x01, 0xc0, 0x15, 0xdc };	// 0xDC15C001, as the other clients send it
enum { AP_PING = 1, AP_PONG = 2, AP_DISCOVER = 3 };

static const u32 MAX_ETH_FRAME_SIZE = 1514;
// What waits for the socket, or for the modem or the adapter, at most
static const size_t MAX_PENDING = 64 * 1024;

static u64 now_ms()
{
	return (u64)cpu_features_get_time_usec() / 1000;
}

static void close_socket(sock_t& s)
{
	if (VALID(s))
		closesocket(s);
	s = INVALID_SOCKET;
}

struct AccessPoint
{
	u32 addr;		// network order
	std::string name;
	int ping;
	int count;
};

static void send_ping(sock_t s, u32 addr)
{
	u8 buf[13];
	sockaddr_in to;
	const u64 now = now_ms();

	memcpy(&buf[0], AP_MAGIC, 4);
	buf[4] = AP_PING;
	memcpy(&buf[5], &now, sizeof(now));		// given back as it is
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(AP_PORT);
	to.sin_addr.s_addr = addr;
	sendto(s, (const char *)buf, sizeof(buf), L_MSG_NOSIGNAL, (const sockaddr *)&to, sizeof(to));
}

/* The address to connect to (network order): the access point that answers
 * fastest, the main server if it lists none, 0 if there is no server. A
 * second at most. */
static u32 find_access_point(std::string& apName)
{
	const u32 mainAddr = netthread_resolve(DCNET_HOST);
	if (mainAddr == 0)
	{
		WARN_LOG(MODEM, "DCNet: can't resolve %s", DCNET_HOST);
		return 0;
	}
	sock_t s = selectable_socket(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
	if (!VALID(s))
		return mainAddr;
	set_non_blocking(s);

	u8 discover[5];
	memcpy(discover, AP_MAGIC, 4);
	discover[4] = AP_DISCOVER;
	sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(AP_PORT);
	to.sin_addr.s_addr = mainAddr;
	sendto(s, (const char *)discover, sizeof(discover), L_MSG_NOSIGNAL, (const sockaddr *)&to, sizeof(to));

	std::vector<AccessPoint> aps;
	bool listed = false;
	bool pingedAgain = false;
	bool done = false;
	const u64 start = now_ms();
	while (!done && netthread_running())
	{
		const int kicks = netthread_kicks();
		const u64 elapsed = now_ms() - start;
		if (elapsed >= 1000)
			break;
		if (elapsed >= 500 && !pingedAgain)
		{
			// Again those that have not answered
			pingedAgain = true;
			for (const AccessPoint& ap : aps)
				if (ap.count == 0)
					send_ping(s, ap.addr);
		}
		u8 buf[512];
		sockaddr_in from;
		socklen_t fromLen = sizeof(from);
		const int len = (int)recvfrom(s, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen);
		if (len < 0)
		{
			fd_set readable, writable;
			FD_ZERO(&readable);
			FD_ZERO(&writable);
			FD_SET(s, &readable);
			const u64 phaseEnd = pingedAgain ? 1000 : 500;
			netthread_wait(kicks, &readable, &writable, (int)s, (int)(phaseEnd - elapsed));
			continue;
		}
		if (len < 5 || from.sin_port != htons(AP_PORT))
			continue;
		if (!listed && buf[4] == AP_DISCOVER && from.sin_addr.s_addr == mainAddr
				&& memcmp(buf, AP_MAGIC, 4) == 0)
		{
			// address (4), name length (1), name: as many as the datagram holds
			listed = true;
			for (int p = 5; p + 5 <= len; )
			{
				AccessPoint ap;
				memcpy(&ap.addr, &buf[p], 4);
				const int l = buf[p + 4];
				if (p + 5 + l > len)
					break;
				ap.name.assign((const char *)&buf[p + 5], l);
				ap.ping = 0;
				ap.count = 0;
				aps.push_back(ap);
				p += 5 + l;
			}
			if (aps.size() > 1)
			{
				for (const AccessPoint& ap : aps)
					send_ping(s, ap.addr);
			}
			else
				done = true;
		}
		else if (len == 13 && buf[4] == AP_PONG)
		{
			u64 ts;
			memcpy(&ts, &buf[5], sizeof(ts));
			const int ping = (int)(now_ms() - ts);
			for (AccessPoint& ap : aps)
			{
				if (ap.addr != from.sin_addr.s_addr)
					continue;
				ap.ping += ping;
				ap.count++;
				if (ap.count < 5)
					send_ping(s, ap.addr);
				else
					// five answers from one access point: enough
					done = true;
				break;
			}
		}
	}
	closesocket(s);

	if (aps.empty())
		return mainAddr;
	const AccessPoint *best = nullptr;
	int bestPing = 1000000;
	for (const AccessPoint& ap : aps)
	{
		if (ap.count == 0) {
			INFO_LOG(MODEM, "DCNet AP %s: no answer", ap.name.c_str());
			continue;
		}
		const int ping = ap.ping / ap.count;
		INFO_LOG(MODEM, "DCNet AP %s: ping %d ms (%d pings)", ap.name.c_str(), ping, ap.count);
		if (ping < bestPing) {
			bestPing = ping;
			best = &ap;
		}
	}
	if (best == nullptr)
		best = &aps[0];
	apName = best->name;
	return best->addr;
}

// A connection to @port of the access point, INVALID_SOCKET if none can be had
static sock_t dcnet_connect(u16 port)
{
	std::string apName;
	const u32 addr = find_access_point(apName);
	if (addr == 0 || !netthread_running())
		return INVALID_SOCKET;

	sock_t s = selectable_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	if (!VALID(s))
		return INVALID_SOCKET;
	set_non_blocking(s);
	set_no_sigpipe(s);
	sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = addr;
	if (connect(s, (const sockaddr *)&to, sizeof(to)) < 0
			&& get_last_error() != EINPROGRESS && get_last_error() != L_EWOULDBLOCK)
	{
		perror("DCNet connect");
		closesocket(s);
		return INVALID_SOCKET;
	}
	// Ten seconds at most, and no longer than the thread is wanted
	const u64 start = now_ms();
	while (true)
	{
		const int kicks = netthread_kicks();
		if (!netthread_running() || now_ms() - start >= 10000)
		{
			if (netthread_running())
				WARN_LOG(MODEM, "DCNet: no connection after 10 s");
			closesocket(s);
			return INVALID_SOCKET;
		}
		fd_set readable, writable, failed;
		FD_ZERO(&readable);
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(s, &writable);
		FD_SET(s, &failed);
		timeval tv = { 0, 0 };
		if (select((int)s + 1, NULL, &writable, &failed, &tv) > 0)
			break;
		FD_ZERO(&writable);
		FD_SET(s, &writable);
		netthread_wait(kicks, &readable, &writable, (int)s, 100);
	}
#ifdef _WIN32
	char value = 0;
#else
	int value = 0;
#endif
	socklen_t l = sizeof(value);
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &l) < 0 || value != 0)
	{
		WARN_LOG(MODEM, "DCNet: connection to %s refused", apName.empty() ? DCNET_HOST : apName.c_str());
		closesocket(s);
		return INVALID_SOCKET;
	}
	set_tcp_nodelay(s);
	NOTICE_LOG(MODEM, "Connected to DCNet%s%s", apName.empty() ? "" : " via ", apName.c_str());
	return s;
}

/* A DHCP release from the Dreamcast: the game is done with the network
 * (the connection is closed; its next frame opens another) */
static bool is_dhcp_release(const u8 *f, u32 len)
{
	if (len < 0x11d
			|| f[0xc] != 0x08 || f[0xd] != 0x00	// type: IPv4
			|| f[0x17] != 0x11					// UDP
			|| ((f[0x22] << 8) | f[0x23]) != 68	// src port: dhcpc
			|| ((f[0x24] << 8) | f[0x25]) != 67)	// dest port: dhcps
		return false;
	// The options, after the magic cookie
	for (u32 i = 0x11a; i < len && f[i] != 0xff; )
	{
		if (f[i] == 0)
		{
			// pad
			i++;
			continue;
		}
		if (i + 2 >= len)
			break;
		if (f[i] == 53 && f[i + 2] == 7)	// message type: release
			return true;
		i += f[i + 1] + 2;
	}
	return false;
}

void dcnet_run(bool bba)
{
	std::string gameId(ip_meta.product_number, sizeof(ip_meta.product_number));
	gameId = trim_trailing_ws(gameId);
	/* These games want their PPP frames each with a flag at both ends, which
	 * a server on its own port sees to */
	const bool powerSmash = !bba
			&& (gameId == "HDR-0113"		// Power Smash
				|| gameId == "HDR-0091"		// Pro Yakyuu Team de Asobou Net!
				|| gameId == "HDR-0040");	// Virtual-On Oratorio Tangram
	const u16 port = bba ? TAP_PORT : powerSmash ? POWER_SMASH_PPP_PORT : PPP_PORT;

	sock_t sock = INVALID_SOCKET;
	// The modem's line is made once: when it is lost, it is lost until the game dials again
	bool lineLost = false;
	// The adapter's connection is opened by a frame of the game's, kept here meanwhile
	static u8 heldFrame[NET_ETH_FRAME_MAX];
	u32 heldSize = 0;
	u64 lastAttempt = 0;
	bool attempted = false;
	std::vector<u8> toNet;		// for the socket
	std::vector<u8> fromNet;	// from the socket: frames (adapter) or bytes to frame (Power Smash)
	std::vector<u8> toModem;	// for the modem, when its ring is full
	size_t fromNetPos = 0;

	while (netthread_running())
	{
		const int kicks = netthread_kicks();

		if (!VALID(sock))
		{
			u8 drain[512];
			if (!bba)
			{
				if (lineLost)
				{
					// What the modem sends goes nowhere
					while (netthread_modem_take(drain, sizeof(drain)) != 0)
						;
				}
				else
				{
					sock = dcnet_connect(port);
					if (!VALID(sock))
					{
						lineLost = true;
						continue;
					}
				}
			}
			else
			{
				if (heldSize == 0)
					heldSize = netthread_eth_take(heldFrame);
				if (heldSize != 0 && is_dhcp_release(heldFrame, heldSize))
					heldSize = 0;
				// One attempt each five seconds at most: the frames in between are lost
				if (heldSize != 0 && attempted && now_ms() - lastAttempt < 5000)
					heldSize = 0;
				if (heldSize != 0)
				{
					attempted = true;
					lastAttempt = now_ms();
					sock = dcnet_connect(port);
					if (VALID(sock))
					{
						static const u8 prolog[] = { 'D', 'C', 'N', 'E', 'T', 1 };
						toNet.clear();
						fromNet.clear();
						fromNetPos = 0;
						toNet.push_back(sizeof(prolog));
						toNet.push_back(0);
						toNet.insert(toNet.end(), prolog, prolog + sizeof(prolog));
						if (heldSize <= MAX_ETH_FRAME_SIZE)
						{
							toNet.push_back((u8)heldSize);
							toNet.push_back((u8)(heldSize >> 8));
							toNet.insert(toNet.end(), heldFrame, heldFrame + heldSize);
						}
					}
					heldSize = 0;
					continue;
				}
			}
			if (!VALID(sock))
			{
				fd_set readable, writable;
				FD_ZERO(&readable);
				FD_ZERO(&writable);
				netthread_wait(kicks, &readable, &writable, -1, 1000);
				continue;
			}
		}

		// To the network
		bool closing = false;
		if (bba)
		{
			u8 frame[NET_ETH_FRAME_MAX];
			while (toNet.size() + 2 + NET_ETH_FRAME_MAX <= MAX_PENDING)
			{
				const u32 size = netthread_eth_take(frame);
				if (size == 0)
					break;
				if (size > MAX_ETH_FRAME_SIZE)
					continue;
				if (is_dhcp_release(frame, size))
				{
					INFO_LOG(MODEM, "DCNet: DHCP release");
					closing = true;
					break;
				}
				toNet.push_back((u8)size);
				toNet.push_back((u8)(size >> 8));
				toNet.insert(toNet.end(), frame, frame + size);
			}
		}
		else
		{
			u8 buf[2048];
			while (toNet.size() < MAX_PENDING)
			{
				const size_t n = netthread_modem_take(buf, std::min(sizeof(buf), MAX_PENDING - toNet.size()));
				if (n == 0)
					break;
				toNet.insert(toNet.end(), buf, buf + n);
			}
		}
		if (!toNet.empty())
		{
			const int r = (int)send(sock, (const char *)&toNet[0], toNet.size(), L_MSG_NOSIGNAL);
			if (r < 0)
			{
				if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				{
					perror("DCNet send");
					closing = true;
				}
			}
			else
				toNet.erase(toNet.begin(), toNet.begin() + r);
		}

		// From the network: what was received before first
		if (!bba)
		{
			if (!toModem.empty())
			{
				const size_t n = netthread_modem_give(&toModem[0], toModem.size());
				toModem.erase(toModem.begin(), toModem.begin() + n);
			}
		}
		else
		{
			while (fromNet.size() - fromNetPos >= 2)
			{
				const u32 size = fromNet[fromNetPos] | (fromNet[fromNetPos + 1] << 8);
				if (size > MAX_ETH_FRAME_SIZE)
				{
					ERROR_LOG(MODEM, "DCNet: invalid Ethernet frame size: %d", size);
					closing = true;
					break;
				}
				if (fromNet.size() - fromNetPos < 2 + size)
					break;
				if (!netthread_eth_give(&fromNet[fromNetPos + 2], size))
					break;
				fromNetPos += 2 + size;
			}
			if (fromNetPos != 0)
			{
				fromNet.erase(fromNet.begin(), fromNet.begin() + fromNetPos);
				fromNetPos = 0;
			}
		}
		// Room for more: everything received has gone, but a part of a frame
		const bool room = !closing && toModem.empty()
				&& (!bba || fromNet.size() < 2 + MAX_ETH_FRAME_SIZE);
		if (room)
		{
			u8 buf[2048];
			const int r = (int)recv(sock, (char *)buf, sizeof(buf), 0);
			if (r == 0)
			{
				INFO_LOG(MODEM, "DCNet: connection closed by the server");
				closing = true;
			}
			else if (r < 0)
			{
				if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				{
					perror("DCNet recv");
					closing = true;
				}
			}
			else if (bba)
				fromNet.insert(fromNet.end(), buf, buf + r);
			else if (!powerSmash)
			{
				const size_t n = netthread_modem_give(buf, r);
				toModem.insert(toModem.end(), buf + n, buf + r);
			}
			else
			{
				// Each frame ends with its flag: one is put before it too, if it has none
				fromNet.insert(fromNet.end(), buf, buf + r);
				size_t start = 0;
				for (size_t i = 1; i < fromNet.size(); i++)
				{
					if (fromNet[i] != '~')
						continue;
					if (fromNet[start] != '~')
						toModem.push_back('~');
					toModem.insert(toModem.end(), fromNet.begin() + start, fromNet.begin() + i + 1);
					start = i + 1;
					i++;
				}
				fromNet.erase(fromNet.begin(), fromNet.begin() + start);
				if (fromNet.size() >= 1542)
				{
					// no flag in sight: as it is
					toModem.insert(toModem.end(), fromNet.begin(), fromNet.end());
					fromNet.clear();
				}
				const size_t n = toModem.empty() ? 0 : netthread_modem_give(&toModem[0], toModem.size());
				toModem.erase(toModem.begin(), toModem.begin() + n);
			}
			if (r > 0)
				// there may be more
				continue;
		}
		if (closing)
		{
			close_socket(sock);
			toNet.clear();
			fromNet.clear();
			fromNetPos = 0;
			toModem.clear();
			if (!bba)
				lineLost = true;
			continue;
		}

		fd_set readable, writable;
		FD_ZERO(&readable);
		FD_ZERO(&writable);
		if (room)
			FD_SET(sock, &readable);
		if (!toNet.empty())
			FD_SET(sock, &writable);
		netthread_wait(kicks, &readable, &writable, (int)sock, 1000);
	}
	close_socket(sock);
}

#endif
