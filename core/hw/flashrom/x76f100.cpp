/*
 * x76f100.cpp
 *
 * From mame x76f100.cpp
 * // license:BSD-3-Clause
 * // copyright-holders:smf
 */
#include "x76f100.h"
#include <string.h>
#include <algorithm>

/* Each write to the port sets the chip's four lines at once, chip select
 * and reset first, then the clock, then the data: a data line that
 * changes in the same write as the clock is not a start or a stop. The
 * password a game sends is not checked. */

void X76F100SerialFlash::setData(const u8 *src)
{
	memcpy(responseToReset, src, sizeof(responseToReset));
	src += sizeof(responseToReset);
	memcpy(writePassword, src, sizeof(writePassword));
	src += sizeof(writePassword);
	memcpy(readPassword, src, sizeof(readPassword));
	src += sizeof(readPassword);
	memcpy(data, src, sizeof(data));
}

void X76F100SerialFlash::reset()
{
	lastSCL = false;
	SCLChanged = false;
	lastSDA = false;
	sda = false;
	lastRST = false;
	lastCS = false;
	state = STOP;
	bit = 0;
	byteOffset = 0;
	command = 0;
	shift = 0;
	memset(writeBuffer, 0, sizeof(writeBuffer));
}

int X76F100SerialFlash::dataOffset() const
{
	// (blocks 14 and 15 are past the 112 bytes)
	const int offset = ((command >> 1) & 0x0f) * 8 + byteOffset;
	return offset < (int)sizeof(data) ? offset : -1;
}

void X76F100SerialFlash::writeCS(bool v)
{
	if (lastCS != v)
		state = STOP;
	if (!lastCS && v)
		sda = false;
	lastCS = v;
}

void X76F100SerialFlash::writeRST(bool v)
{
	if (v && !lastRST && !lastCS)
	{
		state = RESPONSE_TO_RESET;
		bit = 0;
		byteOffset = 0;
	}
	lastRST = v;
}

void X76F100SerialFlash::writeSCL(bool v)
{
	if (!lastCS)
	{
		switch (state)
		{
		case RESPONSE_TO_RESET:
			if (lastSCL && !v)
			{
				if (bit == 0)
					shift = responseToReset[byteOffset];
				sda = shift & 1;
				shift >>= 1;
				if (++bit == 8)
				{
					bit = 0;
					if (++byteOffset == sizeof(responseToReset))
						byteOffset = 0;
				}
			}
			break;

		case LOAD_COMMAND:
		case LOAD_PASSWORD:
		case VERIFY_PASSWORD:
		case WRITE_DATA:
			if (!lastSCL && v)
			{
				if (bit < 8)
				{
					shift = (shift << 1) | (lastSDA ? 1 : 0);
					bit++;
					break;
				}
				// the ninth clock: the byte is in
				sda = false;
				switch (state)
				{
				case LOAD_COMMAND:
					command = shift;
					state = LOAD_PASSWORD;
					break;

				case LOAD_PASSWORD:
					writeBuffer[byteOffset++] = shift;
					if (byteOffset == sizeof(writeBuffer))
						state = VERIFY_PASSWORD;
					break;

				case VERIFY_PASSWORD:
					// the password acknowledge poll: any password is taken
					if (shift == 0x55)
					{
						if ((command & 0x81) == 0x81)
							state = READ_DATA;
						else if ((command & 0x81) == 0x80)
						{
							state = WRITE_DATA;
							// (the eight bytes of the password are not data)
							byteOffset = 0;
						}
					}
					break;

				case WRITE_DATA:
					writeBuffer[byteOffset++] = shift;
					if (byteOffset == sizeof(writeBuffer))
					{
						if (command == 0xfc)
							memcpy(writePassword, writeBuffer, sizeof(writePassword));
						else if (command == 0xfe)
							memcpy(readPassword, writeBuffer, sizeof(readPassword));
						else
						{
							for (byteOffset = 0; byteOffset < sizeof(writeBuffer); byteOffset++)
							{
								const int offset = dataOffset();
								if (offset == -1)
									break;
								data[offset] = writeBuffer[byteOffset];
							}
						}
						byteOffset = 0;
					}
					break;
				}
				bit = 0;
				shift = 0;
			}
			break;

		case READ_DATA:
			if (!lastSCL && v)
			{
				if (bit < 8)
				{
					if (bit == 0)
					{
						const int offset = dataOffset();
						shift = offset != -1 ? data[offset] : 0;
					}
					sda = (shift >> 7) & 1;
					shift <<= 1;
					bit++;
				}
				else
				{
					// the ninth clock: the next byte if the NAOMI acknowledges
					bit = 0;
					sda = false;
					if (!lastSDA)
						byteOffset++;
				}
			}
			break;

		default:
			break;
		}
	}
	SCLChanged = lastSCL != v;
	lastSCL = v;
}

void X76F100SerialFlash::writeSDA(bool v)
{
	if (lastSCL && !SCLChanged && !lastCS)
	{
		if (!lastSDA && v)
		{
			// stop
			state = STOP;
			sda = false;
		}
		if (lastSDA && !v)
		{
			// start
			if (state == STOP)
				state = LOAD_COMMAND;
			bit = 0;
			byteOffset = 0;
			shift = 0;
			sda = false;
		}
	}
	lastSDA = v;
}

void X76F100SerialFlash::serialize(void **data, unsigned int *total_size)
{
	u8 regs[6];

	regs[0] = lastSCL | (SCLChanged << 1) | (lastSDA << 2) | (sda << 3) | (lastRST << 4) | (lastCS << 5);
	regs[1] = state;
	regs[2] = command;
	regs[3] = byteOffset;
	regs[4] = bit;
	regs[5] = shift;
	LIBRETRO_SA(regs, 6);
	LIBRETRO_SA(writeBuffer, 8);
	LIBRETRO_SA(this->data, 112);
	LIBRETRO_SA(readPassword, 8);
	LIBRETRO_SA(writePassword, 8);
}

void X76F100SerialFlash::unserialize(void **data, unsigned int *total_size)
{
	u8 regs[6] = { 0 };

	LIBRETRO_USA(regs, 6);
	LIBRETRO_USA(writeBuffer, 8);
	LIBRETRO_USA(this->data, 112);
	LIBRETRO_USA(readPassword, 8);
	LIBRETRO_USA(writePassword, 8);
	lastSCL = regs[0] & 1;
	SCLChanged = (regs[0] >> 1) & 1;
	lastSDA = (regs[0] >> 2) & 1;
	sda = (regs[0] >> 3) & 1;
	lastRST = (regs[0] >> 4) & 1;
	lastCS = (regs[0] >> 5) & 1;
	state = regs[1] <= WRITE_DATA ? regs[1] : (u8)STOP;
	command = regs[2];
	byteOffset = regs[3];
	bit = std::min<u8>(regs[4], 8);
	shift = regs[5];
	// (where a byte goes into a buffer, its place is inside it)
	if ((state == RESPONSE_TO_RESET && byteOffset >= sizeof(responseToReset))
			|| ((state == LOAD_COMMAND || state == LOAD_PASSWORD || state == WRITE_DATA)
				&& byteOffset >= sizeof(writeBuffer)))
		byteOffset = 0;
	if (state == RESPONSE_TO_RESET && bit >= 8)
		bit = 0;
}
