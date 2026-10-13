/*
 * gdcartridge.h
 *
 *  Created on: Nov 16, 2018
 *      Author: flyinghead
 *
 * From mame naomigd.h
 * // license:BSD-3-Clause
 * // copyright-holders:Olivier Galibert
 *
 */

#ifndef CORE_HW_NAOMI_GDCARTRIDGE_H_
#define CORE_HW_NAOMI_GDCARTRIDGE_H_

#include "naomi_cart.h"
#include "imgread/common.h"

class GDCartridge: public NaomiCartridge {
public:
	GDCartridge(u32 size) : NaomiCartridge(size)
	{
	}
	~GDCartridge();
	virtual bool Init() override
	{
		if (!device_start())
			return false;
		device_reset();
		return true;
	}
	virtual void Reset() override
	{
		NaomiCartridge::Reset();
		device_reset();
		dimm_reset();
	}
	virtual u32 ReadMem(u32 address, u32 size) override;
	virtual void WriteMem(u32 address, u32 data, u32 size) override;
	virtual void* GetDmaPtr(u32 &size) override;
	virtual bool Read(u32 offset, u32 size, void* dst) override;
	virtual std::string GetGameId() override;

	void SetGDRomName(const char *name) { this->gdrom_name = name; }
	// The board answers as a DIMM board (firmware 1.02, no network) at
	// 5f703c-5f704c, and its serial EEPROM at 5f7078/5f707c is an X76F100.
	// Without it, both answer as they do for a ROM cartridge.
	void SetDimmBoard(bool on);

	void SerializeV25(void **data, unsigned int *total_size);
	void UnserializeV25(void **data, unsigned int *total_size);
	void StateBeforeV25();

private:
	/* The X76F100 secure serial flash: 112 bytes behind a read and a write
	 * password. The password a game sends is not checked. */
	class X76F100
	{
	public:
		void reset();
		void setData(const u8 *data);
		const u8 *getProtectedData() const { return data; }
		void writeCS(bool v);
		void writeRST(bool v);
		void writeSCL(bool v);
		void writeSDA(bool v);
		bool readSDA() const { return lastCS ? true : sda; }
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

	void dimm_reset();
	void dimm_process();
	void dimm_system_command(u32 cmd);
	void dimm_return(bool failed, u16 offsetl, u32 parameter);
	static int dimm_sched(int tag, int cycles, int jitter);
	int dimm_poll();

	// the set has a DIMM board, and it answers (see StateBeforeV25())
	bool dimm_board = false;
	bool dimm_on = false;
	X76F100 serial_id;

	enum { FILENAME_LENGTH=24 };

	const char *gdrom_name = nullptr;

	u32 dimm_cur_address = 0;

	u8 *dimm_data = nullptr;
	u32 dimm_data_size = 0;

	static const u32 DES_LEFTSWAP[];
	static const u32 DES_RIGHTSWAP[];
	static const u32 DES_SBOX1[];
	static const u32 DES_SBOX2[];
	static const u32 DES_SBOX3[];
	static const u32 DES_SBOX4[];
	static const u32 DES_SBOX5[];
	static const u32 DES_SBOX6[];
	static const u32 DES_SBOX7[];
	static const u32 DES_SBOX8[];
	static const u32 DES_MASK_TABLE[];
	static const u8 DES_ROTATE_TABLE[16];

	bool device_start();
	void device_reset();
	void find_file(const char *name, const u8 *dir_sector, u32 &file_start, u32 &file_size);

	inline void permutate(u32 &a, u32 &b, u32 m, int shift);
   void des_generate_subkeys(u64 key, u32 *subkeys);
	template<bool decrypt>
	u64 des_encrypt_decrypt(u64 src, const u32 *des_subkeys);
	u64 rev64(u64 src);
	void read_gdrom(Disc *gdrom, u32 sector, u8* dst, u32 count = 1);
};

// the DIMM board's part of the save state from V25 (gdcartridge.cpp)
void naomi_gd_serialize_v25(void **data, unsigned int *total_size);
bool naomi_gd_unserialize_v25(void **data, unsigned int *total_size);
void naomi_gd_state_before_v25(void);

#endif /* CORE_HW_NAOMI_GDCARTRIDGE_H_ */
