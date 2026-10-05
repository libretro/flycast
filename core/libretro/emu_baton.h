#pragma once
/* Who has the emulated machine.
 *
 * In threaded rendering the emulation thread runs the machine. A thread
 * that needs it standing still (save state, load state, reset, unload)
 * takes it with Hold() and gives it back with Release(). The frontend may
 * save states from a background thread, so more than one thread can ask;
 * the state word hands the machine to one of them at a time.
 *
 * There is no lock here. Every change of hands is one atomic operation on
 * the state word, and a thread with nothing to do sleeps on the eventcount
 * until the word changes: nobody polls.
 *
 * tools/threads/run.sh runs this under ThreadSanitizer. */
#include "stdclass.h"

struct EmuBaton
{
	enum
	{
		NONE = 0,	/* no emulation thread, nobody holding */
		HELD,		/* no emulation thread, a caller holds the machine */
		RUNNING,	/* the emulation thread has it */
		PAUSE_REQ,	/* a caller asked the emulation thread to park */
		PARKED,		/* it has parked; that caller holds the machine */
		EXIT_REQ	/* the emulation thread is to shut down and return */
	};

	EmuBaton() { retro_atomic_int_init(&state, NONE); }

	/* Take the machine. With no emulation thread that is one atomic step.
	 * With one, ask it to park and sleep until it says it has. @kick must
	 * make the thread leave the machine's run loop and any wait it can be
	 * stuck in; it is called again every lap of 1 ms, because a stop that
	 * lands just before the run loop starts is overwritten by it. After
	 * @max_laps laps (0: never) the request is withdrawn, @unkick undoes
	 * the last kick, and the call returns false with the thread running. */
	bool Hold(unsigned max_laps, void (*kick)(void), void (*unkick)(void))
	{
		unsigned lap = 0;

		for (;;)
		{
			int key, st;

			if (retro_atomic_cas_int(&state, NONE, HELD))
				return true;
			if (retro_atomic_cas_int(&state, RUNNING, PAUSE_REQ))
				break;

			/* Another caller holds it; sleep until it lets go. */
			key = ec.Prepare();
			st  = retro_atomic_load_acquire_int(&state);
			if (st == NONE || st == RUNNING)
				ec.Cancel();
			else
				ec.Commit(key);
		}

		for (;;)
		{
			int key;

			kick();

			key = ec.Prepare();
			if (retro_atomic_load_acquire_int(&state) == PARKED)
			{
				ec.Cancel();
				return true;
			}
			ec.Commit(key, 1000);
			if (retro_atomic_load_acquire_int(&state) == PARKED)
				return true;

			if (max_laps && ++lap >= max_laps)
			{
				/* Withdraw, unless the thread parked just now. */
				if (!retro_atomic_cas_int(&state, PAUSE_REQ, RUNNING))
					return true;
				unkick();
				ec.Notify();
				return false;
			}
		}
	}

	/* Give the machine back and wake whoever waits for it. */
	void Release()
	{
		if (!retro_atomic_cas_int(&state, HELD, NONE))
			retro_atomic_store_release_int(&state, RUNNING);
		ec.Notify();
	}

	/* Holder only: the emulation thread is about to be created and the
	 * machine is its from here on. */
	void HandToThread()
	{
		retro_atomic_store_release_int(&state, RUNNING);
	}

	/* Holder only: tell the parked emulation thread to shut down. Join
	 * it, then call ThreadGone(). */
	void TellThreadToExit()
	{
		retro_atomic_store_release_int(&state, EXIT_REQ);
		ec.Notify();
	}

	void ThreadGone()
	{
		retro_atomic_store_release_int(&state, NONE);
		ec.Notify();
	}

	/* Emulation thread: call before every pass through the machine's run
	 * loop. Parks for as long as a caller holds the machine. true: run;
	 * false: shut down and return. */
	bool WaitToRun()
	{
		for (;;)
		{
			int st = retro_atomic_load_acquire_int(&state);

			if (st == RUNNING)
				return true;
			if (st == PAUSE_REQ)
			{
				if (retro_atomic_cas_int(&state, PAUSE_REQ, PARKED))
					ec.Notify();
			}
			else if (st == PARKED)
			{
				int key = ec.Prepare();
				if (retro_atomic_load_acquire_int(&state) != PARKED)
					ec.Cancel();
				else
					ec.Commit(key);
			}
			else
				return false;
		}
	}

private:
	retro_atomic_int_t state;
	cEventCount        ec;
};
