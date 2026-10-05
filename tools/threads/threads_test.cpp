/* Stress test for the core's lock-free thread handoffs. See run.sh.
 *
 * Every test shares plain, non-atomic data between threads under the
 * protection of the piece it tests, so ThreadSanitizer reports a race the
 * moment that piece lets two threads in at once, and each test checks for
 * lost or duplicated work itself. A handoff that loses a wake-up hangs;
 * the watchdog turns that into a failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "types.h"
#include "stdclass.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

/* ---- cResetEvent ----------------------------------------------------- */

#define PING_ROUNDS 200000

static cResetEvent ev_go, ev_done;
static long ping_value;		/* plain: owned by whoever was signalled last */

static void ping_thread(void *)
{
	for (int i = 0; i < PING_ROUNDS; i++)
	{
		ev_go.Wait();
		ping_value++;
		ev_done.Set();
	}
}

static void test_reset_event(void)
{
	cResetEvent ev;
	sthread_t *t;

	/* A Set() with nobody waiting is kept for the next Wait(); one that
	 * is Reset() is not, and neither is one already consumed. */
	ev.Set();
	CHECK(ev.Wait(0));
	CHECK(!ev.Wait(0));
	ev.Set();
	ev.Reset();
	CHECK(!ev.Wait(1));
	ev.Set();
	ev.Set();
	ev.Wait();
	CHECK(!ev.Wait(1));

	/* Strict alternation: a lost wake-up hangs, a doubled one races. */
	t = sthread_create(ping_thread, NULL);
	for (int i = 0; i < PING_ROUNDS; i++)
	{
		ping_value++;
		ev_go.Set();
		ev_done.Wait();
	}
	sthread_join(t);
	CHECK(ping_value == 2L * PING_ROUNDS);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	static const struct { const char *name; void (*fn)(void); } tests[] = {
		{ "cResetEvent",   test_reset_event },
	};

	for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
	{
		int before = failures;
#ifndef _WIN32
		alarm(300);	/* a lost wake-up shows up as a hang */
#endif
		tests[i].fn();
		printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
	}
	return failures ? 1 : 0;
}
