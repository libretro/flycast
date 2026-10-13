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
#include "hopper.h"
#include "naomi_cart.h"
#include "naomi_periph.h"
#include "hw/sh4/modules/modules.h"
#include "hw/maple/maple_devs.h"

#include <array>
#include <algorithm>

namespace hopper
{

/* What the board keeps through a power cut, and in its file in the save
 * directory: the credits, the counters and the operator's settings. All
 * of it 32-bit words, flags 0 or 1. */
struct HopperConfig
{
	u32 credit0;
	u32 credit1;
	u32 totalCredit;
	u32 premium;
	u32 gameNumber;
	u32 freePlay;
	u32 autoPayOut;
	u32 autoExchange;
	u32 twoWayMode;
	u32 coinDiscrimination;
	u32 betButton;
	u32 currency;		// 0:medal, 1:pound, 2:dollar, 3:euro, 4:token, 5: any cash (837-14438 only)
	u32 medalExchRate;
	u32 maxHopperFloat;
	u32 maxPay;
	u32 maxCredit;
	u32 hopperSize;
	u32 maxBet;
	u32 minBet;
	u32 addBet;
	u32 paidAmount;
	u32 wonAmount;
	u32 curBase;

	void setInitial()
	{
		credit0 = 0;
		credit1 = 0;
		totalCredit = 100; // min bet
		premium = 0;
		gameNumber = 0;
		freePlay = 0;
		autoPayOut = 0;
		autoExchange = 0;
		twoWayMode = 1;
		coinDiscrimination = 1;
		betButton = 1;
		currency = 0xff;
		medalExchRate = 5;
		maxHopperFloat = 200;
		maxPay = 1999900;
		maxCredit = 1999900;
		hopperSize = 39900;
		maxBet = 10000;
		minBet = 100; // normally 1000
		addBet = 100;
		paidAmount = 0;
		wonAmount = 0;
		curBase = 100;
	}
	void normalize()
	{
		freePlay &= 1;
		autoPayOut &= 1;
		autoExchange &= 1;
		twoWayMode &= 1;
		coinDiscrimination &= 1;
		betButton &= 1;
		currency &= 0xff;
		medalExchRate &= 0xff;
		curBase &= 0xff;
	}
};
static_assert(sizeof(HopperConfig) == 23 * 4, "HopperConfig is words only");

static retro_atomic_int_t config_save_failed;

class BaseHopper : public SerialPort::Pipe
{
public:
	BaseHopper()
	{
		recvLen = 0;
		memset(recv, 0, sizeof(recv));
		expectedBytes = 0;
		toSend.clear();
		started = 0;
		coinKey = 0;
		if (periph_load_file("-hopper.bin", &cfg, sizeof(cfg)) == sizeof(cfg))
			cfg.normalize();
		else
		{
			INFO_LOG(NAOMI, "Hopper config not found");
			cfg.setInitial();
		}
		saved = cfg;
	}

	virtual ~BaseHopper() = default;

	u8 read() override {
		return toSend.pop();
	}

	int available() override {
		return toSend.size();
	}

	// 'H' <u8 command> <u16 length> payload ... <u8 checksum>
	void write(u8 data) override
	{
		if (recvLen == 0 && data != 'H') {
			WARN_LOG(NAOMI, "Ignored data %02x %c", data, data);
			return;
		}
		recv[recvLen++] = data;
		if (recvLen == 3)
			expectedBytes = data;
		else if (recvLen == 4)
		{
			expectedBytes += data << 8;
			// (no message is shorter than its header and checksum, and none the game sends is this long)
			if (expectedBytes < 5 || expectedBytes > sizeof(recv))
			{
				WARN_LOG(NAOMI, "Hopper message of %d bytes dropped", expectedBytes);
				clearRecv();
			}
		}
		else if (expectedBytes > 0 && recvLen == expectedBytes)
		{
			handleMessage(recv[1]);
			clearRecv();
			saveConfig();
		}
	}

	// The coin switch: a coin goes into the hopper
	void poll()
	{
		if (!started)
			return;
		const bool down = (mcfg_ArcadeKcode(0) & NAOMI_COIN_KEY) == 0;
		if (down && !coinKey)
		{
			sendCoinInMessage();
			saveConfig();
		}
		coinKey = down;
	}

	void clearState()
	{
		clearRecv();
		toSend.clear();
		started = 0;
		coinKey = 0;
	}

	HopperConfig cfg;
	u8 recv[256];
	u32 recvLen;
	u32 expectedBytes;
	PeriphQueue<2048> toSend;
	u8 started;
	u8 coinKey;

protected:
	virtual void handleMessage(u8 command) = 0;
	virtual void sendCoinInMessage() = 0;
	virtual void sendPayWinMessage() = 0;

	u32 rd32(u32 offset) const
	{
		u32 v;
		memcpy(&v, &recv[offset], sizeof(v));
		return v;
	}

	void sendMessage(u8 command, const u8 *payload, size_t len)
	{
		DEBUG_LOG(NAOMI, "hopper sending command %x size %x", command, (int)len + 5);
		u8 chksum = 'H' + command;
		toSend.push('H');
		toSend.push(command);
		len += 5;
		toSend.push(len & 0xff);
		chksum += len & 0xff;
		toSend.push(len >> 8);
		chksum += len >> 8;
		len -= 5;
		for (size_t i = 0; i < len; i++, payload++) {
			toSend.push(*payload);
			chksum += *payload;
		}
		toSend.push(chksum);
		scif_update_status();
	}

	void bet(u32 offset)
	{
		u32 values[3];
		for (int i = 0; i < 3; i++)
			values[i] = rd32(offset + i * 4);
		for (int i = 0; i < 2; i++)
		{
			u32& primary = i == 0 ? cfg.credit0 : cfg.credit1;
			u32& second = i == 0 ? cfg.credit1 : cfg.credit0;
			if (primary >= values[i]) {
				primary -= values[i];
			}
			else
			{
				int residual = values[i] - primary;
				primary = 0;
				second = std::max(0, (int)second - residual);
			}
		}
		cfg.premium = std::max(0, (int)cfg.premium - (int)values[2]);
	}

	void payOut(u32 value)
	{
		if (value == 0)
			return;
		cfg.wonAmount += value;
		if (!cfg.autoPayOut)
			cfg.credit0 += value;
		else {
			cfg.paidAmount += value;
			sendPayWinMessage();
		}
	}

	void insertCoin(u32 value) {
		cfg.credit0 += value;
	}

private:
	void clearRecv()
	{
		memset(recv, 0, sizeof(recv));
		recvLen = 0;
		expectedBytes = 0;
	}

	// What has changed of what the board keeps, to its file
	void saveConfig()
	{
		if (!memcmp(&cfg, &saved, sizeof(cfg)))
			return;
		saved = cfg;
		periph_save_file("-hopper.bin", &saved, sizeof(saved), &config_save_failed);
	}

	HopperConfig saved;
};

//
// SEGA 837-14438 hopper board
//
// Used by kick'4'cash
// Can be used by club kart prize (ver.b) and shootout pool prize (ver.b) if P1 btn 0 is pressed at boot
//
class Sega837_14438Hopper : public BaseHopper
{
protected:
	void handleMessage(u8 command) override
	{
		switch (command)
		{
			case 0x40:
			{
				// VERSION
				INFO_LOG(NAOMI, "hopper received VERSION");
				std::array<u32, 4> payload{};
				payload[0] = 0x00010001;
				payload[1] = 3;
				sendMessage(0x20, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x41:
			{
				// POWER ON
				INFO_LOG(NAOMI, "hopper received POWER ON");
				// 8: currency
				if (cfg.currency != recv[8]) {
					cfg.currency = recv[8];
					setDefaults();
				}
				std::array<u32, 0x1ea> payload{};
				payload[0] = cfg.gameNumber;	// game number
				payload[1] = 0;         	// atp num
				payload[2] = 0;         	// atp kind and error code
				payload[3] = status;		// status bitfield. rectangle in background if != 0
				payload[4] = cfg.freePlay | (cfg.autoPayOut << 8) | (cfg.twoWayMode << 16) | (cfg.medalExchRate << 24);
				payload[5] = cfg.autoExchange | (cfg.coinDiscrimination << 8) | (cfg.curBase << 16) | (cfg.betButton << 24);
				payload[6] = 0;				// ? 8c027124
				payload[7] = cfg.maxPay;
				payload[8] = cfg.maxCredit;
				payload[9] = cfg.hopperSize;
				payload[10] = cfg.maxBet;
				payload[11] = cfg.minBet;
				payload[12] = cfg.addBet;
				payload[13] = cfg.currency;	// currency (lsb, 0:medal, 1:pound, 2:dollar, 3:euro, 4:token, 5: any cash)
				payload[14] = cfg.credit0;
				payload[15] = cfg.credit1;
				payload[16] = cfg.totalCredit;
				payload[17] = cfg.premium;
				payload[0x2d] = cfg.wonAmount;
				payload[0x32] = cfg.paidAmount;	// paid? 8c027190
				payload[0x33] = cfg.maxHopperFloat;
				payload[0x37] = 0x80;	// dataportCondition (b6: protocol on, bit7: forever)
				// Default values
				const u32 *def = getDefaultValues();
				payload[0x1c0] = def[0];
				payload[0x1c1] = def[1];
				payload[0x1c3] = def[2];
				payload[0x1c4] = def[3];
				payload[0x1c5] = def[4];
				payload[0x1c6] = def[5];
				payload[0x1c7] = def[6];
				payload[0x1c8] = def[7];
				getRanges(&payload[0x1c9]);
				payload[0x1e9] = 1;		// hopper ready flag
				sendMessage(0x21, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				started = 1;
				break;
			}

			case 0x42:
			{
				// GET_STATUS
				INFO_LOG(NAOMI, "hopper received GET STATUS");
				std::array<u32, 4> payload{};
				payload[0] = 0;         // atp num
				payload[1] = 0;         // atp kind and error code
				payload[2] = status;
				sendMessage(0x22, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x43:
			{
				// GAME START
				INFO_LOG(NAOMI, "hopper received GAME START");
				bet(4);
				std::array<u32, 0x1f> payload{};
				payload[0] = ++cfg.gameNumber;	// game#, ?
				payload[1] = 0;				// atp num
				payload[2] = 0;				// atp kind and error code
				payload[3] = status;
				payload[4] = cfg.credit0;
				payload[5] = cfg.credit1;
				payload[6] = cfg.totalCredit;
				payload[7] = cfg.premium;
				sendMessage(0x23, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x44:
			{
				// GAME END
				INFO_LOG(NAOMI, "hopper received GAME END");
				std::array<u32, 0x1e> payload{};
				payload[0] = cfg.gameNumber;	// gameNum, upper 16 bits: ? 0, -1 or -2
				payload[1] = cfg.credit0;
				payload[2] = cfg.credit1;
				payload[3] = cfg.totalCredit;
				payload[4] = cfg.premium;
				payload[27] = 2;			// wins
				payload[28] = 0;			// last paid
				sendMessage(0x24, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x45:
			{
				// TEST
				INFO_LOG(NAOMI, "hopper received TEST");
				std::array<u32, 0x3c> payload{};
				payload[0] = 0;         // atp num
				payload[1] = 0;         // atp kind and error code
				payload[2] = status;
				payload[0x38] = 0;
				sendMessage(0x25, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x4a:
				// SWITCH
				INFO_LOG(NAOMI, "hopper received SWITCH");
				break;

			case 0x4b:
			{
				// CONFIG_HOP
				INFO_LOG(NAOMI, "hopper received CONFIG HOP");
				if (recv[2] == 0x2c && recv[3] == 0)
				{
					cfg.freePlay = recv[4] & 1;
					cfg.autoPayOut = recv[5] & 1;
					cfg.twoWayMode = recv[6] & 1;
					cfg.medalExchRate = recv[7];
					cfg.autoExchange = recv[8] & 1;
					cfg.coinDiscrimination = recv[9] & 1;
					cfg.curBase = recv[0xa];
					cfg.betButton = recv[0xb] & 1;
					// 0xc ??
					cfg.maxPay = rd32(0x10);
					cfg.maxCredit = rd32(0x14);
					cfg.hopperSize = rd32(0x18);
					cfg.maxBet = rd32(0x1c);
					cfg.minBet = rd32(0x20);
					cfg.addBet = rd32(0x24);
				}
				std::array<u32, 0xa> payload{};
				payload[0] = cfg.freePlay | (cfg.autoPayOut << 8) | (cfg.twoWayMode << 16) | (cfg.medalExchRate << 24);
				payload[1] = cfg.autoExchange | (cfg.coinDiscrimination << 8) | (cfg.curBase << 16) | (cfg.betButton << 24);
				payload[2] = 0;				// ? 8c027124
				payload[3] = cfg.maxPay;
				payload[4] = cfg.maxCredit;
				payload[5] = cfg.hopperSize;
				payload[6] = cfg.maxBet;
				payload[7] = cfg.minBet;
				payload[8] = cfg.addBet;
				payload[9] = cfg.currency;
				sendMessage(0x2b, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x61: // COIN IN ACK
			case 0x62: // COIN OUT ACK
				break;

			default:
				WARN_LOG(NAOMI, "Unexpected hopper message: %x", command);
				break;
		}
	}

private:
	void sendCoinInMessage() override
	{
		insertCoin(100);
		std::array<u32, 8> payload{};
		payload[0] = 0;         // atp num
		payload[1] = 0;         // atp kind and error code
		payload[2] = status;
		payload[3] = cfg.credit0;
		payload[4] = cfg.credit1;
		payload[5] = cfg.totalCredit;
		payload[6] = cfg.premium;
		sendMessage(1, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
	}

	void sendPayWinMessage() override
	{
		std::array<u32, 0xa> payload{};
		payload[0] = 0;         // atp num
		payload[1] = 0;         // atp kind and error code
		payload[2] = status;
		payload[3] = cfg.credit0;
		payload[4] = cfg.credit1;
		payload[5] = cfg.totalCredit;
		payload[6] = cfg.premium;
		payload[7] = cfg.wonAmount;
		payload[8] = cfg.paidAmount;
		sendMessage(3, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
	}

	const u32 *getDefaultValues()
	{
		static const u32 defaults[][8] = {
			// free play...           max pay         hopper size   min bet
			//            auto exch...        max credit      max bet     add bet
			{ 0x05010000, 0x01640100, 1999900, 1999900, 39900, 10000, 1000, 100 },
			{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100,  50 },
			{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
			{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
			{          0, 0x01640000, 1999900, 1999900, 39900, 10000,  100, 100 },
			{      0x100,    0x50000,   10000, 1999900, 39900,   100,  100, 100 },
		};
		if (cfg.currency >= sizeof(defaults) / sizeof(defaults[0]))
			return defaults[0];
		else
			return defaults[cfg.currency];
	}

	void setDefaults()
	{
		const u32 *def = getDefaultValues();
		cfg.freePlay = def[0] & 1;
		cfg.autoPayOut = (def[0] & 0x100) != 0;
		cfg.twoWayMode = (def[0] & 0x10000) != 0;
		cfg.medalExchRate = def[0] >> 24;
		cfg.autoExchange = def[1] & 1;
		cfg.coinDiscrimination = (def[1] & 0x100) != 0;
		cfg.curBase = (def[1] >> 16) & 0xff;
		cfg.betButton = (def[1] & 0x1000000) != 0;
		cfg.maxPay = def[2];
		cfg.maxCredit = def[3];
		cfg.hopperSize = def[4];
		cfg.maxBet = def[5];
		cfg.minBet = def[6];
		cfg.addBet = def[7];
		cfg.totalCredit = cfg.minBet;
	}

	void getRanges(u32 *p)
	{
		switch (cfg.currency)
		{
		case 0: // medal
		case 4: // token
			p[0x0] = 100;
			p[0x1] = 10000;
			p[0x2] = 100 | (499 << 16);
			p[0x3] = 500 | (499 << 16);
			if (cfg.currency == 0)
			{
				p[0x4] = 500 | (49 << 16);
				p[0x5] = 50;
				p[0x6] = 200;
				p[0xa] = 2000 | (200 << 16);
				p[0xb] = 100;
			}
			else
			{
				p[0x4] = 500 | (19 << 16);
				p[0x5] = 20;
				p[0x6] = 200;
				p[0xa] = 200 | (50 << 16);
				p[0xb] = 10;
			}
			p[0x7] = 0;
			p[0x8] = 0;
			p[0x9] = 0;
			p[0xc] = 0;

			p[0xd] = 100;
			p[0xe] = 0;
			p[0xf] = 0;
			p[0x10] = 0;

			p[0x11] = 100 | (200 << 16);
			p[0x12] = 300 | (400 << 16);
			p[0x13] = 500 | (1000 << 16);
			p[0x14] = 2000 | (2500 << 16);
			p[0x15] = 5000 | (10000 << 16);
			p[0x16] = 0;
			break;
		case 1: // pound
			p[0x0] = 50;
			p[0x1] = 100;
			p[0x2] = 50 | (5 << 16);
			p[0x3] = 5 | (499 << 16);
			p[0x4] = 500 | (19 << 16);
			p[0x5] = 20;
			p[0x6] = 70;
			p[0x7] = 0;
			p[0x8] = 0;
			p[0x9] = 0;
			p[0xa] = 70 | (70 << 16);
			p[0xb] = 0;
			p[0xc] = 0;

			p[0xd] = 5 | (10 << 16);
			p[0xe] = 20 | (50 << 16);
			p[0xf] = 50 | (100 << 16);
			p[0x10] = 200;

			p[0x11] = 5 | (10 << 16);
			p[0x12] = 20 | (50 << 16);
			p[0x13] = 100 | (200 << 16);
			p[0x14] = 500 | (1000 << 16);
			p[0x15] = 0;
			p[0x16] = 0;
			break;
		case 3: // euro
			p[0x0] = 50;
			p[0x1] = 100;
			p[0x2] = 50 | (5 << 16);
			p[0x3] = 5 | (499 << 16);
			p[0x4] = 500 | (19 << 16);
			p[0x5] = 20;
			p[0x6] = 200;
			p[0x7] = 0;
			p[0x8] = 0;
			p[0x9] = 0;
			p[0xa] = 200 | (50 << 16);
			p[0xb] = 10;
			p[0xc] = 0;

			p[0xd] = 5 | (10 << 16);
			p[0xe] = 20 | (50 << 16);
			p[0xf] = 50 | (100 << 16);
			p[0x10] = 200 | (100 << 16);

			p[0x11] = 5 | (10 << 16);
			p[0x12] = 20 | (50 << 16);
			p[0x13] = 100 | (200 << 16);
			p[0x14] = 500 | (1000 << 16);
			p[0x15] = 0;
			p[0x16] = 0;
			break;
		case 2: // dollar
		case 5: // any cash
		default:
			p[0x0] = 50;
			p[0x1] = 100;
			p[0x2] = 50 | (5 << 16);
			p[0x3] = 5 | (499 << 16);
			p[0x4] = 500 | (19 << 16);
			p[0x5] = 20;
			p[0x6] = 200;
			p[0x7] = 0;
			p[0x8] = 0;
			p[0x9] = 0;
			p[0xa] = 200 | (50 << 16);
			p[0xb] = 10;
			p[0xc] = 0;

			p[0xd] = 5 | (10 << 16);
			p[0xe] = 20 | (25 << 16);
			p[0xf] = 50 | (100 << 16);
			p[0x10] = 200 | (100 << 16);

			p[0x11] = 5 | (10 << 16);
			p[0x12] = 20 | (25 << 16);
			p[0x13] = 50 | (100 << 16);
			p[0x14] = 200 | (500 << 16);
			p[0x15] = 1000;
			p[0x16] = 0;
			break;
		}
	}

	// ?:1
	// ?:1
	// coinInEnabled?:1
	// yenInEnabled?:1
	// frontDoorOpen:1
	// backDoorOpen:1
	// cashDoorOpen:1
	// needsRefill?:1
	// ?:1
	static constexpr u32 status = 0x00000000;
};

// Naomi SWP Hopper Board
class NaomiHopper : public BaseHopper
{
protected:
	void handleMessage(u8 command) override
	{
		switch (command)
		{
			case 0x20:
			{
				// POWER ON
				static const char hopperErrors[][32] = {
					"UNKNOWN ERROR",
					"ROM IS BAD",
					"LOW BATTERY",
					"ROM HAS CHANGED",
					"RAM DATA IS BAD",
					"I/O ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"COIN IN JAM(HOPPER)",
					"COIN IN JAM(GAME)",
					"HOPPER OVER PAID",
					"HOPPER RUNAWAY",
					"HOPPER EMPTY/JAM",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"COM. TIME OUT",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"I/O BOARD IS BAD",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"UNKNOWN ERROR",
					"ATP NONE\0\0\0\0\0\0\0\0MAX. PAY",
					"HOPPER SIZE\0\0\0\0\0MAX. CREDIT",
					"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0DOOR IS OPEN"
				};

				// 8: currency
				if (cfg.currency != recv[8]) {
					cfg.currency = recv[8];
					setDefaults();
				}
				INFO_LOG(NAOMI, "hopper received POWER ON");
				std::array<u32, 0x13a> payload{};
				payload[0] = cfg.gameNumber;
				payload[1] = 0;			// atp num
				payload[2] = status;	// status bitfield. rectangle in background if != 0
				payload[3] = cfg.freePlay | (cfg.autoPayOut << 8) | (cfg.twoWayMode << 16) | (cfg.medalExchRate << 24);
				payload[4] = cfg.autoExchange | (cfg.coinDiscrimination << 8) | (cfg.curBase << 16) | (cfg.betButton << 24);
				payload[5] = cfg.maxPay;
				payload[6] = cfg.maxCredit;
				payload[7] = cfg.hopperSize;
				payload[8] = cfg.maxBet;
				payload[9] = cfg.minBet;
				payload[10] = cfg.addBet;
				payload[11] = cfg.currency;		// ? BYTE_8c0c9fa8-b
				payload[12] = cfg.credit0;
				payload[13] = cfg.credit1;
				payload[14] = cfg.totalCredit;
				payload[15] = cfg.premium;
				payload[16] = cfg.wonAmount;
				payload[17] = cfg.paidAmount;	// paid amount (if autoPO) else credit won?
				size_t idx = 0x84;
				for (size_t i = 0; i < sizeof(hopperErrors) / sizeof(hopperErrors[0]); i++) {
					memcpy((u8 *)payload.data() + idx, hopperErrors[i], sizeof(hopperErrors[i]));
					idx += 32;
				}
				// init done/hopper ready?
				payload[0x139] = 1;
				sendMessage(0x10, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);

				started = 1;
				break;
			}

			case 0x21:	// GET_STATUS
			{
				INFO_LOG(NAOMI, "hopper received GET STATUS");
				std::array<u32, 3> payload{};
				payload[0] = 0;			// atp num?
				payload[1] = status;
				sendMessage(0x11, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x22:	// GAME START
			{
				INFO_LOG(NAOMI, "hopper received GAME START");
				// 4: bet value: credit0
				// 8:            credit1
				// c:			 premium
				bet(4);
				cfg.wonAmount = 0;
				cfg.paidAmount = 0;
				std::array<u32, 0x17> payload{};
				payload[0] = ++cfg.gameNumber;
				payload[1] = 0;				// atp num
				payload[2] = status;
				payload[3] = cfg.credit0;
				payload[4] = cfg.credit1;
				payload[5] = cfg.totalCredit;
				payload[6] = cfg.premium;
				sendMessage(0x12, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x23:	// GAME END
			{
				INFO_LOG(NAOMI, "hopper received GAME END");
				// 8: amount to pay?
				payOut(rd32(8));
				std::array<u32, 0x15> payload{};
				payload[0] = cfg.gameNumber;	// gameNum, upper 16 bits: ? 0, -1 or -2
				payload[1] = cfg.credit0;
				payload[2] = cfg.credit1;
				payload[3] = cfg.totalCredit;
				payload[4] = cfg.premium;
				sendMessage(0x13, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x24:	// TEST START
			{
				INFO_LOG(NAOMI, "hopper received TEST START");
				std::array<u32, 0x33> payload{};
				payload[0] = 0;				// atp num
				payload[1] = status;
				payload[0x12] = 2023 | (7 << 16) | (19 << 24);	// year month day
				payload[0x13] = (55 << 24) | (48 << 16) | (11 << 8) | 3; // day of week, hour, minute, second
				payload[0x14] = cfg.gameNumber;
				sendMessage(0x14, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x25:	// BACKUP CLEAR
			{
				INFO_LOG(NAOMI, "hopper received BACKUP CLEAR");
				cfg.credit0 = 0;
				cfg.credit1 = 0;
				cfg.premium = 0;
				cfg.paidAmount = 0;
				cfg.wonAmount = 0;
				std::array<u32, 0x44> payload{};
				payload[0] = cfg.credit0;
				payload[1] = cfg.credit1;
				payload[2] = cfg.totalCredit;
				payload[3] = cfg.premium;
				payload[0x23] = 2023 | (7 << 16) | (19 << 24);	// year month day
				payload[0x24] = (55 << 24) | (48 << 16) | (11 << 8) | 3; // day of week, hour, minute, second
				payload[0x25] = cfg.gameNumber;
				sendMessage(0x15, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x26:	// BET
			{
				INFO_LOG(NAOMI, "hopper received BET");
				std::array<u32, 2> payload{};
				payload[0] = rd32(4);
				sendMessage(0x16, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x29:	// SWITCH
				INFO_LOG(NAOMI, "hopper received SWITCH");
				// 4: test button
				// 5: service button
				// 6-7: c01 in test menu
				break;

			case 0x2a:	// CONFIG HOP
			{
				INFO_LOG(NAOMI, "hopper received CONFIG HOP");
				if (recv[2] == 0x28 && recv[3] == 0)
				{
					cfg.freePlay = recv[4] & 1;
					cfg.autoPayOut = recv[5] & 1;
					cfg.twoWayMode = recv[6] & 1;
					cfg.medalExchRate = recv[7];
					cfg.autoExchange = recv[8] & 1;
					cfg.coinDiscrimination = recv[9] & 1;
					cfg.curBase = recv[0xa];
					cfg.betButton = recv[0xb] & 1;
					cfg.maxPay = rd32(0xc);
					cfg.maxCredit = rd32(0x10);
					cfg.hopperSize = rd32(0x14);
					cfg.maxBet = rd32(0x18);
					cfg.minBet = rd32(0x1c);
					cfg.addBet = rd32(0x20);
				}
				std::array<u32, 9> payload{};
				payload[0] = cfg.freePlay | (cfg.autoPayOut << 8) | (cfg.twoWayMode << 16) | (cfg.medalExchRate << 24);
				payload[1] = cfg.autoExchange | (cfg.coinDiscrimination << 8) | (cfg.curBase << 16) | (cfg.betButton << 24);
				payload[2] = cfg.maxPay;
				payload[3] = cfg.maxCredit;
				payload[4] = cfg.hopperSize;
				payload[5] = cfg.maxBet;
				payload[6] = cfg.minBet;
				payload[7] = cfg.addBet;
				sendMessage(0x1a, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
				break;
			}

			case 0x2f: // TEST DATA MON
				INFO_LOG(NAOMI, "hopper received cmd TEST DATA MON");
				// no reply
				break;

			case 0x31: // COIN IN ACK?
			case 0x32: // COIN OUT ACK?
				// no reply
				break;

			default:
				WARN_LOG(NAOMI, "Unexpected hopper message: %x", command);
				break;
		}
	}

private:
	void sendCoinInMessage() override
	{
		insertCoin(100);
		std::array<u32, 7> payload{};
		payload[0] = 0;			// atp num
		payload[1] = status;
		payload[2] = cfg.credit0;
		payload[3] = cfg.credit1;
		payload[4] = cfg.totalCredit;
		payload[5] = cfg.premium;
		sendMessage(1, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
	}

	void sendPayWinMessage() override
	{
		std::array<u32, 9> payload{};
		payload[0] = 0;			// atp num
		payload[1] = status;
		payload[2] = cfg.credit0;
		payload[3] = cfg.credit1;
		payload[4] = cfg.totalCredit;
		payload[5] = cfg.premium;
		payload[6] = cfg.wonAmount;
		payload[7] = cfg.paidAmount;
		sendMessage(3, (const u8 *)payload.data(), payload.size() * sizeof(u32) - 1);
	}

	void setDefaults()
	{
		switch (cfg.currency)
		{
		case 0:	// Medal
			cfg.medalExchRate = 5;
			cfg.twoWayMode = 1;
			cfg.autoPayOut = 0;
			cfg.coinDiscrimination = 1;
			cfg.curBase = 100;
			cfg.betButton = 1;
			cfg.maxPay = 1999900;
			cfg.maxCredit = 1999900;
			cfg.hopperSize = 39900;
			cfg.maxBet = 10000;
			cfg.minBet = 1000;
			cfg.addBet = 100;
			break;
		case 1:	// Pound
			cfg.medalExchRate = 0;
			cfg.twoWayMode = 0;
			cfg.autoPayOut = 1;
			cfg.coinDiscrimination = 0;
			cfg.curBase = 5;
			cfg.betButton = 0;
			cfg.maxPay = 4000;
			cfg.maxCredit = 1999900;
			cfg.hopperSize = 1999900;
			cfg.maxBet = 100;
			cfg.minBet = 100;
			cfg.addBet = 50;
			break;
		case 2:	// Dollar
		case 3:	// Euro
			cfg.medalExchRate = 0;
			cfg.twoWayMode = 0;
			cfg.autoPayOut = 0;
			cfg.coinDiscrimination = 0;
			cfg.curBase = 5;
			cfg.betButton = 1;
			cfg.maxPay = 1999900;
			cfg.maxCredit = 1999900;
			cfg.hopperSize = 39900;
			cfg.maxBet = 10000;
			cfg.minBet = 100;
			cfg.addBet = 100;
			break;
		case 4:	// Token
			cfg.medalExchRate = 0;
			cfg.twoWayMode = 0;
			cfg.autoPayOut = 0;
			cfg.coinDiscrimination = 0;
			cfg.curBase = 100;
			cfg.betButton = 1;
			cfg.maxPay = 1999900;
			cfg.maxCredit = 1999900;
			cfg.hopperSize = 39900;
			cfg.maxBet = 10000;
			cfg.minBet = 100;
			cfg.addBet = 100;
			break;
		default:
			WARN_LOG(NAOMI, "Unsupported currency %d", cfg.currency);
			break;
		}
		cfg.totalCredit = cfg.minBet;
	}

	// error:16
	// atpType:4
	// doorOpen:1
	// unk1:1
	// unk2:1
	// unk3:1
	// unk4:1
	static constexpr u32 status = 0x00000000;
};

static BaseHopper *hopper;

void init()
{
	term();
	if (!strcmp(naomi_game_id, "KICK '4' CASH"))
		hopper = new Sega837_14438Hopper();
	else
		hopper = new NaomiHopper();
	scif_set_pipe(hopper);
}

void term()
{
	if (hopper == NULL)
		return;
	scif_set_pipe(NULL);
	delete hopper;
	hopper = NULL;
}

bool active()
{
	return hopper != NULL;
}

void poll()
{
	if (hopper != NULL)
		hopper->poll();
}

/* The hopper board's part of a save state (V24): what it keeps (the
 * credits and settings, which are in its file too), the message coming
 * in, the bytes it has still to send, whether the game has started it and
 * whether the coin switch was down. 2404 bytes. */
void serialize_v24(void **data, unsigned int *total_size)
{
	if (hopper == NULL)
		return;
	u32 words[23];
	memcpy(words, &hopper->cfg, sizeof(words));
	u8 flags[2] = { hopper->started, hopper->coinKey };
	u16 recvLen = (u16)hopper->recvLen;
	u16 expected = (u16)hopper->expectedBytes;
	LIBRETRO_SA(words, 23);
	LIBRETRO_SA(hopper->recv, sizeof(hopper->recv));
	LIBRETRO_S(recvLen);
	LIBRETRO_S(expected);
	hopper->toSend.serialize(data, total_size);
	LIBRETRO_SA(flags, 2);
}

bool unserialize_v24(void **data, unsigned int *total_size)
{
	if (hopper == NULL)
		return true;
	u32 words[23];
	u8 flags[2] = { 0, 0 };
	u16 recvLen = 0, expected = 0;
	memcpy(words, &hopper->cfg, sizeof(words));
	LIBRETRO_USA(words, 23);
	LIBRETRO_USA(hopper->recv, sizeof(hopper->recv));
	LIBRETRO_US(recvLen);
	LIBRETRO_US(expected);
	hopper->toSend.unserialize(data, total_size);
	if (!LIBRETRO_USA(flags, 2))
		return false;
	memcpy(&hopper->cfg, words, sizeof(words));
	hopper->cfg.normalize();
	hopper->recvLen = recvLen < sizeof(hopper->recv) ? recvLen : 0;
	hopper->expectedBytes = expected <= sizeof(hopper->recv) ? expected : 0;
	if (hopper->recvLen >= 4 && (hopper->expectedBytes < 5 || hopper->recvLen >= hopper->expectedBytes))
	{
		hopper->recvLen = 0;
		hopper->expectedBytes = 0;
	}
	hopper->started = flags[0] != 0;
	hopper->coinKey = flags[1] != 0;
	return true;
}

/* An older state: nothing on its way, and not started - the game will
 * start it again. What it keeps is what its file has. */
void state_before_v24()
{
	if (hopper != NULL)
		hopper->clearState();
}

}	// namespace hopper
