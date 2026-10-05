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
