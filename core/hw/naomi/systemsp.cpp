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
// based on mame code:
// license:BSD-3-Clause
// copyright-holders:David Haywood, MetalliC
#include <ctime>
#include <streams/file_stream.h>
#include "systemsp.h"
#include "naomi_cart.h"
#include "hw/flashrom/flashrom.h"
#include "hw/maple/maple_devs.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "imgread/chd_image.h"
#include "savewriter.h"

extern SRamChip sys_nvmem_sram;
extern char eeprom_file[PATH_MAX];
extern char nvmem_file[PATH_MAX];
extern char g_roms_dir[PATH_MAX];
extern char g_base_name[128];
extern std::vector<sched_list> sch_list;
extern u32 kcode[4];
extern s8 joyx[4];
extern f32 mo_x_abs[4];
extern f32 mo_y_abs[4];

bool naomi_cart_systemsp;

namespace systemsp
{

SystemSpCart *SystemSpCart::Instance;
constexpr u32 SystemSpCart::INT_UART1;
constexpr u32 SystemSpCart::INT_UART2;
constexpr u32 SystemSpCart::INT_DIMM;
constexpr u32 SystemSpCart::INT_ATA;
constexpr u32 SystemSpCart::PIPE_STATE_SIZE;
constexpr u32 SystemSpCart::SECTOR_SIZE;
constexpr u16 SystemSpCart::DIMM_STATUS;
constexpr u32 SystemSpCart::NETMEM_SIZE;
constexpr u32 SystemSpCart::NETMEM_PAGE;
constexpr u32 SystemSpCart::NETMEM_STATE_PAGES;
constexpr u16 SystemSpCart::NET_OK;
constexpr u16 SystemSpCart::NET_ERROR;
int SystemSpCart::ataSchedId = -1;
static int touchSchedId = -1;

static u16 le16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

/* A scheduler event as a save state has it, and back: one that the state
 * says is waiting more than @most cycles from now (a bad state) is due at
 * once. */
static void saveEvent(int id, void **data, unsigned int *total_size)
{
	s32 start = -1, end = -1;
	if (id >= 0)
	{
		start = sch_list[id].start;
		end = sch_list[id].end;
	}
	LIBRETRO_S(start);
	LIBRETRO_S(end);
}

static void loadEvent(int id, u32 most, void **data, unsigned int *total_size)
{
	s32 start = -1, end = -1;
	LIBRETRO_US(start);
	LIBRETRO_US(end);
	if (id < 0)
		return;
	if (end != -1)
	{
		const u32 now = sh4_sched_now();
		if ((u32)end - now > most)
		{
			start = (s32)now;
			end = start == -1 ? 0 : start;
		}
	}
	sch_list[id].start = start;
	sch_list[id].end = end;
}

//
// What the ROM board's serial devices send: a queue of fixed size, which
// what does not fit is dropped from.
//
struct ByteQueue
{
	static constexpr u32 SIZE = 512;
	u8 buf[SIZE];
	u16 head = 0;
	u16 count = 0;

	bool empty() const { return count == 0; }
	int size() const { return count; }
	void clear() { head = 0; count = 0; memset(buf, 0, sizeof(buf)); }
	void push(u8 v)
	{
		if (count == SIZE)
		{
			WARN_LOG(NAOMI, "System SP serial port: nothing reads what the device sends; dropped");
			return;
		}
		buf[(head + count) % SIZE] = v;
		count++;
	}
	void push(const u8 *p, u32 len)
	{
		for (u32 i = 0; i < len; i++)
			push(p[i]);
	}
	u8 pop()
	{
		if (count == 0)
			return 0;
		const u8 v = buf[head];
		head = (head + 1) % SIZE;
		count--;
		return v;
	}
	void serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_S(head);
		LIBRETRO_S(count);
		LIBRETRO_SA(buf, SIZE);
	}
	void unserialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_US(head);
		LIBRETRO_US(count);
		LIBRETRO_USA(buf, SIZE);
		if (head >= SIZE)
			head = 0;
		if (count > SIZE)
			count = 0;
	}
};

constexpr u8 DINOKING_CHIP_DATA[128] = {
    0x12, 0x34, 0x56, 0x78, // Serial No.0
    0x31, 0x00, 0x86, 0x07, // Serial No.1
    0x00, 0x00, 0x00, 0x00, // Key
    0x05, 0x40, 0x00, 0xAA, // Extend  Extend  Access  Mode
    0x23, 0xFF, 0xFF, 0xFF, // Counter4  Counter3  Counter2  Counter1
    0x00, 0x00, 0x00, 0x00, // User Data (first set date: day bits 0-4, month bits 5-8, year bits 9-... + 2000)
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x23, 0xFF, 0xFF, 0xFF, // User Data (max counters)
};
constexpr u8 LOVEBERRY_CHIP_DATA[128] = {
    0x10, 0xc5, 0x10, 0x96, // Serial No.0
    0xab, 0x2f, 0xf1, 0x74, // Serial No.1
    0x99, 0x33, 0x78, 0xa7, // Key
    0x05, 0x77, 0x00, 0xAA, // Extend  Extend  Access  Mode
    0x23, 0xFF, 0xFF, 0xFF, // Counter4  Counter3  Counter2  Counter1
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x00, 0x00, 0x00, 0x00, // User Data
    0x23, 0xFF, 0xFF, 0xFF, // User Data
};

//
// RS232C I/F board (838-14244) connected to RFID Chip R/W board (838-14243 aka Maxell Picochet ME-MR23)
//
class RfidReaderWriter : public SerialPipe
{
	enum Response : u8 {
		OK = 0xa,
		RETRY = 0xca,
		NG = 0x3a,
		NOT = 0x6a,
	};

	enum Command : u8 {
		REQ = 0x4e,
		SEL = 0x2e,
		READ = 0xe,
		WRITE = 0xce,
		WRITE_NOP = 0x8e,
		WRITE_ST = 0xee,
		HALT = 0xae,
		COUNT = 0x6e,
		RESET = 0x7e,
		TEST = 0x5e,
		CHANGE = 0x3e,
	};
	static constexpr u8 RF_OFF = 0x80;

	enum State : u8 {
		Off,
		Connecting,
		Connected,
		FullRead
	};

public:
	RfidReaderWriter(SerialPort *port, int index, const std::string& gameName, int region, const std::string& path)
		: port(port), index(index), gameName(gameName), region(region), path(path)
	{
		retro_atomic_int_init(&failed, 0);
		// (the numbers a new card is made with: they are kept in the state,
		// so a card made while the game runs is the same card again when a
		// frame is run again from a state)
		random = (u32)time(nullptr) * 2654435761u + index;
		port->setPipe(this);

		bool loaded = false;
		RFILE *f = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f != nullptr)
		{
			if (filestream_read(f, cardData, sizeof(cardData)) != (int64_t)sizeof(cardData))
				WARN_LOG(NAOMI, "Rfid card %d: truncated read", index);
			filestream_close(f);
			loaded = true;
		}
		if (!loaded)
			makeNewCard();
	}

	void write(u8 v) override
	{
		if (expectedBytes > 0)
		{
			if (recvLen < sizeof(recvBuffer))
				recvBuffer[recvLen++] = v;
			if (recvLen < expectedBytes)
				return;

			switch (recvBuffer[0])
			{
			case TEST:
				if (recvBuffer[1] == RF_OFF)
				{
					state = Off;
					toSend.push(OK);
				}
				else if (state == Off)
				{
					state = Connecting;
					toSend.push(NOT);
				}
				else
					toSend.push(OK);
				break;
			case SEL:
				if ((state == Connected || state == FullRead)
						&& (recvBuffer[5] != 0 || recvBuffer[6] != 0 || recvBuffer[7] != 0 || recvBuffer[8] != 0))
					state = FullRead;
				else
					state = Connected;
				toSend.push(OK);
				break;
			case COUNT:
				{
					u8 newVal = 0;
					switch (recvBuffer[1])
					{
					case 0xc0:
						newVal = --cardData[16];
						break;
					case 0x30:
						newVal = --cardData[17];
						break;
					case 0x0c:
						newVal = --cardData[18];
						break;
					case 0x03:
						newVal = --cardData[19];
						if (newVal == 0)
							makeNewCard();
						break;
					}
					toSend.push(OK);
					toSend.push(newVal);
					saveData();
					break;
				}
			case WRITE:
				// (a row past the chip's 32 is not written: the count of rows
				// is the game's, and went on past the end of the card's data)
				if (rowCounter < sizeof(cardData) / 4)
				{
					memcpy(&cardData[4 * rowCounter], &recvBuffer[1], 4);
					saveData();
				}
				toSend.push(OK);
				break;
			case WRITE_NOP:
			case WRITE_ST:
			case CHANGE:
			case HALT:
				toSend.push(OK);
				break;
			default:
				break;
			}
			expectedBytes = 0;
		}
		else
		{
			switch (v)
			{
			case TEST:
				expect(v, 2);
				break;
			case CHANGE: // change baud rate
				expect(v, 1);
				break;
			case RESET:
				state = Off;
				toSend.push(OK);
				break;
			case HALT:
				expect(v, 4); // ser0
				break;
			case READ:
				toSend.push(OK);
				if (state == FullRead) {
					toSend.push(cardData, sizeof(cardData));
				}
				else
				{
					toSend.push(cardData, 8);
					static const u8 maskedData[120] = { 0, 0, 0, 0, 0, 0, 0, 0xaa };
					toSend.push(maskedData, sizeof(maskedData));
				}
				break;
			case SEL:
				expect(v, 8);	// ser0 key (key is calc'ed from ser1)
				break;
			case REQ:
				if (state == Connecting)
				{
					state = Connected;
					toSend.push(RETRY);
				}
				else if (state == Connected || state == FullRead)
				{
					toSend.push(OK);
					toSend.push(cardData, 4);	// ser0
				}
				else
					toSend.push(RETRY);
				break;
			case COUNT:
				expect(v, 1);
				break;
			case WRITE:
				rowCounter++;
				expect(v, 4);
				break;
			case WRITE_ST:
			case WRITE_NOP:
				if (v == WRITE_ST)
					rowCounter = 0;
				else
					rowCounter++;
				expect(v, 4);
				break;
			default:
				INFO_LOG(NAOMI, "UART%d write data out: unknown cmd %x", index, v);
				toSend.push(NG);
				break;
			}
		}
		port->updateStatus();
	}

	int available() override {
		return toSend.size();
	}

	u8 read() override
	{
		const u8 b = toSend.pop();
		if (toSend.empty())
			port->updateStatus();
		return b;
	}

	void reset() override
	{
		toSend.clear();
		expectedBytes = 0;
		recvLen = 0;
		memset(recvBuffer, 0, sizeof(recvBuffer));
		state = Off;
		rowCounter = 0;
	}

	void serialize(void **data, unsigned int *total_size) override
	{
		toSend.serialize(data, total_size);
		LIBRETRO_S(expectedBytes);
		LIBRETRO_S(recvLen);
		LIBRETRO_SA(recvBuffer, sizeof(recvBuffer));
		u8 st = state;
		LIBRETRO_S(st);
		LIBRETRO_S(rowCounter);
		LIBRETRO_SA(cardData, sizeof(cardData));
		LIBRETRO_S(random);
	}

	void unserialize(void **data, unsigned int *total_size) override
	{
		toSend.unserialize(data, total_size);
		LIBRETRO_US(expectedBytes);
		LIBRETRO_US(recvLen);
		LIBRETRO_USA(recvBuffer, sizeof(recvBuffer));
		u8 st = Off;
		LIBRETRO_US(st);
		LIBRETRO_US(rowCounter);
		LIBRETRO_USA(cardData, sizeof(cardData));
		LIBRETRO_US(random);
		if (expectedBytes > sizeof(recvBuffer))
			expectedBytes = 0;
		if (recvLen > sizeof(recvBuffer))
			recvLen = 0;
		state = st <= FullRead ? (State)st : Off;
	}

	~RfidReaderWriter() override
	{
		// (the flag is this one's: the writer is done with it first)
		save_writer_drain();
	}

private:
	void expect(u8 cmd, int bytes)
	{
		expectedBytes = bytes + 1;
		recvLen = 0;
		recvBuffer[recvLen++] = cmd;
	}

	u8 nextRandom()
	{
		random = random * 1103515245u + 12345u;
		return random >> 16;
	}

	void makeNewCard()
	{
		INFO_LOG(NAOMI, "Creating new RFID card");
		if (gameName.substr(0, 6) == "dinoki")
		{
			memcpy(&cardData[0], &DINOKING_CHIP_DATA[0], sizeof(DINOKING_CHIP_DATA));
			for (int i = 0; i < 4; i++)
				cardData[i] = nextRandom();

			u32 ser1 = (nextRandom() << 24) | (nextRandom() << 16) | (nextRandom() << 8) | nextRandom();
			ser1 &= ~0xc0000000;
			if (region == 1) // USA (dinoking, dinokior)
				ser1 |= 0x40000000;
			ser1 = dinoShuffle(ser1);
			cardData[4] = ser1 >> 24;
			cardData[5] = ser1 >> 16;
			cardData[6] = ser1 >> 8;
			cardData[7] = ser1;
		}
		else
		{
			memcpy(&cardData[0], &LOVEBERRY_CHIP_DATA[0], sizeof(LOVEBERRY_CHIP_DATA));
			for (int i = 0; i < 4; i++)
				cardData[i] = nextRandom();

			cardData[5] = nextRandom();
			cardData[7] = nextRandom();
			u32 ser1 = (cardData[4] << 24) | (cardData[5] << 16) | (cardData[6] << 8) | cardData[7];
			if (region == 2) // Export
			{
				ser1 = loveberyDeobfuscate(ser1);
				ser1 &= ~0xc0000000;
				ser1 = loveberyObfuscate(ser1);
				cardData[4] = ser1 >> 24;
				cardData[5] = ser1 >> 16;
				cardData[6] = ser1 >> 8;
				cardData[7] = ser1;
			}

			ser1 ^= 0x321c89d3;
			cardData[8] = ser1 >> 24;
			cardData[9] = ser1 >> 16;
			cardData[10] = ser1 >> 8;
			cardData[11] = ser1;
		}
	}

	void saveData()
	{
		if (retro_atomic_load_acquire_int(&failed) && retro_atomic_exchange_int(&failed, 0))
			WARN_LOG(NAOMI, "Can't save RFID card to %s", path.c_str());
		if (!save_writer_put_file(path.c_str(), cardData, sizeof(cardData), &failed))
			WARN_LOG(NAOMI, "Can't save RFID card to %s", path.c_str());
	}

	static u32 dinoShuffle(u32 v) {
		return ((v & 1) << 22) | ((v & 2) << 28) | ((v & 4) << 1) | ((v & 8) << 3)
				| ((v & 0x10) << 21) | ((v & 0x20) << 10) | ((v & 0x40) << 1) | ((v & 0x80) >> 7)
				| ((v & 0x100) << 22) | ((v & 0x200) >> 1) | ((v & 0x400) >> 1) | ((v & 0x800) << 17)
				| ((v & 0x1000) << 4) | ((v & 0x2000) << 18) | ((v & 0x4000) >> 4) | ((v & 0x8000) >> 13)
				| ((v & 0x10000) >> 15) | ((v & 0x20000) >> 4) | ((v & 0x40000) << 1) | ((v & 0x80000) >> 8)
				| ((v & 0x100000) >> 6) | ((v & 0x200000) << 2) | ((v & 0x400000) >> 18) | ((v & 0x800000) >> 2)
				| ((v & 0x1000000) >> 12) | ((v & 0x2000000) >> 7) | ((v & 0x4000000) << 1) | ((v & 0x8000000) >> 3)
				| ((v & 0x10000000) >> 11) | ((v & 0x20000000) >> 9) | ((v & 0x40000000) >> 25) | ((v & 0x80000000) >> 5);
	}

	static u32 loveberyDeobfuscate(u32 v)
	{
		v ^= 0xa35ec5e3;
		// 87654321 -> 34781256
		return ((v & 0xf00000) >> 20) | ((v & 0xf0000) >> 12)
				| ((v & 0xf0) << 4) | ((v & 0xf) << 12)
				| ((v & 0xf0000000) >> 12) | ((v & 0xf000000) >> 4)
				| ((v & 0xf000) << 12) | ((v & 0xf00) << 20);
	}
	static u32 loveberyObfuscate(u32 v)
	{
		v = ((v & 0xf) << 20) | ((v & 0xf0) << 12)
				| ((v & 0xf00) >> 4) | ((v & 0xf000) >> 12)
				| ((v & 0xf0000) << 12) | ((v & 0xf00000) << 4)
				| ((v & 0xf000000) >> 12) | ((v & 0xf0000000) >> 20);
		return v ^ 0xa35ec5e3;
	}

	SerialPort *port;
	const int index;
	const std::string gameName;
	const int region;
	const std::string path;
	retro_atomic_int_t failed;
	ByteQueue toSend;
	u8 cardData[128];
	u8 expectedBytes = 0;
	u8 recvBuffer[10] = {};
	u8 recvLen = 0;
	State state = Off;
	u8 rowCounter = 0;
	u32 random;
};

//
// Isshoni Wanwan Waiwai Puppy touchscreen
//
class Touchscreen : public SerialPipe
{
public:
	Touchscreen(SerialPort *port) : port(port)
	{
		port->setPipe(this);
		touchSchedId = sh4_sched_register(0, schedCallback);
	}

	~Touchscreen() override
	{
		if (touchSchedId >= 0)
			sh4_sched_request(touchSchedId, -1);
	}

	void write(u8 v) override
	{
		if (v == '\r')
		{
			if (recvLen >= 2 && recvBuffer[0] == 1)
			{
				toSend.push(1);
				if (recvLen == 3 && recvBuffer[1] == 'O' && recvBuffer[2] == 'I')
				{
					// get name
					static const u8 name[] { 'A','3','0','9','9','9' };
					toSend.push(name, sizeof(name));
				}
				else if (recvLen == 3 && recvBuffer[1] == 'N' && recvBuffer[2] == 'M')
				{
					// unit verify
					static const u8 id[] { 'E','X','I','I','-','7','7','2','0','S','C',' ','R','e','v',' ','3','.','0' };
					toSend.push(id, sizeof(id));
				}
				else if (recvLen == 3 && recvBuffer[1] == 'U' && recvBuffer[2] == 'V')
				{
					// reset
					static const u8 resp[] { 'Q','M','V','*','*','*','0','0' };
					toSend.push(resp, sizeof(resp));
				}
				else if (recvLen == 2 && recvBuffer[1] == 'R')
				{
					toSend.push('0');
					sh4_sched_request(touchSchedId, SCHED_CYCLES);
				}
				else
				{
					toSend.push('0');
				}
				toSend.push('\r');
				port->updateStatus();

				// FIXME
				if (recvLen == 2 && recvBuffer[1] == 'Z')
					sendPosition(0);
			}
			else
			{
				WARN_LOG(NAOMI, "\\r ignored. buf size %d", (int)recvLen);
			}
			recvLen = 0;
		}
		else
		{
			if (recvLen == 9)
			{
				if (!memcmp(&recvBuffer[0], "Ua0000000", 9))
					sendPosition(1);
				else
					WARN_LOG(NAOMI, "Unknown command %.9s", (const char *)&recvBuffer[0]);
				recvLen = 0;
			}
			else
			{
				recvBuffer[recvLen++] = v;
			}
		}
	}

	int available() override {
		return toSend.size();
	}

	u8 read() override
	{
		const u8 data = toSend.pop();
		if (toSend.empty())
			port->updateStatus();
		return data;
	}

	void reset() override
	{
		toSend.clear();
		recvLen = 0;
		memset(recvBuffer, 0, sizeof(recvBuffer));
		lastPosX = ~0u;
		lastPosY = ~0u;
		lastButton = 0;
		if (touchSchedId >= 0)
			sh4_sched_request(touchSchedId, -1);
	}

	void serialize(void **data, unsigned int *total_size) override
	{
		toSend.serialize(data, total_size);
		LIBRETRO_S(recvLen);
		LIBRETRO_SA(recvBuffer, sizeof(recvBuffer));
		LIBRETRO_S(lastPosX);
		LIBRETRO_S(lastPosY);
		LIBRETRO_S(lastButton);
		saveEvent(touchSchedId, data, total_size);
	}

	void unserialize(void **data, unsigned int *total_size) override
	{
		toSend.unserialize(data, total_size);
		LIBRETRO_US(recvLen);
		LIBRETRO_USA(recvBuffer, sizeof(recvBuffer));
		LIBRETRO_US(lastPosX);
		LIBRETRO_US(lastPosY);
		LIBRETRO_US(lastButton);
		loadEvent(touchSchedId, SCHED_CYCLES, data, total_size);
		if (recvLen > 9)
			recvLen = 0;
	}

private:
	void sendPosition(int type)
	{
		// where the light gun of the first port points, 0-1023
		int ax = (int)mo_x_abs[0];
		int ay = (int)mo_y_abs[0];
		ax = std::min(std::max(ax, 0), 639);
		ay = std::min(std::max(ay, 0), 479);
		const u32 x = (640 - ax) * 1023 / 639;
		const u32 y = ay * 1023 / 479;

		if (type == 1)
		{
			u8 msg[9];
			msg[0] = 'U';
			msg[1] = 'T';
			msg[2] = 0x20;	// bit 0 and 1 are checked
			msg[3] = x & 0xff;
			msg[4] = (x >> 8) & 0x1f;
			msg[5] = y & 0xff;
			msg[6] = (y >> 8) & 0x1f;
			msg[7] = 0;	// z pos
			u8 crc = 0xaa;
			for (int i = 0; i < 8; i++)
				crc += msg[i];
			msg[8] = crc;
			toSend.push(msg, sizeof(msg));
			port->updateStatus();
		}
		else
		{
			const u8 button = (kcode[0] & NAOMI_BTN0_KEY) == 0;
			if (button != lastButton || x != lastPosX || y != lastPosY)
			{
				// bit 6 is touch down
				toSend.push(button ? 0xc0 : 0x80);
				toSend.push((x & 7) << 4);
				toSend.push((x >> 3) & 0x7f);
				toSend.push((y & 7) << 4);
				toSend.push((y >> 3) & 0x7f);
				lastButton = button;
				lastPosX = x;
				lastPosY = y;
				port->updateStatus();
			}
		}
	}

	static int schedCallback(int tag, int cycles, int jitter)
	{
		if (SystemSpCart::Instance == nullptr)
			return 0;
		Touchscreen *ts = (Touchscreen *)SystemSpCart::Instance->touchscreen;
		if (ts == nullptr)
			return 0;
		ts->sendPosition(0);
		return SCHED_CYCLES;
	}

	SerialPort *port;
	ByteQueue toSend;
	u8 recvBuffer[10] = {};
	u8 recvLen = 0;
	u32 lastPosX = ~0u;
	u32 lastPosY = ~0u;
	u8 lastButton = 0;

	static constexpr u32 SCHED_CYCLES = SH4_MAIN_CLOCK / 60;
};

u8 SerialPort::readReg(u32 addr)
{
	switch ((addr & 0x3f) / 4)
	{
	case 0: // data in
		if (pipe != nullptr)
			return pipe->read();
		else
			return 0;
	case 1: // out buffer len
		return 0;
	case 2: // in buffer len
		if (pipe != nullptr)
			return pipe->available();
		else
			return 0;
	case 3: // errors?
	case 4: // unknown
	case 5: // flow control
		return 0;
	case 6: // status. bit 3: receive buffer not empty
		if (pipe != nullptr && pipe->available() > 0)
			return 8;
		else
			return 0;
	case 7: // interrupt status/mask?
	case 8: // unknown
	case 9: // control?
	case 10: // baudrate (lsb)
	case 11: // baudrate (msb)
		return 0;
	default:
		INFO_LOG(NAOMI, "Unknown UART%d port %x", index, addr);
		return 0;
	}
}

void SerialPort::writeReg(u32 addr, u8 v)
{
	switch ((addr & 0x3f) / 4)
	{
	case 0: // data out
		if (pipe != nullptr)
			pipe->write(v);
		else
			INFO_LOG(NAOMI, "UART%d out: %02x %c", index, v, v);
		break;

	case 1: // out buffer len
	case 2: // in buffer len
	case 3: // errors?
	case 4: // unknown
	case 5: // flow control
	case 6: // status
	case 7: // interrupt status/mask?
	case 8: // unknown
	case 9: // control?
		break;

	case 10: // baudrate (lsb)
	case 11: // baudrate (msb)
		flush();
		break;

	default:
		INFO_LOG(NAOMI, "Unknown UART%d port %x", index, addr);
		break;
	}
}

void SerialPort::updateStatus()
{
	cart->updateInterrupt(index == 1 ? SystemSpCart::INT_UART1 : SystemSpCart::INT_UART2);
}

//
// The I/O ports: the cabinet's switches, and what the game drives
//
class IOPortManager
{
public:
	virtual ~IOPortManager() = default;
	virtual u8 getCN9_17_24() = 0;
	virtual u8 getCN9_25_32() = 0;
	virtual u8 getCN9_33_40() = 0;
	virtual u8 getCN9_41_48() = 0;
	virtual u8 getCN9_49_56() = 0;
	virtual u8 getCN10_9_16() = 0;
	virtual void setCN9_33_40(u8 v) {}
	virtual void setCN9_49_56(u8 v) {}
	virtual void setCN10_17_24(u8 v) {}

	void reset()
	{
		outputs = 0;
		for (Sensor& s : sensors)
			s = Sensor();
	}
	void serialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_S(outputs);
		for (Sensor& s : sensors)
		{
			LIBRETRO_S(s.a);
			LIBRETRO_S(s.b);
		}
	}
	void unserialize(void **data, unsigned int *total_size)
	{
		LIBRETRO_US(outputs);
		for (Sensor& s : sensors)
		{
			LIBRETRO_US(s.a);
			LIBRETRO_US(s.b);
		}
	}

protected:
	static constexpr u64 msAsCycles(u64 ms) {
		return (u64)SH4_MAIN_CLOCK * ms / 1000;
	}

	/* A sensor that a motor turns past, or that is on for a while when
	 * something passes it. a and b: where it is in its turn and when it
	 * was last looked at; or when it was set off (0: it was not). */
	struct Sensor
	{
		u64 a = ~0ull;
		u64 b = 0;
	};

	// On and Period in SH4 ms. (Half way round at first.)
	static bool periodic(Sensor& s, u64 on, u64 period, bool actuator = true)
	{
		const u64 now = sh4_sched_now64();
		if (s.a == ~0ull)
			s.a = msAsCycles(period) / 2;
		if (actuator)
			s.a = (s.a + now - s.b) % msAsCycles(period);
		s.b = now;
		return s.a < msAsCycles(on);
	}

	// Width in SH4 ms
	static bool pulse(Sensor& s, u64 width, bool actuator)
	{
		if (!actuator)
		{
			s.b = 0;
			return false;
		}
		const u64 now = sh4_sched_now64();
		if (s.b == 0)
			s.b = now;
		return now - s.b < msAsCycles(width);
	}

	/* The switches of the first two players, active low, as NAOMI keys,
	 * with the game's remapping: a switch of the player's that is another
	 * one of his own (target), or the first player's switch that is one of
	 * the second's (p2_mask). */
	static void getInputState(u32 kc[2])
	{
		kc[0] = kcode[0];
		kc[1] = kcode[1];
		if (naomi_game_inputs == NULL)
			return;
		for (int i = 0; naomi_game_inputs->buttons[i].mask != 0; i++)
		{
			const ButtonDescriptor& bd = naomi_game_inputs->buttons[i];
			if (bd.target == 0xffffffff)
				continue;
			if (bd.p2_mask != 0)
			{
				if ((kc[0] & bd.mask) == 0)
					kc[1] &= ~bd.p2_mask;
			}
			else if (bd.target != 0)
			{
				if ((kc[0] & bd.mask) == 0)
					kc[0] &= ~bd.target;
				if ((kc[1] & bd.mask) == 0)
					kc[1] &= ~bd.target;
			}
		}
	}

	// the motors and lamps the game has turned on, as each kind of cabinet has them
	u8 outputs = 0;
	Sensor sensors[4];
};

/* The buttons as the board has them, upstream's names for them:
 * button 1-6 are BTN0-BTN5, the medal checkers 7 and 8 BTN6 and BTN7. */
class DefaultIOManager : public IOPortManager
{
public:
	// IN_PORT0
	u8 getCN9_17_24() override
	{
		u8 v = 0xff;
		// 0: P1 start
		// 1: P2 start
		// 2: P1 right
		// 3: P2 right
		// 4: P1 left
		// 5: P2 left
		// 6: P1 up
		// 7: P2 up
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_START_KEY))
			v &= ~0x01;
		if (!(kc[1] & NAOMI_START_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x04;
		if (!(kc[1] & NAOMI_RIGHT_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_LEFT_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_UP_KEY))
			v &= ~0x40;
		if (!(kc[1] & NAOMI_UP_KEY))
			v &= ~0x80;
		return v;
	}

	// IN_PORT3 / IO-0
	u8 getCN9_25_32() override
	{
		u8 v = 0xff;
		// 0: P1 down
		// 1: P2 down
		// 2: P1 button 1
		// 3: P2 button 1
		// 4: P1 button 2
		// 5: P2 button 2
		// 6: P1 button 3
		// 7: P2 button 3
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x01;
		if (!(kc[1] & NAOMI_DOWN_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v &= ~0x04;
		if (!(kc[1] & NAOMI_BTN0_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_BTN1_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_BTN1_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_BTN2_KEY))
			v &= ~0x40;
		if (!(kc[1] & NAOMI_BTN2_KEY))
			v &= ~0x80;
		return v;
	}

	// IO-1
	u8 getCN9_33_40() override
	{
		return 0xff;
	}

	// IN_PORT1
	u8 getCN9_41_48() override
	{
		u8 v = 0xff;
		// 0: P1 service
		// 2: P1 test
		// 4: P1 coin
		// 5: P2 coin
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_SERVICE_KEY))
			v &= ~0x01;
		if (!(kc[0] & NAOMI_TEST_KEY))
			v &= ~0x04;
		if (!(kc[0] & NAOMI_COIN_KEY))
			v &= ~0x10;
		if (!(kc[1] & NAOMI_COIN_KEY))
			v &= ~0x20;
		return v;
	}

	// IN_PORT4 / OUT-0
	u8 getCN9_49_56() override
	{
		u8 v = 0;
		// FIXME these are outputs??
		// 0: P1 coin meter
		// 1: P2 coin meter
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_COIN_KEY))
			v |= 1;
		if (!(kc[1] & NAOMI_COIN_KEY))
			v |= 2;
		return v;
	}

	// IN G-PORT
	u8 getCN10_9_16() override
	{
		// dinosaur king, love & berry:
		// 0: 232c sel status 1 (not used)
		// 1: 232c sel status 2 (not used)
		// 2: card status1 (not used)
		// 3: card status2 (not used)
		// 4: card status3 (not used)
		return 0;
	}
};

/* (The card is never inserted: there is no input for it here, nor in
 * upstream's libretro core.) */
class CardReaderIOManager : public DefaultIOManager
{
public:
	u8 getCN9_33_40() override
	{
		// dinosaur king, love & berry:
		// 0: P1 card set (not used)
		// 2: CD1 input ok (active low)
		// 4: CD1 card jam (active low)
		// 6: CD1 empty (active low)
		return 0xfb;
	}
};

class IsshoniIOManager : public CardReaderIOManager
{
public:
	u8 getCN9_17_24() override {
		return 0xff;
	}

	u8 getCN9_25_32() override {
		return 0xff;
	}
};

class BattleRacerIOManager : public CardReaderIOManager
{
public:
	u8 getCN9_17_24() override
	{
		u8 v = 0xff;
		// 0: P1 right
		// 1: P2 right
		// 2: P1 right
		// 3: P2 right
		// 4: P1 left
		// 5: P2 left
		// 6: P1 left
		// 7: P2 left
		// (the wheel is the left stick, 0 to 255 with 128 in the middle:
		// as a 16-bit axis it is turned at -1638 and at -16384, and the
		// same to the right)
		const int x0 = ((int)(u8)joyx[0] - 128) * 256;
		const int x1 = ((int)(u8)joyx[1] - 128) * 256;
		if (x0 <= -16384)
			v &= ~0x40;
		if (x0 <= -1638)
			v &= ~0x10;
		if (x0 >= 1638)
			v &= ~0x04;
		if (x0 >= 16384)
			v &= ~0x01;

		if (x1 <= -16384)
			v &= ~0x80;
		if (x1 <= -1638)
			v &= ~0x20;
		if (x1 >= 1638)
			v &= ~0x08;
		if (x1 >= 16384)
			v &= ~0x02;
		return v;
	}

	u8 getCN9_33_40() override
	{
		// 0: P1 button
		// 1: P2 button
		// 2: CD1 input ok (active low)
		// those aren't confirmed: assuming the same as dinoking and love & berry
		// 4: CD1 card jam (active low)
		// 6: CD1 empty (active low)
		u8 v = 0xfb;
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v &= ~0x01;
		if (!(kc[1] & NAOMI_BTN0_KEY))
			v &= ~0x02;
		return v;
	}
};

class HopperIOManager : public DefaultIOManager
{
	// outputs: bit 0 the hopper; sensors: 0, 1 the coin chutes, 2 the hopper's

	// IN_PORT1
	u8 getCN9_41_48() override
	{
		u8 v = 0xbf;
		// 0: P1 service
		// 2: P1 test
		// 4: P1 coin
		// 5: P2 coin
		// 6: must be 0 (hopper: not connected / mount error)
		// 7: must be 1 (hopper: coin2 chute jammed)
		u32 kc[2];
		getInputState(kc);
		const bool hopperActive = outputs & 1;
		if (!(kc[0] & NAOMI_SERVICE_KEY))
			v &= ~0x01;
		if (!(kc[0] & NAOMI_TEST_KEY))
			v &= ~0x04;
		if (pulse(sensors[0], 100, !(kc[0] & NAOMI_COIN_KEY)))
			v &= ~0x10;
		if (pulse(sensors[1], 100, !(kc[1] & NAOMI_COIN_KEY)))
			v &= ~0x20;
		if (periodic(sensors[2], 10, 50, hopperActive) && hopperActive)
			v |= 0x40;
		return v;
	}

	void setCN9_49_56(u8 v) override
	{
		// 4: hopper
		// 7: ?
		outputs = (v & 0x10) != 0;
	}
};

class ManpukuIOManager : public HopperIOManager
{
	// IN_PORT0
	u8 getCN9_17_24() override
	{
		u8 v = 0xff;
		// 4: Left
		// 5: Middle
		// 6: Right
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x10;
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x20;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x40;
		return v;
	}
};

// Yataimura Kingyosukui, Yataimura Shateki
class KingyoIOManager : public HopperIOManager
{
	// IN_PORT0
	u8 getCN9_17_24() override
	{
		u8 v = 0xff;
		// 0: Left
		// 1: Right
		// 2: Down
		// 3: Up
		// 4: Button 1
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_LEFT_KEY))
			v &= ~0x01;
		if (!(kc[0] & NAOMI_RIGHT_KEY))
			v &= ~0x02;
		if (!(kc[0] & NAOMI_DOWN_KEY))
			v &= ~0x04;
		if (!(kc[0] & NAOMI_UP_KEY))
			v &= ~0x08;
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v &= ~0x10;
		return v;
	}
};

// Notes:
// COUNT HOPPER JAM - LOCK SENSOR ON 1 SEC
// -> fixed by timing countHopperRot sensor to 0.5 s max
// HOPPER EMPTY/JAM - MOTOR DRIVE BUT SENSOR OFF
// -> fixed by resetting hopper sensor when hopper motor is on.
// SLOPE SENSOR TIMEOUT - MOTOR DRIVE BUT SENSOR OFF
// -> set slope sensor low when slope motor on
// ILLEGAL CHACKER IN - MANY CHACKER IN BUT NO COIN IN
// -> fixed by setting COIN L or R when checker on
// COIN IN RATIO TOO HIGH - CTRL COIN IN RATIO OVER 105%
// -> not enough coin L/R vs. checker in?
class MedalIOManager : public DefaultIOManager
{
	// outputs: the motors and the solenoid (bits below);
	// sensors: 0 hopper, 1 pusher, 2 count hopper rotation
	enum {
		JPMECHA_MTR = 1,
		COUNT_HOPPER_MTR = 2,
		HOPPER_MTR = 4,
		PUSHER_MTR = 8,
		SLOPE_MTR = 0x10,
		JPMECHA_SOLENOID = 0x20,
	};

	// IN_PORT0
	u8 getCN9_17_24() override
	{
		u8 v = 0x50;
		// 0: slope sensor up (active high)
		// 1: slope sensor low (active high)
		// 2: count hopper rot. (active high)
		// 3: count hopper sensor (active high)
		// 4: hopper sensor
		// 5: pusher sensor (active high)
		// 6: tilt bob sensor
		// 7: checker 9 (active high)
		u32 kc[2];
		getInputState(kc);
		if (periodic(sensors[2], 100, 500, outputs & COUNT_HOPPER_MTR))
			v |= 0x04;	// count hopper rot sensor
		// FIXME generates FIELD P/O TOO HIGH errors. Activate the sensor 50% of the time?
		if (periodic(sensors[0], 50, 250, outputs & HOPPER_MTR))
			v &= ~0x10; // hopper sensor
		if (outputs & SLOPE_MTR)
			v |= 2;	// slope sensor low
		else
			v |= 1;	// slope sensor up
		if (periodic(sensors[1], 100, 4300, outputs & PUSHER_MTR))
			v |= 0x20;	// pusher sensor
		if (!(kc[0] & NAOMI_START_KEY))
			v |= 0x80; // checker 9
		return v;
	}

	// IN_PORT1
	u8 getCN9_41_48() override
	{
		u8 v = 0x2f;
		// 0: service/reset sw
		// 1: left sw
		// 2: test sw
		// 3: center sw
		// 4: door sensor up (active high)
		// 5: right sw
		// 6: door sensor left (active high)
		// 7: tilt sensor br (active high)
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_SERVICE_KEY))	// service/reset
			v &= ~0x01;
		if (!(kc[0] & NAOMI_TEST_KEY))		// test
			v &= ~0x04;
		if (!(kc[0] & NAOMI_LEFT_KEY))		// left sw
			v &= ~0x02;
		if (!(kc[0] & NAOMI_DOWN_KEY))		// center sw
			v &= ~0x08;
		if (!(kc[0] & NAOMI_RIGHT_KEY))		// right sw
			v &= ~0x20;
		return v;
	}

	// IN_PORT3 / IO-0
	u8 getCN9_25_32() override
	{
		u8 v = 0;
		// 0-7: checker 1-8 (active high)
		u32 kc[2];
		getInputState(kc);
		if (!(kc[0] & NAOMI_BTN0_KEY))
			v |= 0x01;
		if (!(kc[0] & NAOMI_BTN1_KEY))
			v |= 0x02;
		if (!(kc[0] & NAOMI_BTN2_KEY))
			v |= 0x04;
		if (!(kc[0] & NAOMI_BTN3_KEY))
			v |= 0x08;
		if (!(kc[0] & NAOMI_BTN4_KEY))
			v |= 0x10;
		if (!(kc[0] & NAOMI_BTN5_KEY))
			v |= 0x20;
		if (!(kc[0] & NAOMI_BTN6_KEY))
			v |= 0x40;
		if (!(kc[0] & NAOMI_BTN7_KEY))
			v |= 0x80;
		return v;
	}

	// IN G-PORT
	u8 getCN10_9_16() override
	{
		u8 v = 0;
		// 0: jp mecha sensor u (active high)
		// 2: jp mecha sensor d (active high)
		// 3: co. full sensor (active high)
		// 4: coin in L (active high)
		// 5: coin in R (active high)
		// 7: jp solenoid sensor (active high)
		u32 kc[2];
		getInputState(kc);
		if (outputs & JPMECHA_MTR)
			v |= 4;
		else
			v |= 1;
		// FIXME not enough coin ins if multiple checkers pressed at once -> COIN IN RATIO TOO HIGH
		const u32 left = NAOMI_BTN0_KEY | NAOMI_BTN1_KEY | NAOMI_BTN2_KEY | NAOMI_BTN3_KEY | NAOMI_BTN4_KEY | NAOMI_BTN5_KEY;
		const u32 right = NAOMI_BTN6_KEY | NAOMI_BTN7_KEY | NAOMI_START_KEY;
		if ((kc[0] & left) != left)
			v |= 0x10;	// coin in L
		if ((kc[0] & right) != right)
			v |= 0x20;	// coin in R
		if (outputs & JPMECHA_SOLENOID)
			v |= 0x80;
		return v;
	}

	// IO-1 (CN9 33-40)
	void setCN9_33_40(u8 v) override
	{
		// 3: jp mecha motor
		// 4: count hopper motor
		// 5: hopper motor
		// 6: pusher motor
		// 7: slope motor
		// (all active low)
		outputs &= JPMECHA_SOLENOID;
		if (!(v & 0x08))
			outputs |= JPMECHA_MTR;
		if (!(v & 0x10))
			outputs |= COUNT_HOPPER_MTR;
		if (!(v & 0x20))
			outputs |= HOPPER_MTR;
		if (!(v & 0x40))
			outputs |= PUSHER_MTR;
		if (!(v & 0x80))
			outputs |= SLOPE_MTR;
	}

	// OUT-0 (CN9 49-56)
	void setCN9_49_56(u8 v) override
	{
		// 4: sw lamp L
		// 5: sw lamp R
		// 6: patrol lamp
		// 7: jackpot lamp
	}

	// OUT-1 (CN10 17-24)
	void setCN10_17_24(u8 v) override
	{
		// 0: sw.lamp c
		// 1: jp solenoid
		// 6: side lamp L
		// 7: side lamp R
		if (v & 2)
			outputs |= JPMECHA_SOLENOID;
		else
			outputs &= ~JPMECHA_SOLENOID;
	}
};

//
// The serial EEPROM
//
void AT93C46SerialEeprom::writeCLK(bool state)
{
	// CS asserted and CLK rising
	if (!clk && state && cs)
	{
		if (dataOutBits > 0)
			dataOutBits--;
		if (dataOutBits == 0)
		{
			if (commandBits == 0 && !di)
			{
				// the start bit must be 1
			}
			else
			{
				command = (command << 1) | (di ? 1 : 0);
				commandBits++;
				if (commandBits == 9)
				{
					switch ((command >> 6) & 3)
					{
					case 0: // write enable/disable, erase all, write all
						switch ((command >> 4) & 3)
						{
						case 0: // disable write
							writeEnable = false;
							commandBits = 0;
							command = 0;
							break;
						case 1: // write all
							expected = 3 + 6 + 16; // 6 bits of address, 16 bits of data
							break;
						case 2: // erase all
							if (writeEnable)
							{
								memset(data, 0xff, sizeof(data));
								dirty = true;
							}
							commandBits = 0;
							command = 0;
							break;
						case 3: // enable write
							writeEnable = true;
							commandBits = 0;
							command = 0;
							break;
						}
						break;
					case 1: // write
						expected = 3 + 6 + 16; // 6 bits of address, 16 bits of data
						break;
					case 2: // read
						dataOut = data[commandAddress() * 2] | (data[commandAddress() * 2 + 1] << 8);
						dataOutBits = 17; // actually 16 but 0 means no data
						commandBits = 0;
						command = 0;
						break;
					case 3: // erase
						if (writeEnable)
						{
							data[commandAddress() * 2] = 0xff;
							data[commandAddress() * 2 + 1] = 0xff;
							dirty = true;
						}
						commandBits = 0;
						command = 0;
						break;
					}
				}
				else if (expected > 0 && commandBits == expected)
				{
					const u16 v = commandData();
					switch ((command >> (commandBits - 3)) & 3)
					{
					case 0: // write all
						if (writeEnable)
						{
							for (u32 i = 0; i < sizeof(data); i += 2)
							{
								data[i] = (u8)v;
								data[i + 1] = v >> 8;
							}
							dirty = true;
						}
						break;
					case 1: // write
						if (writeEnable)
						{
							data[commandAddress() * 2] = (u8)v;
							data[commandAddress() * 2 + 1] = v >> 8;
							dirty = true;
						}
						break;
					}
					commandBits = 0;
					command = 0;
					expected = 0;
				}
			}
		}
	}
	clk = state;
}

void AT93C46SerialEeprom::reset()
{
	cs = false;
	clk = false;
	di = false;
	command = 0;
	commandBits = 0;
	expected = 0;
	writeEnable = false;
	dataOut = 0;
	dataOutBits = 0;
}

void AT93C46SerialEeprom::serialize(void **data, unsigned int *total_size)
{
	u8 pins = (cs ? 1 : 0) | (clk ? 2 : 0) | (di ? 4 : 0) | (writeEnable ? 8 : 0);
	LIBRETRO_SA(this->data, sizeof(this->data));
	LIBRETRO_S(pins);
	LIBRETRO_S(command);
	LIBRETRO_S(commandBits);
	LIBRETRO_S(expected);
	LIBRETRO_S(dataOut);
	LIBRETRO_S(dataOutBits);
}

void AT93C46SerialEeprom::unserialize(void **data, unsigned int *total_size)
{
	u8 pins = 0;
	LIBRETRO_USA(this->data, sizeof(this->data));
	LIBRETRO_US(pins);
	LIBRETRO_US(command);
	LIBRETRO_US(commandBits);
	LIBRETRO_US(expected);
	LIBRETRO_US(dataOut);
	LIBRETRO_US(dataOutBits);
	cs = pins & 1;
	clk = pins & 2;
	di = pins & 4;
	writeEnable = pins & 8;
	if (expected != 0 && expected != 3 + 6 + 16)
		expected = 0;
	if (commandBits >= 3 + 6 + 16 || (expected == 0 && commandBits >= 9))
	{
		commandBits = 0;
		command = 0;
	}
	if (dataOutBits > 17)
		dataOutBits = 0;
}

//
// The cartridge's cipher, as M4Cartridge has it: a round of it for every
// 16-bit word, once for the cartridge's whole life.
//
static const u8 k_sboxes[4][16] = {
	{9,8,2,11,1,14,5,15,12,6,0,3,7,13,10,4},
	{2,10,0,15,14,1,11,3,7,12,13,8,4,9,5,6},
	{4,11,3,8,7,2,15,13,1,5,14,9,6,12,0,10},
	{1,13,8,2,0,5,6,14,4,11,15,10,12,3,7,9}
};
static u16 spOneRound[0x10000];
static bool spOneRoundDone;

static void encInit()
{
	if (spOneRoundDone)
		return;
	for (int round_input = 0; round_input < 0x10000; round_input++)
	{
		u8 input_nibble[4];
		u8 output_nibble[4];

		for (int nibble_idx = 0; nibble_idx < 4; ++nibble_idx) {
			input_nibble[nibble_idx] = (round_input >> (nibble_idx * 4)) & 0xf;
			output_nibble[nibble_idx] = 0;
		}

		u8 aux_nibble = input_nibble[3];
		for (int nibble_idx = 0; nibble_idx < 4; ++nibble_idx) { // 4 s-boxes per round
			aux_nibble ^= k_sboxes[nibble_idx][input_nibble[nibble_idx]];
			for (int i = 0; i < 4; ++i)  // diffusion of the bits
				output_nibble[(nibble_idx - i) & 3] |= aux_nibble & (1 << i);
		}

		u16 result = 0;
		for (int nibble_idx = 0; nibble_idx < 4; ++nibble_idx)
			result |= (output_nibble[nibble_idx] << (4 * nibble_idx));

		spOneRound[round_input] = result;
	}
	spOneRoundDone = true;
}

u16 SystemSpCart::decrypt(u16 w)
{
	u16 dec = encIv;
	encIv = spOneRound[(u16)(w ^ encIv) ^ subkey1] ^ subkey1;
	dec ^= spOneRound[encIv ^ subkey2] ^ subkey2;
	if (++encCounter == 16)
	{
		encCounter = 0;
		encIv = 0;
	}
	return dec;
}

template<typename T>
T readMemArea0(u32 addr)
{
	if (SystemSpCart::Instance == nullptr)
		return (T)~0;
	return SystemSpCart::Instance->readMemArea0<T>(addr);
}
template u32 readMemArea0<>(u32 addr);
template u16 readMemArea0<>(u32 addr);
template u8 readMemArea0<>(u32 addr);

template<typename T>
T SystemSpCart::readMemArea0(u32 addr)
{
	addr &= 0x1fffff;
	if (addr < 0x10000)
	{
		// banked access to ROM/NET board address space, mainly backup SRAM and ATA
		u32 offset = (addr & 0xffff) | ((bank & 0x3fff) << 16);
		if ((bank & 0x3f00) == 0x3900)
		{
			// SRAM: an 8-bit device on a 16-bit bus
			if (sizeof(T) == 1)
			{
				if (offset & 1)
					return 0xff;
				else
					return sys_nvmem_sram.Read(offset / 2, 1);
			}
			else if (sizeof(T) == 2)
				return (T)(0xff00 | sys_nvmem_sram.Read(offset / 2, 1));
			else
				return (T)(0xff00ff00 | sys_nvmem_sram.Read(offset / 2, 1) | (sys_nvmem_sram.Read(offset / 2 + 1, 1) << 16));
		}
		else if ((bank & 0x3f00) == 0x3a00)
		{
			// CF IDE registers
			switch (addr & 0xffff)
			{
			case 0x00: // RD data
				if (sizeof(T) == 2)
				{
					addr &= ~1;
					u16 ret = readMemArea0<u8>(addr);
					ret |= readMemArea0<u8>(addr) << 8;
					if (bank & 0x4000)
						// decrypt
						ret = decrypt(ret);
					return (T)ret;
				}
				else
				{
					u8 data = 0;
					if (ata.bufferIndex < SECTOR_SIZE)
					{
						data = ata.buffer[ata.bufferIndex++];
						if (ata.bufferIndex == SECTOR_SIZE)
						{
							if (ata.sectorCount > 1)
							{
								// read next sector
								ata.sectorCount--;
								ata.sectorNumber++;
								// (the carry goes on to the head only from a cylinder that went round)
								if (ata.sectorNumber == 0)
								{
									ata.cylinder++;
									if (ata.cylinder == 0)
										ata.driveHead = (ata.driveHead & 0xf0) | ((ata.driveHead + 1) & 0xf);
								}
								readSectors();
								updateInterrupt(INT_ATA);
							}
							else
							{
								// no more data
								ata.status &= ~0x08;	// DRQ
							}
						}
					}
					return data;
				}
			case 0x04: // error
				return 0;
			case 0x08: // sector count
				return ata.sectorCount;
			case 0x0c: // sector no
				return ata.sectorNumber;
			case 0x10: // cylinder (lsb)
				return ata.cylinder & 0xff;
			case 0x14: // cylinder (msb)
				return ata.cylinder >> 8;
			case 0x18: // select card/head
				return ata.driveHead;
			case 0x1c: // status
				{
					// BUSY RDY  DWF  DSC  DRQ  CORR   0  ERR
					const u8 status = ata.status;
					// TODO correct?
					ata.status &= ~0x10;	// DSC
					return status;
				}
			default:
				INFO_LOG(NAOMI, "systemsp::read(%x) CF IDE unknown reg", addr);
				return (T)~0;
			}
		}
		else if ((bank & 0x3f00) == 0x3b00)
		{
			// CF IDE AltStatus/Device Ctrl register
			if ((addr & 0xffff) == 0x18)
				return ata.status;
			INFO_LOG(NAOMI, "systemsp::read(%x) CF IDE AltStatus unknown addr", addr);
			return 0;
		}
		else if ((bank & 0x3f00) == 0x3d00)
		{
			// Network aka Media board shared buffer/RAM
			DEBUG_LOG(NAOMI, "systemsp::read(%x) Network shared RAM. offset %x", addr, offset);
			return (T)~0;
		}
		else if ((bank & 0x3f00) == 0x3f00)
		{
			// Network board present flag (0x01)
			return 0;
		}
		T v = (T)~0;
		Read(offset, sizeof(T), &v);
		return v;
	}
	else if (addr == 0x10000)
	{
		// bank register
		return (T)bank;
	}
	else if (addr < 0x10100)
	{
		// IRQ pending/reset, ATA control
		switch (addr - 0x10000)
		{
		case 0x30:
			return 0;
		case 0x80:
			{
				// interrupt status
				// b0: UART1
				// b1: UART2
				// b3: DIMM
				// b4: ATA controller
				const u8 intPending = ata.interruptPending;
				ata.interruptPending = 0;
				updateInterrupt();
				return intPending;
			}
		case 0x84:
			// interrupt mask?
			// 10084: (dinoking) bit0,1 reset, sometimes set
			return ata.reg84;
		default:
			return 0;
		}
	}
	else if (addr < 0x10128)
	{
		// I/O chip for inputs
		switch (addr - 0x10100)
		{
		case 0x0: // IN_PORT0 (CN9 17-24)
			return ioPortManager->getCN9_17_24();

		case 0x4: // IN_PORT1 (CN9 41-48)
			return ioPortManager->getCN9_41_48();

		case 0x8: // IN_PORT3 (CN9 25-32)
			return ioPortManager->getCN9_25_32();

		case 0xc: // IN CN9 33-40
			return ioPortManager->getCN9_33_40();

		case 0x10: // IN_PORT4 (CN9 49-56)
			return ioPortManager->getCN9_49_56();

		case 0x18: // IN_PORT2 (DIP switches and jumpers, and P1 service for older pcb rev)
			// DIP switches
			// 0: unknown, active low
			// 1: unknown, active low
			// 2: monitor (1: 31 kHz, 0: 15 kHz)
			// 3: unknown, must be on (active low)
			// 4: JP5 (system service)
			// 5: JP6 (system test)
			// 6: JP7
			// 7: JP8
			return 0xf7;

		case 0x20: // IN G_PORT CN10 9-16
			return ioPortManager->getCN10_9_16();

		case 0x24: // bios, write too
		default:
			return 0;
		}
	}
	else if (addr == 0x10128)
	{
		// eeprom
		return eeprom.readDO() << 4;
	}
	else if (addr == 0x10150)
	{
		// CFG rom board debug flags
		// bit 0 - romboard type, 1 = M4
		// bit 1 - debug mode (enable easter eggs in BIOS, can boot game without proper eeproms/settings)
		return 3; // M4 board type, debug on;
	}
	else if (addr < 0x10180) {
		// unknown
	}
	else if (addr < 0x101c0)
	{
		// custom UART 1
		return uart1.readReg(addr);
	}
	else if (addr < 0x101f0)
	{
		// custom UART 2
		return uart2.readReg(addr);
	}
	INFO_LOG(NAOMI, "systemsp::readMemArea0<%d>: Unknown addr %x", (int)sizeof(T), addr);
	return (T)~0;
}

template<typename T>
void writeMemArea0(u32 addr, T v)
{
	if (SystemSpCart::Instance != nullptr)
		SystemSpCart::Instance->writeMemArea0(addr, v);
}
template void writeMemArea0<>(u32 addr, u32 v);
template void writeMemArea0<>(u32 addr, u16 v);
template void writeMemArea0<>(u32 addr, u8 v);

template<typename T>
void SystemSpCart::writeMemArea0(u32 addr, T v)
{
	addr &= 0x1fffff;
	if (addr < 0x10000)
	{
		// banked access to ROM/NET board address space, mainly backup SRAM and ATA
		u32 offset = (addr & 0xffff) | ((bank & 0x3fff) << 16);
		if ((bank & 0x3f00) == 0x3900)
		{
			// SRAM: an 8-bit device on a 16-bit bus
			if (sizeof(T) == 1 && (offset & 1))
				return;
			if (sizeof(T) == 4)
				sys_nvmem_sram.Write(offset / 2 + 1, (u8)(v >> 16), 1);
			sys_nvmem_sram.Write(offset / 2, (u8)v, 1);
			return;
		}
		else if ((bank & 0x3f00) == 0x3a00)
		{
			// CF IDE registers
			switch (addr & 0xffff)
			{
			case 0x00: // WR data
				break;
			case 0x04: // features
				ata.features = v;
				break;
			case 0x08: // sector count
				ata.sectorCount = v;
				break;
			case 0x0c: // sector no
				ata.sectorNumber = v;
				break;
			case 0x10: // cylinder (lsb)
				ata.cylinder = (ata.cylinder & 0xff00) | (v & 0xff);
				break;
			case 0x14: // cylinder (msb)
				ata.cylinder = (ata.cylinder & 0xff) | ((v & 0xff) << 8);
				break;
			case 0x18: // select card/head
				ata.driveHead = (u8)v | 0xa0;
				break;
			case 0x1c: // command
				switch ((u8)v)
				{
				case 0x20:
					// read sectors
					ata.status &= ~0x40;	// RDY
					ata.status |= 0x80 | 0x08;	// BSY, DRQ (FIXME should be done in the callback)
					sh4_sched_request(ataSchedId, 2000); // 10 us
					// (what the decrypting window gives starts again with each command)
					encReset();
					readSectors();
					break;
				case 0xe1:
					// idle immediate
					ata.status |= 0x80;		// BSY
					ata.status &= ~0x40;	// RDY
					sh4_sched_request(ataSchedId, 2000); // 10 us
					break;
				default:
					INFO_LOG(NAOMI, "systemsp::write(%x) CF ATA command unknown: %x", addr, (u32)v);
					break;
				}
				break;
			default:
				INFO_LOG(NAOMI, "systemsp::write(%x) CF ATA unknown reg = %x", addr, (u32)v);
				break;
			}
			return;
		}
		else if ((bank & 0x3f00) == 0x3b00)
		{
			if ((addr & 0xffff) == 0x18)
			{
				// CF IDE AltStatus/Device Ctrl register
				if ((ata.devCtrl & 4) && !(v & 4))
				{
					// software reset
					ata.bufferIndex = ~0u;
					ata.error = 0;
					ata.status &= ~(0x08 | 0x01);	// DRQ, ERR
				}
				ata.devCtrl = v & 0x86;
			}
			else
				INFO_LOG(NAOMI, "systemsp::write(%x) CF IDE unknown reg %x data %x", addr, offset, (u32)v);
			return;
		}
		else if ((bank & 0x3f00) == 0x3d00) {
			// Network aka Media board shared buffer/RAM
			return;
		}
		else if ((bank & 0x3f00) == 0x3f00) {
			// Network board present flag (0x01)
			return;
		}
	}
	else if (addr == 0x10000)
	{
		// bank register
		bank = (u16)v;
		return;
	}
	else if (addr < 0x10100)
	{
		// IRQ pending/reset, ATA control
		// (0x10084 is read back as written as the game has it (the interrupt
		// mask?), but nothing has ever been kept of what is written to it:
		// left so)
		return;
	}
	else if (addr < 0x10128)
	{
		// I/O chip for outputs
		switch (addr - 0x10100)
		{
		case 0x8: // OUT_PORT3 (CN9 25-32)?
			break;
		case 0xc: // IO-1 CN9 33-40
			ioPortManager->setCN9_33_40(v);
			break;
		case 0x10: // OUT_PORT4 (CN9 49-56)
			// 0: (P1 coin meter)
			// 1: (P2 coin meter)
			// giant tetris:
			// 2: P1 start lamp
			// 3: P2 start lamp
			// 4: P1 button 1 lamp
			// 5: P2 button 1 lamp
			// 6: P1 button 2 lamp
			// 7: P2 button 2 lamp
			// dinosaur king, love & berry:
			// 2: coin blocker 1 (not used)
			// 3: coin blocker 2 (not used)
			// 4: cd1 card pickout
			// 5: cd2 card pickout (not used)
			// 6: cd1 jam reset
			// 7: cd2 jam reset (not used)
			// hopper:
			// 4: payout?
			// 7: ?
			ioPortManager->setCN9_49_56(v);
			break;
		case 0x14: // OUT CN10 17-24
			// dinosaur king, love & berry:
			// 0: 232c select1 (not used)
			// 1: 232c select2 (not used)
			// 2: 232c reset (not used)
			// 3: card trigger (not used)
			// 4: rfid chip1 reset
			// 5: rfid chip2 reset
			// 6: rfid chip1 empty lamp
			// 7: rfid chip2 empty lamp
			ioPortManager->setCN10_17_24(v);
			break;
		case 0x24: // read too
		default:
			break;
		}
		return;
	}
	else if (addr == 0x10128)
	{
		// eeprom
		eeprom.writeDI((v & 1) != 0);
		eeprom.writeCS((v & 2) != 0);
		eeprom.writeCLK((v & 4) != 0);
		// (what has been written goes to its file once the chip is let go)
		if (!(v & 2) && eeprom.dirty)
			saveEeprom();
		return;
	}
	else if (addr < 0x10180)
	{
		// rom board dip switches?
		return;
	}
	else if (addr < 0x101c0)
	{
		// custom UART 1
		uart1.writeReg(addr, v);
		return;
	}
	else if (addr < 0x101f0)
	{
		// custom UART 2
		uart2.writeReg(addr, v);
		return;
	}
	INFO_LOG(NAOMI, "systemsp::writeMemArea0<%d>: Unknown addr %x = %x", (int)sizeof(T), addr, (int)v);
}

void SystemSpCart::updateInterrupt(u32 mask)
{
	ata.interruptPending |= mask;
	if ((ata.interruptPending & (INT_UART1 | INT_UART2 | INT_DIMM))
			|| ((ata.interruptPending & INT_ATA) && !(ata.devCtrl & 2)))
		asic_RaiseInterrupt(holly_EXP_PCI);
	else
		asic_CancelInterrupt(holly_EXP_PCI);
}

int ataSchedCallback(int tag, int cycles, int jitter)
{
	if (SystemSpCart::Instance != nullptr)
		SystemSpCart::Instance->ataReady();
	return 0;
}

void SystemSpCart::ataReady()
{
	ata.status |= 0x40;		// RDY
	ata.status &= ~0x80;	// BSY
	updateInterrupt(INT_ATA);
}

SystemSpCart::SystemSpCart(u32 size, const char *gameName, const char *mediaName, const char *parentName)
	: M4Cartridge(size), gameName(gameName), mediaName(mediaName), parentName(parentName), uart1(this, 1), uart2(this, 2)
{
	retro_atomic_int_init(&eepromFailed, 0);
	encInit();
	ataSchedId = sh4_sched_register(0, ataSchedCallback);
	Instance = this;
	netmem.reset(new u8[NETMEM_SIZE]());
	memset(netPages, 0, sizeof(netPages));
	inetaddr = 0x641ea8c0;	// 192.168.30.100
	netmask = 0x00ffffff;	// 255.255.255.0
	sockets = 0;
	memset(&ata, 0, sizeof(ata));
	memset(&flash, 0, sizeof(flash));
	// mb_serial.ic57
	static const u8 eepromData[0x80] = {
		0xf5, 0x90, 0x53, 0x45, 0x47, 0x41, 0x20, 0x45, 0x4e, 0x54, 0x45, 0x52,
		0x50, 0x52, 0x49, 0x53, 0x45, 0x53, 0x2c, 0x4c, 0x54, 0x44, 0x2e, 0x00,
		0x4e, 0x41, 0x4f, 0x4d, 0x49, 0x00, 0x00, 0x00, 0x41, 0x41, 0x46, 0x45,
		0x30, 0x31, 0x44, 0x31, 0x35, 0x39, 0x32, 0x34, 0x38, 0x31, 0x36, 0x00,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	memcpy(eeprom.data, eepromData, sizeof(eepromData));
}

SystemSpCart::~SystemSpCart()
{
	if (ataSchedId >= 0)
		sh4_sched_request(ataSchedId, -1);
	if (mediaName != nullptr && flashDirty)
		saveFlash();
	if (eeprom.dirty)
		saveEeprom();
	// (the flags are this one's: the writer is done with them first)
	save_writer_drain();
	if (chd != nullptr)
		chd_image_close(chd);
	Instance = nullptr;
}

void SystemSpCart::SetKeyData(u8 *key_data)
{
	if (key_data != nullptr)
	{
		subkey1 = (key_data[0x5e2] << 8) | key_data[0x5e0];
		subkey2 = (key_data[0x5e6] << 8) | key_data[0x5e4];
	}
	M4Cartridge::SetKeyData(key_data);
}

std::string SystemSpCart::savePath(const char *suffix) const
{
	// where the game's EEPROM would be, without its ".eeprom"
	std::string path(eeprom_file);
	const std::string ext(".eeprom");
	if (path.size() >= ext.size() && path.compare(path.size() - ext.size(), ext.size(), ext) == 0)
		path.resize(path.size() - ext.size());
	return path + suffix;
}

static const char *regionSuffix(int region)
{
	switch (region)
	{
	case 0:
		return "-jp";
	case 1:
		return "-us";
	default:
		return "-exp";
	}
}

std::string SystemSpCart::getCardPath(int index) const
{
	return savePath(regionSuffix(region)) + ".rfid" + std::to_string(index);
}

bool SystemSpCart::openChd()
{
	std::string dir(g_base_name);
	const size_t dot = dir.rfind('.');
	if (dot != std::string::npos)
		dir.resize(dot);
	std::string path = std::string(g_roms_dir) + dir + "/" + mediaName + ".chd";
	chd = chd_image_open(path.c_str(), nullptr, 0);
	if (chd == nullptr && parentName != nullptr)
		chd = chd_image_open((std::string(g_roms_dir) + parentName + "/" + mediaName + ".chd").c_str(), nullptr, 0);
	if (chd == nullptr)
	{
		ERROR_LOG(NAOMI, "System SP: cannot open the CompactFlash image %s: the game cannot start without it", path.c_str());
		return false;
	}
	hunkbytes = chd_image_hunk_bytes(chd);
	if (hunkbytes == 0 || hunkbytes % SECTOR_SIZE != 0)
	{
		ERROR_LOG(NAOMI, "System SP: %s is not a CompactFlash image", path.c_str());
		chd_image_close(chd);
		chd = nullptr;
		return false;
	}
	chdSize = (u64)hunkbytes * chd_image_hunk_count(chd);
	hunkmem.reset(new u8[hunkbytes]);
	hunknum = ~0u;
	INFO_LOG(NAOMI, "System SP: CompactFlash image %s", path.c_str());
	return true;
}

/* @len bytes of the CompactFlash card from @offset on: zeroes for what is
 * past its end, or cannot be read. */
bool SystemSpCart::readCf(u64 offset, u32 len, u8 *dst)
{
	bool ok = true;
	while (len > 0)
	{
		if (chd == nullptr || offset >= chdSize)
		{
			memset(dst, 0, len);
			return false;
		}
		const u32 hunk = (u32)(offset / hunkbytes);
		const u32 at = (u32)(offset % hunkbytes);
		const u32 n = std::min(len, hunkbytes - at);
		if (hunk != hunknum)
		{
			if (chd_image_read_hunk(chd, hunk, hunkmem.get()))
				hunknum = hunk;
			else
			{
				WARN_LOG(NAOMI, "System SP: CompactFlash hunk %u cannot be read", hunk);
				hunknum = ~0u;
				memset(dst, 0, n);
				ok = false;
				dst += n;
				offset += n;
				len -= n;
				continue;
			}
		}
		memcpy(dst, &hunkmem[at], n);
		dst += n;
		offset += n;
		len -= n;
	}
	return ok;
}

void SystemSpCart::readSectors()
{
	// (an address in cylinders, heads and sectors is not one the BIOS
	// gives: it is taken as a block number all the same)
	const u32 lba = ((ata.driveHead & 0xf) << 24) | (ata.cylinder << 8) | ata.sectorNumber;
	readCf((u64)lba * SECTOR_SIZE, SECTOR_SIZE, ata.buffer);
	ata.bufferIndex = 0;
}

/* The game's header (RomBootID) on a CompactFlash card: the file that the
 * card's 1STREAD.BIN names, in the first partition, a FAT16 one. Both are
 * encrypted, the header twice over but for one game (tetgiano). */
class BootIdLoader
{
	static constexpr u32 SECSIZE = 512;

public:
	BootIdLoader(SystemSpCart& cart) : cart(cart) {}

	bool load(u8 *bootId)
	{
		u8 sec[SECSIZE];
		// master boot record
		if (!cart.readCf(0, SECSIZE, sec))
			return false;
		if (sec[510] != 0x55 || sec[511] != 0xaa)
			return false;
		// partition 1: FAT16B?
		if (sec[446 + 4] != 6)
			return false;
		const u32 start = le32(&sec[446 + 8]);

		// the partition's Bios Parameter Block
		if (!cart.readCf((u64)start * SECSIZE, SECSIZE, sec))
			return false;
		sectorsPerCluster = sec[13];
		const u16 resSectors = le16(&sec[14]);
		const u8 fatCount = sec[16];
		rootFolderSize = le16(&sec[17]); // in entries
		const u16 fatSize = le16(&sec[22]); // in sectors
		// FATs start after reserved sectors
		rootOffset = (u64)start + resSectors + (u64)fatSize * fatCount;
		dataStartOffset = rootOffset + (u64)rootFolderSize * 32 / SECSIZE;

		u64 offset;
		u32 size;
		u8 buffer[0x500];
		if (!openFile("1STREAD.BIN", offset, size) || size <= 0x80)
			return false;
		if (!cart.readCf(offset, 0x100, buffer))
			return false;
		decrypt(buffer, 0x100);
		if (memcmp(&buffer[0], "SPCF", 4))
			// wrong signature
			return false;
		std::string imageName((const char *)&buffer[0x80], strnlen((const char *)&buffer[0x80], 0x80));
		if (imageName.length() > 12)
		{
			// make short 8.3 name
			const size_t dot = imageName.find('.');
			imageName = imageName.substr(0, 6) + "~1" + (dot != std::string::npos ? imageName.substr(dot) : "");
		}
		for (char& c : imageName)
			c = toupper(c);

		if (!openFile(imageName, offset, size) || size < sizeof(buffer))
			return false;
		if (!cart.readCf(offset, sizeof(buffer), buffer))
			return false;
		decrypt(buffer, sizeof(buffer));
		if (memcmp(&buffer[0], "SystemSP", 8))
			// Yes, it is encrypted twice (except tetgiano)
			decrypt(buffer, sizeof(buffer));
		memcpy(bootId, buffer, sizeof(buffer));
		return true;
	}

private:
	void decrypt(u8 *p, u32 len)
	{
		cart.encReset();
		for (u32 i = 0; i + 1 < len; i += 2)
		{
			const u16 w = cart.decrypt(le16(&p[i]));
			p[i] = (u8)w;
			p[i + 1] = w >> 8;
		}
		cart.encReset();
	}

	// (a file in the root folder, all of whose clusters follow each other)
	bool openFile(const std::string& fileName, u64& offset, u32& size)
	{
		for (u32 i = 0; i < rootFolderSize; i++)
		{
			u8 entry[32];
			if (!cart.readCf(rootOffset * SECSIZE + i * 32, 32, entry))
				return false;
			const u8 b = entry[0];
			if (b == 0)
				// end of directory
				return false;
			if (b == 0xe5 || b == 0x2e)
				// unused or ./.. entry
				continue;
			const u8 attributes = entry[11];
			if (attributes == 0xf || (attributes & (0x8 | 0x4)))
				// long name or volume name/system
				continue;
			std::string name((const char *)&entry[0], 8);
			while (!name.empty() && name.back() == ' ')
				name.pop_back();
			std::string ext((const char *)&entry[8], 3);
			while (!ext.empty() && ext.back() == ' ')
				ext.pop_back();
			if (!ext.empty())
				name += "." + ext;
			if (name != fileName)
				continue;
			const u16 cluster = le16(&entry[26]);
			if (cluster < 2)
				return false;
			size = le32(&entry[28]);
			offset = (dataStartOffset + (u64)(cluster - 2) * sectorsPerCluster) * SECSIZE;
			return true;
		}
		return false;
	}

	SystemSpCart& cart;
	u8 sectorsPerCluster = 0;
	u16 rootFolderSize = 0;
	u64 rootOffset = 0;
	u64 dataStartOffset = 0;
};

bool SystemSpCart::Init()
{
	M4Cartridge::Init();

	// The ROM board's battery-backed SRAM is 128 KB, a NAOMI's 32: it is
	// the machine's (sys_nvmem_sram), saved to its file and in the state as
	// a NAOMI's is.
	const u32 sramSize = 128 * 1024;
	if (sys_nvmem_sram.size != sramSize)
	{
		delete[] sys_nvmem_sram.data;
		sys_nvmem_sram.Allocate(sramSize);
		memset(sys_nvmem_sram.data, 0, sramSize);
		sys_nvmem_sram.Load(nvmem_file);
		BBSRAM_SIZE = sramSize;
	}

	bootId.reset(new u8[0x500]);
	bool haveBootId = false;
	if (mediaName != nullptr)
	{
		if (!openChd())
			return false;
		BootIdLoader loader(*this);
		haveBootId = loader.load(bootId.get());
		if (!haveBootId)
			WARN_LOG(NAOMI, "System SP: the game's header is not on the CompactFlash card");

		// What the BIOS put in the flash from the card the last time: it
		// does not do it again if it is there.
		RFILE *f = filestream_open(savePath(".flash").c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (f != nullptr)
		{
			if (filestream_get_size(f) == (int64_t)RomSize
					&& filestream_read(f, RomPtr, RomSize) == (int64_t)RomSize)
				INFO_LOG(NAOMI, "System SP: flash loaded from %s", savePath(".flash").c_str());
			else
				memset(RomPtr, 0xff, RomSize);
			filestream_close(f);
		}
	}
	else if (RomSize >= 0x500)
	{
		memcpy(bootId.get(), RomPtr, 0x500);
		if (memcmp(RomPtr, "NAOMI", 5) && memcmp(RomPtr, "Naomi2", 6) && memcmp(RomPtr, "SystemSP", 8))
		{
			encReset();
			for (u32 i = 0; i < 0x500; i += 2)
			{
				const u16 w = decrypt(le16(&RomPtr[i]));
				bootId[i] = (u8)w;
				bootId[i + 1] = w >> 8;
			}
			encReset();
		}
		haveBootId = true;
	}
	if (!haveBootId)
		bootId.reset();

	// The region: the one asked for, if the game runs there
	region = std::min<int>(settings.dreamcast.region, 2);
	if (bootId)
	{
		const u8 country = bootId[0x428];
		if (country != 0 && (country & (2 << region)) == 0)
		{
			if (country & 4)
				region = 1;
			else if (country & 8)
				region = 2;
			else if (country & 2)
				region = 0;
			NOTICE_LOG(NAOMI, "System SP: the game does not run in that region: region %d", region);
		}
	}

	// dinoki4 doesn't use rfid chips. dinokich and loveber3cn use a different reader/writer protocol
	if (gameName == "dinokich" || gameName == "loveber3cn")
	{
		WARN_LOG(NAOMI, "System SP: %s's card reader is not emulated", gameName.c_str());
	}
	else if ((gameName.compare(0, 6, "dinoki") == 0 && gameName != "dinoki4")
			|| gameName.compare(0, 7, "loveber") == 0)
	{
		new RfidReaderWriter(&uart1, 1, gameName, region, getCardPath(1));
		new RfidReaderWriter(&uart2, 2, gameName, region, getCardPath(2));
	}
	else if (gameName == "isshoni")
	{
		touchscreen = new Touchscreen(&uart1);
		ioPortManager.reset(new IsshoniIOManager());
	}
	else if (gameName == "manpuku") {
		ioPortManager.reset(new ManpukuIOManager());
	}
	else if (gameName.compare(0, 6, "kingyo") == 0 || gameName == "shateki") {
		ioPortManager.reset(new KingyoIOManager());
	}
	else if (gameName == "magicpop"
			|| gameName == "ochaken"
			|| gameName == "puyomedal"
			|| gameName == "unomedal"
			|| gameName == "westdrmg") {
		ioPortManager.reset(new MedalIOManager());
	}
	else if (gameName == "btlracer") {
		ioPortManager.reset(new BattleRacerIOManager());
	}
	if (gameName.compare(0, 6, "dinoki") == 0 || gameName.compare(0, 7, "loveber") == 0)
		ioPortManager.reset(new CardReaderIOManager());
	if (!ioPortManager)
		ioPortManager.reset(new DefaultIOManager());

	Reset();
	return true;
}

void SystemSpCart::LoadEeprom()
{
	const std::string path = savePath(regionSuffix(region)) + ".eeprom";
	RFILE *f = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	bool loaded = false;
	if (f != nullptr)
	{
		u8 data[sizeof(eeprom.data)];
		if (filestream_read(f, data, sizeof(data)) == (int64_t)sizeof(data))
		{
			memcpy(eeprom.data, data, sizeof(data));
			loaded = true;
			INFO_LOG(NAOMI, "System SP: EEPROM loaded from %s", path.c_str());
		}
		filestream_close(f);
	}
	if (!loaded && naomi_default_eeprom != nullptr)
		memcpy(eeprom.data, naomi_default_eeprom, sizeof(eeprom.data));
	eeprom.dirty = false;
}

void SystemSpCart::saveEeprom()
{
	if (retro_atomic_load_acquire_int(&eepromFailed) && retro_atomic_exchange_int(&eepromFailed, 0))
		WARN_LOG(NAOMI, "System SP: cannot save the EEPROM");
	const std::string path = savePath(regionSuffix(region)) + ".eeprom";
	if (save_writer_put_file(path.c_str(), eeprom.data, sizeof(eeprom.data), &eepromFailed))
		eeprom.dirty = false;
}

void SystemSpCart::saveFlash()
{
	const std::string path = savePath(".flash");
	RFILE *f = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == nullptr || filestream_write(f, RomPtr, RomSize) != (int64_t)RomSize)
		WARN_LOG(NAOMI, "System SP: cannot save the flash to %s", path.c_str());
	else
		INFO_LOG(NAOMI, "System SP: flash saved to %s", path.c_str());
	if (f != nullptr)
		filestream_close(f);
	flashDirty = false;
}

void SystemSpCart::Reset()
{
	M4Cartridge::Reset();
	bank = 0;
	memset(&ata, 0, sizeof(ata));
	ata.status = 0x50;		// RDY, DSC
	if (chd == nullptr)
		ata.status = 0x30;	// no card: not ready, drive fault
	ata.driveHead = 0xa0;
	flash.cmdState = INIT;
	flash.progAddress = ~0u;
	flash.wordCount = 0;
	encReset();
	eeprom.reset();
	// the network board's memory
	for (u32 i = 0; i < NETMEM_SIZE / NETMEM_PAGE / 64; i++)
		for (u32 b = 0; b < 64 && netPages[i] != 0; b++)
			if (netPages[i] & (1ull << b))
				memset(&netmem[(i * 64 + b) * NETMEM_PAGE], 0, NETMEM_PAGE);
	memset(netPages, 0, sizeof(netPages));
	inetaddr = 0x641ea8c0;	// 192.168.30.100
	netmask = 0x00ffffff;	// 255.255.255.0
	sockets = 0;
	if (uart1.getPipe() != nullptr)
		uart1.getPipe()->reset();
	if (uart2.getPipe() != nullptr)
		uart2.getPipe()->reset();
	if (ioPortManager)
		ioPortManager->reset();
	if (ataSchedId >= 0)
		sh4_sched_request(ataSchedId, -1);
}

std::string SystemSpCart::GetGameId()
{
	if (!bootId)
		return mediaName != nullptr ? gameName : M4Cartridge::GetGameId();
	// the game's title, as the System SP's BIOS has it: the second
	std::string id((const char *)&bootId[0x50], 0x20);
	while (!id.empty() && (id.back() == ' ' || id.back() == '\0'))
		id.pop_back();
	if (id.empty())
	{
		id = std::string((const char *)&bootId[0x30], 0x20);
		while (!id.empty() && (id.back() == ' ' || id.back() == '\0'))
			id.pop_back();
	}
	return id;
}

u32 SystemSpCart::ReadMem(u32 address, u32 size)
{
	if ((address & 0xff) == 0x4c)
	{
		// DIMM status
		return DIMM_STATUS & ~(((SB_ISTEXT >> 3) & 1) << 8);
	}
	else {
		return M4Cartridge::ReadMem(address, size);
	}
}

void SystemSpCart::WriteMem(u32 address, u32 data, u32 size)
{
	if ((address & 0xff) == 0x4c)
	{
		// DIMM status
		if (data & 0x100)
			asic_CancelInterrupt(holly_EXP_PCI);
		if ((data & 2) == 0)
			// irq to dimm
			process();
	}
	else {
		M4Cartridge::WriteMem(address, data, size);
	}
}

bool SystemSpCart::Read(u32 offset, u32 size, void *dst)
{
	if ((offset & 0x3f000000) == 0x3f000000)
	{
		// network card present
		const u32 rc = 1;
		memcpy(dst, &rc, std::min<u32>(size, 4));
		return true;
	}
	if ((offset & 0x3f000000) == 0x3d000000)
	{
		// network card mem
		switch (size)
		{
		case 4:
			*(u32 *)dst = readNetMem<u32>(offset);
			break;
		case 2:
			*(u16 *)dst = readNetMem<u16>(offset);
			break;
		case 1:
		default:
			*(u8 *)dst = readNetMem<u8>(offset);
			break;
		}
		return true;
	}
	return M4Cartridge::Read(offset, size, dst);
}

void SystemSpCart::flashChanged(u32 op, u32 offset, u32 data)
{
	flash.changes++;
	flash.changesHash = (flash.changesHash ^ (op << 28) ^ offset ^ (data << 7)) * 0x01000193u;
	flashDirty = true;
}

bool SystemSpCart::Write(u32 offset, u32 size, u32 data)
{
	if ((offset & 0x3f000000) == 0x3d000000)
	{
		// network card mem
		switch (size)
		{
		case 4:
			writeNetMem(offset, data);
			break;
		case 2:
			writeNetMem(offset, (u16)data);
			break;
		case 1:
		default:
			writeNetMem(offset, (u8)data);
			break;
		}
		const u32 idx = offset & (NETMEM_SIZE - 2);
		// FIXME
		if (idx == 2)
		{
			memcpy(&netmem[idx + 0x200], &data, std::min<u32>(size, 4));
			netPages[0] |= 1;
		}
		return true;
	}
	switch (flash.cmdState)
	{
	case INIT:
		if ((offset & 0xfff) == 0xaaa && data == 0xaa) {
			flash.cmdState = AAA_AA_1;
			return true;
		}
		else if (offset == flash.progAddress && data == 0x29)
		{
			// write buffer to flash
			flash.progAddress = ~0u;
			return true;
		}
		break;
	case AAA_AA_1:
		if (((offset & 0xfff) == 0x555 || (offset & 0xfff) == 0x554) && data == 0x55) {
			flash.cmdState = _555_55_1;
			return true;
		}
		flash.cmdState = INIT;
		break;
	case _555_55_1:
		if ((offset & 0xfff) == 0xaaa)
		{
			if (data == 0xa0) {
				flash.cmdState = PROGRAM;
				return true;
			}
			else if (data == 0x80) {
				flash.cmdState = AAA_80;
				return true;
			}
		}
		else if (data == 0x25)
		{
			flash.cmdState = WRITE_BUF_1;
			flash.progAddress = offset;
			return true;
		}
		flash.cmdState = INIT;
		break;
	case PROGRAM:
		// (a whole word, inside the flash)
		*(u16 *)&RomPtr[offset & (RomSize - 2)] = data;
		flashChanged(1, offset, data);
		flash.cmdState = INIT;
		return true;
	case WRITE_BUF_1:
		flash.wordCount = data + 1;
		flash.cmdState = WRITE_BUF_2;
		return true;
	case WRITE_BUF_2:
		*(u16 *)&RomPtr[offset & (RomSize - 2)] = data;
		flashChanged(2, offset, data);
		if (--flash.wordCount == 0)
			flash.cmdState = INIT;
		return true;
	case AAA_80:
		if ((offset & 0xfff) == 0xaaa && data == 0xaa) {
			flash.cmdState = AAA_AA_2;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state aaa_80", offset, data);
		flash.cmdState = INIT;
		break;
	case AAA_AA_2:
		if (((offset & 0xfff) == 0x555 || (offset & 0xfff) == 0x554) && data == 0x55) {
			flash.cmdState = _555_55_2;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state aaa_aa_2", offset, data);
		flash.cmdState = INIT;
		break;
	case _555_55_2:
		if ((offset & 0xfff) == 0xaaa && data == 0x10)
		{
			// Erase chip: a chip is 64 MB
			const u32 chip = 64 * 1024 * 1024;
			const u32 start = offset & 0x1fffffff & ~(chip - 1);
			if (start < RomSize)
			{
				memset(&RomPtr[start], 0xff, std::min(chip, RomSize - start));
				flashChanged(3, offset, data);
			}
			flash.cmdState = INIT;
			return true;
		}
		else if (data == 0x30)
		{
			// Erase sector: the chips' sectors are 128 KB, as their CFI
			// data says (m4cartridge.cpp: 512 sectors of 0x200 * 256 bytes)
			const u32 sector = 128 * 1024;
			const u32 start = offset & 0x1fffffff & ~(sector - 1);
			if (start < RomSize)
			{
				memset(&RomPtr[start], 0xff, std::min(sector, RomSize - start));
				flashChanged(4, offset, data);
			}
			flash.cmdState = INIT;
			return true;
		}
		INFO_LOG(NAOMI, "Unexpected command %x %x in state 555_55_2", offset, data);
		flash.cmdState = INIT;
		break;
	default:
		flash.cmdState = INIT;
		break;
	}

	return M4Cartridge::Write(offset, size, data);
}

// inet_addr() of a dotted quad: the address with its first number in its
// lowest byte, as it is in memory
static u32 parseInetAddr(const char *s, size_t len)
{
	u32 addr = 0;
	int part = 0;
	u32 value = 0;
	bool digits = false;
	for (size_t i = 0; i <= len; i++)
	{
		const char c = i < len ? s[i] : '\0';
		if (c >= '0' && c <= '9')
		{
			value = value * 10 + (c - '0');
			if (value > 255)
				return 0xffffffff;
			digits = true;
		}
		else if (c == '.' || c == '\0')
		{
			if (!digits || part > 3)
				return 0xffffffff;
			addr |= value << (part * 8);
			part++;
			value = 0;
			digits = false;
			if (c == '\0')
				break;
		}
		else
			return 0xffffffff;
	}
	return part == 4 ? addr : 0xffffffff;
}

// TODO implement actual networking features. Sockets are numbers given out
// and taken back; nothing goes on any network.
void SystemSpCart::process()
{
	DmaOffset |= 0x40000000; // needed by BIOS. why?
	const u8 reqId = readNetMem<u8>(0x200);
	const int cmd = readNetMem<u16>(0x202);
	const u8 netmem4 = readNetMem<u8>(4); // FIXME memset should be done later on
	memset(&netmem[0], 0, 32);
	netPages[0] |= 1;
	writeNetMem(0, reqId);
	writeNetMem(1, (u8)0);
	switch (cmd)
	{
	case 1: // init? nop?
		writeNetMem(2, NET_OK);
		break;
	case 0x100: // get dimm status
		writeNetMem(2, NET_OK);
		// 0 initializing
		// 1 checking network
		// 2 ?
		// 3 testing prg
		// 4 loading prg
		// 5 ready
		writeNetMem(4, 5);
		break;
	case 0x101: // get firmware version
		writeNetMem(2, NET_OK);
		writeNetMem(4, (u8)0x25);	// minor
		writeNetMem(5, (u8)1);		// major
		break;
	case 0x104: // get network type and address
		writeNetMem(2, NET_OK);
		writeNetMem(4, 5);	// 4: ether (dhcp), 5: ether (static ip)
		writeNetMem(0x24, inetaddr);
		writeNetMem(0x28, netmask);
		break;

	case 0x204: // set network type and address
		// 4: type (0 none, 3,4 ether)
		inetaddr = readNetMem<u32>(0x24);
		netmask = readNetMem<u32>(0x28);
		INFO_LOG(NAOMI, "process: set net type %d ip %08x mask %08x", netmem4, inetaddr, netmask);
		writeNetMem(2, NET_OK);
		break;

	case 0x301: // network test
		{
			// 204: 4
			// (in the system's RAM, the only place it can be)
			const u32 addr = readNetMem<u32>(0x208);
			static const char text[] = "CHECKING NETWORK\nPRETENDING... :P\n--- COMPLETED---\n";
			if (((addr >> 26) & 7) == 3)
				for (u32 i = 0; i < sizeof(text); i++)
					mem_b.data[(addr + i) & RAM_MASK] = text[i];
			writeNetMem(2, NET_OK);
			writeNetMem(4, 1); // TODO how to signal the test progress/completion?
		}
		break;

	case 0x401: // accept
		writeNetMem(2, NET_ERROR);
		break;
	case 0x402: // bind
		writeNetMem(2, NET_OK);
		break;
	case 0x403: // close socket
		{
			const int sockidx = readNetMem<int>(0x208);
			if (sockidx < 1 || sockidx > 63 || !(sockets & (1ull << sockidx)))
				writeNetMem(2, NET_ERROR);
			else
			{
				sockets &= ~(1ull << sockidx);
				writeNetMem(2, NET_OK);
			}
		}
		break;
	case 0x404: // connect
		writeNetMem(2, NET_OK);
		break;
	case 0x406: // inet_addr
		{
			const u32 addr = readNetMem<u32>(0x208) & (NETMEM_SIZE - 1);
			const char *s = (const char *)&netmem[addr];
			const u32 ipaddr = parseInetAddr(s, strnlen(s, NETMEM_SIZE - addr));
			writeNetMem(2, NET_OK);
			writeNetMem(4, ipaddr);
		}
		break;
	case 0x408: // listen
		writeNetMem(2, NET_OK);
		break;
	case 0x409: // recv
		writeNetMem(2, NET_OK);
		break;
	case 0x40a: // send
		{
			const u32 len = readNetMem<u32>(0x210);
			writeNetMem(2, NET_OK);
			writeNetMem(4, len); // ??
		}
		break;
	case 0x40b: // openSocket
		{
			int sockidx = -1;
			for (int i = 1; i <= 63; i++)
				if (!(sockets & (1ull << i)))
				{
					sockets |= 1ull << i;
					sockidx = i;
					break;
				}
			writeNetMem(2, sockidx != -1 ? NET_OK : NET_ERROR);
			if (sockidx != -1)
				writeNetMem(4, sockidx);
		}
		break;
	case 0x40e: // setsockopt
		writeNetMem(2, NET_OK);
		break;
	case 0x410: // settimeout
		writeNetMem(2, NET_OK);
		break;
	case 0x411: // geterrno
		writeNetMem(2, NET_OK);
		writeNetMem(4, 111);	// FIXME errno values TBD
		break;
	case 0x414: // getParambyDHCP?
		writeNetMem(2, NET_OK);
		writeNetMem(4, 1);	// ???
		break;
	case 0x415: // modifyMyIPaddr?
		writeNetMem(2, NET_OK);
		break;

	default:
		WARN_LOG(NAOMI, "Unknown netdimm command: %x", cmd);
		writeNetMem(2, NET_ERROR);
		break;
	}
	updateInterrupt(INT_DIMM);
}

/* The serial devices' state is this long whatever they are: what is not
 * used of it is zeroes. */
static void serializePipe(SerialPort& port, void **data, unsigned int *total_size)
{
	static const u8 zeroes[SystemSpCart::PIPE_STATE_SIZE] = {};
	const unsigned int start = *total_size;
	if (port.getPipe() != nullptr)
		port.getPipe()->serialize(data, total_size);
	const unsigned int used = *total_size - start;
	if (used < SystemSpCart::PIPE_STATE_SIZE)
		LIBRETRO_SA(zeroes, SystemSpCart::PIPE_STATE_SIZE - used);
}

static void unserializePipe(SerialPort& port, void **data, unsigned int *total_size)
{
	static u8 skip[SystemSpCart::PIPE_STATE_SIZE];
	const unsigned int start = *total_size;
	if (port.getPipe() != nullptr)
		port.getPipe()->unserialize(data, total_size);
	const unsigned int used = *total_size - start;
	if (used < SystemSpCart::PIPE_STATE_SIZE)
		LIBRETRO_USA(skip, SystemSpCart::PIPE_STATE_SIZE - used);
}

/* The ROM board, as a state has it from V24:
 *   - the bank register, the CompactFlash controller's registers and sector
 *     buffer, and when its next interrupt is due;
 *   - where a command to the flash has got to, and how many times the
 *     flash was written since the game was loaded (and a hash of it): the
 *     flash itself is not in the state, it is in its file (.flash) for a
 *     game on a CompactFlash card, and the ROMs for one that is not;
 *   - the decrypting window's place in its cipher;
 *   - the serial EEPROM, its contents and its command so far;
 *   - what is plugged into the two serial ports (an RFID reader's card and
 *     what it has to send, a touchscreen's), PIPE_STATE_SIZE bytes each;
 *   - the motors and sensors of a medal or hopper cabinet;
 *   - the network board: its address, mask and open sockets, and the pages
 *     of its memory that were written to, NETMEM_STATE_PAGES at most.
 * 35,146 bytes in all. */
void SystemSpCart::serializeV24(void **data, unsigned int *total_size)
{
	LIBRETRO_S(bank);
	LIBRETRO_S(ata.features);
	LIBRETRO_S(ata.cylinder);
	LIBRETRO_S(ata.sectorCount);
	LIBRETRO_S(ata.sectorNumber);
	LIBRETRO_S(ata.status);
	LIBRETRO_S(ata.error);
	LIBRETRO_S(ata.driveHead);
	LIBRETRO_S(ata.devCtrl);
	LIBRETRO_S(ata.interruptPending);
	LIBRETRO_S(ata.reg84);
	LIBRETRO_SA(ata.buffer, SECTOR_SIZE);
	LIBRETRO_S(ata.bufferIndex);
	saveEvent(ataSchedId, data, total_size);

	LIBRETRO_S(flash.cmdState);
	LIBRETRO_S(flash.progAddress);
	LIBRETRO_S(flash.wordCount);
	LIBRETRO_S(flash.changes);
	LIBRETRO_S(flash.changesHash);
	LIBRETRO_S(encIv);
	LIBRETRO_S(encCounter);

	eeprom.serialize(data, total_size);
	serializePipe(uart1, data, total_size);
	serializePipe(uart2, data, total_size);
	ioPortManager->serialize(data, total_size);

	LIBRETRO_S(inetaddr);
	LIBRETRO_S(netmask);
	LIBRETRO_S(sockets);
	u16 pages[NETMEM_STATE_PAGES];
	u32 count = 0;
	for (u32 i = 0; i < NETMEM_SIZE / NETMEM_PAGE / 64; i++)
		for (u32 b = 0; b < 64 && netPages[i] != 0; b++)
			if (netPages[i] & (1ull << b))
			{
				if (count < NETMEM_STATE_PAGES)
					pages[count++] = i * 64 + b;
				else if (!netPagesWarned)
				{
					WARN_LOG(NAOMI, "System SP: more of the network board's memory is used than a state holds");
					netPagesWarned = true;
				}
			}
	for (u32 i = count; i < NETMEM_STATE_PAGES; i++)
		pages[i] = 0xffff;
	LIBRETRO_S(count);
	LIBRETRO_SA(pages, NETMEM_STATE_PAGES);
	static const u8 zeroes[NETMEM_PAGE] = {};
	for (u32 i = 0; i < NETMEM_STATE_PAGES; i++)
	{
		if (i < count)
			LIBRETRO_SA(&netmem[pages[i] * NETMEM_PAGE], NETMEM_PAGE);
		else
			LIBRETRO_SA(zeroes, NETMEM_PAGE);
	}
}

bool SystemSpCart::unserializeV24(void **data, unsigned int *total_size)
{
	LIBRETRO_US(bank);
	LIBRETRO_US(ata.features);
	LIBRETRO_US(ata.cylinder);
	LIBRETRO_US(ata.sectorCount);
	LIBRETRO_US(ata.sectorNumber);
	LIBRETRO_US(ata.status);
	LIBRETRO_US(ata.error);
	LIBRETRO_US(ata.driveHead);
	LIBRETRO_US(ata.devCtrl);
	LIBRETRO_US(ata.interruptPending);
	LIBRETRO_US(ata.reg84);
	LIBRETRO_USA(ata.buffer, SECTOR_SIZE);
	LIBRETRO_US(ata.bufferIndex);
	loadEvent(ataSchedId, 2000, data, total_size);

	const u32 changes = flash.changes;
	const u32 changesHash = flash.changesHash;
	LIBRETRO_US(flash.cmdState);
	LIBRETRO_US(flash.progAddress);
	LIBRETRO_US(flash.wordCount);
	LIBRETRO_US(flash.changes);
	LIBRETRO_US(flash.changesHash);
	if (flash.cmdState > _555_55_2)
		flash.cmdState = INIT;
	if (flash.changes != changes || flash.changesHash != changesHash)
		WARN_LOG(NAOMI, "System SP: the flash has been written to since this state was saved, or before it was: "
				"the game may not go on as it did");
	LIBRETRO_US(encIv);
	LIBRETRO_US(encCounter);
	if (encCounter >= 16)
		encCounter = 0;

	eeprom.unserialize(data, total_size);
	unserializePipe(uart1, data, total_size);
	unserializePipe(uart2, data, total_size);
	ioPortManager->unserialize(data, total_size);

	LIBRETRO_US(inetaddr);
	LIBRETRO_US(netmask);
	LIBRETRO_US(sockets);
	sockets &= ~1ull;
	u32 count = 0;
	u16 pages[NETMEM_STATE_PAGES];
	LIBRETRO_US(count);
	LIBRETRO_USA(pages, NETMEM_STATE_PAGES);
	if (count > NETMEM_STATE_PAGES)
		count = NETMEM_STATE_PAGES;
	// what is not in the state was never written to
	for (u32 i = 0; i < NETMEM_SIZE / NETMEM_PAGE / 64; i++)
		for (u32 b = 0; b < 64 && netPages[i] != 0; b++)
			if (netPages[i] & (1ull << b))
				memset(&netmem[(i * 64 + b) * NETMEM_PAGE], 0, NETMEM_PAGE);
	memset(netPages, 0, sizeof(netPages));
	static u8 skip[NETMEM_PAGE];
	for (u32 i = 0; i < NETMEM_STATE_PAGES; i++)
	{
		if (i < count && pages[i] < NETMEM_SIZE / NETMEM_PAGE)
		{
			LIBRETRO_USA(&netmem[pages[i] * NETMEM_PAGE], NETMEM_PAGE);
			netPages[pages[i] / 64] |= 1ull << (pages[i] % 64);
		}
		else
			LIBRETRO_USA(skip, NETMEM_PAGE);
	}
	return true;
}

}

void systemsp_serialize_v24(void **data, unsigned int *total_size)
{
	if (SYSTEM_IS_NAOMI() && naomi_cart_systemsp && systemsp::SystemSpCart::Instance != nullptr)
		systemsp::SystemSpCart::Instance->serializeV24(data, total_size);
}

bool systemsp_unserialize_v24(void **data, unsigned int *total_size)
{
	if (SYSTEM_IS_NAOMI() && naomi_cart_systemsp && systemsp::SystemSpCart::Instance != nullptr)
		return systemsp::SystemSpCart::Instance->unserializeV24(data, total_size);
	return true;
}

/* A state from before V24 is never one of a System SP's, which could not
 * be run before: nothing to do. */
void systemsp_state_before_v24(void)
{
}
