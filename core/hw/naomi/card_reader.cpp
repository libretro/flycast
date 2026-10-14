/*
	Copyright 2022 flyinghead

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
#include "card_reader.h"
#include "naomi_cart.h"
#include "naomi_periph.h"
#include "hw/sh4/modules/modules.h"
#include <algorithm>

namespace card_reader {

static constexpr u8 STX = 2;
static constexpr u8 ETX = 3;
static constexpr u8 ENQ = 5;
static constexpr u8 ACK = 6;

template<typename T>
static u8 calcCrc(const T *begin, const T *end)
{
	u32 crc = 0;
	for (const T *it = begin; it != end; it++)
		crc ^= *it;
	return crc;
}

static retro_atomic_int_t card_save_failed;

/* The state of any of the readers, as a save state has it: the parts a
 * reader does not use stay zero. */
struct ReaderState
{
	u8 flags;			// card in, shutter open, read waiting for a card
	u16 inLen;
	u8 in[256];
	PeriphQueue<512> out;
	u16 rxCommandLen;
	u8 rxCommand[256];
	u8 cardData[0x45 * 3];

	void clear()
	{
		flags = 0;
		inLen = 0;
		memset(in, 0, sizeof(in));
		out.clear();
		memset(out.buf, 0, sizeof(out.buf));
		rxCommandLen = 0;
		memset(rxCommand, 0, sizeof(rxCommand));
		memset(cardData, 0, sizeof(cardData));
	}
};

enum { FLAG_INSERTED = 1, FLAG_DOOR_OPEN = 2, FLAG_READ_PENDING = 4 };

enum ReaderKind : u8
{
	KIND_NONE,
	KIND_INITD,		// Sanwa CRP-1231BR on the SCIF
	KIND_DERBY_BR,	// the same on the I/O board bridge
	KIND_DERBY_LR,	// Sanwa CRP-1231LR on the I/O board bridge
	KIND_CLUBKART,	// Sanwa CR-1231R on the SCIF
};

class CardReaderWriter : public SerialPort::Pipe
{
public:
	CardReaderWriter(ReaderKind kind, u32 cardSize) : kind(kind), cardSize(cardSize)
	{
		st.clear();
		// The card in its file, read once: putting it in later reads it from here
		memset(fileCard, 0, sizeof(fileCard));
		fileSize = (u32)periph_load_file(".card", fileCard, cardSize);
	}
	~CardReaderWriter() override = default;

	void insertCard()
	{
		if (fileSize == 0)
			return;
		memcpy(st.cardData, fileCard, cardSize);
		st.flags |= FLAG_INSERTED;
		INFO_LOG(NAOMI, "Card inserted");
		cardLoaded();
	}

	int available() override {
		return st.out.size();
	}

	u8 read() override {
		return st.out.pop();
	}

	const ReaderKind kind;
	ReaderState st;

protected:
	virtual void cardLoaded() { }

	bool inserted() const { return st.flags & FLAG_INSERTED; }
	void setInserted(bool v) { st.flags = v ? st.flags | FLAG_INSERTED : st.flags & ~FLAG_INSERTED; }

	// What the game wrote on the card, to its file
	void saveCard()
	{
		memcpy(fileCard, st.cardData, cardSize);
		fileSize = cardSize;
		periph_save_file(".card", fileCard, cardSize, &card_save_failed);
	}

	const u32 cardSize;

public:
	/* The card out of the reader - what its file has, or what the game last
	 * wrote on it - is the machine's too, in a state: put in after a state
	 * load, it is the card as it was when the state was saved. */
	static constexpr u32 FILE_STATE_SIZE = 1 + 0x45 * 3;

	void serializeFile(void **data, unsigned int *total_size)
	{
		u8 have = fileSize != 0;
		LIBRETRO_S(have);
		LIBRETRO_SA(fileCard, sizeof(fileCard));
	}

	void setFile(const u8 *state)
	{
		memcpy(fileCard, state + 1, sizeof(fileCard));
		fileSize = state[0] != 0 ? cardSize : 0;
	}

private:
	u8 fileCard[0x45 * 3];
	u32 fileSize = 0;
};

/*
	Sanwa CRP-1231BR-10 card reader/writer protocol (from my good friend Metallic)
	used in InitialD and Derby Owners Club

>>>	SEND PKT: [START 02][LEN][CMD][0][0][0]{optional data}[STOP 03][CRC]
<<<	RECV ACK: [OK 06]
>>>	SEND REQ: [RQ 05]
<<<	RECV PKT: [START 02][LEN][CMD][RESULT1][RESULT2][RESULT3]{optional data}[STOP 03][CRC]
<<< RECV ERR: [15]
	RESULT1 (SENSORS): binary value SSTCCCCC
		S - Shutter state:
			0 - "ERROR"
			1 - "CLOSED"
			2 - "OPEN"
			3 - "NOW" (both open and close sensors)
		T - Stocker (card dispenser) state:
			0 - "NO" (empty)
			1 - "OK" (full)
		C - Card sensors:
			0     - "NO CARD"
			1     - "CARD EXIST (ENTER)" (card inserted in front of shutter)
			18    - "CARD EXIST" (card loaded inside of reader)
			other - "CARD EXIST (OTHER)"
	RESULT2: char 01235QRSTUV`
		Error ENUM "OK", "READ ERR", "WRITE ERR", "STOP ERR", "PRINT ERR", "READERR T1", "READERR T2", "READERR T3",
		 "READERR T12", "READERR T13", "READERR T23", "SHUT ERR"
	RESULT3 (CMD_STATE): char 02345
		State ENUM "OK", "DISABLE", "BUSY", "CARD WAIT", "NO CARD IN BOX"

	Protocol dumps and more:
	https://www.arcade-projects.com/threads/naomi-2-chihiro-triforce-card-reader-emulator-initial-d3-wmmt-mario-kart-f-zero-ax.814/

	Bits from YACardEmu: https://github.com/GXTX/YACardEmu/
	Copyright (C) 2020-2023 wutno (https://github.com/GXTX)
    Copyright (C) 2022-2023 tugpoat (https://github.com/tugpoat)
*/
class SanwaCRP1231BR : public CardReaderWriter
{
public:
	SanwaCRP1231BR(ReaderKind kind) : CardReaderWriter(kind, TRACK_SIZE * 3) { }

	void write(u8 b) override
	{
		if (st.inLen == 0 && b == ENQ)
		{
			DEBUG_LOG(NAOMI, "Received RQ(5)");
			handleCommand();
			return;
		}
		st.in[st.inLen++] = b;
		if (st.inLen < 3)
			return;
		if (st.in[0] != STX)
		{
			INFO_LOG(NAOMI, "Unexpected cmd start byte %x", st.in[0]);
			st.inLen = 0;
			return;
		}
		const u32 len = st.in[1];
		if (st.inLen < len + 2)
		{
			if (st.inLen == sizeof(st.in))
			{
				WARN_LOG(NAOMI, "Card reader buffer overflow");
				st.inLen = 0;
			}
			return;
		}
		const u8 crc = calcCrc(st.in + 1, st.in + st.inLen - 1);
		if (crc != st.in[st.inLen - 1])
		{
			INFO_LOG(NAOMI, "Wrong crc: expected %x got %x", crc, st.in[st.inLen - 1]);
			st.inLen = 0;
			return;
		}
		DEBUG_LOG(NAOMI, "Received cmd %x len %d", st.in[2], st.in[1]);
		st.out.push(ACK);
		st.rxCommandLen = std::min<u32>(st.inLen - 3, sizeof(st.rxCommand));
		memcpy(st.rxCommand, &st.in[2], st.rxCommandLen);
		st.inLen = 0;
	}

protected:
	enum Commands
	{
		CARD_INIT			= 0x10,
		CARD_GET_CARD_STATE	= 0x20,
		CARD_CANCEL			= 0x40,
		CARD_LOAD_CARD		= 0xB0,
		CARD_CLEAN_CARD		= 0xA0,
		CARD_READ			= 0x33,
		CARD_WRITE			= 0x53,
		CARD_PRINT			= 0x7C,
		CARD_PRINT_SETTINGS	= 0x78,
		CARD_REGISTER_FONT	= 0x7A,
		CARD_ERASE_PRINT	= 0x7D,
		CARD_DOOR			= 0xD0,
		CARD_EJECT			= 0x80,
		CARD_NEW            = 0xB0,
	};

	bool doorOpen() const { return st.flags & FLAG_DOOR_OPEN; }
	void setDoorOpen(bool v) { st.flags = v ? st.flags | FLAG_DOOR_OPEN : st.flags & ~FLAG_DOOR_OPEN; }

	virtual u8 getStatus1()
	{
		return ((doorOpen() ? 2 : 1) << 6) | 0x20 | (inserted() ? 0x18 : 0);
	}

	void handleCommand()
	{
		if (st.rxCommandLen == 0)
			return;
		const u8 *cmd = st.rxCommand;
		u8 status1 = getStatus1();
		u8 status2 = '0';
		u8 status3 = '0';
		switch (cmd[0])
		{
		case CARD_DOOR:
			setDoorOpen(cmd[4] == '1');
			INFO_LOG(NAOMI, "Door %s", doorOpen() ? "open" : "closed");
			status1 = getStatus1();
			break;

		case CARD_NEW:
			INFO_LOG(NAOMI, "New card");
			setInserted(true);
			setDoorOpen(false);
			status1 = getStatus1();
			break;

		case CARD_WRITE:
			// 4: mode ('0': read 0x45 bytes, '1': variable length write 1-47 bytes)
			// 5: parity ('0': 7-bit parity, '1': 8-bit no parity)
			// 6: track (see below)
			INFO_LOG(NAOMI, "Card write mode %c parity %c track %c", cmd[4], cmd[5], cmd[6]);
			switch (cmd[6])
			{
			case '0': // track 1
				memcpy(st.cardData, &cmd[7], TRACK_SIZE);
				break;
			case '1': // track 2
				memcpy(st.cardData + TRACK_SIZE, &cmd[7], TRACK_SIZE);
				break;
			case '2': // track 3
				memcpy(st.cardData + TRACK_SIZE * 2, &cmd[7], TRACK_SIZE);
				break;
			case '3': // track 1 & 2
				memcpy(st.cardData, &cmd[7], TRACK_SIZE * 2);
				break;
			case '4': // track 1 & 3
				memcpy(st.cardData, &cmd[7], TRACK_SIZE);
				memcpy(st.cardData + TRACK_SIZE * 2, &cmd[7 + TRACK_SIZE], TRACK_SIZE);
				break;
			case '5': // track 2 & 3
				memcpy(st.cardData + TRACK_SIZE, &cmd[7], TRACK_SIZE * 2);
				break;
			case '6': // track 1 2 & 3
				memcpy(st.cardData, &cmd[7], TRACK_SIZE * 3);
				break;
			default:
				WARN_LOG(NAOMI, "Unknown track# %02x", cmd[6]);
				break;
			}
			saveCard();
			break;

		case CARD_READ:
			// 4: mode ('0': read 0x45 bytes, '1': variable length read 1-47 bytes, '2': card capture, pull in card?)
			// 5: parity ('0': 7-bit parity, '1': 8-bit no parity)
			// 6: track (see below)
			DEBUG_LOG(NAOMI, "Card read mode %c parity %c track %c", cmd[4], cmd[5], cmd[6]);
			if (!inserted() || doorOpen())
				status3 = inserted() ? '0' : '4';
			break;

		case CARD_EJECT:
			NOTICE_LOG(NAOMI, "Card ejected");
			setInserted(false);
			status1 = getStatus1();
			break;

		case CARD_CANCEL:
		case CARD_GET_CARD_STATE:
		case CARD_INIT:
		case CARD_REGISTER_FONT:
		case CARD_PRINT_SETTINGS:
		case CARD_PRINT:
		case CARD_CLEAN_CARD:
			break;

		default:
			WARN_LOG(NAOMI, "Unknown command %x", cmd[0]);
			break;
		}
		// STX, length, command, three results, up to three tracks, ETX, crc
		u8 reply[8 + TRACK_SIZE * 3];
		u32 n = 0;
		reply[n++] = STX;
		reply[n++] = 6;		// the length, the data's added below
		reply[n++] = cmd[0];
		reply[n++] = status1;
		reply[n++] = status2;
		reply[n++] = status3;
		if (cmd[0] == CARD_READ && inserted() && !doorOpen() && cmd[4] != '2')
		{
			u32 idx = 0;
			u32 size = TRACK_SIZE;
			switch (cmd[6])
			{
			case '0': // track 1
				break;
			case '1': // track 2
				idx = TRACK_SIZE;
				break;
			case '2': // track 3
				idx = TRACK_SIZE * 2;
				break;
			case '3': // track 1 & 2
				size = TRACK_SIZE * 2;
				break;
			case '4': // track 1 & 3
				memcpy(&reply[n], st.cardData, TRACK_SIZE);
				n += TRACK_SIZE;
				reply[1] += TRACK_SIZE;
				idx = TRACK_SIZE * 2;
				break;
			case '5': // track 2 & 3
				idx = TRACK_SIZE;
				size = TRACK_SIZE * 2;
				break;
			case '6': // track 1 2 & 3
				size = TRACK_SIZE * 3;
				break;
			default:
				WARN_LOG(NAOMI, "Unknown track# %02x", cmd[6]);
				size = 0;
				break;
			}
			memcpy(&reply[n], st.cardData + idx, size);
			n += size;
			reply[1] += size;
		}
		reply[n++] = ETX;
		reply[n] = calcCrc(reply + 1, reply + n);
		n++;
		st.out.push(reply, n);
	}

	static constexpr u32 TRACK_SIZE = 0x45;
};

class SanwaCRP1231LR : public SanwaCRP1231BR
{
public:
	SanwaCRP1231LR(ReaderKind kind) : SanwaCRP1231BR(kind) { }

protected:
	u8 getStatus1() override
	{
		// '0'	no card
		// '1'	pos magnetic read/write
		// '2'	pos thermal printer
		// '3'	pos thermal dispenser
		// '4'	ejected not removed
		return inserted() ? '1' : '0';
	}
};

// Hooked to the SH4 SCIF serial port
class InitialDCardReader final : public SanwaCRP1231BR
{
public:
	InitialDCardReader() : SanwaCRP1231BR(KIND_INITD) {
		scif_set_pipe(this);
	}

	~InitialDCardReader() override {
		scif_set_pipe(NULL);
	}
};

// Hooked to the MIE via a 838-13661 RS232/RS422 converter board
class DerbyBRCardReader final : public SanwaCRP1231BR
{
public:
	DerbyBRCardReader() : SanwaCRP1231BR(KIND_DERBY_BR) {
		naomi_mie_set_pipe(this);
	}

	~DerbyBRCardReader() override {
		naomi_mie_set_pipe(NULL);
	}
};

class DerbyLRCardReader final : public SanwaCRP1231LR
{
public:
	DerbyLRCardReader() : SanwaCRP1231LR(KIND_DERBY_LR) {
		naomi_mie_set_pipe(this);
	}

	~DerbyLRCardReader() override {
		naomi_mie_set_pipe(NULL);
	}
};

/*
	Club Kart - Sanwa CR-1231R

>>>	SEND CMD: [START 02][CMD char1][CMD char2]{parameter char}{data}[STOP 03][CRC]
<<<	RECV ACK: [OK 06] or [ERR 15]
<<<	RECV STX: [START 02]{REPLY}{data}[STOP 03][CRC]
	note: it seems reply packet sent only after command fully completed or error happened

	REPLY: 2chars
		OK - RESULT_OK
		O1 - RESULT_CANCEL_INSERT
		N0 - ERROR_CONNECT / Unknown Error
		N1 - ERROR_COMMAND / Connection Error
		N2 - ERROR_MOTOR / Mechanic Error 1
		N3 - ERROR_HEAD_UPDOWN / Mechanic Error 2
		N4 - ERROR_CARD_STUCK / Card Stuffed
		N5 - ERROR_VERIFY / OK ????
		N6 - ERROR_HEAD_TEMP / Mechanic Error 3
		N7 - ERROR_CARD_EMPTY / Card Empty
		N8 - ERROR_CARD_LOAD / Draw Card Error
		N9 - ERROR_NO_HOPPER / Card Empty
		NA - ERROR_CARD_PRESENT
		NB - ERROR_CARD_EJECT
		NC - ERROR_CANT_CANCEL
		ND - ERROR_NOT_INSERT
		NE - ERROR_NOT_WAIT
		NF - ERROR_BAD_CARD

	CMD SS REPLY: 6 ASCII characters
		5chars '0'/'1' - Card Sensors Status, MSB first
			0
			10 18
			1C C E 8 7 3
			other
		1char '0'/'1'  - Dispenser Status
			'0' - Empty
			'1' - Full

Commands:
IN   - init
CA   - cancel command
OT0  - eject card
HI   - get new card from dispenser
CL   - cleaning
RT5  - unknown
RL   - read data/load card into reader
WL   - write data (followed by 69 bytes of data)
SS   - get status

reply for commands is simple
06
02 'O' 'K' 03 crc
except
- RL command, which have 69 bytes of card data after OK (or no any reply if no card insterted)
- SS command, which is
06
02 '0/1' '0/1' '0/1' '0/1' '0/1' '0/1' 03 crc
there 0/1 - encoded in char binary value described earlier
 */
class ClubKartCardReader final : public CardReaderWriter
{
public:
	ClubKartCardReader() : CardReaderWriter(KIND_CLUBKART, TRACK_SIZE) {
		scif_set_pipe(this);
	}
	~ClubKartCardReader() override {
		scif_set_pipe(NULL);
	}

	void write(u8 data) override
	{
		if (st.inLen == sizeof(st.in))
			st.inLen = 0;
		st.in[st.inLen++] = data;
		if (st.inLen == 5)
		{
			if ((st.in[1] != 'W' || st.in[2] != 'L') && st.in[2] != 'T')
			{
				handleCommand();
				st.inLen = 0;
			}
		}
		else if (st.inLen == 6 && st.in[2] == 'T') // OT0, RT5
		{
			handleCommand();
			st.inLen = 0;
		}
		else if (st.inLen == TRACK_SIZE + 5) // WL
		{
			handleCommand();
			st.inLen = 0;
		}
	}

private:
	enum Commands {
		CARD_INIT,
		CARD_CANCEL_CMD,
		CARD_EJECT,
		CARD_NEW,
		CARD_CLEAN,
		CARD_RT5,
		CARD_READ,
		CARD_WRITE,
		CARD_STATUS,

		CARD_MAX
	};
	static const u8 CommandBytes[][2];

	void cardLoaded() override
	{
		if (st.flags & FLAG_READ_PENDING)
		{
			sendReply(CARD_READ);
			st.flags &= ~FLAG_READ_PENDING;
			scif_update_status();
		}
	}

	void handleCommand()
	{
		st.flags &= ~FLAG_READ_PENDING;
		int cmd;
		for (cmd = 0; cmd < CARD_MAX; cmd++)
			if (st.in[1] == CommandBytes[cmd][0] && st.in[2] == CommandBytes[cmd][1])
				break;
		if (cmd == CARD_MAX)
		{
			WARN_LOG(NAOMI, "Unhandled command '%c%c'", st.in[1], st.in[2]);
			return;
		}
		const u8 crc = calcCrc(st.in + 1, st.in + st.inLen - 1);
		if (crc != st.in[st.inLen - 1])
		{
			WARN_LOG(NAOMI, "Wrong crc: expected %x got %x", crc, st.in[st.inLen - 1]);
			return;
		}
		st.out.push(ACK);
		switch (cmd)
		{
		case CARD_WRITE:
			INFO_LOG(NAOMI, "Card write");
			memcpy(st.cardData, &st.in[3], TRACK_SIZE);
			saveCard();
			break;
		case CARD_READ:
			INFO_LOG(NAOMI, "Card read");
			if (!inserted()) {
				st.flags |= FLAG_READ_PENDING;
				return;
			}
			break;
		case CARD_EJECT:
			NOTICE_LOG(NAOMI, "Card ejected");
			setInserted(false);
			break;
		case CARD_NEW:
			INFO_LOG(NAOMI, "New card");
			setInserted(true);
			break;
		case CARD_INIT:
			DEBUG_LOG(NAOMI, "Card init");
			break;
		case CARD_CANCEL_CMD:
			DEBUG_LOG(NAOMI, "Cancel cmd");
			break;
		case CARD_CLEAN:
			DEBUG_LOG(NAOMI, "Card clean");
			break;
		case CARD_RT5:
			DEBUG_LOG(NAOMI, "Card RT5");
			break;
		case CARD_STATUS:
			DEBUG_LOG(NAOMI, "Card status (cardInserted %d)", inserted());
			break;
		}
		sendReply(cmd);
	}

	void sendReply(int cmd)
	{
		u8 reply[4 + TRACK_SIZE + 2];
		u32 n = 0;
		reply[n++] = STX;
		if (cmd == CARD_STATUS)
		{
			reply[n++] = '0';
			reply[n++] = '0';
			reply[n++] = '0';
			reply[n++] = inserted() ? '1' : '0';
			reply[n++] = inserted() ? '1' : '0';
			reply[n++] = '1'; // dispenser full
		}
		else
		{
			reply[n++] = 'O';
			reply[n++] = 'K';
			if (cmd == CARD_READ) {
				memcpy(&reply[n], st.cardData, TRACK_SIZE);
				n += TRACK_SIZE;
			}
		}
		reply[n++] = ETX;
		reply[n] = calcCrc(reply + 1, reply + n);
		n++;
		st.out.push(reply, n);
	}

	static constexpr u32 TRACK_SIZE = 0x45;
};

const u8 ClubKartCardReader::CommandBytes[][2]
{
	{ 'I', 'N' },	// init
	{ 'C', 'A' },	// cancel command
	{ 'O', 'T' },	// ...0  - eject card
	{ 'H', 'I' },	// get new card from dispenser
	{ 'C', 'L' },	// cleaning
	{ 'R', 'T' },	// ...5  - unknown
	{ 'R', 'L' },	// read data/load card into reader
	{ 'W', 'L' },	// write data (followed by 69 bytes of data)
	{ 'S', 'S' },	// get status
};

static CardReaderWriter *cardReader;

void initdInit() {
	term();
	cardReader = new InitialDCardReader();
}

void derbyInit()
{
	term();
	if (!strcmp(naomi_game_id, " DERBY OWNERS CLUB ------------"))
		cardReader = new DerbyLRCardReader();
	else
		cardReader = new DerbyBRCardReader();
}

void clubkInit() {
	term();
	cardReader = new ClubKartCardReader();
}

void term() {
	delete cardReader;
	cardReader = NULL;
}

/* A barcode reader: what is printed on a card is sent as it is read,
 * followed by the reader's end character. What it reads is the first line
 * of the game's .barcode file in the save directory (Code 39 text, such as
 * "*0123456789*"), unless something has been given it since. */
class BarcodeReader final : public SerialPort::Pipe
{
public:
	BarcodeReader(char eotChar) : eotChar(eotChar)
	{
		toSend.clear();
		char line[64];
		const size_t len = periph_load_file(".barcode", line, sizeof(line) - 1);
		line[len] = '\0';
		line[strcspn(line, "\r\n")] = '\0';
		card = line;
		scif_set_pipe(this);
	}

	~BarcodeReader() override {
		scif_set_pipe(NULL);
	}

	int available() override {
		return toSend.size();
	}

	u8 read() override {
		return toSend.pop();
	}

	void insertCard()
	{
		if (toSend.size() >= 32 || card.empty())
			return;
		INFO_LOG(NAOMI, "Card read: %s", card.c_str());
		for (char c : card)
			toSend.push((u8)c);
		toSend.push((u8)eotChar);
		scif_update_status();
	}

	std::string getCard() const	{
		return card;
	}

	void setCard(const std::string& card) {
		this->card = card.substr(0, 31);
	}

	PeriphQueue<64> toSend;

private:
	const char eotChar;
	std::string card;
};

static BarcodeReader *barcodeReader;

void barcodeInit()
{
	barcodeTerm();
	if (!strcmp(naomi_game_id, "BTR 2K9 VER 1.004"))	// System SP Battle Racer
		barcodeReader = new BarcodeReader('\r');
	else
		barcodeReader = new BarcodeReader('*');
}

void barcodeTerm() {
	delete barcodeReader;
	barcodeReader = NULL;
}

bool barcodeAvailable() {
	return barcodeReader != NULL;
}

std::string barcodeGetCard()
{
	if (barcodeReader != NULL)
		return barcodeReader->getCard();
	else
		return "";
}

void barcodeSetCard(const std::string& card)
{
	if (barcodeReader != NULL)
		barcodeReader->setCard(card);
}

void insertCard(int playerNum)
{
	if (cardReader != NULL)
		cardReader->insertCard();
	else if (barcodeReader != NULL)
		barcodeReader->insertCard();
	else
		insertRfidCard(playerNum);
}

bool readerAvailable()
{
	return cardReader != NULL
			|| barcodeAvailable()
			|| getRfidCardData(0) != NULL;
}

/* The readers' part of a save state (V24): which reader the game has and
 * all of what it holds - the bytes on their way each way, the command
 * being answered, the card in it and the state of its sensors - and the
 * bytes the barcode reader has still to send. A reader the state has and
 * the game has not is read past. 1238 bytes for a card reader, 66 for a
 * barcode reader, 2 more. */
void serialize_v24(void **data, unsigned int *total_size)
{
	u8 kinds[2] = { (u8)(cardReader != NULL ? cardReader->kind : KIND_NONE),
			(u8)(barcodeReader != NULL) };
	LIBRETRO_SA(kinds, 2);
	if (cardReader != NULL)
	{
		const ReaderState& st = cardReader->st;
		LIBRETRO_S(st.flags);
		LIBRETRO_S(st.inLen);
		LIBRETRO_SA(st.in, sizeof(st.in));
		st.out.serialize(data, total_size);
		LIBRETRO_S(st.rxCommandLen);
		LIBRETRO_SA(st.rxCommand, sizeof(st.rxCommand));
		LIBRETRO_SA(st.cardData, sizeof(st.cardData));
	}
	if (barcodeReader != NULL)
		barcodeReader->toSend.serialize(data, total_size);
}

bool unserialize_v24(void **data, unsigned int *total_size)
{
	u8 kinds[2] = { KIND_NONE, 0 };
	if (!LIBRETRO_USA(kinds, 2))
		return false;
	if (kinds[0] > KIND_CLUBKART)
		return false;
	if (kinds[0] != KIND_NONE)
	{
		static ReaderState st;
		st.clear();
		LIBRETRO_US(st.flags);
		LIBRETRO_US(st.inLen);
		LIBRETRO_USA(st.in, sizeof(st.in));
		st.out.unserialize(data, total_size);
		LIBRETRO_US(st.rxCommandLen);
		LIBRETRO_USA(st.rxCommand, sizeof(st.rxCommand));
		if (!LIBRETRO_USA(st.cardData, sizeof(st.cardData)))
			return false;
		st.flags &= FLAG_INSERTED | FLAG_DOOR_OPEN | FLAG_READ_PENDING;
		if (st.inLen >= sizeof(st.in))
			st.inLen = 0;
		if (st.rxCommandLen > sizeof(st.rxCommand))
			st.rxCommandLen = 0;
		if (cardReader != NULL && cardReader->kind == kinds[0])
			cardReader->st = st;
	}
	else if (cardReader != NULL)
		cardReader->st.clear();
	if (kinds[1] != 0)
	{
		static PeriphQueue<64> toSend;
		toSend.unserialize(data, total_size);
		if (barcodeReader != NULL)
			barcodeReader->toSend = toSend;
	}
	else if (barcodeReader != NULL)
		barcodeReader->toSend.clear();
	return true;
}

/* An older state: no reader had anything on its way, and no card was in
 * one, which is how a game that had none saw it. */
void state_before_v24()
{
	if (cardReader != NULL)
		cardReader->st.clear();
	if (barcodeReader != NULL)
		barcodeReader->toSend.clear();
}

}

/* The cards out of the readers, in a save state from V30 on, on a NAOMI: a
 * byte with the card reader's kind (bits 0-2) and whether there are IC card
 * readers (bit 3); then the card reader's card as its file has it (208
 * bytes), and the two IC cards' (256 bytes). 1 byte for a game with none,
 * 209 with a card reader, 257 with IC card readers. What the game wrote on
 * a card goes to its file and stays there whatever state is loaded; without
 * this the card put in after a load was the one written after the state was
 * saved (and run-ahead writes ahead of what is shown). A card the state has
 * and the game has no reader for is read past. */
enum { V30_RFID = 8 };

void naomi_cards_serialize_v30(void **data, unsigned int *total_size)
{
	using namespace card_reader;
	if (!SYSTEM_IS_NAOMI())
		return;
	u8 have = (u8)(cardReader != NULL ? cardReader->kind : KIND_NONE);
	if (getRfidCardData(0) != NULL)
		have |= V30_RFID;
	LIBRETRO_S(have);
	if (cardReader != NULL)
		cardReader->serializeFile(data, total_size);
	if (have & V30_RFID)
		rfid_files_serialize_v30(data, total_size);
}

bool naomi_cards_unserialize_v30(void **data, unsigned int *total_size)
{
	using namespace card_reader;
	if (!SYSTEM_IS_NAOMI())
		return true;
	u8 have = 0;
	if (!LIBRETRO_US(have))
		return false;
	const u8 kind = have & 7;
	// (anything else, and how much follows is not known)
	if (kind > KIND_CLUBKART || (have & ~(7 | V30_RFID)) != 0)
		return false;
	if (kind != KIND_NONE)
	{
		u8 file[CardReaderWriter::FILE_STATE_SIZE];
		if (!LIBRETRO_USA(file, sizeof(file)))
			return false;
		if (cardReader != NULL && cardReader->kind == kind)
			cardReader->setFile(file);
	}
	if (have & V30_RFID)
		return rfid_files_unserialize_v30(data, total_size);
	return true;
}

/* An older state: the cards out of the readers are what they are now, as
 * they were with such a state before. */
void naomi_cards_state_before_v30(void)
{
}
