/* The network thread (core/network/picoppp.cpp + dcnet.cpp) under
 * ThreadSanitizer: picoTCP for the modem (an LCP request answered) and
 * the adapter (ARP), and DCNet against a stand-in relay on the loopback
 * interface (the name lookup of dcnet.flyca.st answered with 127.0.0.1). */
#include <stdio.h>
#include <string.h>
#include <utility>
#include <atomic>
#include <vector>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>

#include "types.h"
#include "stdclass.h"
#include "reios/reios.h"
#include "network/picoppp.h"
#include "network/netservice.h"
#include "network/miniupnp.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

settings_t settings;
ip_meta_t ip_meta;
u8 net_device = NET_DEVICE_MODEM;
u8 net_device_option = NET_DEVICE_MODEM;
bool net_dcnet_option = false;
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *fmt, ...) {}
bool MiniUPnP::Init() { return false; }
void MiniUPnP::Term() {}
bool MiniUPnP::AddPortMapping(int, bool) { return false; }

typedef std::pair<ThreadEntryFP *, void *> thread_start;
static void thread_trampoline(void *p) { thread_start *s = (thread_start *)p; s->first(s->second); delete s; }
cThread::cThread(ThreadEntryFP *function, void *prm) : Entry(function), param(prm), hThread(NULL) {}
void cThread::Start() { hThread = sthread_create(thread_trampoline, new thread_start(Entry, param)); }
void cThread::WaitToEnd() { if (hThread) sthread_join(hThread); hThread = NULL; }

// dcnet.flyca.st and dns.flyca.st are this machine
extern "C" int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	static struct sockaddr_in sa;
	static struct addrinfo ai;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_INET;
	ai.ai_addr = (struct sockaddr *)&sa;
	ai.ai_addrlen = sizeof(sa);
	*res = &ai;
	return 0;
}
extern "C" void freeaddrinfo(struct addrinfo *) {}

static u16 fcs16(const u8 *p, int len)
{
	u16 fcs = 0xffff;
	while (len--) { fcs ^= *p++; for (int i = 0; i < 8; i++) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1; }
	return (u16)~fcs;
}
static void send_escaped(u8 b)
{
	if (b < 0x20 || b == 0x7d || b == 0x7e) { write_pico(0x7d); write_pico(b ^ 0x20); }
	else write_pico(b);
}

static void pico_modem()
{
	static const u8 lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
	settings.network.dns = "8.8.8.8";
	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = false;
	CHECK(start_pico());
	retro_sleep(500);
	// Passive: nothing before the Dreamcast speaks
	CHECK(read_pico() < 0);
	u16 fcs = fcs16(lcp, sizeof(lcp));
	retro_time_t t0 = cpu_features_get_time_usec(), answered = 0;
	int got = 0;
	write_pico(0x7e);
	for (unsigned i = 0; i < sizeof(lcp); i++) send_escaped(lcp[i]);
	send_escaped(fcs & 0xff); send_escaped(fcs >> 8);
	write_pico(0x7e);
	while (cpu_features_get_time_usec() - t0 < 2 * 1000 * 1000)
	{
		if (read_pico() >= 0) { if (!got) answered = cpu_features_get_time_usec() - t0; got++; }
		else if (got) break;
	}
	CHECK(got > 0);
	CHECK(answered < 200 * 1000);
	t0 = cpu_features_get_time_usec();
	stop_pico();
	retro_time_t stopped = cpu_features_get_time_usec() - t0;
	CHECK(stopped < 1000);
	printf("pico modem: %d bytes answered in %ld us, stop %ld us\n", got, (long)answered, (long)stopped);
}

static void pico_bba()
{
	static const u8 arp[60] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  0x08, 0x06,
		0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  192, 168, 169, 2,
		0, 0, 0, 0, 0, 0,                    192, 168, 169, 1,
	};
	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());	// (joins the last one)
	retro_sleep(500);
	int replies = 0;
	u8 frame[NET_ETH_FRAME_MAX];
	retro_time_t t0 = cpu_features_get_time_usec();
	pico_receive_eth_frame(arp, sizeof(arp));
	while (!replies && cpu_features_get_time_usec() - t0 < 2000000)
	{
		u32 n = net_eth_take(frame);
		if (n >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2) replies++;
	}
	CHECK(replies == 1);
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500 * 1000)
	{
		pico_receive_eth_frame(arp, sizeof(arp));
		net_eth_take(frame);
	}
	stop_pico();
	printf("pico bba: ARP answered\n");
}

/* ---- the relay ---- */
static std::atomic<int> relay_stop(0);
static int udp_fd, ppp_fd, tap_fd;
static std::atomic<int> modem_bytes_seen(0), frames_seen(0), prolog_seen(0);

static void serve(int c, bool tap)
{
	std::vector<u8> in;
	while (!relay_stop)
	{
		fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2);
		timeval t2 = { 0, 100000 };
		if (select(c + 1, &r2, NULL, NULL, &t2) <= 0) continue;
		u8 buf[4096];
		int n = recv(c, buf, sizeof(buf), 0);
		if (n <= 0) break;
		if (!tap)
		{
			// PPP: echoed, each byte plus one
			modem_bytes_seen += n;
			for (int i = 0; i < n; i++) buf[i]++;
			send(c, buf, n, MSG_NOSIGNAL);
			continue;
		}
		in.insert(in.end(), buf, buf + n);
		while (in.size() >= 2)
		{
			unsigned len = in[0] | (in[1] << 8);
			if (in.size() < 2 + len) break;
			if (len == 6 && !memcmp(&in[2], "DCNET\1", 6)) prolog_seen++;
			else { frames_seen++; send(c, &in[0], 2 + len, MSG_NOSIGNAL); }	// echoed
			in.erase(in.begin(), in.begin() + 2 + len);
		}
	}
	close(c);
}

static void *relay(void *)
{
	while (!relay_stop)
	{
		fd_set rd; FD_ZERO(&rd); FD_SET(udp_fd, &rd); FD_SET(ppp_fd, &rd); FD_SET(tap_fd, &rd);
		timeval tv = { 0, 100000 };
		if (select(std::max(udp_fd, std::max(ppp_fd, tap_fd)) + 1, &rd, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(udp_fd, &rd))
		{
			u8 buf[64]; sockaddr_in from; socklen_t fl = sizeof(from);
			int n = recvfrom(udp_fd, buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
			if (n == 5 && buf[4] == 3)	// DISCOVER: no access points of its own
				sendto(udp_fd, buf, 5, 0, (sockaddr *)&from, fl);
		}
		if (FD_ISSET(ppp_fd, &rd)) { int c = accept(ppp_fd, NULL, NULL); if (c >= 0) serve(c, false); }
		if (FD_ISSET(tap_fd, &rd)) { int c = accept(tap_fd, NULL, NULL); if (c >= 0) serve(c, true); }
	}
	return NULL;
}

static int listen_on(int type, u16 port)
{
	int fd = socket(AF_INET, type, 0);
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	if (bind(fd, (sockaddr *)&a, sizeof(a)) < 0) return -1;
	if (type == SOCK_STREAM && listen(fd, 4) < 0) return -1;
	return fd;
}

static bool open_relay()
{
	udp_fd = listen_on(SOCK_DGRAM, 7655);
	ppp_fd = listen_on(SOCK_STREAM, 7654);
	tap_fd = listen_on(SOCK_STREAM, 7655);
	return udp_fd >= 0 && ppp_fd >= 0 && tap_fd >= 0;
}

static void dcnet_modem(pthread_t *th)
{
	net_device = NET_DEVICE_MODEM;
	net_dcnet_option = true;
	pthread_create(th, NULL, relay, NULL);
	CHECK(start_pico());
	retro_sleep(300);
	const char msg[] = "~hello dcnet~";
	for (const char *p = msg; *p; p++) write_pico(*p);
	std::vector<u8> got;
	retro_time_t t0 = cpu_features_get_time_usec();
	while (got.size() < strlen(msg) && cpu_features_get_time_usec() - t0 < 3000000)
	{
		int c = read_pico();
		if (c >= 0) got.push_back(c);
	}
	CHECK(got.size() == strlen(msg));
	bool same = got.size() == strlen(msg);
	for (size_t i = 0; same && i < got.size(); i++) same = got[i] == (u8)(msg[i] + 1);
	CHECK(same);
	// a lot both ways: the ring to the modem fills, and the thread waits for the modem
	int sent = 0, recvd = 0;
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 1500000)
	{
		for (int i = 0; i < 64; i++) { write_pico('a' + (sent % 26)); sent++; }
		for (int i = 0; i < 32; i++) if (read_pico() >= 0) recvd++;
	}
	retro_sleep(200);
	while (read_pico() >= 0) recvd++;
	stop_pico();
	printf("dcnet modem: echo ok, %d sent, %d received (relay saw %d)\n", sent, recvd, modem_bytes_seen.load());
	CHECK(recvd > 0);
}

static void dcnet_bba()
{
	net_device = NET_DEVICE_BBA;
	CHECK(start_pico());
	retro_sleep(100);
	u8 fr[100];
	for (int i = 0; i < 100; i++) fr[i] = i;
	pico_receive_eth_frame(fr, sizeof(fr));
	u8 back[NET_ETH_FRAME_MAX];
	int got = 0;
	retro_time_t t0 = cpu_features_get_time_usec();
	while (got < 50 && cpu_features_get_time_usec() - t0 < 3000000)
	{
		if (got < 50 && (cpu_features_get_time_usec() - t0) % 1000 < 50) pico_receive_eth_frame(fr, sizeof(fr));
		u32 n = net_eth_take(back);
		if (n == sizeof(fr) && !memcmp(back, fr, n)) got++;
	}
	CHECK(prolog_seen == 1);
	CHECK(got > 10);
	printf("dcnet bba: prolog %d, %d frames echoed (relay saw %d)\n", prolog_seen.load(), got, frames_seen.load());
	net_thread_join();
}

int main()
{
	pico_modem();
	pico_bba();
	pthread_t th;
	if (!open_relay()) { printf("no relay ports\n"); return 1; }
	dcnet_modem(&th);
	dcnet_bba();
	relay_stop = 1;
	pthread_join(th, NULL);
	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures != 0;
}
