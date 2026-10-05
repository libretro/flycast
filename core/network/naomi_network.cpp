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
#include "naomi_network.h"

#include "types.h"
#include <features/features_cpu.h>
#ifndef __LIBRETRO__
// FIXME implement gui_display_notification with libretro widgets
#include "rend/gui.h"
#endif

sock_t NaomiNetwork::createAndBind(int protocol)
{
	sock_t sock = socket(AF_INET, protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, protocol);
	if (sock == INVALID_SOCKET)
	{
		ERROR_LOG(NETWORK, "Cannot create server socket");
		return INVALID_SOCKET;
	}
	int option = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));

	struct sockaddr_in serveraddr;
	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(SERVER_PORT);

	if (::bind(sock, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
	{
		ERROR_LOG(NETWORK, "NaomiServer: bind() failed. errno=%d", get_last_error());
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	else
		set_non_blocking(sock);

	return sock;
}

bool NaomiNetwork::armWake()
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
		return false;
#endif
	if (wake_sock == INVALID_SOCKET)
	{
		/* A datagram socket on the loopback interface, connected to
		 * itself: a byte sent to it makes it readable. */
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		sock_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (sock == INVALID_SOCKET)
			return false;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0
				|| getsockname(sock, (struct sockaddr *)&addr, &len) < 0
				|| ::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		{
			closesocket(sock);
			return false;
		}
		set_non_blocking(sock);
		wake_sock = sock;
	}
	else
	{
		// What the last shutdown() left in it
		char c;
		while (::recv(wake_sock, &c, 1, 0) > 0)
			;
	}
	retro_atomic_store_release_int(&network_stopping, 0);
	return true;
}

int NaomiNetwork::waitReadable(const sock_t *socks, int count, int64_t usec)
{
	fd_set readable;
	struct timeval tv;
	int max_fd = -1;
	int found = 0;

	if (stopping())
		return 0;

	FD_ZERO(&readable);
	for (int i = 0; i < count; i++)
	{
		if (socks[i] == INVALID_SOCKET)
			continue;
		FD_SET(socks[i], &readable);
		max_fd = std::max(max_fd, (int)socks[i]);
	}
	if (wake_sock != INVALID_SOCKET)
	{
		FD_SET(wake_sock, &readable);
		max_fd = std::max(max_fd, (int)wake_sock);
	}
	else if (usec < 0)
		// Nothing could ever end the wait
		return 0;

	if (usec >= 0)
	{
		tv.tv_sec = (long)(usec / 1000000);
		tv.tv_usec = (long)(usec % 1000000);
	}
	if (select(max_fd + 1, &readable, nullptr, nullptr, usec >= 0 ? &tv : nullptr) <= 0)
		return 0;
	for (int i = 0; i < count; i++)
		if (socks[i] != INVALID_SOCKET && FD_ISSET(socks[i], &readable))
			found++;
	return found;
}

void NaomiNetwork::waitStop(int64_t usec)
{
	waitReadable(nullptr, 0, usec);
}

void NaomiNetwork::waitForData()
{
	if (isMaster())
		waitReadable(slaves.data(), (int)slaves.size(), -1);
	else
		waitReadable(&client_sock, 1, -1);
}

bool NaomiNetwork::init()
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
	{
		ERROR_LOG(NETWORK, "WSAStartup failed. errno=%d", get_last_error());
		return false;
	}
#endif
	if (settings.network.ActAsServer)
   {
#ifdef ENABLE_MODEM
        miniupnp.Init();
		miniupnp.AddPortMapping(SERVER_PORT, true);
#endif // ENABLE_MODEM
		return createBeaconSocket() && createServerSocket();
   }
	else
		return true;
}

bool NaomiNetwork::createServerSocket()
{
	if (server_sock != INVALID_SOCKET)
		return true;

	server_sock = createAndBind(IPPROTO_TCP);
	if (server_sock == INVALID_SOCKET)
		return false;

	if (listen(server_sock, 5) < 0)
	{
		ERROR_LOG(NETWORK, "NaomiServer: listen() failed. errno=%d", get_last_error());
		closesocket(server_sock);
		server_sock = INVALID_SOCKET;
		return false;
	}
	return true;
}

bool NaomiNetwork::createBeaconSocket()
{
	if (beacon_sock == INVALID_SOCKET)
		beacon_sock = createAndBind(IPPROTO_UDP);

    return beacon_sock != INVALID_SOCKET;
}

void NaomiNetwork::processBeacon()
{
	// Receive broadcast queries on beacon socket and reply
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	memset(&addr, 0, sizeof(addr));
	char buf[6];
	ssize_t n;
	do {
		memset(buf, '\0', sizeof(buf));
		if ((n = recvfrom(beacon_sock, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addrlen)) == -1)
		{
			if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
				WARN_LOG(NETWORK, "NaomiServer: Error receiving datagram. errno=%d", get_last_error());
		}
		else
		{
			DEBUG_LOG(NETWORK, "NaomiServer: beacon received %ld bytes", n);
			if (n == sizeof(buf) && !strncmp(buf, "flycast", n))
				sendto(beacon_sock, buf, n, 0, (const struct sockaddr *)&addr, addrlen);
		}
	} while (n != -1);
}

bool NaomiNetwork::findServer()
{
    // Automatically find the adhoc server on the local network using broadcast
	sock_t sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd == INVALID_SOCKET)
    {
        ERROR_LOG(NETWORK, "Datagram socket creation error. errno=%d", get_last_error());
        return false;
    }

    // Allow broadcast packets to be sent
    int broadcast = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) == -1)
    {
        ERROR_LOG(NETWORK, "setsockopt(SO_BROADCAST) failed. errno=%d", get_last_error());
        closesocket(sockfd);
        return false;
    }

    set_non_blocking(sockfd);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;          // host byte order
    addr.sin_port = htons(SERVER_PORT); // short, network byte order
    addr.sin_addr.s_addr = INADDR_BROADCAST;
    memset(addr.sin_zero, '\0', sizeof(addr.sin_zero));

    struct sockaddr server_addr;

    for (int i = 0; i < 3 && !stopping(); i++)
    {
        if (sendto(sockfd, "flycast", 6, 0, (struct sockaddr *)&addr, sizeof addr) == -1)
        {
            WARN_LOG(NETWORK, "Send datagram failed. errno=%d", get_last_error());
            // Try again in a tenth of a second
            waitStop(100 * 1000);
            continue;
        }

        // The answer, for up to a second
        if (waitReadable(&sockfd, 1, 1000 * 1000) <= 0)
        {
            INFO_LOG(NETWORK, "Recv datagram timeout. i=%d", i);
            continue;
        }

        char buf[6];
        memset(&server_addr, '\0', sizeof(server_addr));
        socklen_t addrlen = sizeof(server_addr);
        if (recvfrom(sockfd, buf, sizeof(buf), 0, &server_addr, &addrlen) == -1)
        {
            WARN_LOG(NETWORK, "Recv datagram failed. errno=%d", get_last_error());
            continue;
        }
        server_ip = ((struct sockaddr_in *)&server_addr)->sin_addr;
        char addressBuffer[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &server_ip, addressBuffer, INET_ADDRSTRLEN);
        server_name = addressBuffer;
        break;
    }
    closesocket(sockfd);
    if (server_ip.s_addr == INADDR_NONE)
    {
        WARN_LOG(NETWORK, "Network Error: Can't find ad-hoc server on local network");
#ifndef __LIBRETRO__
        gui_display_notification("No server found", 8000);
#endif
        return false;
    }
    INFO_LOG(NETWORK, "Found ad-hoc server at %s", server_name.c_str());

    return true;
}

bool NaomiNetwork::startNetwork()
{
	if (!init())
		return false;

	slot_id = 0;
	slot_count = 0;
	packet_number = 0;
	slaves.clear();
	got_token = false;

	// Ten seconds
	const retro_time_t timeout = 10 * 1000 * 1000;
	if (settings.network.ActAsServer)
	{
		NOTICE_LOG(NETWORK, "Waiting for slave connections");
		retro_time_t start_time = cpu_features_get_time_usec();
		while (cpu_features_get_time_usec() - start_time < timeout)
		{
			if (stopping())
			{
				for (auto clientSock : slaves)
					if (clientSock != INVALID_SOCKET)
						closesocket(clientSock);
				return false;
			}
			std::string notif = slaves.empty() ? "Waiting for players..."
					: std::to_string(slaves.size()) + " player(s) connected. Waiting...";
#ifndef __LIBRETRO__
			gui_display_notification(notif.c_str(), (int)(timeout / 1000) * 2);
#endif

			processBeacon();

			struct sockaddr_in src_addr;
			socklen_t addr_len = sizeof(src_addr);
			memset(&src_addr, 0, addr_len);
			sock_t clientSock = accept(server_sock, (struct sockaddr *)&src_addr, &addr_len);
			if (clientSock == INVALID_SOCKET)
			{
				if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
					perror("accept");
			}
			else
			{
				NOTICE_LOG(NETWORK, "Slave connection accepted");
				slaves.push_back(clientSock);
				if (slaves.size() == 3)
					break;
			}
			// Until someone connects or asks who is serving, or time is up
			{
				const sock_t waiting[2] = { server_sock, beacon_sock };
				const retro_time_t left = timeout - (cpu_features_get_time_usec() - start_time);
				if (left > 0)
					waitReadable(waiting, 2, left);
			}
		}
		slot_id = 0;
		slot_count = slaves.size() + 1;
		u8 buf[2] = { (u8)slot_count, 0 };
		int slot_num = 1;
		{
			for (int socket : slaves)
			{
				buf[1] = { (u8)slot_num };
				slot_num++;
				::send(socket, (const char *)buf, 2, 0);
				set_non_blocking(socket);
				set_tcp_nodelay(socket);
			}
		}
		NOTICE_LOG(NETWORK, "Master starting: %zd slaves", slaves.size());
#ifndef __LIBRETRO__
		if (slot_count > 1)
			gui_display_notification("Starting game", 2000);
		else
			gui_display_notification("No player connected", 8000);
#endif

		return !slaves.empty();
	}
	else
	{
		if (!settings.network.server.empty())
		{
			struct addrinfo *resultAddr;
			if (getaddrinfo(settings.network.server.c_str(), 0, nullptr, &resultAddr))
				WARN_LOG(NETWORK, "Server %s is unknown", settings.network.server.c_str());
			else
			{
				for (struct addrinfo *ptr = resultAddr; ptr != nullptr; ptr = ptr->ai_next)
					if (ptr->ai_family == AF_INET)
					{
						server_ip = ((sockaddr_in *)ptr->ai_addr)->sin_addr;
						break;
					}
				freeaddrinfo(resultAddr);
			}
		}

		NOTICE_LOG(NETWORK, "Connecting to server");
#ifndef __LIBRETRO__
		gui_display_notification("Connecting to server", 10000);
#endif
		retro_time_t start_time = cpu_features_get_time_usec();
		while (client_sock == INVALID_SOCKET && !stopping()
				&& cpu_features_get_time_usec() - start_time < timeout)
		{
			if (server_ip.s_addr == INADDR_NONE && !findServer())
				continue;

			client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			struct sockaddr_in src_addr;
			src_addr.sin_family = AF_INET;
			src_addr.sin_addr = server_ip;
			src_addr.sin_port = htons(SERVER_PORT);
			if (::connect(client_sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
			{
				ERROR_LOG(NETWORK, "Socket connect failed");
				closesocket(client_sock);
				client_sock = INVALID_SOCKET;
				// Try again in a tenth of a second
				waitStop(100 * 1000);
			}
			else
			{
#ifndef __LIBRETRO__
				gui_display_notification("Waiting for server to start", 10000);
#endif
				/* Wait for the server to start the game, for up to twice
				 * the timeout. */
				u8 buf[2];
				int got = 0;
				retro_time_t wait_start = cpu_features_get_time_usec();
				while (got < 2 && !stopping())
				{
					const retro_time_t left = timeout * 2 - (cpu_features_get_time_usec() - wait_start);
					if (left <= 0 || waitReadable(&client_sock, 1, left) <= 0)
						break;
					ssize_t l = ::recv(client_sock, (char *)buf + got, 2 - got, 0);
					if (l <= 0)
						break;
					got += (int)l;
				}
				if (got < 2)
				{
					ERROR_LOG(NETWORK, "Connection failed: errno=%d", get_last_error());
					closesocket(client_sock);
					client_sock = -1;
#ifndef __LIBRETRO__
					gui_display_notification("Connection failed", 10000);
#endif

					return false;
				}
				slot_count = buf[0];
				slot_id = buf[1];
				got_token = slot_id == 1;
				set_tcp_nodelay(client_sock);
				set_non_blocking(client_sock);
				std::string notif = "Connected as slot " + std::to_string(slot_id);
#ifndef __LIBRETRO__
				gui_display_notification(notif.c_str(), 2000);
#endif

				return true;
			}
		}
		return false;
	}
}

void NaomiNetwork::pipeSlaves()
{
	if (!isMaster() || slot_count < 3)
		return;
	char buf[16384];
	for (auto it = slaves.begin(); it != slaves.end() - 1; it++)
	{
		ssize_t l = ::recv(*it, buf, sizeof(buf), 0);
		if (l > 0)
			::send(*(it + 1), buf, l, 0);
		// TODO handle errors
	}
}

bool NaomiNetwork::receive(u8 *data, u32 size)
{
	sock_t sockfd = INVALID_SOCKET;
	if (isMaster())
		sockfd = slaves.empty() ? INVALID_SOCKET : slaves.back();
	else
		sockfd = client_sock;
	if (sockfd == INVALID_SOCKET)
		return false;

	u16 pktnum;
	ssize_t l = ::recv(sockfd, (char *)&pktnum, sizeof(pktnum), 0);
	if (l <= 0)
	{
		if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
		{
			WARN_LOG(NETWORK, "receiveNetwork: read failed. errno=%d", get_last_error());
			if (isMaster())
			{
				slaves.back() = -1;
				closesocket(sockfd);
				got_token = false;
			}
		}
		return false;
	}
	packet_number = pktnum;

	ssize_t received = 0;
	while (received != size && !stopping())
	{
		l = ::recv(sockfd, (char*)(data + received), size - received, 0);
		if (l <= 0)
		{
			if (l < 0 && (get_last_error() == L_EAGAIN || get_last_error() == L_EWOULDBLOCK))
			{
				// The rest of the packet is on its way
				waitReadable(&sockfd, 1, -1);
				continue;
			}
			{
				WARN_LOG(NETWORK, "receiveNetwork: read failed. errno=%d", get_last_error());
				if (isMaster())
				{
					slaves.back() = -1;
					closesocket(sockfd);
					got_token = false;
				}
				return false;
			}
		}
		else
			received += l;
	}
	DEBUG_LOG(NETWORK, "[%d] Received %d bytes", slot_id, size);
	got_token = true;
	return true;
}

void NaomiNetwork::send(u8 *data, u32 size)
{
	if (!got_token)
		return;

	sock_t sockfd;
	if (isMaster())
		sockfd = slaves.empty() ? INVALID_SOCKET : slaves.front();
	else
		sockfd = client_sock;
	if (sockfd == INVALID_SOCKET)
		return;

	u16 pktnum = packet_number + 1;
	if (::send(sockfd, (const char *)&pktnum, sizeof(pktnum), 0) < 2)
	{
		if (errno != L_EAGAIN && errno != L_EWOULDBLOCK)
		{
			WARN_LOG(NETWORK, "send failed. errno=%d", get_last_error());
			if (isMaster())
			{
				slaves.front() = -1;
				closesocket(sockfd);
			}
		}
		return;
	}
	if (::send(sockfd, (const char *)data, size, 0) < size)
	{
		WARN_LOG(NETWORK, "send failed. errno=%d", get_last_error());
		if (isMaster())
		{
			slaves.front() = -1;
			closesocket(sockfd);
		}
	}
	else
	{
		DEBUG_LOG(NETWORK, "[%d] Sent %d bytes", slot_id, size);
		got_token = false;
		packet_number = pktnum;
	}
}

void NaomiNetwork::shutdown()
{
	retro_atomic_store_release_int(&network_stopping, 1);
	// Ends whatever wait the network thread is in
	if (wake_sock != INVALID_SOCKET)
		::send(wake_sock, "", 1, 0);
}
void NaomiNetwork::closeSockets()
{
	for (auto& clientSock : slaves)
	{
		if (clientSock != INVALID_SOCKET)
			closesocket(clientSock);
		clientSock = INVALID_SOCKET;
	}
	if (client_sock != INVALID_SOCKET)
	{
		closesocket(client_sock);
		client_sock = INVALID_SOCKET;
	}
}

void NaomiNetwork::terminate()
{
	shutdown();
	closeSockets();
#ifdef ENABLE_MODEM
   if (settings.network.ActAsServer)
		miniupnp.Term();
#endif // ENABLE_MODEM
	if (beacon_sock != INVALID_SOCKET)
	{
		closesocket(beacon_sock);
		beacon_sock = INVALID_SOCKET;
	}
	if (server_sock != INVALID_SOCKET)
	{
		closesocket(server_sock);
		server_sock = INVALID_SOCKET;
	}
}
