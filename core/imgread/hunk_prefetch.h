#pragma once
/* Read-ahead for hunk-compressed disc images.
 *
 * A worker thread decodes the hunk the reader is expected to want next,
 * so a sequential read finds it already decoded. Decoding is deterministic,
 * so the reader gets the same bytes whichever thread produced them; only
 * the CPU time moves.
 *
 * The two threads share three hunk buffers through a cTripleBuffer and
 * never wait for one another: the reader either finds the hunk it asked
 * for or decodes it itself, and the worker sleeps on an eventcount until
 * the reader aims it at a new hunk. No lock is taken anywhere. */
#include "types.h"
#include "lockfree.h"

struct HunkPrefetch
{
	/* Decode @hunk into @dst on the worker thread. false: could not. */
	typedef bool (*ReadFn)(void *opaque, u32 hunk, u8 *dst);

	static const u32 NO_HUNK = 0xFFFFFFFFu;

	HunkPrefetch() : thread(NULL)
	{
		buf[0].mem = buf[1].mem = buf[2].mem = NULL;
	}
	~HunkPrefetch() { Stop(); }

	bool Running() const { return thread != NULL; }

	bool Start(ReadFn read_fn, void *read_opaque, u32 hunkbytes, u32 hunk_count)
	{
		if (thread != NULL)
			return false;
		for (int i = 0; i < 3; i++)
		{
			buf[i].mem  = new u8[hunkbytes];
			buf[i].hunk = NO_HUNK;
		}
		tri.Reset();
		read        = read_fn;
		opaque      = read_opaque;
		total_hunks = hunk_count;
		retro_atomic_int_init(&request, (int)NO_HUNK);
		retro_atomic_int_init(&ready, (int)NO_HUNK);
		retro_atomic_int_init(&stop, 0);
		thread = sthread_create(worker_entry, this);
		if (thread == NULL)
			free_buffers();
		return thread != NULL;
	}

	void Stop()
	{
		if (thread == NULL)
			return;
		retro_atomic_store_release_int(&stop, 1);
		ec.Notify();
		sthread_join(thread);
		thread = NULL;
		free_buffers();
	}

	/* Reader: the buffer the reader owns right now. Fetch() may change
	 * which one that is. */
	u8* Buffer() { return buf[tri.Front()].mem; }

	/* Reader: true when Buffer() now holds @hunk, decoded by the worker.
	 * false when it does not, and what Buffer() holds is then unknown;
	 * the reader decodes into it itself. Either way the worker is aimed
	 * at the hunk after @hunk. */
	bool Fetch(u32 hunk)
	{
		bool hit = false;

		/* ready only says a take is worth trying; the buffer's own tag,
		 * read once the buffer is ours, is what counts. */
		if ((u32)retro_atomic_load_acquire_int(&ready) == hunk && tri.Take())
			hit = buf[tri.Front()].hunk == hunk;

		retro_atomic_store_release_int(&request, (int)(hunk + 1));
		ec.Notify();
		return hit;
	}

private:
	struct
	{
		u8 *mem;
		u32 hunk;	/* written by the worker before it publishes the buffer */
	} buf[3];
	cTripleBuffer      tri;
	retro_atomic_int_t request;	/* hunk the reader wants decoded ahead */
	retro_atomic_int_t ready;	/* hunk the worker published last */
	retro_atomic_int_t stop;
	cEventCount        ec;
	sthread_t         *thread;
	ReadFn             read;
	void              *opaque;
	u32                total_hunks;

	void free_buffers()
	{
		for (int i = 0; i < 3; i++)
		{
			delete [] buf[i].mem;
			buf[i].mem = NULL;
		}
	}

	static void worker_entry(void *p) { ((HunkPrefetch *)p)->worker(); }

	void worker()
	{
		/* The last request answered, decoded or not: it is not tried
		 * again until the reader asks for something else. */
		u32 done = NO_HUNK;

		for (;;)
		{
			u32 req;

			for (;;)
			{
				int key;

				if (retro_atomic_load_acquire_int(&stop))
					return;
				req = (u32)retro_atomic_load_acquire_int(&request);
				if (req != NO_HUNK && req != done)
					break;

				key = ec.Prepare();
				req = (u32)retro_atomic_load_acquire_int(&request);
				if (retro_atomic_load_acquire_int(&stop)
						|| (req != NO_HUNK && req != done))
					ec.Cancel();
				else
					ec.Commit(key);
			}

			if (req < total_hunks && read(opaque, req, buf[tri.Back()].mem))
			{
				buf[tri.Back()].hunk = req;
				retro_atomic_store_release_int(&ready, (int)req);
				tri.Publish();
			}
			done = req;
		}
	}
};
