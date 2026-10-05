#pragma once
/* The controller and mouse state the emulation thread reads.
 *
 * In threaded rendering the libretro thread samples input once a frame
 * and the emulation thread reads it when the game polls its controllers.
 * The libretro thread fills a snapshot of all four ports and publishes it
 * whole; the emulation thread reads the newest one. They go through a
 * cTripleBuffer, so neither thread waits for the other, nothing takes a
 * lock, and a read never sees half of one frame's state and half of the
 * next.
 *
 * Mouse motion is published as running totals, not per-frame amounts: the
 * reader subtracts what it has already been given, so motion from a frame
 * the game did not poll in is carried into the next poll instead of lost,
 * and polling twice in a frame does not count it twice.
 *
 * tools/threads/run.sh runs this under ThreadSanitizer. */
#include "types.h"
#include "lockfree.h"

struct InputLatch
{
	struct Port
	{
		u32 kcode;		/* active low */
		s8  joyx, joyy, joyrx, joyry;
		u8  rt, lt;
		u32 mo_buttons;		/* active low */
		u32 mo_x, mo_y, mo_wheel;	/* running totals, wrap around */
	};
	struct State
	{
		Port port[4];
	};

	InputLatch()
	{
		for (int i = 0; i < 3; i++)
			for (int p = 0; p < 4; p++)
			{
				Port& s = buf[i].port[p];
				s.kcode = 0xFFFFFFFF;
				s.joyx = s.joyy = s.joyrx = s.joyry = 0;
				s.rt = s.lt = 0;
				s.mo_buttons = 0xFFFFFFFF;
				s.mo_x = s.mo_y = s.mo_wheel = 0;
			}
		for (int p = 0; p < 4; p++)
			seen_x[p] = seen_y[p] = seen_wheel[p] = 0;
	}

	/* Libretro thread: fill the returned state, then Publish(). */
	State& Next() { return buf[tri.Back()]; }
	void Publish() { tri.Publish(); }

	/* Emulation thread: the newest published state of @port. */
	const Port& Read(u32 port)
	{
		tri.Take();
		return buf[tri.Front()].port[port];
	}

	/* Emulation thread: mouse buttons, and the motion since the last
	 * call for @port. */
	void ReadMouse(u32 port, u32 *buttons, f32 *dx, f32 *dy, f32 *dwheel)
	{
		const Port& s = Read(port);

		*buttons = s.mo_buttons;
		*dx      = (f32)(s32)(s.mo_x - seen_x[port]);
		*dy      = (f32)(s32)(s.mo_y - seen_y[port]);
		*dwheel  = (f32)(s32)(s.mo_wheel - seen_wheel[port]);
		seen_x[port]     = s.mo_x;
		seen_y[port]     = s.mo_y;
		seen_wheel[port] = s.mo_wheel;
	}

private:
	State         buf[3];
	cTripleBuffer tri;
	u32           seen_x[4], seen_y[4], seen_wheel[4];	/* emulation thread */
};
