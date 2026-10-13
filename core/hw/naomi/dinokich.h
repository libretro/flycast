/*
	Copyright 2026 flyinghead

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
 * The IC card reader of Dinosaur King (China) and Love and Berry 3 (China),
 * made by Hirocon: two card slots, on the ROM board's first serial port.
 *
 * A request is AA cmd 00 N, N bytes, and a byte that makes the xor of the
 * whole frame 0; the answer is 55 status 00 N, N bytes, and the same. The
 * cards are 1 KB EEPROMs, kept in the save directory as the RFID cards of
 * the other sets are (<game>-<region>.rfid1 and .rfid2), and made when
 * there are none.
 */
#pragma once
#include <string>
#include <retro_atomic.h>
#include "systemsp.h"

namespace systemsp
{

class DinokichCardReader : public SerialPipe
{
public:
	DinokichCardReader(SerialPort *port, const std::string& gameName, const std::string& path1, const std::string& path2);
	~DinokichCardReader() override;

	void write(u8 v) override;
	int available() override { return txCount; }
	u8 read() override;
	void reset() override;
	// (nothing in the part of the state that is V24's: see serializeV25())
	void serialize(void **, unsigned int *) override { }
	void unserialize(void **, unsigned int *) override { }

	// What the reader has in a save state from V25: STATE_SIZE bytes
	void serializeV25(void **data, unsigned int *total_size);
	void unserializeV25(void **data, unsigned int *total_size);
	void stateBeforeV25();

	static constexpr u32 EEPROM_SIZE = 1024;
	static constexpr u32 MAX_FRAME = 255 + 5;
	static constexpr u32 TX_SIZE = 512;
	static constexpr u32 STATE_SIZE = 2 * (1 + EEPROM_SIZE) + 2 + MAX_FRAME + 4 + TX_SIZE;

private:
	void handleFrame();
	u8 command(const u8 *f, u32 len, u8 *payload, u32& payloadLen);
	void sendResponse(u8 status, const u8 *payload, u32 len);
	void makeCard(int slot, bool loveBerry, u32& random);
	void saveCard(int slot);

	SerialPort *port;
	std::string paths[2];
	retro_atomic_int_t failed[2];

	enum : u8 {
		PRESENT = 1,
		POWERED = 2,
	};
	struct Slot {
		u8 flags;
		u8 eeprom[EEPROM_SIZE];
	} slots[2];

	// the request so far
	u8 rx[MAX_FRAME];
	u16 rxLen = 0;
	// what the reader has to send
	u8 tx[TX_SIZE];
	u16 txHead = 0;
	u16 txCount = 0;
};

}	// namespace systemsp
