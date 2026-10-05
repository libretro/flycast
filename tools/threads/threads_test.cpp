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

#include <retro_timers.h>

#include "types.h"
#include "stdclass.h"
#include "lockfree.h"
#include "libretro/emu_baton.h"
#include "imgread/hunk_prefetch.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

static unsigned rng_next(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

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

/* ---- cSlotCache ------------------------------------------------------ */

#define CACHE_OBJECTS 8
#define CACHE_ROUNDS  300000

struct cache_obj { long uses; };	/* plain: owned by whoever took it */

static cSlotCache<cache_obj, 3> cache;
static retro_atomic_int_t cache_live;

static void cache_thread(void *)
{
	cache_obj *mine = NULL;

	for (int i = 0; i < CACHE_ROUNDS; i++)
	{
		if (mine == NULL)
			mine = cache.Take();
		if (mine == NULL)
		{
			mine = new cache_obj();
			mine->uses = 0;
			retro_atomic_fetch_add_int(&cache_live, 1);
		}
		mine->uses++;
		if (cache.Put(mine))
			mine = NULL;
		else if (i & 1)
		{
			delete mine;
			retro_atomic_fetch_sub_int(&cache_live, 1);
			mine = NULL;
		}
	}
	if (mine != NULL)
	{
		delete mine;
		retro_atomic_fetch_sub_int(&cache_live, 1);
	}
}

static void test_slot_cache(void)
{
	sthread_t *t[3];
	cache_obj *o;
	int left = 0;

	retro_atomic_int_init(&cache_live, 0);
	for (int i = 0; i < 3; i++)
		t[i] = sthread_create(cache_thread, NULL);
	for (int i = 0; i < 3; i++)
		sthread_join(t[i]);
	while ((o = cache.Take()) != NULL)
	{
		delete o;
		left++;
	}
	/* Nothing handed out twice (that would be a double delete, and a race
	 * on uses) and nothing lost. */
	CHECK(left <= 3);
	CHECK(retro_atomic_load_acquire_int(&cache_live) == left);
}

/* ---- cTripleBuffer --------------------------------------------------- */

#define TRIPLE_ROUNDS 200000
#define TRIPLE_WORDS  64

static cTripleBuffer triple;
static unsigned triple_buf[3][TRIPLE_WORDS];
static retro_atomic_int_t triple_done;

static void triple_writer(void *)
{
	for (unsigned v = 1; v <= TRIPLE_ROUNDS; v++)
	{
		unsigned *b = triple_buf[triple.Back()];
		for (int i = 0; i < TRIPLE_WORDS; i++)
			b[i] = v;
		triple.Publish();
	}
	retro_atomic_store_release_int(&triple_done, 1);
}

static void test_triple_buffer(void)
{
	sthread_t *t;
	unsigned last = 0;
	long takes = 0;

	memset(triple_buf, 0, sizeof(triple_buf));
	retro_atomic_int_init(&triple_done, 0);
	CHECK(!triple.Take());
	t = sthread_create(triple_writer, NULL);
	for (;;)
	{
		int done = retro_atomic_load_acquire_int(&triple_done);
		if (triple.Take())
		{
			const unsigned *b = triple_buf[triple.Front()];
			/* A whole buffer from one Publish, never older than the last. */
			for (int i = 1; i < TRIPLE_WORDS; i++)
				CHECK(b[i] == b[0]);
			CHECK(b[0] > last);
			last = b[0];
			takes++;
		}
		else if (done)
			break;
	}
	sthread_join(t);
	CHECK(last == TRIPLE_ROUNDS);
	CHECK(takes > 0);
}

/* ---- HunkPrefetch ---------------------------------------------------- */

#define HUNK_BYTES 4096
#define HUNK_COUNT 512
#define HUNK_READS 60000

static bool hunk_is_bad(u32 hunk) { return hunk % 97 == 96; }

static bool hunk_decode(void *, u32 hunk, u8 *dst)
{
	if (hunk_is_bad(hunk))
		return false;
	for (u32 i = 0; i < HUNK_BYTES; i++)
		dst[i] = (u8)(hunk * 31 + i);
	return true;
}

static bool hunk_matches(u32 hunk, const u8 *p)
{
	for (u32 i = 0; i < HUNK_BYTES; i++)
		if (p[i] != (u8)(hunk * 31 + i))
			return false;
	return true;
}

static void test_hunk_prefetch(void)
{
	HunkPrefetch pf;
	unsigned seed = 12345;
	u32 hunk = 0;
	long hits = 0;

	CHECK(pf.Start(hunk_decode, NULL, HUNK_BYTES, HUNK_COUNT));
	CHECK(!pf.Start(hunk_decode, NULL, HUNK_BYTES, HUNK_COUNT));
	for (int i = 0; i < HUNK_READS; i++)
	{
		unsigned r = rng_next(&seed);

		/* Mostly sequential, as a disc is read, with a seek now and then. */
		if (r % 50 == 0)
			hunk = rng_next(&seed) % HUNK_COUNT;
		else
			hunk = (hunk + 1) % HUNK_COUNT;

		if (pf.Fetch(hunk))
		{
			/* The worker never hands over a hunk it could not decode. */
			CHECK(!hunk_is_bad(hunk));
			CHECK(hunk_matches(hunk, pf.Buffer()));
			hits++;
		}
		else if (hunk_decode(NULL, hunk, pf.Buffer()))
			CHECK(hunk_matches(hunk, pf.Buffer()));

		/* Give the worker a moment now and then, as emulation between
		 * two disc reads does. */
		if (i % 64 == 0)
			retro_sleep(1);
	}
	pf.Stop();
	CHECK(!pf.Running());
	CHECK(hits > 0);

	/* Starts again after a stop. */
	CHECK(pf.Start(hunk_decode, NULL, HUNK_BYTES, HUNK_COUNT));
	pf.Stop();
	printf("  hunk prefetch: %ld of %d reads found decoded\n", hits, HUNK_READS);
}

/* ---- EmuBaton -------------------------------------------------------- */

#define BATON_HOLDS 20000

static EmuBaton baton;
static long machine;			/* plain: whoever has the machine */
static retro_atomic_int_t cpu_running;	/* stands in for Sh4cntx.CpuRunning */
static retro_atomic_int_t baton_passes;

/* Stands in for dc_run(): sets its own run flag on entry, which is what
 * loses a stop that arrives just before it. */
static void fake_run(void)
{
	retro_atomic_store_relaxed_int(&cpu_running, 1);
	while (retro_atomic_load_relaxed_int(&cpu_running))
		machine++;
}

static void fake_stop(void)  { retro_atomic_store_relaxed_int(&cpu_running, 0); }
static void fake_start(void) { retro_atomic_store_relaxed_int(&cpu_running, 1); }

static void baton_emu_thread(void *)
{
	while (baton.WaitToRun())
	{
		retro_atomic_fetch_add_int(&baton_passes, 1);
		fake_run();
	}
	machine++;	/* shutting down: still the thread's machine */
}

static void baton_holder(void *)
{
	for (int i = 0; i < BATON_HOLDS; i++)
	{
		if (baton.Hold(5000, fake_stop, fake_start))
		{
			machine++;
			baton.Release();
		}
		else
			CHECK(!"Hold timed out");
	}
}

static void test_emu_baton(void)
{
	sthread_t *emu, *saver;

	retro_atomic_int_init(&baton_passes, 0);

	/* No emulation thread: taking the machine never waits. */
	CHECK(baton.Hold(1, fake_stop, fake_start));
	machine++;
	baton.Release();

	/* Start the emulation thread the way retro_run does. */
	CHECK(baton.Hold(0, fake_stop, fake_start));
	baton.HandToThread();
	emu = sthread_create(baton_emu_thread, NULL);

	/* Two threads take the machine over and over: the libretro thread and
	 * a background save state. */
	saver = sthread_create(baton_holder, NULL);
	baton_holder(NULL);
	sthread_join(saver);

	/* Unload. */
	CHECK(baton.Hold(0, fake_stop, fake_start));
	baton.TellThreadToExit();
	sthread_join(emu);
	baton.ThreadGone();

	/* And the machine can be taken again afterwards. */
	CHECK(baton.Hold(1, fake_stop, fake_start));
	baton.Release();
	CHECK(retro_atomic_load_acquire_int(&baton_passes) > 0);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	static const struct { const char *name; void (*fn)(void); } tests[] = {
		{ "cResetEvent",   test_reset_event },
		{ "cSlotCache",    test_slot_cache },
		{ "cTripleBuffer", test_triple_buffer },
		{ "HunkPrefetch",  test_hunk_prefetch },
		{ "EmuBaton",      test_emu_baton },
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
