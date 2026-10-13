/*
	Copyright 2023 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "touchscreen.h"
#include "naomi_periph.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/modules.h"
#include "hw/maple/maple_devs.h"

#include <algorithm>
#include <vector>

extern std::vector<sched_list> sch_list;

namespace touchscreen
{

//
// 837-14672 touchscreen sensor board
// used by Manic Panic Ghosts and Touch De Zunou
//
// Once the game has sent it something, the board sends where both players
// touch the screen sixty times a second. Each player's position is where
// the player's gun points, the touch is button 1, and button 2 is "charge".
//
class TouchscreenPipe final : public SerialPort::Pipe
{
public:
	TouchscreenPipe()
	{
		clear();
		schedId = sh4_sched_register(0, &schedCallback);
		scif_set_pipe(this);
	}

	~TouchscreenPipe() override
	{
		scif_set_pipe(NULL);
		sh4_sched_request(schedId, -1);
	}

	void write(u8 data) override
	{
		if (data == 0x39)
		{
			// status request
			static const u8 status[] = { 0xaa, 0x39, 0 };
			send(status, sizeof(status));
		}
		if (!schedulerStarted)
		{
			sh4_sched_request(schedId, FRAME_CYCLES);
			schedulerStarted = 1;
		}
	}

	int available() override {
		return count;
	}

	u8 read() override
	{
		if (count == 0)
			return 0;
		const u8 data = toSend[0];
		count--;
		memmove(toSend, toSend + 1, count);
		return data;
	}

	void clear()
	{
		memset(toSend, 0, sizeof(toSend));
		count = 0;
		schedulerStarted = 0;
		touch[0] = touch[1] = 0;
	}

	u8 toSend[48];
	u32 count;
	u8 schedulerStarted;
	u8 touch[2];
	int schedId = -1;

private:
	void send(const u8 *msg, int size)
	{
		if (count >= 32)
			return;
		memcpy(toSend + count, msg, size);
		count += size;
		toSend[count++] = calcChecksum(msg, size);
		scif_update_status();
	}

	static u8 calcChecksum(const u8 *data, int size)
	{
		u8 c = 0;
		for (int i = 0; i < size; i++)
			c += data[i];
		return 256 - c;
	}

	static int schedCallback(int tag, int cycles, int jitter);

	static constexpr int FRAME_CYCLES = SH4_MAIN_CLOCK / 60;
};

static TouchscreenPipe *touchscreen;

int TouchscreenPipe::schedCallback(int tag, int cycles, int jitter)
{
	TouchscreenPipe *instance = touchscreen;
	if (instance == NULL)
		return 0;
	u32 pack[2];
	for (int i = 0; i < 2; i++)
	{
		f32 fx, fy;
		mcfg_ArcadeAbsPos(i, &fx, &fy);
		const u32 kcode = mcfg_ArcadeKcode(i);
		const u32 x = (u32)std::min(std::max((int)fx, 0), 1023);
		const u32 y = (u32)std::min(std::max((int)fy, 0), 1023);
		const u32 hit = (kcode & NAOMI_BTN0_KEY) == 0;
		const u32 charge = (kcode & NAOMI_BTN1_KEY) == 0;
		// touches require bits 20, 21 and 22
		// drag needs bit 22 off
		// bit 23 is charge
		pack[i] = (charge << 23) | (hit << 21) | (hit << 20) | (y << 10) | x;
		if (!instance->touch[i])
			pack[i] |= hit << 22;
		instance->touch[i] = hit;
	}
	const u8 msg[] = { 0xaa, 0x10,
			u8(pack[0] >> 16), u8(pack[0] >> 8), u8(pack[0]),
			u8(pack[1] >> 16), u8(pack[1] >> 8), u8(pack[1]) };
	instance->send(msg, sizeof(msg));

	return FRAME_CYCLES;
}

void init()
{
	term();
	touchscreen = new TouchscreenPipe();
}

void term()
{
	delete touchscreen;
	touchscreen = NULL;
}

bool active()
{
	return touchscreen != NULL;
}

/* The panel's part of a save state (V24): what it has still to send,
 * whether it has started sending, whether each player was touching the
 * last time, and when it sends next. 60 bytes. */
void serialize_v24(void **data, unsigned int *total_size)
{
	if (touchscreen == NULL)
		return;
	u8 counts[4] = { (u8)touchscreen->count, touchscreen->schedulerStarted,
			touchscreen->touch[0], touchscreen->touch[1] };
	s32 start = sch_list[touchscreen->schedId].start;
	s32 end = sch_list[touchscreen->schedId].end;
	LIBRETRO_SA(touchscreen->toSend, sizeof(touchscreen->toSend));
	LIBRETRO_SA(counts, 4);
	LIBRETRO_S(start);
	LIBRETRO_S(end);
}

bool unserialize_v24(void **data, unsigned int *total_size)
{
	if (touchscreen == NULL)
		return true;
	u8 counts[4] = { 0, 0, 0, 0 };
	s32 start = -1, end = -1;
	LIBRETRO_USA(touchscreen->toSend, sizeof(touchscreen->toSend));
	LIBRETRO_USA(counts, 4);
	LIBRETRO_US(start);
	if (!LIBRETRO_US(end))
		return false;
	touchscreen->count = std::min<u32>(counts[0], sizeof(touchscreen->toSend));
	touchscreen->schedulerStarted = counts[1] != 0;
	touchscreen->touch[0] = counts[2] != 0;
	touchscreen->touch[1] = counts[3] != 0;
	periph_sched_restore(touchscreen->schedId, start, end);
	return true;
}

/* An older state: the panel had not been spoken to, which is what the game
 * found when it had none. */
void state_before_v24()
{
	if (touchscreen == NULL)
		return;
	touchscreen->clear();
	sh4_sched_request(touchscreen->schedId, -1);
}

}
