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

/* A request carries three words (the loader's are a texture's three
 * names), written by the producer before it counts the request and read
 * by the consumer after it has read the count. What the consumer made of
 * them is only looked at once the count is back to zero - and has then
 * to be what the node's last request asked for, whole. */
struct mpsc_node
{
	retro_atomic_int_t pending;	/* requests not answered yet */
	mpsc_node *next;
	long handled;			/* plain: consumer only */
	retro_atomic_int_t want[3];	/* the request */
	int got[3];			/* plain: the consumer's, until pending is zero */
	int last;			/* plain: producer only */
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
		const int want = (int)(rng_next(&seed) | 1u);

		n->last = want;
		retro_atomic_store_release_int(&n->want[0], want);
		retro_atomic_store_release_int(&n->want[1], want ^ 0x5a5a5a5a);
		retro_atomic_store_release_int(&n->want[2], want + 12345);
		/* Queue the node only when it is not queued already. */
		if (retro_atomic_fetch_add_int(&n->pending, 1) == 0)
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
				int requests = retro_atomic_load_acquire_int(&n->pending);
				n->got[0] = retro_atomic_load_acquire_int(&n->want[0]);
				n->got[1] = retro_atomic_load_acquire_int(&n->want[1]);
				n->got[2] = retro_atomic_load_acquire_int(&n->want[2]);
				n->handled += requests;
				if (retro_atomic_fetch_sub_int(&n->pending, requests) == requests)
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
			mpsc_node *n = &mpsc_nodes[i][j];

			CHECK(retro_atomic_load_acquire_int(&n->pending) == 0);
			handled += n->handled;
			/* What was made of the last request is that request's, and all of it. */
			if (n->handled)
			{
				CHECK(n->got[0] == n->last);
				CHECK(n->got[1] == (n->last ^ 0x5a5a5a5a));
				CHECK(n->got[2] == n->last + 12345);
			}
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

#define BATON_FRAMES 30000

static EmuBaton baton;
static long machine;		/* plain: whoever has the machine */
static long video_memory;	/* plain: the guest's, read by the renderer before it acks */
static long frames_made, renders_made, renders_seen;

struct fake_render { long snapshot; };

/* Stands in for the emulation thread: each frame is some emulation, up to
 * three renders handed over, then more emulation. */
static void baton_emu_thread(void *)
{
	static fake_render render;
	unsigned seed = 7;

	while (baton.WaitForFrame())
	{
		int renders = rng_next(&seed) % 4;

		machine++;
		for (int i = 0; i < renders; i++)
		{
			video_memory++;
			baton.WaitWorkDone();
			render.snapshot = video_memory;
			renders_made++;
			baton.HandOver(&render);
			/* The renderer has read video memory; writing it again is
			 * safe while it draws. */
			video_memory++;
			machine++;
		}
		frames_made++;
		baton.EndFrame();
	}
	machine++;	/* shutting down: still the thread's machine */
}

/* Stands in for a background save state. */
static retro_atomic_int_t baton_saver_stop;
static long baton_saves;

static void baton_saver(void *)
{
	while (!retro_atomic_load_acquire_int(&baton_saver_stop))
	{
		baton.Hold();
		machine++;
		baton_saves++;
		baton.Release();
	}
}

static void test_emu_baton(void)
{
	sthread_t *emu, *saver;
	void *w;

	/* No frame being made: taking the machine never waits. */
	baton.Hold();
	machine++;
	baton.Release();

	emu = sthread_create(baton_emu_thread, NULL);
	retro_atomic_int_init(&baton_saver_stop, 0);
	saver = sthread_create(baton_saver, NULL);

	/* retro_run, over and over. */
	for (int frame = 0; frame < BATON_FRAMES; frame++)
	{
		baton.Hold();
		machine++;
		baton.StartFrame();
		while ((w = baton.WaitWork()) != NULL)
		{
			fake_render *r = (fake_render *)w;
			/* What the emulation thread wrote before handing over is
			 * what is there until the ack. */
			CHECK(r->snapshot == video_memory);
			baton.AckWork();
			renders_seen++;
			baton.WorkDone();
		}
		/* The frame is over: the machine is this thread's again. */
		machine++;
		CHECK(frames_made == frame + 1);
		baton.Release();
	}

	retro_atomic_store_release_int(&baton_saver_stop, 1);
	sthread_join(saver);

	/* Unload. */
	baton.Hold();
	baton.TellThreadToExit();
	sthread_join(emu);
	baton.ThreadGone();

	baton.Hold();
	baton.Release();
	/* Every render handed over was drawn, none twice. */
	CHECK(renders_seen == renders_made);
	CHECK(baton_saves > 0);
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
