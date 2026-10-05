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
#include "libretro/input_latch.h"
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

/* ---- cMpscList, used the way the custom texture loader uses it -------- */

#define MPSC_PRODUCERS 3
#define MPSC_NODES     4
#define MPSC_REQUESTS  100000

struct mpsc_node
{
	std::atomic_int pending;	/* requests not answered yet */
	mpsc_node *next;
	long handled;			/* plain: consumer only */
};

static cMpscList<mpsc_node, &mpsc_node::next> mpsc_list;
static cResetEvent mpsc_wake;
static mpsc_node mpsc_nodes[MPSC_PRODUCERS][MPSC_NODES];
static retro_atomic_int_t mpsc_stop;

static void mpsc_producer(void *p)
{
	mpsc_node *nodes = (mpsc_node *)p;
	unsigned seed = (unsigned)(size_t)p;

	for (int i = 0; i < MPSC_REQUESTS; i++)
	{
		mpsc_node *n = &nodes[rng_next(&seed) % MPSC_NODES];
		/* Queue the node only when it is not queued already. */
		if (n->pending++ == 0)
		{
			mpsc_list.Push(n);
			mpsc_wake.Set();
		}
	}
}

static void mpsc_consumer(void *)
{
	for (;;)
	{
		int stop = retro_atomic_load_acquire_int(&mpsc_stop);
		mpsc_node *n = mpsc_list.TakeAll();

		if (n == NULL && stop)
			break;
		while (n != NULL)
		{
			mpsc_node *next = n->next;
			for (;;)
			{
				int requests = n->pending;
				n->handled += requests;
				if (n->pending.fetch_sub(requests) == requests)
					break;
			}
			n = next;
		}
		mpsc_wake.Wait(1);
	}
}

static void test_mpsc_list(void)
{
	sthread_t *prod[MPSC_PRODUCERS], *cons;
	long handled = 0;

	retro_atomic_int_init(&mpsc_stop, 0);
	cons = sthread_create(mpsc_consumer, NULL);
	for (int i = 0; i < MPSC_PRODUCERS; i++)
		prod[i] = sthread_create(mpsc_producer, mpsc_nodes[i]);
	for (int i = 0; i < MPSC_PRODUCERS; i++)
		sthread_join(prod[i]);
	retro_atomic_store_release_int(&mpsc_stop, 1);
	mpsc_wake.Set();
	sthread_join(cons);

	for (int i = 0; i < MPSC_PRODUCERS; i++)
		for (int j = 0; j < MPSC_NODES; j++)
		{
			CHECK(mpsc_nodes[i][j].pending == 0);
			handled += mpsc_nodes[i][j].handled;
		}
	/* Every request answered exactly once. */
	CHECK(handled == (long)MPSC_PRODUCERS * MPSC_REQUESTS);
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

/* ---- InputLatch ------------------------------------------------------ */

#define LATCH_FRAMES 200000

static InputLatch latch;
static retro_atomic_int_t latch_done;

/* One frame's sample: every field of every port follows from @frame. */
static void latch_writer(void *)
{
	for (u32 frame = 1; frame <= LATCH_FRAMES; frame++)
	{
		InputLatch::State& next = latch.Next();
		for (u32 p = 0; p < 4; p++)
		{
			InputLatch::Port& s = next.port[p];
			s.kcode      = frame;
			s.joyx       = (s8)(frame + p);
			s.joyy       = (s8)(frame * 3);
			s.joyrx      = (s8)(frame * 5);
			s.joyry      = (s8)(frame * 7);
			s.rt         = (u8)(frame * 11);
			s.lt         = (u8)(frame * 13);
			s.mo_buttons = ~frame;
			s.mo_x       = frame * 2;	/* moved 2 right, 3 up a frame */
			s.mo_y       = (u32)0 - frame * 3;
			s.mo_wheel   = frame / 16;
		}
		latch.Publish();
	}
	retro_atomic_store_release_int(&latch_done, 1);
}

static void test_input_latch(void)
{
	sthread_t *t;
	u32 last = 0, buttons;
	f32 dx, dy, dw;
	double x = 0, y = 0, w = 0;

	/* Before anything is sampled: nothing held, nothing moved. */
	CHECK(latch.Read(2).kcode == 0xFFFFFFFF);
	latch.ReadMouse(2, &buttons, &dx, &dy, &dw);
	CHECK(buttons == 0xFFFFFFFF && dx == 0 && dy == 0 && dw == 0);

	retro_atomic_int_init(&latch_done, 0);
	t = sthread_create(latch_writer, NULL);
	for (;;)
	{
		int done = retro_atomic_load_acquire_int(&latch_done);
		const InputLatch::Port& a = latch.Read(0);
		u32 frame = a.kcode;

		/* One whole frame, on every port, never an older one. */
		CHECK(frame >= last);
		last = frame;
		if (frame != 0xFFFFFFFF)
			for (u32 p = 0; p < 4; p++)
			{
				const InputLatch::Port& s = latch.Read(p);
				if (s.kcode != frame)
				{
					/* A newer frame arrived between two ports; each
					 * port is still whole. */
					frame = s.kcode;
					last  = frame;
				}
				CHECK(s.joyx == (s8)(frame + p) && s.joyy == (s8)(frame * 3));
				CHECK(s.joyrx == (s8)(frame * 5) && s.joyry == (s8)(frame * 7));
				CHECK(s.rt == (u8)(frame * 11) && s.lt == (u8)(frame * 13));
				CHECK(s.mo_buttons == ~frame);
			}
		else
			last = 0;

		latch.ReadMouse(1, &buttons, &dx, &dy, &dw);
		x += dx;
		y += dy;
		w += dw;
		if (done)
			break;
	}
	sthread_join(t);
	latch.ReadMouse(1, &buttons, &dx, &dy, &dw);
	x += dx;
	y += dy;
	w += dw;
	/* However the polls fell against the frames, all of the motion
	 * arrived and none of it twice. */
	CHECK(x == 2.0 * LATCH_FRAMES);
	CHECK(y == -3.0 * LATCH_FRAMES);
	CHECK(w == LATCH_FRAMES / 16);
}

/* ---- cMarkSet -------------------------------------------------------- */

#define MARK_NUMBERS 4096
#define MARK_ROUNDS  400000

static cMarkSet<MARK_NUMBERS> marks;
static retro_atomic_int_t mark_stamp[MARK_NUMBERS];	/* last round to mark it */
static int mark_seen[MARK_NUMBERS];			/* collector only */
static retro_atomic_int_t mark_done;

/* Stamps a number with the round, then marks it: the order the core's
 * fault handler works in, state first and the mark after. */
static void mark_thread(void *)
{
	unsigned seed = 99;

	for (int round = 1; round <= MARK_ROUNDS; round++)
	{
		unsigned n = rng_next(&seed) % MARK_NUMBERS;
		retro_atomic_store_release_int(&mark_stamp[n], round);
		marks.Mark(n);
	}
	retro_atomic_store_release_int(&mark_done, 1);
}

static void mark_collect(void)
{
	marks.Collect([](unsigned n) {
		mark_seen[n] = retro_atomic_load_acquire_int(&mark_stamp[n]);
	});
}

static void test_mark_set(void)
{
	sthread_t *t;
	int lost = 0;

	CHECK(!marks.Pending());
	retro_atomic_int_init(&mark_done, 0);
	t = sthread_create(mark_thread, NULL);
	while (!retro_atomic_load_acquire_int(&mark_done))
		if (marks.Pending())
			mark_collect();
	sthread_join(t);
	mark_collect();
	CHECK(!marks.Pending());

	/* Every mark was collected after it was made: what the collector
	 * saw last for a number is the last round that marked it. */
	for (int n = 0; n < MARK_NUMBERS; n++)
		if (mark_seen[n] != retro_atomic_load_acquire_int(&mark_stamp[n]))
			lost++;
	CHECK(lost == 0);
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	static const struct { const char *name; void (*fn)(void); } tests[] = {
		{ "cResetEvent",   test_reset_event },
		{ "cSlotCache",    test_slot_cache },
		{ "cTripleBuffer", test_triple_buffer },
		{ "cMpscList",     test_mpsc_list },
		{ "HunkPrefetch",  test_hunk_prefetch },
		{ "EmuBaton",      test_emu_baton },
		{ "InputLatch",    test_input_latch },
		{ "cMarkSet",      test_mark_set },
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
