/* Test for the modem's network thread. See run.sh.
 *
 * Builds the real core/network/picoppp.cpp with picoTCP and plays the
 * Dreamcast's side of the modem line: it starts the thread, sends one PPP
 * frame (an LCP Configure-Request) a byte at a time, the way the modem
 * does, and reads what the stack answers.
 *
 * The thread no longer ticks every five milliseconds; it waits for work.
 * What is checked:
 *  - the answer comes at once: the bytes written wake the thread
 *  - left alone, the thread hardly runs: a few voluntary context switches
 *    a second for the whole process, where ticking made two hundred
 *  - stop_pico() returns at once: stopping wakes the thread too
 *
 * Run with "bba" it is the broadband adapter instead: Ethernet frames go
 * to the stack and come back through two rings, and the test, as the
 * emulation thread, is the only one that ever hands the adapter a frame. */
#include <stdio.h>
#include <string.h>
#include <utility>
#include <pthread.h>
#include <sys/resource.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#include <retro_timers.h>

#include "types.h"
#include "stdclass.h"
#include "reios/reios.h"
#include "network/picoppp.h"
#include "network/miniupnp.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

/* ---- what the thread needs from the rest of the core ------------------ */

settings_t settings;
ip_meta_t ip_meta;
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}

/* The scheduler: the test is the emulation thread, and calls what is
 * registered itself to let emulated time pass. */
#include "hw/sh4/sh4_sched.h"
static sh4_sched_callback *scheduled;
int sh4_sched_register(int, sh4_sched_callback *cb) { scheduled = cb; return 0; }
void sh4_sched_request(int, int) {}

/* The adapter's receive side. Only the emulation thread may be in here:
 * this is where the real one writes guest memory and raises its
 * interrupt. */
static pthread_t emu_thread;
static int arp_replies, off_thread_calls;

int pico_send_eth_frame(const u8 *frame, u32 size)
{
	if (!pthread_equal(pthread_self(), emu_thread))
		off_thread_calls++;
	/* An ARP reply from 192.168.169.1 */
	if (size >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2
			&& frame[28] == 192 && frame[29] == 168 && frame[30] == 169 && frame[31] == 1)
		arp_replies++;
	return 1;
}
bool MiniUPnP::Init() { return false; }
void MiniUPnP::Term() {}
bool MiniUPnP::AddPortMapping(int, bool) { return false; }

typedef std::pair<ThreadEntryFP *, void *> thread_start;

static void thread_trampoline(void *p)
{
	thread_start *start = (thread_start *)p;
	start->first(start->second);
	delete start;
}

cThread::cThread(ThreadEntryFP *function, void *prm) : Entry(function), param(prm), hThread(NULL) {}
void cThread::Start() { hThread = sthread_create(thread_trampoline, new thread_start(Entry, param)); }
void cThread::WaitToEnd()
{
	if (hThread)
		sthread_join(hThread);
	hThread = NULL;
}

/* ---- the Dreamcast's end of the line ----------------------------------- */

/* PPP's frame check sequence (RFC 1662). */
static u16 fcs16(const u8 *p, int len)
{
	u16 fcs = 0xffff;

	while (len--)
	{
		fcs ^= *p++;
		for (int i = 0; i < 8; i++)
			fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
	}
	return (u16)~fcs;
}

static void send_escaped(u8 b)
{
	if (b < 0x20 || b == 0x7d || b == 0x7e)
	{
		write_pico(0x7d);
		write_pico(b ^ 0x20);
	}
	else
		write_pico(b);
}

static long voluntary_switches(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_nvcsw;
}

/* The broadband adapter instead of the modem: the Dreamcast asks, over
 * Ethernet, who has the gateway's address, and the stack answers. */
static int run_bba(void)
{
	static const u8 arp[60] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  0x08, 0x06,
		0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x01,  192, 168, 169, 2,
		0, 0, 0, 0, 0, 0,                    192, 168, 169, 1,
	};
	retro_time_t t0, stopped;
	int sent = 0;

	emu_thread = pthread_self();
	settings.network.dns = "8.8.8.8";
	settings.network.EmulateBBA = true;

	CHECK(start_pico());
	CHECK(scheduled != NULL);
	retro_sleep(500);

	/* One question, answered at once... */
	t0 = cpu_features_get_time_usec();
	pico_receive_eth_frame(arp, sizeof(arp));
	while (!arp_replies && cpu_features_get_time_usec() - t0 < 2 * 1000 * 1000)
		if (scheduled)
			scheduled(0, 0, 0);
	CHECK(arp_replies == 1);
	CHECK(cpu_features_get_time_usec() - t0 < 200 * 1000);

	/* ...then the same question as fast as it will go for two and a half
	 * seconds, with the adapter taking its frames all the while. picoTCP
	 * answers one ARP request a second, so what comes back is few; what
	 * this is for is the two threads working the rings flat out. */
	t0 = cpu_features_get_time_usec();
	while (cpu_features_get_time_usec() - t0 < 2500 * 1000)
	{
		pico_receive_eth_frame(arp, sizeof(arp));
		sent++;
		if (scheduled)
			scheduled(0, 0, 0);
	}
	CHECK(arp_replies >= 2);
	/* Every frame reached the adapter on the emulation thread. */
	CHECK(off_thread_calls == 0);

	t0 = cpu_features_get_time_usec();
	stop_pico();
	stopped = cpu_features_get_time_usec() - t0;
	CHECK(stopped < 300 * 1000);

	printf("%s broadband adapter (%d ARP replies to %d requests)\n",
			failures ? "FAIL" : "ok  ", arp_replies, sent + 1);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	/* Address, control, protocol LCP, Configure-Request id 1, no options */
	static const u8 lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
	retro_time_t t0, answered = 0, stopped;
	long before, switches;
	int got = 0;
	u16 fcs;

	if (argc > 1 && !strcmp(argv[1], "bba"))
		return run_bba();

	settings.network.dns = "8.8.8.8";

	CHECK(start_pico());
	retro_sleep(500);	/* let it set the stack and the PPP device up */

	/* Left alone it waits; it does not tick. */
	before = voluntary_switches();
	retro_sleep(3000);
	switches = voluntary_switches() - before;
	CHECK(switches < 60);

	/* A frame from the Dreamcast is answered at once. */
	fcs = fcs16(lcp, sizeof(lcp));
	t0 = cpu_features_get_time_usec();
	write_pico(0x7e);
	for (unsigned i = 0; i < sizeof(lcp); i++)
		send_escaped(lcp[i]);
	send_escaped(fcs & 0xff);
	send_escaped(fcs >> 8);
	write_pico(0x7e);
	while (cpu_features_get_time_usec() - t0 < 2 * 1000 * 1000)
	{
		if (read_pico() >= 0)
		{
			if (!got)
				answered = cpu_features_get_time_usec() - t0;
			got++;
		}
		else if (got)
			break;
	}
	CHECK(got > 0);
	CHECK(answered < 200 * 1000);

	t0 = cpu_features_get_time_usec();
	stop_pico();
	stopped = cpu_features_get_time_usec() - t0;
	CHECK(stopped < 300 * 1000);

	printf("%s modem thread (%ld switches in 3 s idle, answer in %ld us, stop in %ld us)\n",
			failures ? "FAIL" : "ok  ", switches, (long)answered, (long)stopped);
	return failures ? 1 : 0;
}
