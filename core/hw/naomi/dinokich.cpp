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
//
// The IC card reader of dinokich and loveber3cn
//
#include <ctime>
#include <string.h>
#include <streams/file_stream.h>
#include "dinokich.h"
#include "dinokich_card.h"
#include "savewriter.h"

namespace systemsp
{

constexpr u32 DinokichCardReader::EEPROM_SIZE;
constexpr u32 DinokichCardReader::MAX_FRAME;
constexpr u32 DinokichCardReader::TX_SIZE;
constexpr u32 DinokichCardReader::STATE_SIZE;

static constexpr u8 REQ_SOH = 0xaa;
static constexpr u8 RESP_SOH = 0x55;
// what the game finds on a card
static constexpr u32 CARD_ID = 0x20;		// "BKACHN" (Dinosaur King) or "BLXCHN" (Love and Berry), and the card's number
static constexpr u32 CARD_CHECK = 0x30;		// 0x12
static constexpr u32 CARD_BLOCK = 0x40;		// the encrypted block, 64 bytes
static constexpr u32 CARD_SUM = 0x80;		// the low byte of the sum of the block's bytes
static constexpr u32 CARD_CHECK_VALUE = 0x3fe;	// the block's check value, 2 bytes
// the status of an answer
static constexpr u8 ST_OK = 0x00;
static constexpr u8 ST_NO_CARD = 0x10;
static constexpr u8 ST_BAD_ADDRESS = 0x70;
static constexpr u8 ST_BAD_COMMAND = 0x80;

static u8 xorSum(const u8 *p, u32 len)
{
	u8 s = 0;
	for (u32 i = 0; i < len; i++)
		s ^= p[i];
	return s;
}

static u8 nextRandom(u32& random)
{
	random = random * 1103515245u + 12345u;
	return random >> 16;
}

DinokichCardReader::DinokichCardReader(SerialPort *port, const std::string& gameName, const std::string& path1, const std::string& path2)
	: port(port)
{
	paths[0] = path1;
	paths[1] = path2;
	u32 random = (u32)time(nullptr) * 2654435761u;
	for (int i = 0; i < 2; i++)
	{
		retro_atomic_int_init(&failed[i], 0);
		memset(slots[i].eeprom, 0, EEPROM_SIZE);
		RFILE *f = filestream_open(paths[i].c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f != nullptr)
		{
			if (filestream_read(f, slots[i].eeprom, EEPROM_SIZE) != (int64_t)EEPROM_SIZE)
				WARN_LOG(NAOMI, "IC card %d: truncated read", i + 1);
			filestream_close(f);
		}
		else
		{
			makeCard(i, gameName.compare(0, 4, "love") == 0, random);
			saveCard(i);
		}
		// both cards are in, all the time
		slots[i].flags = PRESENT;
	}
	memset(rx, 0, sizeof(rx));
	memset(tx, 0, sizeof(tx));
	port->setPipe(this);
}

DinokichCardReader::~DinokichCardReader()
{
	// (the flags are this one's: the writer is done with them first)
	save_writer_drain();
}

/* A new card, with all the credits a card can have: the game takes one a
 * play. The contents, key, pad and check value are random, as is the
 * card's number. */
void DinokichCardReader::makeCard(int slot, bool loveBerry, u32& random)
{
	INFO_LOG(NAOMI, "Creating new IC card %d", slot + 1);
	dinokich_plain_t plain;
	memset(&plain, 0, sizeof(plain));
	memcpy(plain.serial_prefix, "DINO", sizeof(plain.serial_prefix));
	memcpy(plain.tag, "CHINA-CARD!", sizeof(plain.tag));
	plain.credits = 999;
	for (u8& b : plain.pad)
		b = nextRandom(random);
	for (u8& b : plain.bufout)
		b = nextRandom(random);
	u8 key[8];
	for (u8& b : key)
		b = nextRandom(random);

	u8 *eeprom = slots[slot].eeprom;
	memset(eeprom, 0, EEPROM_SIZE);
	dinokich_encode_card(&plain, key, &eeprom[CARD_BLOCK]);
	eeprom[CARD_CHECK_VALUE] = plain.bufout[0];
	eeprom[CARD_CHECK_VALUE + 1] = plain.bufout[1];
	// read on its own, and the card is refused without it
	memcpy(&eeprom[CARD_ID], loveBerry ? "BLXCHN" : "BKACHN", 6);
	for (int i = 0; i < 4; i++)
		eeprom[CARD_ID + 9 + i] = nextRandom(random);
	eeprom[CARD_CHECK] = 0x12;
	eeprom[CARD_SUM] = (u8)dinokich_sum_bytes(&eeprom[CARD_BLOCK], 64);
}

void DinokichCardReader::saveCard(int slot)
{
	if (retro_atomic_load_acquire_int(&failed[slot]) && retro_atomic_exchange_int(&failed[slot], 0))
		WARN_LOG(NAOMI, "Can't save IC card to %s", paths[slot].c_str());
	if (!save_writer_put_file(paths[slot].c_str(), slots[slot].eeprom, EEPROM_SIZE, &failed[slot]))
		WARN_LOG(NAOMI, "Can't save IC card to %s", paths[slot].c_str());
}

void DinokichCardReader::write(u8 v)
{
	// (a frame starts with its first byte: anything before it is dropped)
	if (rxLen != 0 || v == REQ_SOH)
	{
		rx[rxLen++] = v;
		if (rxLen >= 4 && rxLen == rx[3] + 5u)
		{
			handleFrame();
			rxLen = 0;
		}
	}
	port->updateStatus();
}

u8 DinokichCardReader::read()
{
	if (txCount == 0)
		return 0;
	const u8 v = tx[txHead];
	txHead = (txHead + 1) % TX_SIZE;
	txCount--;
	if (txCount == 0)
		port->updateStatus();
	return v;
}

void DinokichCardReader::reset()
{
	rxLen = 0;
	txHead = 0;
	txCount = 0;
	memset(rx, 0, sizeof(rx));
	memset(tx, 0, sizeof(tx));
	for (Slot& slot : slots)
		slot.flags = PRESENT;
}

void DinokichCardReader::handleFrame()
{
	if (xorSum(rx, rxLen) != 0)
	{
		WARN_LOG(NAOMI, "IC card reader: bad checksum, frame dropped");
		return;
	}
	u8 payload[255];
	u32 payloadLen = 0;
	const u8 status = command(rx, rxLen, payload, payloadLen);
	sendResponse(status, payload, payloadLen);
}

/* A command's answer: its status, and @payloadLen bytes in @payload. The
 * request is @len bytes: AA cmd 00 N, N bytes from f[4], its xor. A byte a
 * command reads past those is not there, and the command is refused. */
u8 DinokichCardReader::command(const u8 *f, u32 len, u8 *payload, u32& payloadLen)
{
	// the request's bytes, the xor not counted, are f[0] to f[last]
	const u32 last = len - 2;
	const u8 cmd = f[1];
	const int slot = last >= 4 ? f[4] - 1 : -1;
	switch (cmd)
	{
	case 0xb0:
		// The firmware's version. The game reads its digits at 5 and 7, and
		// only needs an answer.
		if (len != 12 || f[10] != 0x08)
			return ST_BAD_COMMAND;
		memcpy(payload, "DK_C 1.0", 8);
		payloadLen = 8;
		return ST_OK;

	case 0xc0:
		// how a slot did, for its lamp
		return ST_OK;

	case 0xb9:
		// which slots have a card: bit 1 the first, bit 0 the second
		return ((slots[0].flags & PRESENT) ? 2 : 0) | ((slots[1].flags & PRESENT) ? 1 : 0);

	case 0x67:
		{
			// The reader's answer to the salt the game sends, which the game
			// unscrambles and wants above 0
			if (last < 10 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			const u8 ok = 1;
			dinokich_md5_scramble(&f[9], &ok, 1, payload);
			payloadLen = 1;
			return ST_OK;
		}

	case 0xb2:
	case 0xb3:
		// the card's power on and off
		if (slot != 0 && slot != 1)
			return ST_BAD_COMMAND;
		if (cmd == 0xb2)
		{
			if (!(slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			slots[slot].flags |= POWERED;
		}
		else
			slots[slot].flags &= ~POWERED;
		return ST_OK;

	case 0x61:
		{
			/* Read: slot at 4, address at 7 (high) and 8, salt at 9 and 10,
			 * length at 12. Whether the card is powered does not matter: the
			 * game reads the card's number when it has just turned it off. */
			if (last < 12 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			const u32 addr = (f[7] << 8) | f[8];
			const u32 n = f[12];
			if (n == 0 || addr + n > EEPROM_SIZE)
				return ST_BAD_ADDRESS;
			dinokich_md5_scramble(&f[9], &slots[slot].eeprom[addr], n, payload);
			payloadLen = n;
			return ST_OK;
		}

	case 0x60:
	case 0x6d:
		{
			/* Write (0x6d is the same, for 16 bytes at 0x20 of the first
			 * card): slot at 4, address at 7 and 8, salt at 9 and 10, the
			 * data from 11 to the end, so N - 7 bytes of it. */
			if (slot != 0 && slot != 1)
				return ST_BAD_COMMAND;
			if (!(slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			if (f[3] < 8)
				return ST_BAD_ADDRESS;
			const u32 addr = (f[7] << 8) | f[8];
			const u32 n = f[3] - 7u;
			if (addr + n > EEPROM_SIZE)
				return ST_BAD_ADDRESS;
			dinokich_md5_scramble(&f[9], &f[11], n, &slots[slot].eeprom[addr]);
			saveCard(slot);
			return ST_OK;
		}

	case 0x64:
	case 0x63:
		{
			/* The card's check value, scrambled, at 11 and 12: 0x64 shows the
			 * reader the game knows it, which is taken as it is; 0x63 writes
			 * it at the end of the card. */
			if (last < 12 || (slot != 0 && slot != 1))
				return ST_BAD_COMMAND;
			if (!(slots[slot].flags & PRESENT))
				return ST_NO_CARD;
			if (cmd == 0x63)
			{
				dinokich_md5_scramble(&f[9], &f[11], 2, &slots[slot].eeprom[CARD_CHECK_VALUE]);
				saveCard(slot);
			}
			return ST_OK;
		}

	default:
		WARN_LOG(NAOMI, "IC card reader: command %02x unknown", cmd);
		return ST_BAD_COMMAND;
	}
}

void DinokichCardReader::sendResponse(u8 status, const u8 *payload, u32 len)
{
	if (txCount + len + 5 > TX_SIZE)
	{
		WARN_LOG(NAOMI, "IC card reader: nothing reads what the reader sends; dropped");
		return;
	}
	u8 x = 0;
	const u8 head[4] = { RESP_SOH, status, 0, (u8)len };
	for (u32 i = 0; i < len + 5; i++)
	{
		const u8 b = i < 4 ? head[i] : i < len + 4 ? payload[i - 4] : x;
		x ^= b;
		tx[(txHead + txCount) % TX_SIZE] = b;
		txCount++;
	}
}

/* In the state from V25, for dinokich and loveber3cn only: for each card,
 * whether it is in and powered, and its 1 KB; the request so far; what is
 * still to be sent. STATE_SIZE (2,828) bytes. */
void DinokichCardReader::serializeV25(void **data, unsigned int *total_size)
{
	for (Slot& slot : slots)
	{
		LIBRETRO_S(slot.flags);
		LIBRETRO_SA(slot.eeprom, EEPROM_SIZE);
	}
	LIBRETRO_S(rxLen);
	LIBRETRO_SA(rx, MAX_FRAME);
	LIBRETRO_S(txHead);
	LIBRETRO_S(txCount);
	LIBRETRO_SA(tx, TX_SIZE);
}

void DinokichCardReader::unserializeV25(void **data, unsigned int *total_size)
{
	for (Slot& slot : slots)
	{
		LIBRETRO_US(slot.flags);
		LIBRETRO_USA(slot.eeprom, EEPROM_SIZE);
		slot.flags &= PRESENT | POWERED;
	}
	LIBRETRO_US(rxLen);
	LIBRETRO_USA(rx, MAX_FRAME);
	LIBRETRO_US(txHead);
	LIBRETRO_US(txCount);
	LIBRETRO_USA(tx, TX_SIZE);
	// (a request so far is one that has not ended yet: anything else is not one)
	if (rxLen > MAX_FRAME || (rxLen != 0 && rx[0] != REQ_SOH) || (rxLen >= 4 && rxLen >= rx[3] + 5u))
		rxLen = 0;
	if (txHead >= TX_SIZE || txCount > TX_SIZE)
	{
		txHead = 0;
		txCount = 0;
	}
}

/* A state from before V25 was saved when the reader was not emulated: the
 * cards are what their files have, and nothing is under way. */
void DinokichCardReader::stateBeforeV25()
{
	reset();
}

}	// namespace systemsp
