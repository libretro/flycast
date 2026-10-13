/*
	Copyright 2025 flyinghead

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
/*
	The force feedback board of Initial D, Club Kart, The King of Route 66
	and Sega Driving Simulator, on the sound chip's MIDI port.

	The game sends it commands of four bytes, the first with bit 7 set and
	the last a check of the other three; the board answers each with four
	bytes that say where the wheel is. What the board does with the wheel
	is given to the frontend as rumble, as far as a rumble motor can show
	it: the force pulling the wheel and the shaking. Its springs and
	dampers depend on where the wheel is held and are left out.

	The commands, as worked out by others for force feedback adapters:
	https://www.arcade-projects.com/threads/force-feedback-translator-sega-midi-sega-rs422-and-namco-rs232.924/
*/
#include "midiffb.h"
#include "naomi_cart.h"
#include "hw/aica/aica.h"
#include <math.h>
#include <string.h>
#include <algorithm>

bool midiffb_enabled;

extern s8 joyx[4];

// The command on its way from the game
static u8 tx_buf[4];
static u8 tx_index;
// The board has been told to work, and is finding the wheel's centre
static bool active;
static bool calibrating;
// The power the game has set, 0 to 15
static u8 power_level = 12;
// The force the game has set: from -0x80 (right) to 0x3f7f
static s16 torque;
// Where the wheel is, from 0 to 16383, 8192 at the centre
static f32 position = 8192.f;

/* The MIDI input of the sound chip: what the board has sent, not yet read
 * by the game. */
#define MIDI_IN_SIZE 64
static u8 midi_in[MIDI_IN_SIZE];
static u8 midi_in_head;
static u8 midi_in_count;

static float power(void)
{
	return power_level / 15.f;
}

static void midi_in_put(u8 b)
{
	// (a game that does not read what comes in does not get more of it)
	if (midi_in_count >= MIDI_IN_SIZE)
		return;
	midi_in[(midi_in_head + midi_in_count) % MIDI_IN_SIZE] = b;
	midi_in_count++;
	/* The sound CPU and the SH4 are told: they see it at the sound chip's
	 * next step, which is sooner than the four bytes take on the line. */
	SCIPD->MIDI_IN = 1;
	MCIPD->MIDI_IN = 1;
}

int midiffb_in_byte(bool pop)
{
	if (midi_in_count == 0)
		return -1;
	const u8 b = midi_in[midi_in_head];
	if (pop)
	{
		midi_in_head = (midi_in_head + 1) % MIDI_IN_SIZE;
		midi_in_count--;
	}
	return b;
}

static void midi_send(u8 b1, u8 b2, u8 b3)
{
	midi_in_put(b1);
	midi_in_put(b2);
	midi_in_put(b3);
	midi_in_put((b1 ^ b2 ^ b3) & 0x7f);
}

static void rumble_torque(void)
{
	float v = fabsf(torque / 128.f * power());
	naomi_ffb_rumble(NAOMI_FFB_FORCE, v > 1.f ? 1.f : v, 0);
}

void midiffb_receive(u8 data)
{
	// The position the wheel is turned to while the board looks for its centre
	position = std::min(16383.f, std::max(0.f, position + torque));
	if (data & 0x80)
		tx_index = 0;
	tx_buf[tx_index] = data;
	if (tx_index == 3 && ((tx_buf[0] ^ tx_buf[1] ^ tx_buf[2]) & 0x7f) == tx_buf[3])
	{
		const u8 cmd = tx_buf[0] & 0x7f;
		switch (cmd)
		{
		case 0:
			// Force feedback off (0) or on (1)
			if (tx_buf[2] == 0)
			{
				active = false;
				calibrating = false;
				naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
			}
			else if (tx_buf[2] == 1)
				active = true;
			break;

		case 3:
			// Power: 0 to 0x7f
			power_level = tx_buf[1] >> 3;
			break;

		case 4:
			// The force: 0 is the most to the right, 0x17f the most to the left
			torque = (s16)(((tx_buf[1] << 7) | tx_buf[2]) - 0x80);
			if (active && !calibrating)
				rumble_torque();
			break;

		case 5:
			// Shaking: its frequency in half Hz, then its amplitude
			if (active)
			{
				float v = (tx_buf[2] - 1) / 80.f * power();
				naomi_ffb_rumble(NAOMI_FFB_SHAKE, v < 0.f ? 0.f : v > 1.f ? 1.f : v, 1000);
			}
			break;

		case 0x7f:
			// The board is reset, and finds the wheel's centre
			calibrating = true;
			position = 8192.f;
			break;

		/* 1: the most force; 2: unknown; 6: damper; 0x0b: spring; 0x70: the
		 * centre; 0x7a: how the board answers; 0x7c: phase alignment; 0x7d:
		 * status. Nothing a rumble motor can show. */
		default:
			break;
		}

		if (!calibrating)
		{
			// The wheel as the first player turns it
			const bool inverted = naomi_game_inputs != NULL && naomi_game_inputs->axes[0].inverted;
			const int axis = ((u8)joyx[0] - 128) * 256;
			const float v = axis * (inverted ? 1 : -1) / 4.f + 8192.f;
			position = std::min(16383.f, std::max(0.f, v));
		}
		// (the game wants bits 0-4 of the first byte to be 0x10, and bit 6 clear)
		midi_send(0x90, ((int)position >> 7) & 0x7f, (int)position & 0x7f);
	}
	tx_index = (tx_index + 1) % 4;
}

void midiffb_reset(void)
{
	active = false;
	calibrating = false;
	tx_index = 0;
	power_level = 12;
	torque = 0;
	midi_in_head = 0;
	midi_in_count = 0;
	if (midiffb_enabled)
		naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
}

void midiffb_init(void)
{
	static const char *const games[] = {
		"INITIAL D",
		"INITIAL D Ver.2",
		"INITIAL D Ver.3",
		"INITIAL D CYCRAFT",
		"THE KING OF ROUTE66",
		"CLUB KART IN JAPAN",
		"SEGA DRIVING SIMULATOR",
	};

	midiffb_enabled = false;
	if (SYSTEM_IS_NAOMI() && !naomi_cart_systemsp)
		for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++)
			if (!strcmp(naomi_game_id, games[i]))
				midiffb_enabled = true;
	position = 8192.f;
	midiffb_reset();
	if (midiffb_enabled)
		NOTICE_LOG(NAOMI, "The game has a force feedback board on the MIDI port");
}

void midiffb_term(void)
{
	if (midiffb_enabled)
		naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
	midiffb_enabled = false;
}

/* In a save state from V27, on a game with the board only (79 bytes):
 *   u8  tx_buf[4], tx_index   the command on its way
 *   u8  flags                 bit 0 active, bit 1 calibrating
 *   u8  power_level
 *   s16 torque
 *   f32 position
 *   u8  midi_in[64], midi_in_head, midi_in_count */
void midiffb_serialize_v27(void **data, unsigned int *total_size)
{
	if (!midiffb_enabled)
		return;
	const u8 flags = (active ? 1 : 0) | (calibrating ? 2 : 0);
	LIBRETRO_SA(tx_buf, 4);
	LIBRETRO_S(tx_index);
	LIBRETRO_S(flags);
	LIBRETRO_S(power_level);
	LIBRETRO_S(torque);
	LIBRETRO_S(position);
	LIBRETRO_SA(midi_in, MIDI_IN_SIZE);
	LIBRETRO_S(midi_in_head);
	LIBRETRO_S(midi_in_count);
}

bool midiffb_unserialize_v27(void **data, unsigned int *total_size)
{
	if (!midiffb_enabled)
		return true;
	u8 flags = 0;
	LIBRETRO_USA(tx_buf, 4);
	LIBRETRO_US(tx_index);
	LIBRETRO_US(flags);
	LIBRETRO_US(power_level);
	LIBRETRO_US(torque);
	LIBRETRO_US(position);
	LIBRETRO_USA(midi_in, MIDI_IN_SIZE);
	LIBRETRO_US(midi_in_head);
	LIBRETRO_US(midi_in_count);
	tx_index &= 3;
	active = (flags & 1) != 0;
	calibrating = (flags & 2) != 0;
	if (power_level > 15)
		power_level = 12;
	if (!(position >= 0.f && position <= 16383.f))
		position = 8192.f;
	midi_in_head %= MIDI_IN_SIZE;
	if (midi_in_count > MIDI_IN_SIZE)
		midi_in_count = 0;
	if (active && !calibrating)
		rumble_torque();
	else
		naomi_ffb_rumble(NAOMI_FFB_STOP, 0.f, 0);
	return true;
}

/* An older state had no board: the game had sent it nothing, or nothing
 * that came to anything, and had been answered nothing. */
void midiffb_state_before_v27(void)
{
	position = 8192.f;
	midiffb_reset();
}
