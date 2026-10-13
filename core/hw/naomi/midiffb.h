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
#pragma once
#include "types.h"

/* The force feedback board of the driving games that talk to their wheel
 * through the sound chip's MIDI port (Initial D, Club Kart, The King of
 * Route 66, Sega Driving Simulator). */

// Whether the game has one: set by midiffb_init()
extern bool midiffb_enabled;

// When a game is loaded: the board, if this game has one
void midiffb_init(void);
void midiffb_term(void);
// A hard reset of the machine
void midiffb_reset(void);

// A byte the game writes to the MIDI output (AICA MOBUF)
void midiffb_receive(u8 data);
/* The byte at the head of the MIDI input, taken out of it if @pop; -1 when
 * there is none */
int midiffb_in_byte(bool pop);

// In a save state from V27
void midiffb_serialize_v27(void **data, unsigned int *total_size);
bool midiffb_unserialize_v27(void **data, unsigned int *total_size);
void midiffb_state_before_v27(void);

/* A wheel's force, made into the rumble of the first port's controller, if
 * "NAOMI Wheel Force Feedback" is on (libretro.cpp). A force lasts until the
 * next one; a shake for @duration_ms. */
enum
{
	NAOMI_FFB_STOP,		// everything stops
	NAOMI_FFB_FORCE,	// the wheel pulled one way, @level from 0 to 1
	NAOMI_FFB_SHAKE,	// the wheel shaken, @level from 0 to 1
};
void naomi_ffb_rumble(int kind, float level, u32 duration_ms);
