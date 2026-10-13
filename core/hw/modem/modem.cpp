/*
	Originally based on nullDC: nullExtDev/modem.cpp

	Created on: Sep 9, 2018

	Copyright 2018 skmp, flyinghead

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

#include "modem.h"
#include "modem_regs.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include <libretro.h>
#include <features/features_cpu.h>
#include <pico_config.h>
#include "network/picoppp.h"
#include "reios/reios.h"
#include "v42.h"
#include <algorithm>

#define MODEM_COUNTRY_RES 0
#define MODEM_COUNTRY_JAP 1
#define MODEM_COUNTRY_USA 2

#define MODEM_MAKER_SEGA 0
#define MODEM_MAKER_ROCKWELL 1

#define MODEM_DEVICE_TYPE_336K 0

#define LOG(...) DEBUG_LOG(MODEM, __VA_ARGS__)

const static u32 MODEM_ID[2] =
{
	MODEM_COUNTRY_USA,
	(MODEM_MAKER_ROCKWELL<<4) | (MODEM_DEVICE_TYPE_336K),
};

static modemreg_t modem_regs;

static u8 dspram[0x1000];

// (-1 on a machine that has no modem, and has never registered the event)
int modem_sched = -1;

enum ModemStates
{
	MS_INVALID,				//needs reset
	MS_RESET,				//reset is low
	MS_RESETING,			//reset is hi
	MS_ST_CONTROLER,		//Controller self test
	MS_ST_DSP,				//DSP self test
	MS_END_DSP,				//DSP self test end
	MS_NORMAL,	 			//Normal operation

};
static ModemStates state = MS_INVALID;

enum ConnectState
{
	DISCONNECTED,
	DIALING,
	RINGING,
	HANDSHAKING,
	PRE_CONNECTED,
	CONNECTED,
	NEGO_COMPLETE,			// the V.8 bis exchange is over: the game dials the line itself next
};
static ConnectState connect_state = DISCONNECTED;
// The transmitter's FIFO: how many of the bytes the game wrote have not gone yet
static float txFifoSize = 0.f;
static modem::ByteFifo<16> rxFifo;
// A byte at the speed of the connection, and the speed sent at over the one received at
#define DEFAULT_CYCLES_PER_BYTE (SH4_MAIN_CLOCK / 1000000 * 238)
static int cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
static float txRxRatio = 1.f;

static void schedule_callback(int ms);

static void update_interrupt()
{
	modem_regs.reg1e.RDBIA = modem_regs.reg1e.RDBIE && modem_regs.reg1e.RDBF;
	modem_regs.reg1e.TDBIA = modem_regs.reg1e.TDBIE && modem_regs.reg1e.TDBE;

	if (modem_regs.reg1f.NCIA || modem_regs.reg1f.NSIA || modem_regs.reg1e.RDBIA || modem_regs.reg1e.TDBIA)
		asic_RaiseInterrupt(holly_EXP_8BIT);
	else
		asic_CancelInterrupt(holly_EXP_8BIT);
}

static u32 get_masked_status(u32 reg)
{
	u8 int_mask = dspram[regs_int_mask_addr[reg]];
	return int_mask & modem_regs.ptr[reg];
}

#define SET_STATUS_BIT(reg, bit, value) do {		\
	if ((bit) != (value))							\
	{												\
		if (!modem_regs.reg1f.NSIE)					\
		{											\
			bit = (value);							\
		}											\
		else										\
		{											\
			u8 before = get_masked_status(reg);		\
			bit = (value);							\
			if (before != get_masked_status(reg))	\
				modem_regs.reg1f.NSIA = 1;			\
		}											\
	}												\
} while (false);

static void ControllerTestEnd();
static void DSPTestStart();
static void DSPTestEnd();

static u64 last_dial_time;
static u64 connected_time;

/* The telephone line.
 *
 * A save state holds the modem - its registers, the DSP's memory, how far a
 * call has got - but not the line: the PPP stack at the far end and the
 * host's sockets behind it are this process's, and cannot be put back. So a
 * modem that is connected says in the state which line it is connected over,
 * by a number no other line has, and the modem of a state that is loaded is
 * only on the line again if that line is the one this process has up:
 *
 * - it is (run-ahead and rewind load states of a moment ago): the line is
 *   left alone. What the game took from it since the state was made is
 *   given to it again from rx_log, in the same order, and what it sent since
 *   then, and now sends again, is not sent twice;
 * - it is not (a state of another session or of an earlier call, or one from
 *   too long ago for rx_log): a line that is up now is hung up, since the
 *   game of the state knows nothing of it, and the state's own line is dead.
 *   Nothing comes over a dead line, and after as long as a modem waits for a
 *   carrier that has gone, the modem reports the far end's hanging up: the
 *   game can dial again.
 *
 * What comes over the line is taken at each of the modem's steps into
 * line_fifo, which is in the state, and all the modem does with it is done
 * from there. Each byte's place in rx_log has the time it was taken at: a
 * state loaded on the live line takes them again at the same times, and
 * nothing from the line itself at a time the line was already looked at,
 * so that what the game is given is the same, byte for byte and step for
 * step. */
#define LINE_WINDOW 4096
// A modem gives the line up after this long without a carrier
#define DEAD_LINE_CYCLES ((u64)SH4_MAIN_CLOCK * 14 / 10)
static u32 line_id;			// the line the modem is connected over, 0 when it is not connected (in the state)
static u32 rx_pos;			// bytes the modem has taken from that line (in the state)
static u32 tx_pos;			// bytes the modem has given to it (in the state)
static u16 dead_steps;		// steps a connected modem has gone without its line (in the state)
static u32 live_line;		// the line this process has up, 0 for none
static u32 rx_taken;		// bytes taken from the live line
static u32 tx_sent;			// bytes given to the live line
static u8 rx_log[LINE_WINDOW];	// the last bytes taken from the live line
static u64 rx_log_time[LINE_WINDOW];	// and when
static u64 line_looked;		// the last time the live line itself was read
static u64 live_line_made;	// when the live line was made (emulated time)
static modem::ByteFifo<256> line_fifo;	// taken from the line, not yet used (in the state)

static bool line_is_live()
{
	return live_line != 0 && line_id == live_line;
}

// The line is hung up, if one is up
static void line_close()
{
	stop_pico();
	live_line = 0;
}

// What has come over the line, into line_fifo
static void line_fetch()
{
	const u64 now = sh4_sched_now64();

	while (line_fifo.room() > 0)
	{
		int c;

		if (rx_pos != rx_taken)
		{
			// Again: at the time it was taken the first time
			if (rx_log_time[rx_pos % LINE_WINDOW] > now)
				break;
			c = rx_log[rx_pos % LINE_WINDOW];
		}
		else
		{
			// The line had nothing more at this time when it was read
			if (now <= line_looked)
				break;
			c = read_pico();
			if (c < 0)
				break;
			rx_log[rx_taken % LINE_WINDOW] = (u8)c;
			rx_log_time[rx_taken % LINE_WINDOW] = now;
			rx_taken++;
		}
		rx_pos++;
		line_fifo.push((u8)c);
	}
	if (rx_pos == rx_taken && now > line_looked)
		line_looked = now;
}

static void line_write(u8 b)
{
	if (tx_pos == tx_sent)
	{
		write_pico(b);
		tx_sent++;
	}
	tx_pos++;
}

static void line_lost();

using namespace modem;

/* The modem's end of the line: what has come over it, and what is sent. In
 * synchronous mode the V.42 protocol is between these and the game. */
class NetIn : public InStream
{
public:
	int read() override {
		return line_fifo.pop();
	}
	int available() override {
		return line_fifo.size();
	}
};

class NetOut : public OutStream
{
public:
	void write(u8 v) override {
		// (what is sent over a dead line is lost)
		if (line_is_live())
			line_write(v);
	}
};

static NetIn netIn;
static NetOut netOut;
static InStream *curInput = &netIn;
static OutStream *curOutput = &netOut;

static V42Protocol v42Proto(netIn, netOut);

static bool v8bis;
static V8bisProtocol v8bisProto;

static bool game_is(const char *id)
{
	std::string gameId(ip_meta.product_number, sizeof(ip_meta.product_number));
	return trim_trailing_ws(gameId) == id;
}

static void updateRxFifoStatus()
{
	// The rx fifo timeout set in dspram[0x32c]:2-4 doesn't seem to be used
	if (rxFifo.size() != 0)
	{
		modem_regs.reg00 = rxFifo.front();
		modem_regs.reg1e.RDBF = (u8)((unsigned)rxFifo.size() >= rxFifoTrigger[(dspram[0x32c] >> 6) & 3]);
		if (modem_regs.reg04.FIFOEN)
			SET_STATUS_BIT(0x0c, modem_regs.reg0c.RXFNE, 1);
		SET_STATUS_BIT(0x01, modem_regs.reg01.RXHF, rxFifo.size() >= 8);
	}
	else
	{
		modem_regs.reg1e.RDBF = 0;
		if (modem_regs.reg04.FIFOEN)
			SET_STATUS_BIT(0x0c, modem_regs.reg0c.RXFNE, 0);
		SET_STATUS_BIT(0x01, modem_regs.reg01.RXHF, 0);
	}
}

static void configureStreams()
{
	if (v8bis) {
		curInput = &v8bisProto;
		curOutput = &v8bisProto;
	}
	else if (modem_regs.reg08.ASYN == 0) {
		// Synchronous: V.42 or V.14
		curInput = &v42Proto;
		curOutput = &v42Proto;
	}
	else {
		curInput = &netIn;
		curOutput = &netOut;
	}
}

/* The speed of the connection, from the configuration the game has asked for
 * (CONF). False if it is not one a connection can be made with. */
static bool setModemSpeedFromCONF()
{
	static const int v90Speeds[] = {
		0, 28800, 29333, 30667, 32000, 33333, 34667, 36000, 37333, 38667, 40000, 41333,
		42667, 44000, 45333, 46667, 48000, 49333, 50667, 52000, 53333, 54667, 56000
	};
	static const int v34Speeds[] = {
		0, 2400, 4800, 7200, 9600, 12000, 14400, 16800, 19200, 21600, 24000, 26400, 28800, 31200, 33600
	};
	const u8 conf = modem_regs.reg12;
	modem_regs.reg0e.SPEED = 0;
	int speed;
	int rxSpeed = 0;
	switch (conf)
	{
	default:
		if (conf >= 0xe0 && conf <= 0xf6)
		{
			// V.90
			rxSpeed = v90Speeds[conf - 0xe0];
			if (rxSpeed == 0) {
				speed = 0;
			}
			else
			{
				dspram[0x2e4] = conf - 0xe0;
				speed = 33600;
				modem_regs.reg0e.SPEED = 0x10;
				dspram[0x2e5] = 0xd;
			}
		}
		else if (conf >= 0x90 && conf <= 0x9d)
		{
			// K56flex
			if (conf == 0x90) {
				speed = 0;
			}
			else
			{
				rxSpeed = 30000 + (conf - 0x90) * 2000;
				speed = 33600;
				modem_regs.reg0e.SPEED = 0x10;
			}
		}
		else if (conf >= 0xc0 && conf <= 0xce)
		{
			// V.34
			speed = v34Speeds[conf - 0xc0];
			if (speed != 0)
				modem_regs.reg0e.SPEED = conf - 0xc0 + 2;
			dspram[0x2e4] = conf - 0xc0;
			dspram[0x2e5] = dspram[0x2e4];
		}
		else {
			speed = 0;
		}
		break;

	// V.33
	case 0x31: speed = 14400; break;
	case 0x32: speed = 12000; break;
	case 0x34: speed = 9600; break;
	case 0x38: speed = 7200; break;
	// V.32 [bis]
	case 0x76: speed = 14400; break;
	case 0x72: speed = 12000; break;
	case 0x74: speed = 9600; break;
	case 0x75: speed = 9600; break;
	case 0x78: speed = 7200; break;
	case 0x71: speed = 4800; break;
	case 0x70: speed = 0; break;
	// V.17
	case 0xb1: speed = 14400; break;
	case 0xb2: speed = 12000; break;
	case 0xb4: speed = 9600; break;
	case 0xb8: speed = 7200; break;
	// V.29
	case 0x14: speed = 9600; break;
	case 0x12: speed = 7200; break;
	case 0x11: speed = 4800; break;
	// V.27 ter, V.22 bis, V.22, V.21
	case 0x02: speed = 4800; break;
	case 0x01: speed = 2400; break;
	case 0x84: speed = 2400; break;
	case 0x82: speed = 1200; break;
	case 0x52: speed = 1200; break;
	case 0x51: speed = 600; break;
	case 0xa0: speed = 300; break;
	case 0xa8: speed = 300; break;
	// Bell, V.23
	case 0x23: speed = 4800; break;
	case 0x62: speed = 1200; break;
	case 0x60: speed = 300; break;
	case 0xa4: speed = 1200; rxSpeed = 75; break;
	case 0xa1: speed = 75; rxSpeed = 1200; break;
	}

	if (rxSpeed == 0)
		rxSpeed = speed;
	if (speed == 0)
	{
		modem_regs.reg0e.SPEED = 0;
		cyclesPerByte = SH4_MAIN_CLOCK * 8 / 300;
		txRxRatio = 1.f;
	}
	else
	{
		if (modem_regs.reg0e.SPEED == 0)
		{
			switch (speed)
			{
			case 14400: modem_regs.reg0e.SPEED = 7; break;
			case 12000: modem_regs.reg0e.SPEED = 6; break;
			case 9600: modem_regs.reg0e.SPEED = 5; break;
			case 7200: modem_regs.reg0e.SPEED = 8; break;
			case 4800: modem_regs.reg0e.SPEED = 4; break;
			case 2400: modem_regs.reg0e.SPEED = 3; break;
			case 1200: modem_regs.reg0e.SPEED = 2; break;
			case 600: modem_regs.reg0e.SPEED = 1; break;
			default: modem_regs.reg0e.SPEED = 0; break;
			}
		}
		/* The modem steps at the speed it receives at, a byte to the game at
		 * each step, and sends at its own speed: speed / rxSpeed bytes a
		 * step. (Stepping at the sending speed, with that ratio, had a V.90
		 * line receive at 33600 and send at 20160 bits a second.) */
		cyclesPerByte = SH4_MAIN_CLOCK * 8 / rxSpeed;
		txRxRatio = (float)speed / rxSpeed;
	}
	NOTICE_LOG(MODEM, "MODEM Connected %s %d/%d bps", modem_regs.reg08.ASYN ? "ASYNC" : "SYNC", rxSpeed, speed);
	return speed != 0;
}

#ifndef NDEBUG
static unsigned long last_comm_stats;
static int sent_bytes;
static int recvd_bytes;
static FILE *recv_fp;
static FILE *sent_fp;
#endif

static int modem_sched_func(int tag, int cycles, int jitter)
{
#ifndef NDEBUG
	if (PICO_TIME() - last_comm_stats >= 2)
	{
		if (last_comm_stats != 0)
		{
			DEBUG_LOG(MODEM, "Stats sent %d (%.2f kB/s) received %d (%.2f kB/s) TDBE %d RDBF %d\n", sent_bytes, sent_bytes / 2000.0,
					recvd_bytes, recvd_bytes / 2000.0,
					modem_regs.reg1e.TDBE, modem_regs.reg1e.RDBF);
			sent_bytes = 0;
			recvd_bytes = 0;
		}
		last_comm_stats = PICO_TIME();
	}
#endif
	int callback_cycles = 0;

	switch (state)
	{
	case MS_ST_CONTROLER:
		ControllerTestEnd();
		break;
	case MS_ST_DSP:
		DSPTestStart();
		break;
	case MS_END_DSP:
		DSPTestEnd();
		break;
	case MS_NORMAL:
		modem_regs.reg1f.NEWC = 0;		// Not needed when state is CONNECTED but who cares

		switch (connect_state)
		{
		case DIALING:
			if (last_dial_time != 0 && sh4_sched_now64() - last_dial_time >= (u64)(SH4_MAIN_CLOCK + jitter))
			{
				LOG("Switching to RINGING state");
				connect_state = RINGING;
				schedule_callback(100);
			}
			else
			{
				last_dial_time = sh4_sched_now64();

				modem_regs.reg1e.TDBE = 1;
				schedule_callback(1000);	// To switch to Ringing state
			}
			break;
		case RINGING:
		case NEGO_COMPLETE:
			last_dial_time = 0;
			LOG("\t\t *** %s STATE ***", connect_state == RINGING ? "RINGING" : "NEGO COMPLETE");
			modem_regs.reg1f.NEWS = 1;
			if (!modem_regs.reg09.DATA && connect_state == RINGING)
			{
				SET_STATUS_BIT(0x0f, modem_regs.reg0f.RI, 1);
				SET_STATUS_BIT(0x0b, modem_regs.reg0b.ATV25, 1);
			}
			break;
		case HANDSHAKING:
			LOG("\t\t *** HANDSHAKING STATE ***");
			if (modem_regs.reg12 == 0xAA)
			{
				// V8 AUTO mode
				dspram[0x302] |= 1 << 3;				// ANSam detected
			}
			modem_regs.reg1f.NEWS = 1;
			SET_STATUS_BIT(0x0f, modem_regs.reg0f.RI, 0);
			SET_STATUS_BIT(0x0b, modem_regs.reg0b.ATV25, 0);

			callback_cycles = SH4_MAIN_CLOCK / 1000 * 500;		// 500 ms
			connect_state = PRE_CONNECTED;

			break;

		case PRE_CONNECTED:
			{
				if (modem_regs.reg03.RLSDE)
					SET_STATUS_BIT(0x0f, modem_regs.reg0f.RLSD, 1);
				bool validConfig;
				if (modem_regs.reg12 == 0xAA)
				{
					// V8 AUTO mode
					dspram[0x302] |= 1 << 4;				// protocol octet received
					dspram[0x302] = (dspram[0x302] & 0x1f) | (dspram[0x304] & 0xe0);	// Received Call Function
					dspram[0x301] |= 1 << 4;				// JM detected
					dspram[0x303] |= 0xE1;					// Received protocol bits (?), Received GSTN Octet
					dspram[0x2e3] = 5;						// Symbol rate 3429
					// set the negotiated speed to the max configured by the game
					if ((dspram[0x6a3] & 1) == 1 && (dspram[0x6a2] & 0x20) == 0x20)
					{
						if (game_is("HDR-0113") || game_is("HDR-0091"))
						{
							// Set symmetric speed (V.34 33.6) for Power Smash and Pro Yakyuu Team to avoid lag
							modem_regs.reg12 = 0xCE;
						}
						else
						{
							// V.90
							modem_regs.reg12 = 0xE0 | (dspram[0x6a2] & 0x1F);
							dspram[0x6c1] = 0xff;		// V90 Server DPCM Transmit Data Rate Mask
							dspram[0x6c2] = 0xff;
							dspram[0x6c3] = 0x3f;
						}
					}
					else {
						modem_regs.reg12 = dspram[0x309];
					}
					validConfig = setModemSpeedFromCONF();
					dspram[0x239] = 12;						// RTD 0 @ 3429 sym rate
					if (modem_regs.reg1f.NSIE)
					{
						// CONF
						if (dspram[regs_int_mask_addr[0x12]] & (1 << 7))
							modem_regs.reg1f.NSIA = 1;
					}
				}
				else {
					validConfig = setModemSpeedFromCONF();
				}
				if (modem_regs.reg1f.NSIE)
				{
					// SPEED
					if (dspram[regs_int_mask_addr[0x0e]] & 0x1f)
						modem_regs.reg1f.NSIA = 1;
				}
				if (modem_regs.reg15.AUTO == 1)
				{
					modem_regs.reg09.DATA = 1;
					modem_regs.reg15.AUTO = 0;
				}
				if (!validConfig)
				{
					modem_regs.reg14 = 1;	// ABCODE: (fake) FED turned off while waiting to get into round trip delay estimate (RTDE)
					connect_state = DISCONNECTED;
				}
				else
				{
					INFO_LOG(MODEM, "MODEM Connected");
					modem_regs.reg14 = 0x00;			// ABCODE: no error
					SET_STATUS_BIT(0x0f, modem_regs.reg0f.DSR, 1);
					if (modem_regs.reg02.v0.RTSDE)
						SET_STATUS_BIT(0x0f, modem_regs.reg0f.RTSDT, 1);

					// Energy detected. Required for games to detect the connection
					SET_STATUS_BIT(0x0f, modem_regs.reg0f.FED, 1);

					if (live_line != 0 && line_id == 0 && live_line_made == sh4_sched_now64()
							&& rx_taken <= LINE_WINDOW)
					{
						/* The line was made at this very moment, by the
						 * machine of a state loaded since (run-ahead): it
						 * is this call's, and what came over it is given
						 * again as it came */
						line_id = live_line;
						rx_pos = 0;
						tx_pos = 0;
					}
					else
					{
						line_close();
						start_pico();
						// The time is the line's number: no other line, of this
						// process or of another, was made at this microsecond
						live_line = (u32)cpu_features_get_time_usec();
						if (live_line == 0)
							live_line = 1;
						live_line_made = sh4_sched_now64();
						line_id = live_line;
						rx_pos = rx_taken = 0;
						tx_pos = tx_sent = 0;
						line_looked = 0;
					}
					line_fifo.clear();
					dead_steps = 0;
					connect_state = CONNECTED;
					connected_time = 0;
				}
				if (modem_regs.reg1f.NSIE)
				{
					// ABCODE
					if (dspram[regs_int_mask_addr[0x014]])
						modem_regs.reg1f.NSIA = 1;
				}
				modem_regs.reg1f.NEWS = 1;

				// V.34 Remote Modem Data Rate Capability
				dspram[0x208] = 0xff;	// 2.4 - 19.2 kpbs supported
				dspram[0x209] = 0xbf;	// 21.6 - 33.6 kpbs supported, asymmetric supported

				callback_cycles = SH4_MAIN_CLOCK / 1000000 * 238;	// 238 us
				v42Proto.reset();
			}
			break;

		case CONNECTED:
#ifndef NDEBUG
			static bool mem_dumped;
			if (!mem_dumped)
			{
				mem_dumped = true;
				for (int i = 0 ; i < sizeof(modem_regs); i++)
					LOG("modem_regs %02x == %02x", i, modem_regs.ptr[i]);
			}
#endif
			callback_cycles = cyclesPerByte;
			if (!line_is_live())
			{
				// The line of a state that was loaded, which is not up
				if (++dead_steps >= DEAD_LINE_CYCLES / (u64)cyclesPerByte)
				{
					line_lost();
					break;
				}
			}
			else
				line_fetch();
			txFifoSize = std::max(txFifoSize - txRxRatio, 0.f);
			modem_regs.reg1e.TDBE = txFifoSize == 0.f;

			if (v8bis && v8bisProto.completed())
			{
				connect_state = NEGO_COMPLETE;
				v8bis = false;
				configureStreams();
			}
			if (rxFifo.size() < 16)
			{
				int c = curInput->read();
				if (c >= 0)
				{
#ifndef NDEBUG
					recvd_bytes++;
#endif
					rxFifo.push((u8)c);
					updateRxFifoStatus();
				}
			}
			break;

		default:
			break;
		}
		break;

	default:
		break;
	}
	update_interrupt();

	return callback_cycles;
}

void ModemInit()
{
	modem_sched = sh4_sched_register(0, &modem_sched_func);
}

void ModemTerm()
{
	line_close();
	line_id = 0;
	dead_steps = 0;
	/* A reset of the console calls this as well. The step of the chip's
	 * self test or of a call that was due is not to come: it used to, after
	 * the reset and with the registers the game before had left, and when
	 * that game was connected it came 4200 times a second from then on.
	 * And a chip that has been used is as it is when the console is
	 * switched on: waiting for the game to reset it, nothing dialled, no
	 * interrupt asked for. */
	if (sh4_sched_is_scheduled(modem_sched))
		sh4_sched_request(modem_sched, -1);
	line_fifo.clear();
	if (state != MS_INVALID)
	{
		memset(&modem_regs, 0, sizeof(modem_regs));
		state = MS_INVALID;
		connect_state = DISCONNECTED;
		last_dial_time = 0;
		connected_time = 0;
		txFifoSize = 0.f;
		rxFifo.clear();
		cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
		txRxRatio = 1.f;
		v8bis = false;
		v8bisProto.reset();
		v42Proto.reset();
		configureStreams();
		update_interrupt();
	}
}

static void schedule_callback(int ms)
{
   sh4_sched_request(modem_sched, SH4_MAIN_CLOCK / 1000 * ms);
}

/* The far end has hung up: the carrier and what comes with it are gone, the
 * abort code says that a cleardown was asked for, and the modem is ready to
 * dial again. */
static void line_lost()
{
	connect_state = DISCONNECTED;
	line_id = 0;
	dead_steps = 0;

	modem_regs.reg14 = 0x96;			// ABCODE: cleardown
	if (modem_regs.reg1f.NSIE)
	{
		// ABCODE
		if (dspram[regs_int_mask_addr[0x014]])
			modem_regs.reg1f.NSIA = 1;
	}
	modem_regs.reg1f.NEWS = 1;
	SET_STATUS_BIT(0x0f, modem_regs.reg0f.RLSD, 0);
	SET_STATUS_BIT(0x0f, modem_regs.reg0f.FED, 0);
	SET_STATUS_BIT(0x0f, modem_regs.reg0f.DSR, 0);
	SET_STATUS_BIT(0x0f, modem_regs.reg0f.RTSDT, 0);
	modem_regs.reg1e.TDBE = 1;
}

#define SetReg16(rh,rl,v) {modem_regs.ptr[rh]=(v)>>8;modem_regs.ptr[rl]=(v)&0xFF; }

static void NormalDefaultRegs()
{
	verify(state == MS_NORMAL);
	verify(sizeof(regs_write_mask) == sizeof(modem_regs));
	verify(sizeof(por_dspram) == sizeof(dspram));

	// Default values for normal state
	memset(&modem_regs, 0, sizeof(modem_regs));
	memcpy(dspram, por_dspram, sizeof(dspram));
	modem_regs.reg05.CEQ = 1;
	modem_regs.reg12 = 0x76;	// CONF: V.32 bis TCM 14400
	modem_regs.reg09.DATA = 1;
	modem_regs.reg09.DTMF = 1;
	modem_regs.reg15.HWRWK = 1;
	modem_regs.reg07.RDLE = 1;
	modem_regs.reg15.RDWK = 1;
	modem_regs.reg03.RLSDE = 1;
	modem_regs.reg02.TDE = 1;
	modem_regs.reg13.TLVL = 0x9;

	modem_regs.reg1e.TDBE = 1;
	connect_state = DISCONNECTED;
	last_dial_time = 0;
	txFifoSize = 0.f;
	rxFifo.clear();
	v8bis = false;
	configureStreams();
}
static void DSPTestEnd()
{
	verify(state==MS_END_DSP);
	state=MS_NORMAL;

	LOG("DSPTestEnd");
	NormalDefaultRegs();
}
static void DSPTestStart()
{
	verify(state==MS_ST_DSP);
	state=MS_END_DSP;
	LOG("DSPTestStart");

	modem_regs.reg1e.TDBE = 1;
	SetReg16(0x1B,0x1A,0xF083);	// EC checksum
	SetReg16(0x19,0x18,0x46EE); // Multiplier checksum
	SetReg16(0x17,0x16,0x00FA); // RAM checksum
	SetReg16(0x15,0x14,0x0A09); // ROM checksum
	SetReg16(0x13,0x12,0x3730); // Part number
	SetReg16(0x11,0x00,0x2041); // Revision level

	schedule_callback(50);
}
static void ControllerTestEnd()
{
	verify(state==MS_ST_CONTROLER);
	state=MS_ST_DSP;

	schedule_callback(50);
}

//End the reset and start internal tests
static void ControllerTestStart()
{
	verify(state==MS_RESETING);
	//Set Self test values :)
	state=MS_ST_CONTROLER;
	//k, lets set values

	//1E:3 -> set
	modem_regs.reg1e.TDBE=1;

/*
	RAM1 Checksum  = 0xEA3C or 0x451
	RAM2 Checksum  = 0x5536 or 0x49A5
	ROM1 Checksum  = 0x5F4C
	ROM2 Checksum  = 0x3835 or 0x3836
	Timer/ROM/RAM  = 0x801 or 0xDE00
	Part Number    = 0x3730 or 0x3731
	Revision Level = 0x4241
*/
	SetReg16(0x1D,0x1C,0xEA3C);
	SetReg16(0x1B,0x1A,0x5536);
	SetReg16(0x19,0x18,0x5F4C);
	SetReg16(0x17,0x16,0x3835);
	SetReg16(0x15,0x14,0x801);
	SetReg16(0x13,0x12,0x3730);
	SetReg16(0x11,0x0,0x4241);

	ControllerTestEnd();
}

static void modem_reset(u32 v)
{
	if (v == 0)
	{
		memset(&modem_regs, 0, sizeof(modem_regs));
		state = MS_RESET;
		LOG("Modem reset start ...");
		// The line drops as soon as reset is asserted: some games never
		// release it again
		line_close();
		line_id = 0;
		dead_steps = 0;
		line_fifo.clear();
	}
	else
	{
		if (state == MS_RESET)
		{
			memset(&modem_regs, 0, sizeof(modem_regs));
			state = MS_RESETING;
			ControllerTestStart();
			INFO_LOG(MODEM, "MODEM Reset");
		}
		modem_regs.ptr[0x20] = v;
	}
}

static void check_start_handshake()
{
	if (modem_regs.reg09.DTR
			&& (modem_regs.reg09.DATA || modem_regs.reg15.AUTO)
			&& (connect_state == RINGING || connect_state == NEGO_COMPLETE))
	{
		LOG("DTR asserted. starting handshaking");
		connect_state = HANDSHAKING;
		schedule_callback(1);
	}
}

static bool word_dspram_write;
static bool module_download;
static u32 reg1b_save;
static u8 download_crc;

static void ModemNormalWrite(u32 reg, u32 data)
{
#ifndef NDEBUG
	if (recv_fp == NULL)
	{
		recv_fp = fopen("ppp_recv.dump", "w");
		sent_fp = fopen("ppp_sent.dump", "w");
	}
#endif
	//if (!module_download && reg != 0x10)
	//	LOG("ModemNormalWrite : %03X=%X", reg,data);
	u32 old = modem_regs.ptr[reg];
	modem_regs.ptr[reg] = (old & ~regs_write_mask[reg]) | (data & regs_write_mask[reg]);

	switch(reg)
	{
	case 0x02:
		modem_regs.reg0f.RTSDT = modem_regs.reg02.v0.RTSDE && connect_state == CONNECTED;
		break;

	case 0x06:
		LOG("PEN = %d", modem_regs.reg06.PEN);
		if (modem_regs.reg06.PEN)
			WARN_LOG(MODEM, "Parity not supported");
		if (modem_regs.reg06.HDLC)
			WARN_LOG(MODEM, "HDLC mode not supported");
		break;

	case 0x08:
		LOG("TPDM = %d ASYN = %d", modem_regs.reg08.TPDM, modem_regs.reg08.ASYN);
		if ((old ^ modem_regs.ptr[reg]) & 0x80)
			// ASYN changed
			configureStreams();
		break;

	case 0x09:
		check_start_handshake();	// DTR and DATA
		if (connect_state == CONNECTED)
		{
			if (modem_regs.reg09.DTR) {
				LOG("DTR asserted");
				SET_STATUS_BIT(0x0f, modem_regs.reg0f.DSR, 1);
			}
			else
			{
				LOG("DTR reset");
				SET_STATUS_BIT(0x0f, modem_regs.reg0f.DSR, 0);
				SET_STATUS_BIT(0x0f, modem_regs.reg0f.RLSD, 0);
			}
		}
		break;

	case 0x10:	// TBUFFER
		if (v8bis && modem_regs.reg08.RTS == 0) {
			// emit V8 bis tone
			v8bisProto.emitTone((u8)data);
		}
		else if (module_download)
		{
			download_crc = (download_crc << 1) + ((download_crc & 0x80) >> 7) + (data & 0xFF);
		}
		else if (connect_state == DISCONNECTED || connect_state == DIALING)
		{
			//LOG("ModemNormalWrite : TBUFFER = %X", data);
			if (connect_state == DISCONNECTED)
			{
				INFO_LOG(MODEM, "MODEM Dialing");
				connect_state = DIALING;
				modem_regs.reg16 = 0;		// SECRXB
			}
			schedule_callback(100);
		}
		else if (connect_state == CONNECTED && modem_regs.reg08.RTS)
		{
			//LOG("ModemNormalWrite : TBUFFER = %X", data);
#ifndef NDEBUG
			sent_bytes++;
			if (sent_fp)
				fputc(data, sent_fp);
#endif
			curOutput->write((u8)data);

			modem_regs.reg1e.TDBE = 0;
			txFifoSize += 1.f;
			if (modem_regs.reg04.FIFOEN) {
				SET_STATUS_BIT(0x0d, modem_regs.reg0d.TXFNF, (u8)(txFifoSize <= 15.f));
				SET_STATUS_BIT(0x01, modem_regs.reg01.TXHF, (u8)(txFifoSize >= 8.f));
			}
			else {
				SET_STATUS_BIT(0x0d, modem_regs.reg0d.TXFNF, 0);
				SET_STATUS_BIT(0x01, modem_regs.reg01.TXHF, 1);
			}
		}
		break;

	case 0x11:
		LOG("PARSL = %d", modem_regs.reg11.PARSL);
		break;

	case 0x14:	// ABCODE
		if (data == 0x4f || data == 0x5f)
		{
			reg1b_save = modem_regs.ptr[0x1b];
			modem_regs.ptr[0x1b] = data;
			//LOG("SPX/CTL download initiated");
			module_download = true;
			download_crc = 0;
		}
		else if (data == 0 && module_download)
		{
			// If module download is in progress, this signals the end of it
			modem_regs.reg17 = 0xFF;
			modem_regs.reg16 = download_crc;
			// Restore reg 1b
			modem_regs.ptr[0x1b] = reg1b_save;
			module_download = false;
			//LOG("SPX/CTL download finished CRC %02x", download_crc);
		}
		break;

	case 0x15:
		check_start_handshake();	// AUTO
		break;

		//Data Write Regs, for transfers to DSP
	case 0x18:	// MEDAL
		break;

	case 0x19:	// MEDAM
		word_dspram_write = true;
		break;

	case 0x1a:
		if (connect_state == CONNECTED && modem_regs.reg1a.SCIBE)
			WARN_LOG(MODEM, "Unexpected state: connected and SCIBE==1");
		break;

		//Address low
	case 0x1C:	// MEADDL
		break;

	case 0x1D:	// MEADDH
		if (modem_regs.reg1c_1d.MEMW && !(old & (1 << 5)))
		{
			word_dspram_write = false;
		}
		if (modem_regs.reg1c_1d.MEACC)
		{
			modem_regs.reg1c_1d.MEACC = 0;
			modem_regs.reg1f.NEWS = 1;
			u32 dspram_addr = modem_regs.reg1c_1d.MEMADD_l | (modem_regs.reg1c_1d.MEMADD_h << 8);
			if (modem_regs.reg1c_1d.MEMW)
			{
				LOG("DSP mem Write%s address %08x = %x", word_dspram_write ? " (w)" : "", dspram_addr, modem_regs.reg18_19);
				if (word_dspram_write)
				{
					if (dspram_addr & 1)
					{
						// The address has 12 bits: the second byte of a word
						// at the last one is the first byte of the memory
						dspram[dspram_addr] = modem_regs.reg18_19 & 0xFF;
						dspram[(dspram_addr + 1) & (sizeof(dspram) - 1)] = (modem_regs.reg18_19 >> 8) & 0xFF;
					}
					else
						*(u16*)&dspram[dspram_addr] = modem_regs.reg18_19;
				}
				else
					dspram[dspram_addr] = modem_regs.reg18_19 & 0xFF;
				if ((dspram_addr == 0x26B || dspram_addr == 0x26c) && (modem_regs.reg18_19 & 0xFF) != 0)
					dspram[0x26F] = 1; // Saved Filtered EQM
			}
			else
			{
				if (dspram_addr & 1)
					modem_regs.reg18_19 = dspram[dspram_addr] | (dspram[(dspram_addr + 1) & (sizeof(dspram) - 1)] << 8);
				else
					modem_regs.reg18_19 = *(u16*)&dspram[dspram_addr];
				LOG("DSP mem Read address %08x == %x", dspram_addr, modem_regs.reg18_19 );
			}
		}
		break;

	case 0x1F:
		if (!modem_regs.reg1f.NCIE)
			modem_regs.reg1f.NCIA = 0;
		if (modem_regs.reg1f.NEWC)
		{
			if(modem_regs.reg1a.SFRES)
			{
				modem_regs.reg1a.SFRES = 0;
				LOG("Soft Reset SET && NEWC, executing reset and init");
				// As the reset line pulled and let go, but with NEWC left
				// set until the self tests are over
				modem_reset(0);
				modem_reset(1);
				modem_regs.reg1f.NEWC = 1;
				modem_regs.ptr[0x20] = 0;
			}
			else
			{
				modem_regs.reg1f.NEWC = 0;	// accept the settings
				if (modem_regs.reg1f.NCIE)
					modem_regs.reg1f.NCIA = 1;
				LOG("NEWC CONF=%x", modem_regs.reg12);
			}
			if (modem_regs.reg12 == 0x81 && !v8bis)
			{
				// V.8 bis detector
				v8bis = (dspram[0x439] & 1) == 1;
				if (v8bis) {
					v8bisProto.reset();
					configureStreams();
				}
			}
		}
		// Don't allow NEWS to be set if 0
		if ((old & (1 << 3)) == 0)
			modem_regs.reg1f.NEWS = 0;
		if (!modem_regs.reg1f.NEWS)
		{
			modem_regs.reg1f.NSIA = 0;
		}
		break;

	default:
		//LOG("ModemNormalWrite : undef %03X = %X", reg, data);
		break;
	}
	update_interrupt();
}

u32 ModemReadMem_A0_006(u32 addr, u32 size)
{
	u32 reg = (addr & 0x7FF) >> 2;

	if (reg < 0x100)
		return MODEM_ID[reg & 1];

	reg -= 0x100;
	if (reg < 0x21)
	{
		switch (state)
		{
		case MS_NORMAL:
			{
				// Dial tone is detected if TONEA, TONEB and TONEC are set
				//if (reg==0xF)
				{
					const bool tone = connect_state == DISCONNECTED || connect_state == NEGO_COMPLETE;
					SET_STATUS_BIT(0x0f, modem_regs.reg0f.CTS, modem_regs.reg08.RTS && connect_state == CONNECTED);
					SET_STATUS_BIT(0x0b, modem_regs.reg0b.TONEA, tone);
					SET_STATUS_BIT(0x0b, modem_regs.reg0b.TONEB, tone);
					SET_STATUS_BIT(0x0b, modem_regs.reg0b.TONEC, tone);
					if (modem_regs.reg04.FIFOEN || module_download) {
						SET_STATUS_BIT(0x0d, modem_regs.reg0d.TXFNF, (u8)(txFifoSize <= 15.f));
						SET_STATUS_BIT(0x01, modem_regs.reg01.TXHF, (u8)(txFifoSize >= 8.f));
					}
					else {
						SET_STATUS_BIT(0x0d, modem_regs.reg0d.TXFNF, (u8)(txFifoSize == 0.f));
						SET_STATUS_BIT(0x01, modem_regs.reg01.TXHF, (u8)(txFifoSize >= 1.f));
					}
				}
				u8 data = modem_regs.ptr[reg];
				if (reg == 0x00)	// RBUFFER
				{
					//LOG("Read RBUFFER = %X", data);
#ifndef NDEBUG
					if (connect_state == CONNECTED && recv_fp)
						fputc(data, recv_fp);
#endif
					if (rxFifo.size() != 0)
					{
						rxFifo.pop();
						updateRxFifoStatus();
						update_interrupt();
					}
				}
				else if (reg == 0x16)	// SECRXB
				{
					if (v8bis)
					{
						if (connect_state == CONNECTED)
							data = 0;
						else
							data = v8bisProto.detectTone();
						modem_regs.reg16 = data;
					}
				}
				//else
				//	LOG("Read Reg %03x = %x", reg, data);

				return data;
			}

		case MS_ST_CONTROLER:
		case MS_ST_DSP:
			if (reg==0x10)
			{
				modem_regs.reg1e.TDBE=0;
				return 0;
			}
			else
			{
				return modem_regs.ptr[reg];
			}

		case MS_END_DSP:
			// Nothing to read, but NEWC: a soft reset is not over, and no
			// write is taken, until the last self test has ended
			return reg == 0x1f ? modem_regs.ptr[reg] : 0;

		case MS_RESETING:
			return 0; //still reset

		default:
			//LOG("Read (reset) reg %03x == %x", reg, modem_regs.ptr[reg]);
			return 0;
		}
	}

	LOG("modem reg %03X read -- wtf is it ?",reg);
	return 0;
}

void ModemWriteMem_A0_006(u32 addr, u32 data, u32 size)
{
	u32 reg = (addr & 0x7FF) >> 2;
	if (reg < 0x100)
	{
		LOG("modem reg %03X write -- MODEM ID?!",reg);
		return;
	}

	reg -= 0x100;
	if (reg < 0x20)
	{
		if (state == MS_NORMAL)
			ModemNormalWrite(reg,data);
		else
			LOG("modem reg %03X write %X -- undef state?", reg, data);
		return;
	}
	if (reg == 0x20)
	{
		//Hard reset
		modem_reset(data);
		return;
	}

	LOG("modem reg %03X write %X -- wtf is it?",reg,data);
}

/* The modem in a save state, since version 21 of the format: everything a
 * game can see through the registers, and which line the modem is on (see
 * line_id). The step that is due next is the scheduler's, and has been in
 * the state all along. Only a Dreamcast has a modem. */
void modem_serialize_v21(void **data, unsigned int *total_size)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return;

	u8 state8 = (u8)state;
	u8 connect_state8 = (u8)connect_state;
	u8 word_dspram_write8 = word_dspram_write;
	u8 module_download8 = module_download;
	u8 reg1b_save8 = (u8)reg1b_save;

	LIBRETRO_SA(modem_regs.ptr, sizeof(modem_regs.ptr));
	LIBRETRO_SA(dspram, sizeof(dspram));
	LIBRETRO_S(state8);
	LIBRETRO_S(connect_state8);
	LIBRETRO_S(word_dspram_write8);
	LIBRETRO_S(module_download8);
	LIBRETRO_S(reg1b_save8);
	LIBRETRO_S(download_crc);
	LIBRETRO_S(dead_steps);
	LIBRETRO_S(line_id);
	LIBRETRO_S(rx_pos);
	LIBRETRO_S(tx_pos);
	LIBRETRO_S(last_dial_time);
	LIBRETRO_S(connected_time);
}

bool modem_unserialize_v21(void **data, unsigned int *total_size)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return true;

	u8 state8;
	u8 connect_state8;
	u8 word_dspram_write8;
	u8 module_download8;
	u8 reg1b_save8;

	LIBRETRO_USA(modem_regs.ptr, sizeof(modem_regs.ptr));
	LIBRETRO_USA(dspram, sizeof(dspram));
	LIBRETRO_US(state8);
	LIBRETRO_US(connect_state8);
	LIBRETRO_US(word_dspram_write8);
	LIBRETRO_US(module_download8);
	LIBRETRO_US(reg1b_save8);
	LIBRETRO_US(download_crc);
	LIBRETRO_US(dead_steps);
	LIBRETRO_US(line_id);
	LIBRETRO_US(rx_pos);
	LIBRETRO_US(tx_pos);
	LIBRETRO_US(last_dial_time);
	LIBRETRO_US(connected_time);

	// Any byte is a register's or the DSP memory's; the rest is checked
	state = state8 <= MS_NORMAL ? (ModemStates)state8 : MS_INVALID;
	connect_state = connect_state8 <= NEGO_COMPLETE ? (ConnectState)connect_state8 : DISCONNECTED;
	word_dspram_write = word_dspram_write8 != 0;
	module_download = module_download8 != 0;
	reg1b_save = reg1b_save8;
	// Only a modem that is connected is on a line
	if (state != MS_NORMAL || connect_state != CONNECTED)
		line_id = 0;

	// The line that is up is not the state's, or the state is from too long
	// ago on it for the game to be given again what it has taken since
	if (live_line != 0
			&& (line_id != live_line
				|| rx_taken - rx_pos > LINE_WINDOW
				|| tx_sent - tx_pos > LINE_WINDOW)
			// (but a line made after the state's time, which its machine
			// may make again at the same time: see PRE_CONNECTED)
			&& !(line_id == 0 && live_line_made > sh4_sched_now64() && rx_taken <= LINE_WINDOW))
		line_close();

	return true;
}

/* A state from before version 21 has nothing of the modem but its next step:
 * the modem, and the line, stay as they are. */
void modem_state_before_v21(void)
{
}

/* What a state holds of the modem since version 24, in the network devices'
 * part (net_serialize_v24()): what has come over the line and not been
 * used yet, the receive FIFO, how full the transmit FIFO is, the speed of
 * the connection, and the V.42 / V.42 bis and V.8 bis ends of it. */
void modem_serialize_v24(void **data, unsigned int *total_size)
{
	s32 cpb = cyclesPerByte;
	u8 v8 = v8bis;

	line_fifo.serialize(data, total_size);
	rxFifo.serialize(data, total_size);
	LIBRETRO_S(txFifoSize);
	LIBRETRO_S(cpb);
	LIBRETRO_S(txRxRatio);
	LIBRETRO_S(v8);
	v42Proto.serialize(data, total_size);
	v8bisProto.serialize(data, total_size);
}

bool modem_unserialize_v24(void **data, unsigned int *total_size)
{
	s32 cpb = DEFAULT_CYCLES_PER_BYTE;
	u8 v8 = 0;

	line_fifo.unserialize(data, total_size);
	rxFifo.unserialize(data, total_size);
	LIBRETRO_US(txFifoSize);
	LIBRETRO_US(cpb);
	LIBRETRO_US(txRxRatio);
	LIBRETRO_US(v8);
	v42Proto.unserialize(data, total_size);
	v8bisProto.unserialize(data, total_size);

	// (the comparisons are false for a NaN)
	if (!(txFifoSize >= 0.f && txFifoSize <= 64.f))
		txFifoSize = 0.f;
	// From 56000 to 75 bits a second
	cyclesPerByte = cpb >= SH4_MAIN_CLOCK * 8 / 56000 && cpb <= SH4_MAIN_CLOCK * 8 / 75 ? cpb : DEFAULT_CYCLES_PER_BYTE;
	if (!(txRxRatio > 0.f && txRxRatio <= 16.f))
		txRxRatio = 1.f;
	v8bis = v8 != 0;
	configureStreams();
	return true;
}

/* An older state: the modem passed bytes straight between the game and the
 * line, one each 238 us, and a byte in the receive buffer is all its FIFO
 * holds. */
void modem_state_before_v24(void)
{
	line_fifo.clear();
	rxFifo.clear();
	if (modem_regs.reg1e.RDBF)
		rxFifo.push(modem_regs.reg00);
	txFifoSize = 0.f;
	cyclesPerByte = DEFAULT_CYCLES_PER_BYTE;
	txRxRatio = 1.f;
	v8bis = false;
	v8bisProto.reset();
	v42Proto.reset();
	configureStreams();
}
