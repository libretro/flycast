/*
    Created on: Sep 23, 2019

	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"

struct Cheat
{
	const char *game_id;
	const char *area_or_version;
	u32 addresses[16];
	u32 values[16];
	/* Where not 0: the value is only written over this one. (The game puts
	 * the same word there for other uses, which must not be changed.) */
	u32 orig_values[16];
};

/* A change a game needs to work, made whatever the options say: @value is
 * written at @address in main memory whenever something else is there -
 * or, with @only_if set, only when @if_value is there (code that is
 * patched once it is loaded, and left alone if it is not the code). */
struct GameFix
{
	const char *game_id;
	u32 bits;		// 8, 16 or 32
	u32 address;
	u32 value;
	bool only_if;
	u32 if_value;
};

class CheatManager
{
public:
	CheatManager() : _widescreen_cheat(nullptr), _have_original(0), _fixes(nullptr), _fix_count(0) {}
	bool Reset();	// Returns true if using 16:9 anamorphic screen ratio
	void Apply();
	/* The option changed while the game runs (the machine standing still).
	 * Returns what Reset() does. */
	bool Change();
private:
	/* What the game had where the cheat writes, as last seen there: put
	 * back when the cheat is turned off. */
	u32 _original[16];
	u32 _have_original;
	static const Cheat _widescreen_cheats[];
	static const Cheat _naomi_widescreen_cheats[];
	const Cheat *_widescreen_cheat;
	static const GameFix _game_fixes[];
	const GameFix *_fixes;
	u32 _fix_count;
};

extern CheatManager cheatManager;
