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
 *  - stop_pico() returns at once: stopping wakes the thread too */
#include <stdio.h>
#include <string.h>
#include <utility>
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
int pico_send_eth_frame(const u8 *, u32) { return 0; }
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

int main(void)
{
	/* Address, control, protocol LCP, Configure-Request id 1, no options */
	static const u8 lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
	retro_time_t t0, answered = 0, stopped;
	long before, switches;
	int got = 0;
	u16 fcs;

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
