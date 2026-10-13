/*
 *  Created on: Nov 13, 2018

	Copyright 2018 flyinghead

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

#ifndef CORE_HW_NAOMI_NAOMI_ROMS_INPUT_H_
#define CORE_HW_NAOMI_NAOMI_ROMS_INPUT_H_

#include "hw/maple/maple_devs.h"

//
// NAOMI Games
//

#define NAO_BASE_BTN_DESC { NAOMI_COIN_KEY, "COIN" }, \
						 { NAOMI_TEST_KEY, "TEST" }, \
						 { NAOMI_SERVICE_KEY, "SERVICE" },
#define NAO_START_DESC { NAOMI_START_KEY, "START" },

// The analog inputs of a game whose controls have none: read as with no
// descriptor (the sticks, centred), and not shown to the frontend. With no
// axes at all they would read uncentred.
#define NO_ANALOG_AXES { "", Full, 0 }, \
						{ "", Full, 1 }, \
						{ "", Full, 2 }, \
						{ "", Full, 3 },

InputDescriptors _18wheelr_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "HORN" },
			{ NAOMI_DOWN_KEY, "VIEW" },
			{ NAOMI_BTN1_KEY, "SHIFT L", NAOMI_DOWN_KEY },		// This button uses P2 inputs for P1
			{ NAOMI_BTN2_KEY, "SHIFT H", NAOMI_UP_KEY },		// This button uses P2 inputs for P1
			{ NAOMI_BTN3_KEY, "SHIFT R", NAOMI_LEFT_KEY },		// This button uses P2 inputs for P1
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors alienfnt_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LEFT SHOT" },
			{ NAOMI_BTN1_KEY, "ROTATION R" },
			{ NAOMI_BTN2_KEY, "RIGHT SHOT" },
			{ NAOMI_BTN3_KEY, "ROTATION L" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "WHEEL", Full, 0 },
			{ "RIGHT PEDAL", Half, 4 },
			{ "LEFT PEDAL", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors alpilot_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LANDING GEAR SW" },
			{ NAOMI_BTN1_KEY, "VIEW CHANGE" },
			{ NAOMI_BTN2_KEY, "FLAP SWITCH" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			// the control wheel pitches the plane on the stick's vertical axis
			{ "ELEVATOR", Full, 1 },
			{ "AILERON", Full, 0 },
			{ "", Full, 3 },
			{ "RUDDER PEDAL", Full, 2 },
			{ "THRUST LEVER L", Half, 5 },
			{ "THRUST LEVER R", Half, 4 },
			{ NULL },
	  },
};

InputDescriptors capsnk_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LIGHT PUNCH" },
			{ NAOMI_BTN1_KEY, "STRONG PUNCH" },
			{ NAOMI_BTN3_KEY, "LIGHT KICK" },
			{ NAOMI_BTN4_KEY, "STRONG KICK" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors crzytaxi_inputs = {
	  {
			{ NAOMI_UP_KEY, "DRIVE GEAR" },
			{ NAOMI_DOWN_KEY, "REVERSE GEAR" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors cspike_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT1" },
			{ NAOMI_BTN1_KEY, "SHOT2" },
			{ NAOMI_BTN2_KEY, "SHOT3" },
			{ NAOMI_BTN3_KEY, "SHOT4" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors trigger_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRIGGER" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors deathcox_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRIGGER" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "GUN-X", Full, 0 },
			{ "GUN-Y", Full, 1 },
			{ NULL },
	  },
};

InputDescriptors gunsur2_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "GUN BUTTON" },
			{ NAOMI_BTN1_KEY, "TRIGGER" },
			{ NAOMI_UP_KEY, "SELECT UP" },
			{ NAOMI_DOWN_KEY, "SELECT DOWN" },
			{ NAOMI_START_KEY, "ENTER" },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "ROLL", Full, 0 },
			{ "PITCH", Full, 1 },
			{ "YAW", Full, 2 },
			{ NULL },
	  },
};

InputDescriptors jambo_inputs = {
	  {
			{ NAOMI_UP_KEY, "LEVER UP", NAOMI_DOWN_KEY },	// This button uses P2 inputs for P1
			{ NAOMI_DOWN_KEY, "LEVER DOWN", NAOMI_UP_KEY },	// This button uses P2 inputs for P1
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors mvsc2_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LIGHT PUNCH" },
			{ NAOMI_BTN1_KEY, "STRONG PUNCH" },
			{ NAOMI_BTN2_KEY, "ASSIST A" },
			{ NAOMI_BTN3_KEY, "LIGHT KICK" },
			{ NAOMI_BTN4_KEY, "STRONG KICK" },
			{ NAOMI_BTN5_KEY, "ASSIST B" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors sfz3ugd_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LIGHT PUNCH" },
			{ NAOMI_BTN1_KEY, "MEDIUM PUNCH" },
			{ NAOMI_BTN2_KEY, "STRONG PUNCH" },
			{ NAOMI_BTN3_KEY, "LIGHT KICK" },
			{ NAOMI_BTN4_KEY, "MEDIUM KICK" },
			{ NAOMI_BTN5_KEY, "STRONG KICK" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors ninjaslt_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "ENTER" },
			{ NAOMI_BTN1_KEY, "TRIGGER 1P" },	// swapped with btn4
			{ NAOMI_BTN2_KEY, "START 1P" },
			{ NAOMI_BTN3_KEY, "START 2P" },
			{ NAOMI_BTN5_KEY, "TRIGGER 2P" },
			{ NAOMI_UP_KEY, "SELECT UP" },
			{ NAOMI_DOWN_KEY, "SELECT DOWN" },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors vonot_inputs = {
	  {
			{ NAOMI_UP_KEY, "L UP" },
			{ NAOMI_DOWN_KEY, "L DOWN" },
			{ NAOMI_LEFT_KEY, "L LEFT" },
			{ NAOMI_RIGHT_KEY, "L RIGHT" },
			{ NAOMI_BTN0_KEY, "L TRIGGER" },
			{ NAOMI_BTN1_KEY, "L TURBO" },
			{ NAOMI_BTN2_KEY, "QM" },
			// These buttons use P2 inputs for P1
			{ NAOMI_BTN3_KEY, "R TRIGGER", NAOMI_BTN0_KEY },
			{ NAOMI_BTN4_KEY, "R TURBO", NAOMI_BTN1_KEY },
			{ NAOMI_BTN5_KEY, "R UP", NAOMI_UP_KEY },
			{ NAOMI_BTN6_KEY, "R DOWN", NAOMI_DOWN_KEY },
			{ NAOMI_BTN7_KEY, "R LEFT", NAOMI_LEFT_KEY },
			{ NAOMI_BTN8_KEY, "R RIGHT", NAOMI_RIGHT_KEY },

			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors vtenis2c_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT1" },
			{ NAOMI_BTN1_KEY, "SHOT2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors monkeyba_inputs = {
	  {
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "STICK V", Full, 1 },
			{ "STICK H", Full, 0 },
			{ NULL },
	  },
};

InputDescriptors cvs2_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LIGHT PUNCH" },
			{ NAOMI_BTN1_KEY, "MEDIUM PUNCH" },
			{ NAOMI_BTN2_KEY, "STRONG PUNCH" },
			{ NAOMI_BTN3_KEY, "LIGHT KICK" },
			{ NAOMI_BTN4_KEY, "MEDIUM KICK" },
			{ NAOMI_BTN5_KEY, "STRONG KICK" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors tokyobus_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "HORN" },
			{ NAOMI_DOWN_KEY, "VIEW CHANGE" },
			{ NAOMI_LEFT_KEY, "ANNOUNCE" },
			{ NAOMI_RIGHT_KEY, "DOOR CLOSE" },
			// These buttons uses P2 inputs for P1
			{ NAOMI_BTN1_KEY, "WINKER RIGHT", NAOMI_BTN0_KEY },
			{ NAOMI_BTN2_KEY, "WINKER LEFT", NAOMI_BTN1_KEY },
			{ NAOMI_BTN3_KEY, "SHIFT FRONT", NAOMI_UP_KEY },
			{ NAOMI_BTN4_KEY, "SHIFT REVERSE", NAOMI_DOWN_KEY },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors wrungp_inputs = {
	  {
			{ NAOMI_UP_KEY, "VIEW" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE BAR", Full, 0 },
			{ "THROTTLE LEVER", Half, 4, true },
			{ "ROLL", Full, 2 },
			{ "PITCH", Full, 3 },
			{ NULL },
	  },
};

InputDescriptors marine_fishing_inputs = {
	  {
			{ NAOMI_START_KEY, "CAST" },
			{ NAOMI_UP_KEY, "LURE" },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "ROD Y", Full, 1 },
			{ "ROD X", Full, 0 },
			{ "STICK X", Full, 2 },
			{ "STICK Y", Full, 3 },
			{ "REEL SPEED", Half, 4 },
			{ "TENSION", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors f355_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "ASSIST SC" },
			{ NAOMI_BTN1_KEY, "ASSIST TC" },
			{ NAOMI_BTN2_KEY, "ASSIST ABS" },
			{ NAOMI_BTN3_KEY, "ASSIST IBS", NAOMI_BTN1_KEY },
			{ NAOMI_BTN4_KEY, "WING SHIFT L", NAOMI_DOWN_KEY },
			{ NAOMI_BTN5_KEY, "WING SHIFT R", NAOMI_UP_KEY },

			// Manual gearshift (Deluxe only)
			//    L   R
			//  U 1 3 5
			//
			//  D 2 4 6
			{ NAOMI_UP_KEY, "SPEED SHIFT UP" },
			{ NAOMI_DOWN_KEY, "SPEED SHIFT DOWN" },
			{ NAOMI_LEFT_KEY, "SPEED SHIFT LEFT" },
			{ NAOMI_RIGHT_KEY, "SPEED SHIFT RIGHT" },

			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "ACCEL", Half, 0 },
			{ "BRAKE", Half, 1 },
			{ "CLUTCH", Full, 2 },	// Deluxe only
			{ "unused", Full, 4 },
			{ "HANDLE", Full, 0 },
			{ NULL },
	  },
};

InputDescriptors zombie_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "BTN0" },
			{ NAOMI_BTN1_KEY, "BTN1" },
			{ NAOMI_BTN2_KEY, "BTN2" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "UP - DOWN", Full, 1, true },
			{ "LEFT - RIGHT", Full, 0, true },
			{ "", Half, 4 },	// unused but P2 starts at axis 4
			{ "", Half, 5 },	// unused but P2 starts at axis 4
			{ NULL },
	  },
};

// World Series 99 / Super Major League 99
// World Series Baseball / Super Major League
static InputDescriptors wsbb_inputs = {
	{
		{ NAOMI_BTN0_KEY, "BTN A" },
		{ NAOMI_BTN1_KEY, "BTN B" },
		NAO_START_DESC
		NAO_BASE_BTN_DESC
		{ 0 },
	},
	{
		{ "STICK Y", Full, 1, true },
		{ "STICK X", Full, 0, true },
		{ "BAT", Half, 4, true },
		{ "", Half, 5 },	// unused but P2 starts at axis 4
		{ NULL },
	},
};

static InputDescriptors ringout_inputs  = {
	{
		{ NAOMI_BTN0_KEY, "BUMPER" },
		{ NAOMI_BTN1_KEY, "BACK" },
		NAO_START_DESC
		NAO_BASE_BTN_DESC
		{ 0 },
	},
	{
		{ "STEER", Full, 0 },
		{ "ACCEL", Half, 4 },
		{ NULL },
	},
};

static InputDescriptors sstrkfgt_inputs = {
	{
		{ NAOMI_BTN0_KEY, "GUN TRIGGER" },
		{ NAOMI_BTN1_KEY, "MISSILE BTN" },
		{ NAOMI_BTN2_KEY, "AIR BRAKE" },
		{ NAOMI_BTN3_KEY, "VIEW CHANGE" },
		NAO_START_DESC
		NAO_BASE_BTN_DESC
		{ 0 },
	},
	{
		{ "ELEVATOR", Full, 1 },
		{ "AILERON", Full, 0 },
		{ "THRUST LEVER", Half, 4 },
		{ "RUDDER PEDAL", Full, 2 },
		{ NULL },
	},
};
InputDescriptors asndynmt_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "PUNCH" },
			{ NAOMI_BTN1_KEY, "KICK" },
			{ NAOMI_BTN2_KEY, "JUMP" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors ausfache_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Weak Attack" },
			{ NAOMI_BTN1_KEY, "Medium Attack" },
			{ NAOMI_BTN2_KEY, "Strong Attack" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors azumanga_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "BUTTON A" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors bdrdown_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT" },
			{ NAOMI_BTN1_KEY, "LASER" },
			{ NAOMI_BTN2_KEY, "SPEED" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors button12_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "BUTTON 1" },
			{ NAOMI_BTN1_KEY, "BUTTON 2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors capcom_4btn_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LIGHT PUNCH" },
			{ NAOMI_BTN1_KEY, "HEAVY PUNCH" },
			{ NAOMI_BTN3_KEY, "LIGHT KICK" },
			{ NAOMI_BTN4_KEY, "HEAVY KICK" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors cfield_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRG1" },
			{ NAOMI_BTN1_KEY, "TRG2" },
			{ NAOMI_BTN2_KEY, "TRG3" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors crackindj_inputs = {
	  {
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "FADER", Full, 0, true },
			{ NULL },
	  },
};

InputDescriptors csmash_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SMASH" },
			{ NAOMI_BTN1_KEY, "JUMP" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors doa2_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Free" },
			{ NAOMI_BTN1_KEY, "Punch" },
			{ NAOMI_BTN2_KEY, "Kick" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors dygolf_inputs = {
	  {
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors ggx_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "PUNCH" },
			{ NAOMI_BTN1_KEY, "KICK" },
			{ NAOMI_BTN2_KEY, "SLASH" },
			{ NAOMI_BTN3_KEY, "HSLASH" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors giant_gram_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Attack" },
			{ NAOMI_BTN1_KEY, "Hold" },
			{ NAOMI_BTN2_KEY, "Throw" },
			{ NAOMI_BTN3_KEY, "Move" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors guilty_gear_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "KICK" },
			{ NAOMI_BTN1_KEY, "SLASH" },
			{ NAOMI_BTN2_KEY, "HSLASH" },
			{ NAOMI_BTN3_KEY, "PUNCH" },
			{ NAOMI_BTN5_KEY, "DUST ATTACK", 0, NAOMI_BTN4_KEY },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors hmgeo_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Fire" },
			{ NAOMI_BTN1_KEY, "Attack" },
			{ NAOMI_BTN2_KEY, "Jump" },
			{ NAOMI_BTN3_KEY, "Target" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors ikaruga_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT" },
			{ NAOMI_BTN1_KEY, "CHANGE" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors illvelo_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT" },
			{ NAOMI_BTN1_KEY, "DOLL" },
			{ NAOMI_BTN2_KEY, "SPECIAL" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors jingystm_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "GUARD" },
			{ NAOMI_BTN1_KEY, "PUNCH" },
			{ NAOMI_BTN2_KEY, "KICK" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors kick4csh_inputs = {
	  {
			{ NAOMI_BTN1_KEY, "VIEW" },
			{ NAOMI_BTN2_KEY, "CHANCE" },
			{ NAOMI_START_KEY, "START/DECIDE" },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors lightgun_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRIGGER" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors mamonoro_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOOT" },
			{ NAOMI_BTN1_KEY, "SPECIAL" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors mazan_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRIGGER" },
			{ NAOMI_UP_KEY, "SELECT UP" },
			{ NAOMI_DOWN_KEY, "SELECT DOWN" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors meltyb_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LAttack" },
			{ NAOMI_BTN1_KEY, "MAttack" },
			{ NAOMI_BTN2_KEY, "HAttack" },
			{ NAOMI_BTN3_KEY, "Guard" },
			{ NAOMI_BTN5_KEY, "Quick Action", 0, NAOMI_BTN4_KEY },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors mushik_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "HIT" },
			{ NAOMI_BTN1_KEY, "PINCH" },
			{ NAOMI_BTN2_KEY, "THROW" },
			{ NAOMI_BTN3_KEY, "HIT P2", NAOMI_BTN0_KEY },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors otrigger_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "TRIGGER" },
			{ NAOMI_BTN1_KEY, "CHANGE" },
			{ NAOMI_BTN2_KEY, "JUMP" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors pstone2_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Punch" },
			{ NAOMI_BTN1_KEY, "Jump" },
			{ NAOMI_BTN2_KEY, "Attack" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors pstone_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Punch" },
			{ NAOMI_BTN1_KEY, "Kick" },
			{ NAOMI_BTN2_KEY, "Jump" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors psyvariar_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT" },
			{ NAOMI_BTN1_KEY, "BOMB" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors puyoda_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "STAR" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors puyofev_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "ROTATE1" },
			{ NAOMI_BTN1_KEY, "ROTATE2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors radirgy_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOOT" },
			{ NAOMI_BTN1_KEY, "SWORD" },
			{ NAOMI_BTN2_KEY, "SHIELD/SPECIAL" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors rhytngk_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT A" },
			{ NAOMI_BTN1_KEY, "SHOT B" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors samba_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "MARACAS R" },
			{ NAOMI_BTN1_KEY, "MARACAS L" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "MARACAS R X", Full, 0 },
			{ "MARACAS R Y", Full, 1 },
			{ "MARACAS L X", Full, 2 },
			{ "MARACAS L Y", Full, 3 },
			{ NULL },
	  },
};

InputDescriptors senko_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "ACTION" },
			{ NAOMI_BTN1_KEY, "MAIN" },
			{ NAOMI_BTN2_KEY, "SUB" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors senkosp_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "MAIN" },
			{ NAOMI_BTN1_KEY, "SUB" },
			{ NAOMI_BTN2_KEY, "MAIN+SUB" },
			{ NAOMI_BTN3_KEY, "ACTION" },
			{ NAOMI_BTN5_KEY, "OVER DRIVE", 0, NAOMI_BTN4_KEY },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors service_btns_inputs = {
	  {
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors sgtetris_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SW1" },
			{ NAOMI_BTN1_KEY, "SW2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors shaktam_inputs = {
	  {
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ NAOMI_BTN0_KEY, "SHAKE L" },
			{ NAOMI_BTN1_KEY, "SHAKE R" },
			{ NAOMI_BTN2_KEY, "KNOCK", 0, NAOMI_DOWN_KEY },
			{ NAOMI_DOWN_KEY, "DOWN", 0, NAOMI_LEFT_KEY },
			{ NAOMI_UP_KEY, "UP", 0, NAOMI_RIGHT_KEY },
			{ 0 },
	  },
	  {
			{ "TAMBOURINE X", Full, 0 },
			{ "TAMBOURINE Y", Full, 1 },
			{ "", Full, 2 },	// unused but P2 starts at axis 4
			{ "", Full, 3 },	// unused but P2 starts at axis 4
			{ NULL },
	  },
};

InputDescriptors shootout_inputs = {
	  {
			{ NAOMI_START_KEY, "START/MODE", 0, NAOMI_BTN2_KEY },
			{ NAOMI_BTN0_KEY, "TOP/VIEW" },	// !prize
			{ NAOMI_BTN1_KEY, "BET", 0, NAOMI_BTN6_KEY },	// prize only
			{ NAOMI_BTN3_KEY, "CUE ROLLER" },	// only used by emulator. press to use cue roller instead of cue aim
			{ NAOMI_UP_KEY, "ZOOM IN" },
			{ NAOMI_DOWN_KEY, "ZOOM OUT" },
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "CUE TIP U/D", Full, 1, true },
			{ "CUE TIP L/R", Full, 0, true },
			{ NULL },
	  },
};

InputDescriptors shot1234_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT1" },
			{ NAOMI_BTN1_KEY, "SHOT2" },
			{ NAOMI_BTN2_KEY, "SHOT3" },
			{ NAOMI_BTN3_KEY, "SHOT4" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors shot12_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT1" },
			{ NAOMI_BTN1_KEY, "SHOT2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors sl2007_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "PUSH 1" },
			{ NAOMI_BTN1_KEY, "PUSH 2" },
			{ NAOMI_BTN2_KEY, "PUSH 3" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors slashout_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Blade" },
			{ NAOMI_BTN1_KEY, "Charge" },
			{ NAOMI_BTN2_KEY, "Jump" },
			{ NAOMI_BTN3_KEY, "Shift" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors spkrbtl_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "BEAT" },
			{ NAOMI_BTN1_KEY, "CHARGE" },
			{ NAOMI_BTN2_KEY, "JUMP" },
			{ NAOMI_BTN3_KEY, "SHIFT" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors toukon4_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "X" },
			{ NAOMI_BTN1_KEY, "Y" },
			{ NAOMI_BTN2_KEY, "R" },
			{ NAOMI_BTN3_KEY, "A" },
			{ NAOMI_BTN5_KEY, "B", 0, NAOMI_BTN4_KEY },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors toyfight_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Punch" },
			{ NAOMI_BTN1_KEY, "Kick" },
			{ NAOMI_BTN2_KEY, "Dodge" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors trgheart_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "SHOT" },
			{ NAOMI_BTN1_KEY, "ANCHOR" },
			{ NAOMI_BTN2_KEY, "BOMB" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors vathlete_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "RUN1" },
			{ NAOMI_BTN1_KEY, "ACTION" },
			{ NAOMI_BTN2_KEY, "RUN2" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors virnba_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "PASS" },
			{ NAOMI_BTN1_KEY, "SHOOT" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

// The I/O board (jvs_namco_v226) reads the sticks and kicks itself, four
// channels a group: the descriptor gives the size of a group.
InputDescriptors wldkicks_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "BUTTON" },
			{ NAOMI_BTN3_KEY, "ENTER" },	// service mode
			{ NAOMI_UP_KEY, "UP" },	// service mode
			{ NAOMI_DOWN_KEY, "DOWN" },	// service mode
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "STICK L/R", Full, 0 },	// P1
			{ "STICK U/D", Full, 1 },
			{ "", Full, 2 },	// P2
			{ "", Full, 3 },
			{ NULL },
	  },
};

InputDescriptors wldkickspcb_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "CHANGE" },	// original label: C BUTTON
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "STICK L/R", Full, 0 },
			{ "STICK U/D", Full, 1 },
			{ "", Full, 2 },
			{ "BALL", Half, 4 },	// this is wrong, just to indicate RT is used
			{ NULL },
	  },
};

InputDescriptors wwfroyal_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "ATTACK" },
			{ NAOMI_BTN1_KEY, "GRAPPLE" },
			{ NAOMI_BTN2_KEY, "SUPPORT" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

//
// AtomisWave games
//

#define AW_BASE_BTN_DESC { AWAVE_COIN_KEY, "COIN" }, \
						 { AWAVE_TEST_KEY, "TEST" }, \
						 { AWAVE_SERVICE_KEY, "SERVICE" },
#define AW_START_DESC { AWAVE_START_KEY, "START" },

InputDescriptors ftspeed_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "BOOST" },
			{ AWAVE_UP_KEY, "HIGH GEAR" },
			{ AWAVE_DOWN_KEY, "LOW GEAR" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "STEERING WHEEL", Full, 0 },
			{ "GAS PEDAL", Half, 4 },
			{ "BRAKE PEDAL", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors kofnw_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LIGHT PUNCH" },
			{ AWAVE_BTN1_KEY, "STRONG PUNCH" },
			{ AWAVE_BTN2_KEY, "HEAT MODE" },
			{ AWAVE_BTN3_KEY, "LIGHT KICK" },
			{ AWAVE_BTN4_KEY, "STRONG KICK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  }
};

InputDescriptors kofxi_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LIGHT PUNCH" },
			{ AWAVE_BTN1_KEY, "STRONG PUNCH" },
			{ AWAVE_BTN2_KEY, "SPECIAL ATTACK" },
			{ AWAVE_BTN3_KEY, "LIGHT KICK" },
			{ AWAVE_BTN4_KEY, "STRONG KICK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  }
};

InputDescriptors maxspeed_inputs = {
	  {
			{ AWAVE_UP_KEY, "HIGH SHIFT" },
			{ AWAVE_DOWN_KEY, "LOW SHIFT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "STEERING", Full, 0 },
			{ "ACCELERATOR", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors ngbc_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LIGHT PUNCH" },
			{ AWAVE_BTN1_KEY, "STRONG PUNCH" },
			{ AWAVE_BTN2_KEY, "SWAP CHARACTERS" },
			{ AWAVE_BTN3_KEY, "LIGHT KICK" },
			{ AWAVE_BTN4_KEY, "STRONG KICK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  }
};

InputDescriptors samsptk_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LIGHT SLASH" },
			{ AWAVE_BTN1_KEY, "MEDIUM SLASH" },
			{ AWAVE_BTN2_KEY, "STRONG SLASH" },
			{ AWAVE_BTN3_KEY, "KICK" },
			{ AWAVE_BTN4_KEY, "SPECIAL EVASION" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  }
};

InputDescriptors blockpong_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "SHOT1" },
			{ AWAVE_BTN1_KEY, "SHOT2" },
			{ AWAVE_BTN2_KEY, "SHOT3" },
			{ AWAVE_BTN3_KEY, "SHOT4" },
			{ AWAVE_BTN4_KEY, "SHOT5" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "ANALOG X", Full, 0, true },
			{ "ANALOG Y", Full, 1, false },	// not used?
			{ NULL },
	  },
};

//
// NAOMI 2 Games
//
InputDescriptors aw_lightgun_inputs = {
	  {
			{ AWAVE_TRIGGER_KEY, "TRIGGER" },
			{ AWAVE_BTN0_KEY, "PUMP" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors aw_shot123_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "SHOT1" },
			{ AWAVE_BTN1_KEY, "SHOT2" },
			{ AWAVE_BTN2_KEY, "SHOT3" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors basschal_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "ROTATE LEFT" },
			{ AWAVE_BTN1_KEY, "ROTATE RIGHT" },
			{ AWAVE_LEFT_KEY, "LEFT POINT" },
			{ AWAVE_RIGHT_KEY, "RIGHT POINT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors demofist_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "GUARD" },
			{ AWAVE_BTN1_KEY, "ATTACK" },
			{ AWAVE_BTN2_KEY, "JUMP" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors dolphin_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "SHOOT" },
			{ AWAVE_BTN1_KEY, "JUMP" },
			{ AWAVE_BTN2_KEY, "SPECIAL" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors fotns_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LP" },
			{ AWAVE_BTN1_KEY, "HP" },
			{ AWAVE_BTN2_KEY, "BOOST" },
			{ AWAVE_BTN3_KEY, "LK" },
			{ AWAVE_BTN4_KEY, "HK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors ggx15_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "KICK" },
			{ AWAVE_BTN1_KEY, "SLASH" },
			{ AWAVE_BTN2_KEY, "HSLASH" },
			{ AWAVE_BTN3_KEY, "PUNCH" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors guilty_gear_aw_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "KICK" },
			{ AWAVE_BTN1_KEY, "SLASH" },
			{ AWAVE_BTN2_KEY, "HSLASH" },
			{ AWAVE_BTN3_KEY, "PUNCH" },
			{ AWAVE_BTN4_KEY, "DUST ATTACK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors kov7sprt_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LIGHT ATTACK" },
			{ AWAVE_BTN1_KEY, "HEAVY ATTACK" },
			{ AWAVE_BTN2_KEY, "JUMP" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors mslug6_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "SHOOT" },
			{ AWAVE_BTN1_KEY, "JUMP" },
			{ AWAVE_BTN2_KEY, "GRENADE" },
			{ AWAVE_BTN3_KEY, "METAL SLUG ATTACK" },
			{ AWAVE_BTN4_KEY, "SWITCH WEAPONS" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors rumblef_inputs = {
	  {
			{ AWAVE_BTN0_KEY, "LP" },
			{ AWAVE_BTN1_KEY, "SP" },
			{ AWAVE_BTN2_KEY, "Dodge" },
			{ AWAVE_BTN3_KEY, "LK" },
			{ AWAVE_BTN4_KEY, "SK" },
			{ AWAVE_UP_KEY, "UP" },
			{ AWAVE_DOWN_KEY, "DOWN" },
			{ AWAVE_LEFT_KEY, "LEFT" },
			{ AWAVE_RIGHT_KEY, "RIGHT" },
			AW_START_DESC
			AW_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			NO_ANALOG_AXES
			{ NULL },
	  },
};

InputDescriptors vf4_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "PUNCH" },
			{ NAOMI_BTN1_KEY, "KICK" },
			{ NAOMI_BTN2_KEY, "GUARD" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors vs2_2k_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "LONG PASS" },
			{ NAOMI_BTN1_KEY, "SHOOT" },
			{ NAOMI_BTN2_KEY, "SHORT PASS" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors beachspi_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "A" },
			{ NAOMI_BTN1_KEY, "B" },
			{ NAOMI_UP_KEY, "UP" },
			{ NAOMI_DOWN_KEY, "DOWN" },
			{ NAOMI_LEFT_KEY, "LEFT" },
			{ NAOMI_RIGHT_KEY, "RIGHT" },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ NULL },
	  },
};

InputDescriptors initd_inputs = {
	  {
			{ NAOMI_DOWN_KEY, "VIEW" },
			{ NAOMI_BTN0_KEY, "GEAR UP", NAOMI_UP_KEY },		// This button uses P2 inputs for P1
			{ NAOMI_BTN1_KEY, "GEAR DOWN", NAOMI_DOWN_KEY },	// This button uses P2 inputs for P1
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors clubkart_inputs = {
	  {
			{ NAOMI_DOWN_KEY, "VIEW" },	// not the prize versions (they use START)
			{ NAOMI_BTN1_KEY, "BET" },	// the prize versions only
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors kingrt66_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "HORN" },
			{ NAOMI_BTN1_KEY, "WIPER" },
			{ NAOMI_DOWN_KEY, "VIEW" },
			{ NAOMI_BTN2_KEY, "SHIFT L", NAOMI_DOWN_KEY },		// This button uses P2 inputs for P1
			{ NAOMI_BTN3_KEY, "SHIFT H", NAOMI_UP_KEY },		// This button uses P2 inputs for P1
			{ NAOMI_BTN4_KEY, "SHIFT R", NAOMI_LEFT_KEY | NAOMI_DOWN_KEY },
															// This button uses P2 inputs for P1
			{ NAOMI_BTN5_KEY, "MIC SWITCH", 0, NAOMI_BTN2_KEY },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLE", Full, 0 },
			{ "ACCEL", Half, 4 },
			{ "BRAKE", Half, 5 },
			{ NULL },
	  },
};

InputDescriptors wldrider_inputs = {
	  {
			{ NAOMI_UP_KEY, "PUSH", 0, NAOMI_LEFT_KEY },
			{ NAOMI_DOWN_KEY, "PULL", 0, NAOMI_RIGHT_KEY },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "HANDLEBAR", Full, 0 },
			{ "ACCELERATOR", Half, 4 },
			{ "FRONT BRAKE", Half, 5 },
			{ "REAR BRAKE", Half, 6 },	// nothing is mapped to it: let go
			{ NULL },
	  },
};

InputDescriptors soulsurfer_inputs = {
	  {
			{ NAOMI_LEFT_KEY, "LEFT", 0, NAOMI_BTN0_KEY },
			{ NAOMI_RIGHT_KEY, "RIGHT", 0, NAOMI_BTN1_KEY },
			{ NAOMI_BTN4_KEY, "FLOORMAT", 0, 0xffffffff },		// always on
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "SWING", Full, 2 },
			{ "ROLL", Full, 0 },
			{ "PITCH", Full, 1, true },
			{ NULL },
	  },
};

#ifdef NAOMI_MULTIBOARD
InputDescriptors drvsim_inputs = {
	  {
			{ NAOMI_BTN0_KEY, "Turn R" },
			{ NAOMI_BTN1_KEY, "Turn L" },
			{ NAOMI_BTN2_KEY, "Shift 2" },
			{ NAOMI_BTN3_KEY, "Shift 3" },
			{ NAOMI_BTN4_KEY, "O/D Switch" },
			{ NAOMI_BTN5_KEY, "S-Brake" },
			{ NAOMI_BTN6_KEY, "IG-START", NAOMI_LEFT_KEY },
			{ NAOMI_BTN7_KEY, "Shift 0", NAOMI_BTN0_KEY },
			{ NAOMI_UP_KEY, "Horn" },
			{ NAOMI_DOWN_KEY, "Light 0" },
			{ NAOMI_LEFT_KEY, "Light 1" },
			{ NAOMI_RIGHT_KEY, "Light 2" },
			{ NAOMI_BTN8_KEY, "BELT", NAOMI_BTN2_KEY, 0xffffffff },
			NAO_START_DESC
			NAO_BASE_BTN_DESC
			{ 0 },
	  },
	  {
			{ "", Full, 0xffffffff, true },	// Master audio volume
			{ "Accelerator", Half, 4 },
			{ "Brake", Half, 5 },
			{ "Clutch", Half, 6 },
			{ "Wheel", Full, 0, true },	// only read via ffb board, not jvs
			{ NULL },
	  },
};
#endif

#endif /* CORE_HW_NAOMI_NAOMI_ROMS_INPUT_H_ */
