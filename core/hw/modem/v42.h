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
#include "internal.h"
#include "v42bis.h"

/* The far end of a synchronous modem connection: V.14 (characters framed
 * by start and stop bits over the synchronous line), V.42 error correction
 * (LAPM: HDLC frames) and V.42 bis compression, and the V.8 bis exchange
 * some games start a call with. All of it has a fixed size, so that it can
 * be in a save state, and all that the other end sends is checked against
 * those sizes. */
namespace modem
{

class V14Encoder
{
public:
	void write(u8 v);
	void flush();
	void reset();
	int read() {
		return buffer.pop();
	}
	int available() const {
		return buffer.size();
	}
	void setStopBits(int stopBits) {
		txStopBits = stopBits;
	}

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	void transmitBit(u8 v);

	ByteFifo<512> buffer;
	u8 txCurByte = 0;
	u32 txBitCount = 0;
	int txStopBits = 1;
};

class V14Decoder
{
public:
	void write(u8 v);
	int read() {
		return buffer.pop();
	}
	int receivedStopBits();
	void reset();

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	enum RxState {
		RX_IDLE,
		RX_RECEIVING,
		RX_STOP_BIT
	};
	ByteFifo<16> buffer;
	u8 rxCurByte = 0;
	RxState rxState = RX_IDLE;
	u32 rxBitCount = 0;
	int charsSinceMissingStop = 0;
	int rxStopBits = 0;
	int maxStopBits = 0;
};

// The bytes of HDLC frames, as they go on the line: flags, bit stuffing, CRC
template<unsigned N>
class HdlcEncoder
{
public:
	// False, and nothing sent, if there is no room for it
	bool sendFrame(const u8 *data, unsigned len);
	void sendFlag();
	void reset();
	int read() {
		return buffer.pop();
	}
	int available() const {
		return buffer.size();
	}

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	void sendByte(u8 byte);

	ByteFifo<N> buffer;
	u32 txBitBuffer = 0;
	int txBitCount = 0;
};

/* The frames in the bits received. A frame longer than N bytes (its CRC
 * included) is dropped. */
template<unsigned N>
class HdlcDecoder
{
public:
	void write(u8 v);
	// A whole frame has been received: its bytes, without the CRC
	bool frameAvailable() const {
		return ready;
	}
	const u8 *frame() const {
		return rxFrame;
	}
	unsigned frameSize() const {
		return readySize;
	}
	void frameTaken() {
		ready = false;
	}
	void reset();

	// (a frame that has been received is always taken before the state is saved)
	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	u8 rxFrame[N];
	unsigned rxSize = 0;
	bool overflow = false;
	bool ready = false;
	unsigned readySize = 0;
	int rxOnes = 0;
	int rxPosition = 0;
	u8 rxCurByte = 0;
};

// The longest information field of an I frame either way (N401)
static const unsigned V42_MAX_INFO = 256;

class V42Protocol : public InStream, public OutStream
{
public:
	V42Protocol(InStream& in, OutStream& out)
	: inputStream(in), outputStream(out)
	{
		reset();
	}

	int read() override;
	void write(u8 v) override;
	void reset();

	int available() override {
		return 1;
	}

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	InStream& inputStream;
	OutStream& outputStream;

	enum Phase {
		None,
		Detection,
		Establish,
		Connected,
		Release,
		V14			// Bomberman Online, Power Smash, Pro Yakyu Team de asobou net!
	};
	Phase phase;
	u8 lastRx;
	int odpCount;
	V14Encoder v14Encoder;
	V14Decoder v14Decoder;
	// for V42 mode
	HdlcEncoder<2048> hdlcEncoder;
	HdlcDecoder<V42_MAX_INFO + 5 + 64> hdlcDecoder;
	u8 txSeqNum;
	u8 rxSeqNum;
	u8 txSeqAck;
	bool receiverReady;
	unsigned txMaxSize;
	int txWindow;
	// The I frames sent and not acknowledged yet, by their number (the window is 15 at most)
	u8 sentFrames[16][3 + V42_MAX_INFO];
	u16 sentSize[16];
	// The next frame to send again after a REJ, until txSeqNum
	u8 resendSeq;
	bool resending;
	// V42 bis
	v42b::Compressor compressor;
	v42b::Decompressor decompressor;
	bool compressionEnabled;
	u64 detectionStart;		// Detection phase timer (T400)
	u64 inactivityStart;	// Inactivity timer (T403)

	void handleFrame();
	void handleSabme(u8 address, u8 control);
	void handleDisc(u8 address);
	void handleIFrame(const u8 *frame, unsigned size);
	void handleXid(const u8 *frame, unsigned size);
	void handleReject(u8 control2);

	void sendFlag() {
		hdlcEncoder.sendFlag();
	}
	void sendFrame(const u8 *data, unsigned len) {
		if (!hdlcEncoder.sendFrame(data, len))
			INFO_LOG(MODEM, "V.42: no room for a frame of %d bytes", len);
	}

	void sendIFrame();
	void ackIFrame(int seqNum);
	// V14 mode: the characters from the network, as many as the encoder needs
	void feedV14();
};

//
// V.8 bis is used by older browsers (Planet Web 1.125, DreamKey 1.0/1.5 and the many games that include them)
// and Windows CE games.
// To enable 56k, a non-standard info field must be returned indicating K56flex/V90 support.
//
class V8bisProtocol : public InStream, public OutStream
{
public:
	V8bisProtocol() {
		reset();
	}
	void emitTone(u8 tone);
	u8 detectTone();
	int read() override;
	int available() override;
	void write(u8 v) override;
	void reset();

	bool completed() {
		return available() == 0 && done;
	}

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	void sendCL();
	void sendAck(int n = 1);
	void sendNak(int n = 1);
	void handleMS(const u8 *frame, unsigned size);

	HdlcEncoder<256> encoder;
	HdlcDecoder<128> decoder;
	int toneState;
	u64 toneTime;
	bool dataMode;
	bool done;
};

}	// namespace modem
