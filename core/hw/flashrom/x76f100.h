/*
 * x76f100.h
 *
 * From mame x76f100.h
 * // license:BSD-3-Clause
 * // copyright-holders:smf
 *
 * The X76F100 secure serial flash: 112 bytes behind a read and a write
 * password. A NAOMI has one on its main board and one on its ROM board,
 * which hold the boards' serial numbers.
 */
#pragma once
#include "types.h"

class X76F100SerialFlash
{
public:
	void reset();
	// the whole chip, 0x84 bytes: the response to reset (4), the write
	// password (8), the read password (8) and the data (112)
	void setData(const u8 *data);
	const u8 *getProtectedData() const { return data; }
	void writeCS(bool v);
	void writeRST(bool v);
	void writeSCL(bool v);
	void writeSDA(bool v);
	bool readSDA() const { return lastCS ? true : sda; }
	// 142 bytes: the lines and the command under way, the buffer, the
	// data and the passwords
	void serialize(void **data, unsigned int *total_size);
	void unserialize(void **data, unsigned int *total_size);

private:
	enum { STOP, RESPONSE_TO_RESET, LOAD_COMMAND, LOAD_PASSWORD,
		VERIFY_PASSWORD, READ_DATA, WRITE_DATA };
	int dataOffset() const;

	u8 data[112] {};
	u8 readPassword[8] {};
	u8 writePassword[8] {};
	u8 responseToReset[4] {};
	u8 writeBuffer[8] {};
	bool lastSCL = false;
	bool lastSDA = false;
	bool sda = false;
	bool lastRST = false;
	bool lastCS = false;
	bool SCLChanged = false;
	u8 state = STOP;
	u8 command = 0;
	u8 byteOffset = 0;
	u8 bit = 0;
	u8 shift = 0;
};
