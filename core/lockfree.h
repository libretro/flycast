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

/* A list any thread may Push onto and one thread empties. The link lives
 * in the object (the Next member), so a Push allocates nothing; an object
 * must not be pushed again before the consumer has taken it. */
template<typename T, T *T::*Next>
class cMpscList
{
	retro_atomic_ptr_t head;

public:
	cMpscList()
	{
		retro_atomic_ptr_init(&head, NULL);
	}

	void Push(T *p)
	{
		for (;;)
		{
			void *old = retro_atomic_load_relaxed_ptr(&head);
			p->*Next = (T *)old;
			if (retro_atomic_cas_ptr(&head, old, p))
				return;
		}
	}

	/* Consumer only. Everything pushed so far, oldest first, chained
	 * through Next; NULL when the list is empty. */
	T* TakeAll()
	{
		T *list = (T *)retro_atomic_exchange_ptr(&head, NULL);
		T *out = NULL;
		while (list != NULL)
		{
			T *next = list->*Next;
			list->*Next = out;
			out = list;
			list = next;
		}
		return out;
	}
};

/* A set of numbers below N that one thread marks and another collects.
 * Mark() is two atomic operations and nothing else, so it may be called
 * from a signal handler. A mark made before Collect() starts is always
 * handed to that Collect() or the next one; marking a number twice before
 * it is collected hands it over once. */
template<int N>
class cMarkSet
{
	retro_atomic_int_t bits[(N + 31) / 32];
	retro_atomic_int_t pending;

public:
	cMarkSet()
	{
		for (int i = 0; i < (N + 31) / 32; i++)
			retro_atomic_int_init(&bits[i], 0);
		retro_atomic_int_init(&pending, 0);
	}

	void Mark(unsigned n)
	{
		retro_atomic_fetch_or_int(&bits[n >> 5], (int)(1u << (n & 31)));
		retro_atomic_store_release_int(&pending, 1);
	}

	/* Collector: cheap test for "is there anything to collect". */
	bool Pending()
	{
		return retro_atomic_load_relaxed_int(&pending) != 0;
	}

	/* Collector: calls f(n) for every number marked since the last time. */
	template<typename F>
	void Collect(F f)
	{
		/* Clear the flag before looking: a mark that lands behind the
		 * scan sets it again. */
		if (!retro_atomic_exchange_int(&pending, 0))
			return;
		for (int w = 0; w < (N + 31) / 32; w++)
		{
			unsigned word;

			if (!retro_atomic_load_relaxed_int(&bits[w]))
				continue;
			word = (unsigned)retro_atomic_exchange_int(&bits[w], 0);
			for (unsigned b = 0; word != 0; b++, word >>= 1)
				if (word & 1)
					f((unsigned)w * 32 + b);
		}
	}
};
