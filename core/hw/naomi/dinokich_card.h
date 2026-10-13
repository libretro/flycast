/*
	Copyright 2026 flyinghead

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
 * Dinosaur King (China), Love and Berry 3 (China): the cards of the IC card
 * reader, and the scrambling of what goes over its serial line.
 */
#pragma once
#include <stddef.h>
#include "types.h"

namespace systemsp
{

// The block at offset 0x40 of a card, decrypted: 56 bytes
#pragma pack(push, 1)
typedef struct {
    u8  serial_prefix[4];   /* 0x00 */
    u16 games_played;       /* 0x04  little-endian; one more each play */
    u16 prev_credits;       /* 0x06  little-endian; the credits before the last play */
    u8  tag[11];            /* 0x08 */
    u16 credits;            /* 0x13  little-endian; one less each play */
    u8  tail[30];           /* 0x15 */
    u8  pad[3];             /* 0x33  random */
    u8  bufout[2];          /* 0x36  the card's check value */
} dinokich_plain_t;
#pragma pack(pop)

// The game's own DES, which is not quite the standard one (dinokich_card.cpp)
void dinokich_des_encrypt(const u8 *plain, size_t len, const u8 key[8], u8 *cipher);

/* The 56 bytes encrypted with the 8 of the key, as a 64-byte block: after
 * each 8 bytes of it, as many bytes of the key as there are odd digits
 * ('1', '3'...) among them, and what is left of the key at the end. */
void dinokich_interleave(const u8 cipher_in[56], const u8 key_in[8], u8 out[64]);

// A new card's block at offset 0x40, from its contents and key
void dinokich_encode_card(const dinokich_plain_t *in, const u8 key[8], u8 card[64]);

// The sum of the bytes, each signed
int dinokich_sum_bytes(const u8 *data, size_t len);

// The game's own MD5, which is not quite the standard one
typedef struct {
    u32 state[4];
    u64 bit_count;
    u8  buffer[64];
} dinokich_md5_ctx;

void dinokich_md5_init(dinokich_md5_ctx *ctx);
void dinokich_md5_update(dinokich_md5_ctx *ctx, const u8 *data, size_t len);
void dinokich_md5_final(dinokich_md5_ctx *ctx, u8 digest[16]);
void dinokich_md5(const u8 *data, size_t len, u8 digest[16]);

/* What goes over the line is xored with the MD5 of the 2-byte salt sent
 * with it, over and over: @in to @out, @len bytes. */
void dinokich_md5_scramble(const u8 salt[2], const u8 *in, size_t len, u8 *out);

}	// namespace systemsp
