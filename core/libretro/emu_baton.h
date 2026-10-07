#pragma once
/* Who has the emulated machine, and how a frame gets made.
 *
 * In threaded rendering the machine is emulated on its own thread and its
 * frames are rendered on the libretro thread, but nothing runs free: every
 * retro_run() emulates exactly one frame, the same one non-threaded
 * rendering would, and returns when it is done.
 *
 *   libretro thread                      emulation thread
 *   Hold()                               (asleep in WaitForFrame)
 *   StartFrame()                 ---->   runs the machine up to vblank
 *   WaitWork() gets a render     <----   HandOver(render): asleep until...
 *     reads what it needs of the
 *     guest's memory, AckWork()  ---->   ...the guest may carry on
 *     draws it, WorkDone()
 *   WaitWork() gets NULL         <----   EndFrame()
 *   audio, video, Release()
 *
 * Between AckWork() and the end of the frame the two threads run side by
 * side: one draws, the other emulates.
 *
 * Any other thread that needs the machine standing still (the frontend
 * saves states from a task thread) calls Hold() as well and gets it
 * between two frames.
 *
 * There is no lock here. Every change of hands is one atomic operation and
 * a thread with nothing to do sleeps on the eventcount until something
 * changes: nobody polls and nothing times out.
 *
 * tools/threads/run.sh runs this under ThreadSanitizer. */
#include "stdclass.h"

struct EmuBaton
{
	enum
	{
		NONE = 0,	/* nobody has the machine */
		HELD,		/* a caller of Hold() has it */
		FRAME,		/* the emulation thread is running a frame */
		EXIT		/* the emulation thread is to shut down and return */
	};

	EmuBaton()
	{
		retro_atomic_int_init(&state, NONE);
		retro_atomic_ptr_init(&work, NULL);
		retro_atomic_int_init(&acked, 0);
		retro_atomic_int_init(&busy, 0);
	}

	/* Any thread: take the machine. Sleeps while another caller has it or
	 * a frame is being made. */
	void Hold()
	{
		for (;;)
		{
			int key;

			if (retro_atomic_cas_int(&state, NONE, HELD))
				return;
			key = ec.Prepare();
			if (retro_atomic_load_acquire_int(&state) == NONE)
				ec.Cancel();
			else
				ec.Commit(key);
		}
	}

	/* Holder: give the machine back. */
	void Release()
	{
		retro_atomic_store_release_int(&state, NONE);
		ec.Notify();
	}

	/* Holder: have the emulation thread make one frame. The machine is
	 * the holder's again once WaitWork() has returned NULL. */
	void StartFrame()
	{
		retro_atomic_store_release_int(&state, FRAME);
		ec.Notify();
	}

	/* Holder, after StartFrame(): the next thing the emulation thread
	 * hands over, or NULL when the frame is finished. */
	void* WaitWork()
	{
		for (;;)
		{
			int key;
			void *w = retro_atomic_load_acquire_ptr(&work);

			if (w != NULL)
			{
				retro_atomic_store_release_int(&busy, 1);
				retro_atomic_store_release_ptr(&work, NULL);
				return w;
			}
			if (retro_atomic_load_acquire_int(&state) != FRAME)
				return NULL;

			key = ec.Prepare();
			if (retro_atomic_load_acquire_ptr(&work) != NULL
					|| retro_atomic_load_acquire_int(&state) != FRAME)
				ec.Cancel();
			else
				ec.Commit(key);
		}
	}

	/* Holder: the emulation thread may go on past its HandOver(). Once
	 * for each piece of work. */
	void AckWork()
	{
		retro_atomic_store_release_int(&acked, 1);
		ec.Notify();
	}

	/* Holder: finished with the work WaitWork() returned. */
	void WorkDone()
	{
		retro_atomic_store_release_int(&busy, 0);
		ec.Notify();
	}

	/* Holder: tell the emulation thread, asleep between frames, to shut
	 * down. Join it, then call ThreadGone(). */
	void TellThreadToExit()
	{
		retro_atomic_store_release_int(&state, EXIT);
		ec.Notify();
	}

	void ThreadGone()
	{
		retro_atomic_store_release_int(&state, NONE);
		ec.Notify();
	}

	/* Emulation thread: sleep until there is a frame to make. false: shut
	 * down and return instead. */
	bool WaitForFrame()
	{
		for (;;)
		{
			int key, st = retro_atomic_load_acquire_int(&state);

			if (st == FRAME)
				return true;
			if (st == EXIT)
				return false;

			key = ec.Prepare();
			st  = retro_atomic_load_acquire_int(&state);
			if (st == FRAME || st == EXIT)
				ec.Cancel();
			else
				ec.Commit(key);
		}
	}

	/* Any thread: a frame is being made, and the holder is in WaitWork(). */
	bool FrameRunning()
	{
		return retro_atomic_load_acquire_int(&state) == FRAME;
	}

	/* Emulation thread: the frame is made; the machine goes back to the
	 * holder that asked for it. */
	void EndFrame()
	{
		retro_atomic_store_release_int(&state, HELD);
		ec.Notify();
	}

	/* Emulation thread: sleep until the holder has finished the work
	 * handed over before, so that two never overlap. */
	void WaitWorkDone()
	{
		sleep_while_zero(&busy, true);
	}

	/* Emulation thread: hand @w to the holder and sleep until it calls
	 * AckWork(). */
	void HandOver(void *w)
	{
		retro_atomic_store_release_int(&acked, 0);
		retro_atomic_store_release_ptr(&work, w);
		ec.Notify();
		sleep_while_zero(&acked, false);
	}

private:
	retro_atomic_int_t state;
	retro_atomic_ptr_t work;	/* handed over, not taken yet */
	retro_atomic_int_t acked;	/* the holder has let the emulation thread go on */
	retro_atomic_int_t busy;	/* the holder is still on the last work */
	cEventCount        ec;

	/* Sleep until *flag is non-zero, or zero when @until_zero. */
	void sleep_while_zero(retro_atomic_int_t *flag, bool until_zero)
	{
		for (;;)
		{
			int key;

			if ((retro_atomic_load_acquire_int(flag) == 0) == until_zero)
				return;
			key = ec.Prepare();
			if ((retro_atomic_load_acquire_int(flag) == 0) == until_zero)
				ec.Cancel();
			else
				ec.Commit(key);
		}
	}
};
