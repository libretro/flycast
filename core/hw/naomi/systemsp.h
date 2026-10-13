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
/*
 * Sega System SP: a NAOMI main board with a ROM board of its own. The ROM
 * board answers in the G2 extension area (area 0, 0x01000000) with banked
 * windows into its address space - battery-backed SRAM, a CompactFlash
 * slot, a network ("media") board - and its I/O ports, two serial ports
 * and a serial EEPROM. The cartridge is M4 flash, which the BIOS fills from
 * the CompactFlash card for the games that come on one.
 *
 * The machine runs as a NAOMI (settings.System): naomi_cart_systemsp says
 * that it is this one.
 */
#pragma once
#include <memory>
#include <string>
#include <retro_atomic.h>
#include "types.h"
#include "m4cartridge.h"

struct chd_image;

namespace systemsp
{

template<typename T>
T readMemArea0(u32 addr);
template<typename T>
void writeMemArea0(u32 addr, T v);

class SystemSpCart;
class IOPortManager;

// What is plugged into one of the ROM board's serial ports
class SerialPipe
{
public:
	virtual ~SerialPipe() = default;
	virtual void write(u8 v) = 0;
	virtual int available() = 0;
	virtual u8 read() = 0;
	// what a power-on leaves it with (what it keeps in a file stays)
	virtual void reset() = 0;
	// at most PIPE_STATE_SIZE bytes, in the save state from V24
	virtual void serialize(void **data, unsigned int *total_size) = 0;
	virtual void unserialize(void **data, unsigned int *total_size) = 0;
};

// The ROM board's own UARTs
class SerialPort
{
public:
	SerialPort(SystemSpCart *cart, int index) : cart(cart), index(index) {}
	~SerialPort() { delete pipe; }

	u8 readReg(u32 addr);
	void writeReg(u32 addr, u8 v);
	void setPipe(SerialPipe *pipe) { this->pipe = pipe; }
	SerialPipe *getPipe() const { return pipe; }
	void updateStatus();

private:
	void flush()
	{
		if (pipe != nullptr)
			while (pipe->available())
				pipe->read();
	}

	SystemSpCart *cart;
	const int index;
	SerialPipe *pipe = nullptr;
};

// Three-wire serial EEPROM, 1 Kb (64 words of 16 bits)
class AT93C46SerialEeprom
{
public:
	AT93C46SerialEeprom() { memset(data, 0xff, sizeof(data)); }

	// DO, or READY when no data is going out
	bool readDO() const
	{
		if (dataOutBits > 0)
			return (dataOut >> (dataOutBits - 1)) & 1;
		else
			return true;
	}
	void writeCS(bool state) { cs = state; }
	void writeCLK(bool state);
	void writeDI(bool state) { di = state; }
	// power-on: the contents stay
	void reset();
	void serialize(void **data, unsigned int *total_size);
	void unserialize(void **data, unsigned int *total_size);

	u8 data[128];
	// written to since it was last saved
	bool dirty = false;

private:
	u8 commandAddress() const { return (command >> (commandBits - 9)) & 0x3f; }
	u16 commandData() const { return command & 0xffff; }

	bool cs = false;
	bool clk = false;
	bool di = false;
	// the bits of the command so far, the first one highest
	u32 command = 0;
	u8 commandBits = 0;
	u8 expected = 0;
	bool writeEnable = false;
	u16 dataOut = 0;
	u8 dataOutBits = 0;
};

class SystemSpCart : public M4Cartridge
{
public:
	/* @gameName is the set's name; @mediaName the CompactFlash image of a
	 * game that comes on one (found as <set>/<media>.chd beside the set,
	 * or <parent>/<media>.chd), NULL for one in ROM. */
	SystemSpCart(u32 size, const char *gameName, const char *mediaName, const char *parentName);
	~SystemSpCart() override;

	bool Init() override;
	void Reset() override;
	void SetKeyData(u8 *key_data) override;
	std::string GetGameId() override;

	u32 ReadMem(u32 address, u32 size) override;
	void WriteMem(u32 address, u32 data, u32 size) override;
	bool Read(u32 offset, u32 size, void *dst) override;
	bool Write(u32 offset, u32 size, u32 data) override;

	/* The region the game is started in: the one asked for, unless the
	 * game's header says it does not run there (known after Init()). */
	int GetRegion() const { return region; }
	/* The EEPROM's file for that region, or the BIOS's default EEPROM for
	 * it (naomi_default_eeprom, once the BIOS is loaded), or the board's
	 * own when there is neither. */
	void LoadEeprom();

	template<typename T>
	T readMemArea0(u32 addr);
	template<typename T>
	void writeMemArea0(u32 addr, T v);

	void updateInterrupt(u32 mask = 0);

	void serializeV24(void **data, unsigned int *total_size);
	bool unserializeV24(void **data, unsigned int *total_size);

	static constexpr u32 INT_UART1 = 0x01;
	static constexpr u32 INT_UART2 = 0x02;
	static constexpr u32 INT_DIMM = 0x08;
	static constexpr u32 INT_ATA = 0x10;
	static constexpr u32 PIPE_STATE_SIZE = 768;

	static SystemSpCart *Instance;
	static int ataSchedId;

	// the RFID chips' files, by port number
	std::string getCardPath(int index) const;
	// the touchscreen on the first port, if there is one
	SerialPipe *touchscreen = nullptr;

private:
	friend class BootIdLoader;
	friend int ataSchedCallback(int tag, int cycles, int jitter);

	bool openChd();
	bool readCf(u64 offset, u32 len, u8 *dst);
	void readSectors();
	void ataReady();
	void process();
	std::string savePath(const char *suffix) const;
	void saveEeprom();
	void saveFlash();
	void flashChanged(u32 op, u32 offset, u32 data);

	// the cartridge's cipher, for what the CompactFlash slot gives through
	// the decrypting window and for the game's header
	void encReset() { encIv = 0; encCounter = 0; }
	u16 decrypt(u16 w);

	template<typename T>
	T readNetMem(u32 addr) {
		return *(T *)&netmem[addr & (NETMEM_SIZE - sizeof(T))];
	}
	template<typename T>
	void writeNetMem(u32 addr, T data)
	{
		addr &= NETMEM_SIZE - sizeof(T);
		*(T *)&netmem[addr] = data;
		netPages[addr / NETMEM_PAGE / 64] |= 1ull << (addr / NETMEM_PAGE % 64);
	}

	std::string gameName;
	const char *mediaName;
	const char *parentName;
	chd_image *chd = nullptr;
	u64 chdSize = 0;
	u32 hunkbytes = 0;
	std::unique_ptr<u8[]> hunkmem;
	u32 hunknum = ~0u;
	// the game's header, when it is known
	std::unique_ptr<u8[]> bootId;
	int region = 0;

	AT93C46SerialEeprom eeprom;
	retro_atomic_int_t eepromFailed;
	SerialPort uart1;
	SerialPort uart2;
	std::unique_ptr<IOPortManager> ioPortManager;

	u16 bank = 0;
	u16 subkey1 = 0;
	u16 subkey2 = 0;
	u16 encIv = 0;
	u8 encCounter = 0;

	static constexpr u32 SECTOR_SIZE = 512;
	struct {
		u8 features;
		u16 cylinder;
		u8 sectorCount;
		u8 sectorNumber;
		u8 status;		// BSY RDY DF DSC DRQ CORR IDX ERR
		u8 error;
		u8 driveHead;	// 1 LBA 1 DRV HEAD(4)
		u8 devCtrl;		// HOB - - - - SRST nIEN -
		u8 interruptPending;
		u8 reg84;
		u8 buffer[SECTOR_SIZE];
		u32 bufferIndex;
	} ata;

	enum CmdState : u8 {
		INIT,
		AAA_AA_1,
		_555_55_1,
		WRITE_BUF_1,
		WRITE_BUF_2,
		PROGRAM,
		AAA_80,
		AAA_AA_2,
		_555_55_2,
	};
	struct {
		u8 cmdState;
		u32 progAddress;
		u16 wordCount;
		// how many times the flash has been programmed or erased, and what
		u32 changes;
		u32 changesHash;
	} flash;
	// the flash differs from its file
	bool flashDirty = false;

	static constexpr u16 DIMM_STATUS = 0x8102;
	static constexpr u32 NETMEM_SIZE = 8 * 1024 * 1024;
	static constexpr u32 NETMEM_PAGE = 1024;
	// the pages of the network board's memory a save state holds at most
	static constexpr u32 NETMEM_STATE_PAGES = 32;
	std::unique_ptr<u8[]> netmem;
	// which of its pages have been written to since it was cleared
	u64 netPages[NETMEM_SIZE / NETMEM_PAGE / 64];
	bool netPagesWarned = false;
	u32 inetaddr;
	u32 netmask;
	// the sockets the game has opened, 1 to 63
	u64 sockets;
	static constexpr u16 NET_OK = 0x8001;
	static constexpr u16 NET_ERROR = 0x8000;
};

}

/* State from V24: the System SP's ROM board (nothing when the machine is
 * not one). */
void systemsp_serialize_v24(void **data, unsigned int *total_size);
bool systemsp_unserialize_v24(void **data, unsigned int *total_size);
void systemsp_state_before_v24(void);
