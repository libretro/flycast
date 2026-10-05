#pragma once
/* Lock-free pieces the core's threads hand work across with. None of them
 * takes a lock or makes a system call; a thread that has to sleep until
 * the other side moves does it on a retro_eventcount (see cResetEvent in
 * stdclass.h). tools/threads/run.sh exercises all of them under
 * ThreadSanitizer. */
#include <stddef.h>
#include <retro_atomic.h>

/* A few spare objects kept for reuse. Any thread may Put and any thread
 * may Take; each object is in at most one slot and a Take hands it to
 * exactly one caller. */
template<typename T, int N>
class cSlotCache
{
	retro_atomic_ptr_t slot[N];

public:
	cSlotCache()
	{
		for (int i = 0; i < N; i++)
			retro_atomic_ptr_init(&slot[i], NULL);
	}

	/* NULL when there is nothing to reuse. */
	T* Take()
	{
		for (int i = 0; i < N; i++)
		{
			if (retro_atomic_load_relaxed_ptr(&slot[i]) != NULL)
			{
				void *p = retro_atomic_exchange_ptr(&slot[i], NULL);
				if (p != NULL)
					return (T *)p;
			}
		}
		return NULL;
	}

	/* false when every slot is taken: the object stays the caller's. */
	bool Put(T *p)
	{
		for (int i = 0; i < N; i++)
			if (retro_atomic_cas_ptr(&slot[i], NULL, p))
				return true;
		return false;
	}
};

/* Which of three buffers each side of a one-writer, one-reader pair owns.
 * The writer fills Back() and calls Publish(); the reader calls Take() and
 * reads Front(). Each side always holds one buffer to itself and the third
 * waits in between, so neither ever waits for the other, and a Publish the
 * reader has not taken yet is simply replaced by the next one. */
class cTripleBuffer
{
	enum { FRESH = 4 };
	retro_atomic_int_t mid;	/* the buffer neither side holds, | FRESH */
	int back;
	int front;

public:
	cTripleBuffer() { Reset(); }

	/* Only while neither side is running. */
	void Reset()
	{
		back  = 0;
		front = 1;
		retro_atomic_int_init(&mid, 2);
	}

	/* Writer only. */
	int Back() const { return back; }
	void Publish()
	{
		back = retro_atomic_exchange_int(&mid, back | FRESH) & 3;
	}

	/* Reader only. Take() is true when Front() now names a buffer
	 * published since the last Take(). */
	int Front() const { return front; }
	bool Take()
	{
		if (!(retro_atomic_load_acquire_int(&mid) & FRESH))
			return false;
		front = retro_atomic_exchange_int(&mid, front) & 3;
		return true;
	}
};
