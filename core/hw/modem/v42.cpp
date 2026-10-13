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
#include "v42.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_if.h"
#include <algorithm>

namespace modem
{

// V.42 timers
#define T400_CYCLES ((u64)SH4_MAIN_CLOCK / 1000 * 750)	// detection phase
#define T403_CYCLES ((u64)SH4_MAIN_CLOCK / 1000 * 1000)	// inactivity

void V14Encoder::write(u8 v)
{
	transmitBit(0); // start bit
	for (int i = 0; i < 8; i++) {
		transmitBit(v & 1);
		v >>= 1;
	}
	 // stop bit(s)
	for (int i = 0; i < txStopBits; i++)
		transmitBit(1);
}

void V14Encoder::flush()
{
	if (txBitCount != 0) {
		while (txBitCount != 0)
			transmitBit(1);
	}
	else {
		for (int i = 0; i < 8; i++)
			transmitBit(1);
	}
}

void V14Encoder::reset()
{
	buffer.clear();
	txCurByte = 0;
	txBitCount = 0;
	txStopBits = 1;
}

void V14Encoder::transmitBit(u8 v)
{
	txCurByte = (txCurByte >> 1) | (v << 7);
	txBitCount++;
	if (txBitCount == 8)
	{
		buffer.push(txCurByte);
		txCurByte = 0;
		txBitCount = 0;
	}
}

void V14Encoder::serialize(void **data, unsigned int *total_size) const
{
	u8 bitCount = (u8)txBitCount;
	u8 stopBits = (u8)txStopBits;

	buffer.serialize(data, total_size);
	LIBRETRO_S(txCurByte);
	LIBRETRO_S(bitCount);
	LIBRETRO_S(stopBits);
}

void V14Encoder::unserialize(void **data, unsigned int *total_size)
{
	u8 bitCount = 0;
	u8 stopBits = 1;

	buffer.unserialize(data, total_size);
	LIBRETRO_US(txCurByte);
	LIBRETRO_US(bitCount);
	LIBRETRO_US(stopBits);
	txBitCount = bitCount & 7;
	txStopBits = stopBits >= 1 && stopBits <= 16 ? stopBits : 1;
}

void V14Decoder::write(u8 v)
{
	for (int i = 0; i < 8; i++)
	{
		if (rxState == RX_STOP_BIT)
		{
			// waiting for 1st stop bit
			if ((v & 1) == 0)
			{
				if (charsSinceMissingStop < 4)
					INFO_LOG(MODEM, "V.14 stop bit missing after %d chars", charsSinceMissingStop);
				charsSinceMissingStop = 0;
				rxState = RX_RECEIVING;
				rxBitCount = 0;
				maxStopBits = std::max(maxStopBits, rxStopBits);
			}
			else {
				rxStopBits = 1;
				rxState = RX_IDLE;
			}
		}
		else if (rxState == RX_IDLE)
		{
			// idle or more stop bits state
			if ((v & 1) == 0)
			{
				 // start bit => now receiving
				rxState = RX_RECEIVING;
				rxBitCount = 0;
				maxStopBits = std::max(maxStopBits, rxStopBits);
			}
			else if (rxStopBits < 0x10000) {
				rxStopBits++;
			}
		}
		else
		{
			rxCurByte = (rxCurByte >> 1) | ((v & 1) << 7);
			rxBitCount++;
			if (rxBitCount == 8)
			{
				rxState = RX_STOP_BIT;
				buffer.push(rxCurByte);
				if (charsSinceMissingStop < 0x10000)
					charsSinceMissingStop++;
				rxStopBits = 0;
			}
		}
		v >>= 1;
	}
}

int V14Decoder::receivedStopBits()
{
	int max = maxStopBits;
	maxStopBits = 0;
	return max;
}

void V14Decoder::reset()
{
	buffer.clear();
	rxCurByte = 0;
	rxState = RX_IDLE;
	rxBitCount = 0;
	charsSinceMissingStop = 0;
	rxStopBits = 0;
	maxStopBits = 0;
}

void V14Decoder::serialize(void **data, unsigned int *total_size) const
{
	u8 st = (u8)rxState;
	u8 bitCount = (u8)rxBitCount;
	s32 since = charsSinceMissingStop;
	s32 stopBits = rxStopBits;
	s32 maxStop = maxStopBits;

	buffer.serialize(data, total_size);
	LIBRETRO_S(rxCurByte);
	LIBRETRO_S(st);
	LIBRETRO_S(bitCount);
	LIBRETRO_S(since);
	LIBRETRO_S(stopBits);
	LIBRETRO_S(maxStop);
}

void V14Decoder::unserialize(void **data, unsigned int *total_size)
{
	u8 st = 0;
	u8 bitCount = 0;
	s32 since = 0;
	s32 stopBits = 0;
	s32 maxStop = 0;

	buffer.unserialize(data, total_size);
	LIBRETRO_US(rxCurByte);
	LIBRETRO_US(st);
	LIBRETRO_US(bitCount);
	LIBRETRO_US(since);
	LIBRETRO_US(stopBits);
	LIBRETRO_US(maxStop);
	rxState = st <= RX_STOP_BIT ? (RxState)st : RX_IDLE;
	rxBitCount = bitCount <= 8 ? bitCount : 0;
	charsSinceMissingStop = std::min(std::max(since, 0), 0x10000);
	rxStopBits = std::min(std::max(stopBits, 0), 0x10000);
	maxStopBits = std::min(std::max(maxStop, 0), 0x10000);
}

static u16 calcCrc16(const u8 *data, unsigned len)
{
	u16 crc = 0xffff;
	for (unsigned i = 0; i < len; i++)
	{
		u16 word = (crc ^ data[i]) & 0xFFu;
		word = (u16)(word ^ (u16)((word << 4u) & 0xFFu));
		word = (u16)((word << 8u) ^ (word << 3u) ^ (word >> 4u));
		crc = ((crc >> 8) ^ word);
	}
	return crc ^ 0xffff;
}

template<unsigned N>
bool HdlcEncoder<N>::sendFrame(const u8 *data, unsigned len)
{
	// Flags, the frame and its CRC with a stuffed bit after every five ones at most
	if ((unsigned)buffer.room() < (len + 2) * 6 / 5 + 4)
		return false;
	// opening flag
	sendFlag();
	// data
	for (unsigned i = 0; i < len; i++)
		sendByte(data[i]);
	// crc
	u16 crc = calcCrc16(data, len);
	sendByte(crc & 0xff);
	sendByte(crc >> 8);
	// closing flag
	sendFlag();
	return true;
}

template<unsigned N>
void HdlcEncoder<N>::sendFlag()
{
	txBitBuffer |= 0x7e << (8 + txBitCount);
	txBitBuffer >>= 8;
	buffer.push(txBitBuffer & 0xff);
}

template<unsigned N>
void HdlcEncoder<N>::sendByte(u8 byte)
{
	txBitBuffer |= (u32)byte << (8 + txBitCount);
	u32 mask = 0x1f0 << txBitCount;
	for (int i = 0; i < 8; i++)
	{
		if ((txBitBuffer & mask) == mask)
		{
			u32 lShiftMask = 0xffffffff << (8 + txBitCount + i + 1);
			txBitBuffer = ((txBitBuffer & lShiftMask) << 1) | (txBitBuffer & ~lShiftMask);
			txBitCount++;
			mask <<= 1;
		}
		mask <<= 1;
	}
	txBitCount += 8;
	while (txBitCount >= 8)
	{
		txBitBuffer >>= 8;
		buffer.push(txBitBuffer & 0xff);
		txBitCount -= 8;
	}
}

template<unsigned N>
void HdlcEncoder<N>::reset()
{
	buffer.clear();
	txBitCount = 0;
	txBitBuffer = 0;
}

template<unsigned N>
void HdlcEncoder<N>::serialize(void **data, unsigned int *total_size) const
{
	u8 bitCount = (u8)txBitCount;

	buffer.serialize(data, total_size);
	LIBRETRO_S(txBitBuffer);
	LIBRETRO_S(bitCount);
}

template<unsigned N>
void HdlcEncoder<N>::unserialize(void **data, unsigned int *total_size)
{
	u8 bitCount = 0;

	buffer.unserialize(data, total_size);
	LIBRETRO_US(txBitBuffer);
	LIBRETRO_US(bitCount);
	// Fewer bits than an octet wait between two calls
	txBitCount = bitCount & 7;
}

template<unsigned N>
void HdlcDecoder<N>::write(u8 v)
{
	// (a frame not taken by now is dropped)
	ready = false;
	for (int bit = 0; bit < 8; bit++)
	{
		if (rxOnes >= 5)
		{
			if ((v & 1) == 1)
			{
				// we either have a flag or an abort
				rxCurByte = (rxCurByte >> 1) | 0x80;
				rxPosition++;
			}
			else
			{
				if (rxOnes == 6)
				{
					// we have a flag
					if (rxSize != 0 || overflow)
					{
						if (overflow) {
							INFO_LOG(MODEM, "HDLC frame too long");
						}
						else if (rxSize < 3) {
							INFO_LOG(MODEM, "HDLC frame too short: %d bytes", rxSize);
						}
						else
						{
							u16 crc = calcCrc16(rxFrame, rxSize - 2);
							if (crc != (rxFrame[rxSize - 2] | (rxFrame[rxSize - 1] << 8)))
							{
								WARN_LOG(MODEM, "Invalid CRC in received frame");
							}
							else
							{
								ready = true;
								readySize = rxSize - 2;
							}
						}
						rxSize = 0;
						overflow = false;
					}
					rxCurByte = 0;
					rxPosition = 0;
				}
				else if (rxOnes > 6)
				{
					// abort
					INFO_LOG(MODEM, "HDLC abort received");
					rxCurByte = 0;
					rxPosition = 1; // keep the last 0
					rxSize = 0;
					overflow = false;
				}
				// ignore stuffed 0
				rxOnes = 0;
			}
		}
		else {
			rxCurByte = (rxCurByte >> 1) | ((v & 1) << 7);
			rxPosition++;
		}
		if (rxPosition == 8)
		{
			if (rxSize < N)
				rxFrame[rxSize++] = rxCurByte;
			else
				overflow = true;
			rxCurByte = 0;
			rxPosition = 0;
		}
		if ((v & 1) == 1)
		{
			// (a long run of ones is an idle line: only how many up to 7 matters)
			if (rxOnes < 7)
				rxOnes++;
		}
		else
			rxOnes = 0;
		v >>= 1;
	}
}

template<unsigned N>
void HdlcDecoder<N>::reset()
{
	rxSize = 0;
	overflow = false;
	ready = false;
	readySize = 0;
	rxPosition = 0;
	rxCurByte = 0;
	rxOnes = 0;
}

template<unsigned N>
void HdlcDecoder<N>::serialize(void **data, unsigned int *total_size) const
{
	u8 frame[N];
	u16 size = (u16)rxSize;
	u8 bits[4] = { (u8)overflow, (u8)rxOnes, (u8)rxPosition, rxCurByte };

	for (unsigned i = 0; i < N; i++)
		frame[i] = i < rxSize ? rxFrame[i] : 0;
	LIBRETRO_S(size);
	LIBRETRO_SA(frame, N);
	LIBRETRO_SA(bits, 4);
}

template<unsigned N>
void HdlcDecoder<N>::unserialize(void **data, unsigned int *total_size)
{
	u16 size = 0;
	u8 bits[4] = { 0 };

	LIBRETRO_US(size);
	LIBRETRO_USA(rxFrame, N);
	LIBRETRO_USA(bits, 4);
	rxSize = size <= N ? size : 0;
	overflow = bits[0] != 0;
	rxOnes = std::min<int>(bits[1], 7);
	rxPosition = bits[2] & 7;
	rxCurByte = bits[3];
	ready = false;
	readySize = 0;
}

template class HdlcEncoder<2048>;
template class HdlcEncoder<256>;
template class HdlcDecoder<V42_MAX_INFO + 5 + 64>;
template class HdlcDecoder<128>;

static inline unsigned seqDistance(unsigned from, unsigned to) {
	return (to - from) & 127;
}

static inline u8 incMod128(unsigned v) {
	return (v + 1) & 127;
}

/* The time in SH4 cycles. The protocol of the modem's static object is
 * reset before the machine is made, when there is no time yet. */
static inline u64 now() {
	return p_sh4rcb != NULL ? sh4_sched_now64() : 0;
}

void V42Protocol::handleFrame()
{
	if (!hdlcDecoder.frameAvailable())
		return;
	hdlcDecoder.frameTaken();
	const u8 *rxFrame = hdlcDecoder.frame();
	const unsigned size = hdlcDecoder.frameSize();
	if (size < 2) {
		WARN_LOG(MODEM, "Invalid frame: %d bytes", size);
		return;
	}
	if ((rxFrame[0] & 1) == 0) {
		WARN_LOG(MODEM, "Invalid frame: Unexpected extended address");
		return;
	}
	if ((rxFrame[0] >> 2) != 0) {
		WARN_LOG(MODEM, "Invalid frame: Unknown address %02x", rxFrame[0] >> 2);
		return;
	}

	if ((rxFrame[1] & 3) != 3)
	{
		// Information and Supervisory frames have a second control byte
		if (size < 3) {
			WARN_LOG(MODEM, "Invalid frame: control %02x and %d bytes", rxFrame[1], size);
			return;
		}
		// and they acknowledge sent frames (except SREJ)
		if (rxFrame[1] != 0xd)
			ackIFrame(rxFrame[2] >> 1);
	}
	inactivityStart = now();

	switch (rxFrame[1] & 0xef)
	{
	case 0x01:	// RR
		if (!receiverReady)
			NOTICE_LOG(MODEM, "Received RR");
		receiverReady = true;
		break;
	case 0x05:	// RNR
		if (receiverReady)
			WARN_LOG(MODEM, "Received RNR");
		receiverReady = false;
		break;
	case 0x09:	// REJECT
		receiverReady = true;
		handleReject(rxFrame[2]);
		break;
	case 0x0d:	// SREJ
		WARN_LOG(MODEM, "Received SREJ");
		break;
	case 0x6f:	// SABME
		handleSabme(rxFrame[0], rxFrame[1]);
		break;
	case 0x0f:	// DM
		break;
	case 0x03:	// UI
		WARN_LOG(MODEM, "Received UI");
		break;
	case 0x43:	// DISC
		handleDisc(rxFrame[0]);
		break;
	case 0x63:	// UA
		receiverReady = true;
		break;
	case 0x87:	// FRMR
		WARN_LOG(MODEM, "Received FRMR");
		break;
	case 0xaf:	// XID
		handleXid(rxFrame, size);
		break;
	case 0xe3:	// TEST
		break;
	default:
		if (rxFrame[1] & 1)
		{
			WARN_LOG(MODEM, "Invalid HDLC command: %02x", rxFrame[1]);
			return;
		}
		// I-frame
		if (phase != Connected) {
			WARN_LOG(MODEM, "I-frame received but not connected");
			return;
		}
		handleIFrame(rxFrame, size);
		break;
	}
}

void V42Protocol::handleSabme(u8 address, u8 control)
{
	if (phase == Connected)
		WARN_LOG(MODEM, "V.42: SABME received while already connected");
	else
		INFO_LOG(MODEM, "V.42: Received SABME");
	const u8 ua[] { address, (u8)(0x63 | (control & 0x10)) };
	sendFrame(ua, sizeof(ua));

	phase = Connected;
	txSeqNum = 0;
	rxSeqNum = 0;
	txSeqAck = 0;
	receiverReady = true;
	memset(sentSize, 0, sizeof(sentSize));
	resending = false;
}

void V42Protocol::handleDisc(u8 address)
{
	INFO_LOG(MODEM, "V.42: Received DISC");
	phase = Release;
	const u8 ua[] { address, 0x73 };
	sendFrame(ua, sizeof(ua));
}

void V42Protocol::handleIFrame(const u8 *rxFrame, unsigned size)
{
	const int seqNum = rxFrame[1] >> 1;
	if (seqNum != rxSeqNum)
	{
		INFO_LOG(MODEM, "Received frame num out of sequence: expected %d but recv %d", rxSeqNum, seqNum);
		// send REJECT: N(R) is in the upper 7 bits
		const u8 reject[] { rxFrame[0], 9, (u8)(rxSeqNum << 1) };
		sendFrame(reject, sizeof(reject));
		return;
	}
	rxSeqNum = incMod128(rxSeqNum);
	if (compressionEnabled)
	{
		for (unsigned i = 3; i < size; i++)
		{
			if (!decompressor.write(rxFrame[i], outputStream))
			{
				// send DISC
				const u8 disc[] { 1, 0x43 };
				sendFrame(disc, sizeof(disc));
				phase = Release;
				memset(sentSize, 0, sizeof(sentSize));
				resending = false;
				return;
			}
		}
	}
	else {
		for (unsigned i = 3; i < size; i++)
			outputStream.write(rxFrame[i]);
	}

	// if P bit set, respond with RR
	// if not set, respond with RR if no I-frame available to send
	if ((rxFrame[2] & 1) || inputStream.available() == 0)
	{
		const u8 rr[] { rxFrame[0], 1, u8((rxSeqNum << 1) | (rxFrame[2] & 1)) };
		sendFrame(rr, sizeof(rr));
	}
}

// The value of a parameter: big-endian, 4 bytes at most
static u32 xidValue(const u8 *p, unsigned len)
{
	u32 v = 0;
	for (unsigned i = 0; i < len; i++)
		v = (v << 8) | p[i];
	return v;
}

static void xidSetValue(u8 *p, unsigned len, u32 v)
{
	for (unsigned i = len; i > 0; i--)
	{
		p[i - 1] = v & 0xff;
		v >>= 8;
	}
}

/* The other end proposes the parameters of the link and of compression: the
 * answer is the same frame, with the values this end cannot have lowered to
 * what it can, which the negotiation allows. */
void V42Protocol::handleXid(const u8 *rxFrame, unsigned size)
{
	u8 echo[sizeof(sentFrames[0]) + 64];
	u32 maxCodeWords = v42b::MIN_TOTAL_CODEWORDS;
	u32 maxStringLength = v42b::DEF_STRING_SIZE;
	compressionEnabled = false;

	// format identifier
	if (size < 3 || rxFrame[2] != 0x82) {
		WARN_LOG(MODEM, "Unexpected XID format: %02x", size < 3 ? 0 : rxFrame[2]);
		return;
	}
	if (size > sizeof(echo))
		size = sizeof(echo);
	memcpy(echo, rxFrame, size);
	unsigned echoSize = size;

	// Iterate over parameter groups
	enum {
		GID_PARAM = 0x80,
		GID_PRIVATE = 0xf0,
		GID_USER_DATA = 0xff,
	};
	for (unsigned i = 3; i + 3 <= size; )
	{
		const u8 gid = echo[i];
		if (gid == GID_USER_DATA)
		{
			// not answered
			echoSize = i;
			break;
		}
		const unsigned groupSize = echo[i + 1] * 256 + echo[i + 2];
		const unsigned end = std::min(i + 3 + groupSize, size);
		if (gid != GID_PARAM && gid != GID_PRIVATE)
		{
			INFO_LOG(MODEM, "Unexpected XID GI: %02x", gid);
			i = end;
			continue;
		}
		// Parameters
		for (unsigned j = i + 3; j + 2 <= end; )
		{
			const u8 paramId = echo[j];
			const u8 paramSize = echo[j + 1];
			u8 *pv = &echo[j + 2];
			if (j + 2 + paramSize > end)
				break;
			const bool small = paramSize <= 4;
			const u32 v = small ? xidValue(pv, paramSize) : 0;
			if (!small)
				WARN_LOG(MODEM, "Unexpected param length for PI %d: %x", paramId, paramSize);
			switch (paramId)
			{
			case 0:	// (private) Parameter set identification
				break;
			case 1:	// (private) Data compression request
				if (gid == GID_PRIVATE && paramSize >= 1)
				{
					DEBUG_LOG(MODEM, "xid: Data compression request %x", pv[0]);
					if (pv[0] == 3)
						compressionEnabled = true;
					else
						// Disable all compression if only one direction is compressed (not used anyway)
						pv[0] = 0;
				}
				break;
			case 2:	// (private) Number of codewords
				if (gid == GID_PRIVATE && small)
				{
					DEBUG_LOG(MODEM, "xid: Number of codewords %d", v);
					maxCodeWords = v;
					if (v > v42b::MAX_TOTAL_CODEWORDS)
					{
						maxCodeWords = v42b::MAX_TOTAL_CODEWORDS;
						xidSetValue(pv, paramSize, maxCodeWords);
					}
				}
				break;
			case 3:
				if (gid == GID_PARAM && small)
				{
					// HDLC optional function (public)
					DEBUG_LOG(MODEM, "xid: HDLC optional function %x", v);
				}
				else if (gid == GID_PRIVATE && small)
				{
					// (private) Maximum string length
					DEBUG_LOG(MODEM, "xid: Maximum string length %d", v);
					maxStringLength = v;
					if (v > (u32)v42b::MAX_STRING_SIZE)
					{
						maxStringLength = v42b::MAX_STRING_SIZE;
						xidSetValue(pv, paramSize, maxStringLength);
					}
				}
				break;
			case 5:	// Maximum length of information field: tx
			case 6:	// Maximum length of information field: rx
				if (gid == GID_PARAM && small)
				{
					DEBUG_LOG(MODEM, "xid: Max %s len %d bits", paramId == 5 ? "tx" : "rx", v);
					u32 bits = v;
					if (bits > V42_MAX_INFO * 8)
					{
						bits = V42_MAX_INFO * 8;
						xidSetValue(pv, paramSize, bits);
					}
					if (paramId == 5 && bits >= 8)
						txMaxSize = bits / 8;
				}
				break;
			case 7:	// Window size: tx
			case 8:	// Window size: rx
				if (gid == GID_PARAM && small)
				{
					DEBUG_LOG(MODEM, "xid: %s window %d", paramId == 7 ? "Tx" : "Rx", v);
					u32 k = v;
					if (k > 15)
					{
						k = 15;
						xidSetValue(pv, paramSize, k);
					}
					if (paramId == 7 && k >= 1)
						txWindow = k;
				}
				break;
			default:
				WARN_LOG(MODEM, "Unexpected PI %d (size %x, group id %x)", paramId, paramSize, gid);
				break;
			}
			j += 2 + paramSize;
		}
		i = end;
	}
	sendFrame(echo, echoSize);

	if (compressionEnabled)
	{
		NOTICE_LOG(MODEM, "V.42bis compression enabled: max dictionary size %d", maxCodeWords);
		compressor.setParams(maxCodeWords, maxStringLength);
		compressor.reset();
		decompressor.setParams(maxCodeWords, maxStringLength);
		decompressor.reset();
	}
}

void V42Protocol::handleReject(u8 control2)
{
	const u8 seq = control2 >> 1;
	INFO_LOG(MODEM, "Received REJECT %d", seq);
	// The frames from N(R) on are sent again, one at a time as the line has room
	if (seqDistance(txSeqAck, seq) <= seqDistance(txSeqAck, txSeqNum) && seq != txSeqNum)
	{
		resendSeq = seq;
		resending = true;
	}
}

void V42Protocol::sendIFrame()
{
	if (!receiverReady || inputStream.available() == 0)
		return;
	// check that tx window isn't reached
	if ((int)seqDistance(txSeqAck, txSeqNum) >= txWindow)
		return;

	u8 *frame = sentFrames[txSeqNum & 15];
	unsigned len = 3;
	frame[0] = 1;
	frame[1] = txSeqNum << 1;
	frame[2] = rxSeqNum << 1;
	if (compressionEnabled)
	{
		while (compressor.available() < (int)txMaxSize)
		{
			int c = inputStream.read();
			if (c == -1)
				break;
			compressor.write(c);
		}
		compressor.flush();
		while (len - 3 < txMaxSize)
		{
			int cc = compressor.read();
			if (cc == -1)
				break;
			frame[len++] = cc & 0xff;
		}
	}
	else
	{
		while (len - 3 < txMaxSize)
		{
			int c = inputStream.read();
			if (c == -1)
				break;
			frame[len++] = c & 0xff;
		}
	}
	if (len == 3)
		// don't send an empty frame
		return;
	sentSize[txSeqNum & 15] = len;
	txSeqNum = incMod128(txSeqNum);
	sendFrame(frame, len);
}

void V42Protocol::feedV14()
{
	while (v14Encoder.available() < 64)
	{
		int c = inputStream.read();
		if (c == -1)
			break;
		v14Encoder.write(c);
	}
}

int V42Protocol::read()
{
	if (phase == None || phase == Detection) {
		v14Encoder.flush();
		return v14Encoder.read();
	}
	else if (phase == V14)
	{
		feedV14();
		if (v14Encoder.available() == 0)
			v14Encoder.flush();
		return v14Encoder.read();
	}
	if (hdlcEncoder.available() == 0)
	{
		if (phase == Connected)
		{
			// Frames rejected by the other end first
			if (resending && seqDistance(txSeqAck, resendSeq) > seqDistance(txSeqAck, txSeqNum))
				// acknowledged in the meantime
				resendSeq = txSeqAck;
			while (resending && hdlcEncoder.available() == 0)
			{
				if (resendSeq == txSeqNum) {
					resending = false;
					break;
				}
				const unsigned slot = resendSeq & 15;
				if (sentSize[slot] != 0 && (sentFrames[slot][1] >> 1) == resendSeq)
				{
					sentFrames[slot][2] = rxSeqNum << 1;
					sendFrame(sentFrames[slot], sentSize[slot]);
				}
				else {
					WARN_LOG(MODEM, "REJ frame %d not found", resendSeq);
				}
				resendSeq = incMod128(resendSeq);
			}
			if (hdlcEncoder.available() == 0)
				sendIFrame();
			if (hdlcEncoder.available() == 0 && now() - inactivityStart >= T403_CYCLES)
			{
				// send an RR frame with P bit set
				const u8 rr[] { 1, 1, u8((rxSeqNum << 1) | 1) };
				sendFrame(rr, sizeof(rr));
			}
		}
		if (hdlcEncoder.available() == 0)
			sendFlag();
	}
	return hdlcEncoder.read();
}

void V42Protocol::write(u8 v)
{
	if (phase == None || phase == Detection)
	{
		if (now() - detectionStart >= T400_CYCLES) {
			INFO_LOG(MODEM, "Switching to V.14 mode");
			phase = V14;
		}
		else
		{
			v14Decoder.write(v);
			int c = v14Decoder.read();
			if (c == -1)
				return;
			v = c;
		}
	}
	if (phase == None || phase == Detection)
	{
		// V.42 ODP: DC1 with alternating parity followed by 8+1 or 16+1 ones
		bool dc1 = false;
		if (v == 0x11 || v == 0x91)
		{
			const int stopBits = v14Decoder.receivedStopBits();
			dc1 = stopBits == 9 || stopBits == 17;
		}
		if (phase == None)
		{
			if (dc1)
			{
				phase = Detection;
				lastRx = v;
				odpCount = 1;
			}
		}
		else
		{
			if (dc1 && v != lastRx)
			{
				odpCount++;
				if (odpCount == 4)
				{
					odpCount = 0;
					// send ADP
					v14Encoder.setStopBits(9);
					v14Encoder.write('E');
					v14Encoder.write('C');
					//v14Encoder.write(0);	// switches to v14 only and start ppp nego
					v14Encoder.setStopBits(1);
				}
			}
			else
			{
				odpCount = 0;
				if (v == 0x7e)
					phase = Establish;
			}
			lastRx = v;
		}
	}
	else if (phase == Establish || phase == Connected) {
		hdlcDecoder.write(v);
		handleFrame();
	}
	else if (phase == V14)
	{
		v14Decoder.write(v);
		for (int c = v14Decoder.read(); c != -1; c = v14Decoder.read())
			outputStream.write(c);
	}
}

void V42Protocol::reset()
{
	phase = None;
	lastRx = 0;
	odpCount = 0;
	v14Encoder.reset();
	v14Decoder.reset();
	hdlcEncoder.reset();
	hdlcDecoder.reset();
	txSeqNum = 0;
	rxSeqNum = 0;
	txSeqAck = 0;
	receiverReady = true;
	txMaxSize = 128;
	txWindow = 15;
	memset(sentFrames, 0, sizeof(sentFrames));
	memset(sentSize, 0, sizeof(sentSize));
	resendSeq = 0;
	resending = false;
	detectionStart = now();
	inactivityStart = detectionStart;
	compressionEnabled = false;
	compressor.setParams(v42b::MIN_TOTAL_CODEWORDS, v42b::DEF_STRING_SIZE);
	compressor.reset();
	decompressor.setParams(v42b::MIN_TOTAL_CODEWORDS, v42b::DEF_STRING_SIZE);
	decompressor.reset();
}

void V42Protocol::ackIFrame(int seqNum)
{
	// N(R) acknowledges the frames before it: no further than the last one sent
	if (seqDistance(txSeqAck, seqNum) <= seqDistance(txSeqAck, txSeqNum))
	{
		for (unsigned seq = txSeqAck; seq != (unsigned)seqNum; seq = incMod128(seq))
			sentSize[seq & 15] = 0;
		txSeqAck = seqNum;
	}
	else {
		WARN_LOG(MODEM, "Ack seq# %d outside of the frames sent %d-%d", seqNum, txSeqAck, txSeqNum);
	}
}

void V42Protocol::serialize(void **data, unsigned int *total_size) const
{
	u8 b[11] = {
		(u8)phase, lastRx, (u8)odpCount, txSeqNum, rxSeqNum, txSeqAck, (u8)receiverReady,
		(u8)txWindow, resendSeq, (u8)resending, (u8)compressionEnabled
	};
	u16 maxSize = (u16)txMaxSize;

	LIBRETRO_SA(b, 11);
	LIBRETRO_S(maxSize);
	LIBRETRO_S(detectionStart);
	LIBRETRO_S(inactivityStart);
	LIBRETRO_SA(&sentSize[0], 16);
	LIBRETRO_SA(&sentFrames[0][0], sizeof(sentFrames));
	v14Encoder.serialize(data, total_size);
	v14Decoder.serialize(data, total_size);
	hdlcEncoder.serialize(data, total_size);
	hdlcDecoder.serialize(data, total_size);
	compressor.serialize(data, total_size);
	decompressor.serialize(data, total_size);
}

void V42Protocol::unserialize(void **data, unsigned int *total_size)
{
	u8 b[11] = { 0 };
	u16 maxSize = 128;

	LIBRETRO_USA(b, 11);
	LIBRETRO_US(maxSize);
	LIBRETRO_US(detectionStart);
	LIBRETRO_US(inactivityStart);
	LIBRETRO_USA(&sentSize[0], 16);
	LIBRETRO_USA(&sentFrames[0][0], sizeof(sentFrames));
	v14Encoder.unserialize(data, total_size);
	v14Decoder.unserialize(data, total_size);
	hdlcEncoder.unserialize(data, total_size);
	hdlcDecoder.unserialize(data, total_size);
	compressor.unserialize(data, total_size);
	decompressor.unserialize(data, total_size);

	phase = b[0] <= V14 ? (Phase)b[0] : None;
	lastRx = b[1];
	odpCount = b[2] & 3;
	txSeqNum = b[3] & 127;
	rxSeqNum = b[4] & 127;
	txSeqAck = b[5] & 127;
	receiverReady = b[6] != 0;
	txWindow = b[7] >= 1 && b[7] <= 15 ? b[7] : 15;
	resendSeq = b[8] & 127;
	resending = b[9] != 0;
	compressionEnabled = b[10] != 0;
	txMaxSize = maxSize >= 1 && maxSize <= V42_MAX_INFO ? maxSize : 128;
	if ((int)seqDistance(txSeqAck, txSeqNum) > txWindow)
		txSeqAck = txSeqNum;
	for (int i = 0; i < 16; i++)
		if (sentSize[i] > 3 + V42_MAX_INFO)
			sentSize[i] = 0;
}

u8 V8bisProtocol::detectTone()
{
	switch (toneState)
	{
	case 0:
		if (toneTime == 0) {
			toneTime = now();
		}
		else if (now() - toneTime >= SH4_MAIN_CLOCK / 1000 * 400) {
			toneState++;
			toneTime = now();
		}
		break;

	case 1: // Segment 1 dual tone received and single tone segment 2 is being detected
		if (now() - toneTime >= SH4_MAIN_CLOCK / 1000 * 100) {
			toneState++;
			toneTime = now();
		}
		return 0xE0;

	case 2:
		return 0x20; // Mode Request (MRe) received
	}
	return 0;
}

void V8bisProtocol::sendCL()
{
	dataMode = true;
	static const u8 cl[] = {
			0x12,	// CL, rev.1
			//0x13,	// CLR, rev.1
					// identification field (I)
			0xc3,	// NPar1: V.8, short V.8, non-standard field
			0x80,	// SPar1: network type not specified
					// standard information field (S)
			0x80,	// NPar1
			0x01,	// SPar2[1]: data
			0x80,	// SPar2[2]
			  0x0F,	// NPar2[1]: transparent data, V.42, V.42 bis, V.14
			  0x30,	// NPar2[2]: V.34, V.32 bis
			  0xFF,	// NPar2[3]: V.32, V.22 bis, V.22, V.21, V.90 digital+ana
			  //0xC0,	// V.8 bis rev.2: NPar2[4]
					// non-standard information (NS)
			0x09,	// size
			0xb5,	// country code (USA)
			0x02,	// manuf. code len
			0x00,	// manufacturer code (K56flex)
			0x94,
			0x81,	// licensee code (Rockwell)
			0x83,	// capabilities (K56flex, V90?)
			0x43,	// K56flex version number
			0x47,	// Rockwell MDP version number
			0xC4,	// u-law (none) and controller version number (4)
	};
	encoder.sendFlag();
	encoder.sendFrame(cl, sizeof(cl));
	encoder.sendFlag();
	encoder.sendFlag();
}

void V8bisProtocol::sendAck(int n)
{
	const u8 ack = u8(0x13 + n); // 0x14: ACK(1), 0x15: ACK(2)
	encoder.sendFlag();
	encoder.sendFrame(&ack, 1);
	encoder.sendFlag();
	encoder.sendFlag();
}

void V8bisProtocol::sendNak(int n)
{
	const u8 nak = u8(7 + n); // 8: NAK(1), 9: NAK(2), ...
	encoder.sendFlag();
	encoder.sendFrame(&nak, 1);
	encoder.sendFlag();
	encoder.sendFlag();
}

void V8bisProtocol::emitTone(u8 tone)
{
	if (toneState == 2 && tone == 0x33) // emit Capabilities Request (CRd)
		// should send CL or CLR
		sendCL();
}

int V8bisProtocol::read()
{
	if (!dataMode)
		return -1;
	else
		return encoder.read();
}

int V8bisProtocol::available()
{
	if (!dataMode)
		return 0;
	else
		return encoder.available();
}

/* The fields of a V.8 bis message are runs of octets, the last of which has
 * its top bit set (the delimiter): the length of the one at @p, delimiter
 * included, as far as @end */
static unsigned paramSize(const u8 *p, const u8 *end, unsigned delimMask = 0x80)
{
	const u8 *begin = p;
	while (p < end && (*p & delimMask) == 0)
		p++;
	return p - begin + 1;
}

void V8bisProtocol::handleMS(const u8 *frame, unsigned size)
{
	const u8 *end = frame + size;
	// the octet at @p, 0 past the end of the message
	#define AT(p) ((p) < end ? *(p) : 0)
	// identification field (I)
	// NPar1
	const u8 *p = &frame[1];
	const bool v8 = AT(p) & 1;
	const bool shortV8 = AT(p) & 2;
	const bool txAck = AT(p) & 8;
	const bool hasNSF = AT(p) & 0x40;
	unsigned sz = paramSize(p, end);
	p += sz;
	sz = paramSize(p, end);
	if (AT(p) & 0x7f) {
		p += sz;
		p += paramSize(p, end); // NPar2's
	}
	else {
		p += sz;
	}
	// standard info field (S)
	p += paramSize(p, end); // NPar1
	sz = paramSize(p, end);
	const u8 spar1 = AT(p);
	if ((spar1 & 1) == 0)
	{
		WARN_LOG(MODEM, "V.8 bis MS: Data bit not set in field S - SPar1");
		sendNak();
		return;
	}
	p += sz;
	const bool v42 = AT(p) & 2;
	const bool v42bis = AT(p) & 4;
	const bool v14 = AT(p) & 8;
	int speed = 0;
	if ((AT(p) & 0xc0) == 0)
	{
		++p;
		if (AT(p) & 0x10)
			// V34
			speed = 33600;
		else if (AT(p) & 0x20)
			// V32 bis
			speed = 14400;
		if (speed == 0 && (AT(p) & 0xc0) == 0)
		{
			++p;
			if (AT(p) & 1)
				// V32
				speed = 9600;
			else if (AT(p) & 2)
				// V22 bis
				speed = 2400;
			else if (AT(p) & 4)
				// V22
				speed = 1200;
			else if (AT(p) & 8)
				// V21
				speed = 300;
		}
	}
	p += paramSize(p, end);
	if (hasNSF && AT(p) >= 5 && AT(p + 2) == 2)
	{
		if (AT(p + 3) == 0 && AT(p + 4) == 0x94)
			// K56flex / V90
			speed = 56000;
	}
	#undef AT
	if (txAck)
		sendAck(1);
	DEBUG_LOG(MODEM, "V8bis: received MS: V8 %d sV8 %d V14 %d V42 %d V42bis %d speed %d",
			v8, shortV8, v14, v42, v42bis, speed);
}

void V8bisProtocol::write(u8 v)
{
	if (!dataMode)
		return;

	decoder.write(v);
	if (!decoder.frameAvailable())
		return;
	decoder.frameTaken();

	const u8 *frame = decoder.frame();
	const unsigned size = decoder.frameSize();
	const u8 frameType = frame[0] & 0xf;
	switch (frameType)
	{
	case 1: // MS
		handleMS(frame, size);
		break;
	default:
		WARN_LOG(MODEM, "Unhandled V.8 bis frame type %d", frameType);
		break;
	}
	done = true;
}

void V8bisProtocol::reset()
{
	encoder.reset();
	decoder.reset();
	dataMode = false;
	toneState = 0;
	toneTime = 0;
	done = false;
}

void V8bisProtocol::serialize(void **data, unsigned int *total_size) const
{
	u8 b[3] = { (u8)toneState, (u8)dataMode, (u8)done };

	encoder.serialize(data, total_size);
	decoder.serialize(data, total_size);
	LIBRETRO_SA(b, 3);
	LIBRETRO_S(toneTime);
}

void V8bisProtocol::unserialize(void **data, unsigned int *total_size)
{
	u8 b[3] = { 0 };

	encoder.unserialize(data, total_size);
	decoder.unserialize(data, total_size);
	LIBRETRO_USA(b, 3);
	LIBRETRO_US(toneTime);
	toneState = b[0] <= 2 ? b[0] : 0;
	dataMode = b[1] != 0;
	done = b[2] != 0;
}

}	// namespace modem
