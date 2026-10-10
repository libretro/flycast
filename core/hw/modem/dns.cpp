/*
	Created on: Sep 24, 2018

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
#include "types.h"
#include <stdio.h>
#include <errno.h>

#include "network/net_platform.h"

extern "C" {
#include <pico_stack.h>
#include <pico_ipv4.h>
#include <pico_dns_common.h>
}

void get_host_by_name(const char *name, struct pico_ip4 dnsaddr);
int get_dns_answer(struct pico_ip4 *address, struct pico_ip4 dnsaddr);
void set_non_blocking(sock_t fd);

static sock_t sock_fd = INVALID_SOCKET;
static unsigned short qid = PICO_TIME_MS();
static int qname_len;

void get_host_by_name(const char *host, struct pico_ip4 dnsaddr)
{
	DEBUG_LOG(MODEM, "get_host_by_name: %s", host);
	if (!VALID(sock_fd))
	{
		sock_fd = selectable_socket(socket(AF_INET , SOCK_DGRAM , IPPROTO_UDP));
		set_non_blocking(sock_fd);
	}

	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_port = htons(53);
	dest.sin_addr.s_addr = dnsaddr.addr;

	// DNS Packet header
	char buf[1024];
	pico_dns_packet *dns = (pico_dns_packet *)&buf;

	dns->id = qid++;
	dns->qr = PICO_DNS_QR_QUERY;
	dns->opcode = PICO_DNS_OPCODE_QUERY;
	dns->aa = PICO_DNS_AA_NO_AUTHORITY;
	dns->tc = PICO_DNS_TC_NO_TRUNCATION;
	dns->rd = PICO_DNS_RD_IS_DESIRED;
	dns->ra = PICO_DNS_RA_NO_SUPPORT;
	dns->z = 0;
	dns->rcode = PICO_DNS_RCODE_NO_ERROR;
	dns->qdcount = htons(1);	// One question
	dns->ancount = 0;
	dns->nscount = 0;
	dns->arcount = 0;

	char *qname = &buf[sizeof(pico_dns_packet)];

	strcpy(qname + 1, host);
	pico_dns_name_to_dns_notation(qname, 128);
	qname_len = strlen(qname) + 1;

	struct pico_dns_question_suffix *qinfo = (struct pico_dns_question_suffix *) &buf[sizeof(pico_dns_packet) + qname_len]; //fill it
	qinfo->qtype = htons(PICO_DNS_TYPE_A);		// Address record
	qinfo->qclass = htons(PICO_DNS_CLASS_IN);

	if (sendto(sock_fd, buf, sizeof(pico_dns_packet) + qname_len + sizeof(struct pico_dns_question_suffix), L_MSG_NOSIGNAL, (struct sockaddr *)&dest, sizeof(dest)) < 0)
		perror("DNS sendto failed");
}

sock_t get_dns_socket(void)
{
	return sock_fd;
}

/* The length of the name at @p: labels, each behind its length, up to a zero
 * byte or to a pointer to an earlier name (two bytes). 0 if the name does
 * not end before @end. */
static int dns_name_len(const u8 *p, const u8 *end)
{
	const u8 *start = p;

	while (p < end)
	{
		if ((*p & 0xC0) == 0xC0)
			return end - p >= 2 ? (int)(p - start) + 2 : 0;
		if (*p == 0)
			return (int)(p - start) + 1;
		// A label is 63 bytes at most: a length above that is of a kind
		// not known here
		if (*p & 0xC0)
			return 0;
		if (end - p <= *p)
			return 0;
		p += *p + 1;
	}
	return 0;
}

int get_dns_answer(struct pico_ip4 *address, struct pico_ip4 dnsaddr)
{
	struct sockaddr_in peer;
	socklen_t peer_len = sizeof(peer);
	char buf[1024];

	int r = recvfrom(sock_fd, buf, sizeof(buf), 0, (struct sockaddr*)&peer , &peer_len);
	if (r < 0)
	{
		if (get_last_error() != L_EAGAIN && get_last_error() != L_EWOULDBLOCK)
			perror("DNS recvfrom failed");
		return -1;
	}
	if (peer.sin_addr.s_addr != dnsaddr.addr)
		return -1;

	if (r < (int)sizeof(pico_dns_packet))
		return -1;

	pico_dns_packet *dns = (pico_dns_packet*) buf;

	// Every count, length and name in it is the sender's word: nothing is
	// read past the bytes that were received
	const u8 *end = (const u8 *)buf + r;
	// move to the first answer
	const u8 *reader = (const u8 *)&buf[sizeof(pico_dns_packet) + qname_len + sizeof(struct pico_dns_question_suffix)];

	for (int i = 0; i < ntohs(dns->ancount); i++)
	{
		// FIXME Check name?
		const int name_len = reader < end ? dns_name_len(reader, end) : 0;
		if (name_len == 0 || (size_t)(end - reader) - name_len < sizeof(struct pico_dns_record_suffix))
			return -1;
		reader = reader + name_len;

		struct pico_dns_record_suffix record;
		memcpy(&record, reader, sizeof(record));
		reader = reader + sizeof(record);

		const u32 rdlength = ntohs(record.rdlength);
		if ((size_t)(end - reader) < rdlength)
			return -1;
		if (ntohs(record.rtype) == PICO_DNS_TYPE_A && rdlength >= 4) // Address record
		{
			memcpy(&address->addr, reader, 4);

			return 0;
		}
		reader = reader + rdlength;
	}
	return -1;
}
