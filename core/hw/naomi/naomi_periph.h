/*
	The devices an arcade game has besides its I/O board: card readers,
	touch panel, hopper (naomi.cpp, card_reader.cpp, touchscreen.cpp,
	hopper.cpp, and the IC card reader in maple_devs.cpp).
*/
#pragma once
#include "types.h"
#include "hw/serial_port.h"
#include <retro_atomic.h>

#include <algorithm>

/* The bytes a device has for the game, oldest first. A device that is
 * sent more than this without the game reading drops the rest. */
template<u32 N>
struct PeriphQueue
{
	u8 buf[N];
	u32 head = 0;
	u32 count = 0;

	bool empty() const { return count == 0; }
	u32 size() const { return count; }
	void clear() { head = count = 0; }
	void push(u8 b)
	{
		if (count == N)
			return;
		buf[(head + count) % N] = b;
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
		const u8 b = buf[head];
		head = (head + 1) % N;
		count--;
		return b;
	}
	// oldest first, and zeroes after: the same bytes however far round the ring they are
	void serialize(void **data, unsigned int *total_size) const
	{
		u8 lin[N];
		for (u32 i = 0; i < N; i++)
			lin[i] = i < count ? buf[(head + i) % N] : 0;
		u16 c = (u16)count;
		LIBRETRO_S(c);
		LIBRETRO_SA(lin, N);
	}
	void unserialize(void **data, unsigned int *total_size)
	{
		u16 c = 0;
		LIBRETRO_US(c);
		LIBRETRO_USA(buf, N);
		head = 0;
		count = std::min<u32>(c, N);
	}
};

// The ROM set's name ("clubkrt"), or "" for a game that is not a set
extern char naomi_set_name[];
// Core option: the card readers and the barcode reader are plugged in
extern bool naomi_card_readers_enabled;

/* The game's devices made, for the game that is loaded: on a hard reset,
 * before the maple devices are (naomi.cpp). */
void naomi_periph_init(void);
void naomi_periph_term(void);
// What a card reader game's I/O board bridge does once each time it is asked
void naomi_periph_poll(void);
// Whether a game has a card to put in (the "Insert Card" button)
bool naomi_periph_has_card_reader(void);
// Whether the game's maple bus has the IC card readers
bool naomi_periph_rfid_game(void);

/* A file next to the game's EEPROM in the save directory, named by the
 * game and @suffix. Reading returns how many bytes there were (the rest
 * of @buf is zeroes), 0 when there is no such file. */
size_t periph_load_file(const char *suffix, void *buf, size_t size);
// the same, made anew from @buf; the writing is done by the save writer
void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed);

/* An event of the scheduler as a save state has it. One that would come
 * later than any device asks for, or whose time has gone by, is due now. */
void periph_sched_restore(int id, s32 start, s32 end);

// The buttons a player holds (active low) and where they point (maple_cfg.cpp)
u32 mcfg_ArcadeKcode(int port);
void mcfg_ArcadeAbsPos(int port, f32 *x, f32 *y);

// The IC card readers (maple_devs.cpp)
void rfid_init(bool game, bool insert);
void insertRfidCard(int playerNum);
const u8 *getRfidCardData(int playerNum);
void rfid_serialize_v24(void **data, unsigned int *total_size);
bool rfid_unserialize_v24(void **data, unsigned int *total_size);
void rfid_state_before_v24(void);

// The RS-422 port of the I/O board bridge, which a card reader can be on (maple_devs.cpp)
void naomi_mie_set_pipe(SerialPort::Pipe *pipe);

/* What the devices have in a save state since V24 (naomi.cpp) */
void naomi_periph_serialize_v24(void **data, unsigned int *total_size);
bool naomi_periph_unserialize_v24(void **data, unsigned int *total_size);
void naomi_periph_state_before_v24(void);
