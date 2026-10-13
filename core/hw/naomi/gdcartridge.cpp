/*
 * gdcartridge.cpp
 *
 *  Created on: Nov 16, 2018
 *      Author: flyinghead
 *
 * From mame naomigd.cpp
 * // license:BSD-3-Clause
 * // copyright-holders:Olivier Galibert
 *
 */

#include "gdcartridge.h"
#include "naomi.h"
#include "naomi_periph.h"
#include "naomi_regs.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"

extern char g_base_name[128];
extern char g_parent_name[128];
extern char g_roms_dir[PATH_MAX];

/*

  GPIO pins(main board: EEPROM, DIMM SPDs, option board: PIC16, JPs)
   |
  SH4 <-> 315-6154 <-> PCI bus -> Sega 315-6322 -> Host system interface (NAOMI, Triforce, Chihiro)
   |         |                                  -> 2x DIMM RAM modules
  RAM       RAM                -> Altera (PCI IDE Bus Master Controller) -> IDE bus -> GD-ROM or CF
 16MB       4MB                -> PCnet-FAST III -> Ethernet

315-6154 - SH4 CPU to PCI bridge and SDRAM controller, also used in Sega Hikaru (2x)
315-6322 - DIMM SDRAM controller, DES decryption, host system communication

 SH4 address space
-------------------
00000000 - 001FFFFF Flash ROM (1st half - stock firmware, 2nd half - updated firmware)
04000000 - 040000FF memory/PCI bridge registers (Sega 315-6154)
0C000000 - 0CFFFFFF SH4 local RAM
10000000 - 103FFFFF memory/PCI controller RAM
14000000 - 1BFFFFFF 8x banked pages

internal / PCI memory space
-------------------
00000000 - 000000FF DIMM controller registers (Sega 315-6322)
10000000 - 4FFFFFFF DIMM memory, upto 1GB (if register 28 bit 1 is 0, otherwise some unknown MMIO)
70000000 - 70FFFFFF SH4 local RAM
78000000 - 783FFFFF 315-6154 PCI bridge RAM
C00001xx   IDE registers                 \
C00003xx   IDE registers                  | software configured in VxWorks, preconfigured or hardcoded in 1.02
C000CCxx   IDE Bus Master DMA registers  /
C1xxxxxx   Network registers

PCI configuration space (enabled using memctl 1C reg)
-------------------
00000000 - 00000FFF unknown, write 142 to reg 04 at very start
00001000 - 00001FFF PCI IDE controller (upper board Altera Flex) Vendor 11db Device 189d
00002000 - 00002FFF AMD AM79C973BVC PCnet-FAST III Network

DIMM controller registers
-------------------
14 5F703C |
18 5F7040 |
1C 5F7044 | 16bit  4x Communication registers
20 5F7048 |
24 5F704C   16bit  Interrupt register
                   -------c ---b---a
                    a - IRQ to DIMM (SH4 IRL3): 0 set / 1 clear
                    b - unk, mask of a ???
                    c - IRQ to NAOMI (HOLLY EXT 3): 0 set / 1 clear (write 0 from NAOMI seems ignored)

28          16bit  dddd---c ------ba
                    a - 0->1 NAOMI reset
                    b - 1 seems disable DIMM RAM access, followed by write 01010101 to bank 10 offset 000110 or 000190 (some MMIO?)
                    c - unk, set to 1 in VxWorks, 0 in 1.02
                    d - unk, checked for == 1 in 1.02

2A           8bit  possible DES decryption area size 8 MSB bits (16MB units number)
                   VxWorks firmwares set this to ((DIMMsize >> 24) - 1), 1.02 set it to FF

2C          32bit  SDRAM config
30          32bit  DES key low
34          32bit  DES key high

SH4 IO port A bits
-------------------
9 select input, 0 - main/lower board, 1 - option/upper board (IDE, Net, PIC)
     0             1
0 DIMM SPD clk   JP? 0 - enable IDE
1 DIMM SPD data  JP? 0 - enable Network
2 93C46 DI       PIC16 D0
3 93C46 CS       PIC16 D1
4 93C46 CLK      PIC16 D2
5 93C46 DO       PIC16 CLK



    Dimm board communication registers software level usage:

    Name:                   Naomi   Dimm Bd.
    NAOMI_DIMM_COMMAND    = 5f703c  14000014 (16 bit):
        if bits all 1 no dimm board present and other registers not used
        bit 15: during an interrupt is 1 if the dimm board has a command to be executed
        bit 14-9: 6 bit command number (naomi bios understands 0 1 3 4 5 6 8 9 a)
        bit 7-0: higher 8 bits of 24 bit offset parameter
    NAOMI_DIMM_OFFSETL    = 5f7040  14000018 (16 bit):
        bit 15-0: lower 16 bits of 24 bit offset parameter
    NAOMI_DIMM_PARAMETERL = 5f7044  1400001c (16 bit)
    NAOMI_DIMM_PARAMETERH = 5f7048  14000020 (16 bit)
    NAOMI_DIMM_STATUS     = 5f704c  14000024 (16 bit):
        bit 0: when 0 signal interrupt from naomi to dimm board
        bit 8: when 0 signal interrupt from dimm board to naomi

*/

const u32 GDCartridge::DES_LEFTSWAP[] = {
	0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
	0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101
};

const u32 GDCartridge::DES_RIGHTSWAP[] = {
	0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
	0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

const u32 GDCartridge::DES_SBOX1[] = {
	0x00808200, 0x00000000, 0x00008000, 0x00808202, 0x00808002, 0x00008202, 0x00000002, 0x00008000,
	0x00000200, 0x00808200, 0x00808202, 0x00000200, 0x00800202, 0x00808002, 0x00800000, 0x00000002,
	0x00000202, 0x00800200, 0x00800200, 0x00008200, 0x00008200, 0x00808000, 0x00808000, 0x00800202,
	0x00008002, 0x00800002, 0x00800002, 0x00008002, 0x00000000, 0x00000202, 0x00008202, 0x00800000,
	0x00008000, 0x00808202, 0x00000002, 0x00808000, 0x00808200, 0x00800000, 0x00800000, 0x00000200,
	0x00808002, 0x00008000, 0x00008200, 0x00800002, 0x00000200, 0x00000002, 0x00800202, 0x00008202,
	0x00808202, 0x00008002, 0x00808000, 0x00800202, 0x00800002, 0x00000202, 0x00008202, 0x00808200,
	0x00000202, 0x00800200, 0x00800200, 0x00000000, 0x00008002, 0x00008200, 0x00000000, 0x00808002
};

const u32 GDCartridge::DES_SBOX2[] = {
	0x40084010, 0x40004000, 0x00004000, 0x00084010, 0x00080000, 0x00000010, 0x40080010, 0x40004010,
	0x40000010, 0x40084010, 0x40084000, 0x40000000, 0x40004000, 0x00080000, 0x00000010, 0x40080010,
	0x00084000, 0x00080010, 0x40004010, 0x00000000, 0x40000000, 0x00004000, 0x00084010, 0x40080000,
	0x00080010, 0x40000010, 0x00000000, 0x00084000, 0x00004010, 0x40084000, 0x40080000, 0x00004010,
	0x00000000, 0x00084010, 0x40080010, 0x00080000, 0x40004010, 0x40080000, 0x40084000, 0x00004000,
	0x40080000, 0x40004000, 0x00000010, 0x40084010, 0x00084010, 0x00000010, 0x00004000, 0x40000000,
	0x00004010, 0x40084000, 0x00080000, 0x40000010, 0x00080010, 0x40004010, 0x40000010, 0x00080010,
	0x00084000, 0x00000000, 0x40004000, 0x00004010, 0x40000000, 0x40080010, 0x40084010, 0x00084000
};

const u32 GDCartridge::DES_SBOX3[] = {
	0x00000104, 0x04010100, 0x00000000, 0x04010004, 0x04000100, 0x00000000, 0x00010104, 0x04000100,
	0x00010004, 0x04000004, 0x04000004, 0x00010000, 0x04010104, 0x00010004, 0x04010000, 0x00000104,
	0x04000000, 0x00000004, 0x04010100, 0x00000100, 0x00010100, 0x04010000, 0x04010004, 0x00010104,
	0x04000104, 0x00010100, 0x00010000, 0x04000104, 0x00000004, 0x04010104, 0x00000100, 0x04000000,
	0x04010100, 0x04000000, 0x00010004, 0x00000104, 0x00010000, 0x04010100, 0x04000100, 0x00000000,
	0x00000100, 0x00010004, 0x04010104, 0x04000100, 0x04000004, 0x00000100, 0x00000000, 0x04010004,
	0x04000104, 0x00010000, 0x04000000, 0x04010104, 0x00000004, 0x00010104, 0x00010100, 0x04000004,
	0x04010000, 0x04000104, 0x00000104, 0x04010000, 0x00010104, 0x00000004, 0x04010004, 0x00010100
};

const u32 GDCartridge::DES_SBOX4[] = {
	0x80401000, 0x80001040, 0x80001040, 0x00000040, 0x00401040, 0x80400040, 0x80400000, 0x80001000,
	0x00000000, 0x00401000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00400040, 0x80400000,
	0x80000000, 0x00001000, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x80001000, 0x00001040,
	0x80400040, 0x80000000, 0x00001040, 0x00400040, 0x00001000, 0x00401040, 0x80401040, 0x80000040,
	0x00400040, 0x80400000, 0x00401000, 0x80401040, 0x80000040, 0x00000000, 0x00000000, 0x00401000,
	0x00001040, 0x00400040, 0x80400040, 0x80000000, 0x80401000, 0x80001040, 0x80001040, 0x00000040,
	0x80401040, 0x80000040, 0x80000000, 0x00001000, 0x80400000, 0x80001000, 0x00401040, 0x80400040,
	0x80001000, 0x00001040, 0x00400000, 0x80401000, 0x00000040, 0x00400000, 0x00001000, 0x00401040
};

const u32 GDCartridge::DES_SBOX5[] = {
	0x00000080, 0x01040080, 0x01040000, 0x21000080, 0x00040000, 0x00000080, 0x20000000, 0x01040000,
	0x20040080, 0x00040000, 0x01000080, 0x20040080, 0x21000080, 0x21040000, 0x00040080, 0x20000000,
	0x01000000, 0x20040000, 0x20040000, 0x00000000, 0x20000080, 0x21040080, 0x21040080, 0x01000080,
	0x21040000, 0x20000080, 0x00000000, 0x21000000, 0x01040080, 0x01000000, 0x21000000, 0x00040080,
	0x00040000, 0x21000080, 0x00000080, 0x01000000, 0x20000000, 0x01040000, 0x21000080, 0x20040080,
	0x01000080, 0x20000000, 0x21040000, 0x01040080, 0x20040080, 0x00000080, 0x01000000, 0x21040000,
	0x21040080, 0x00040080, 0x21000000, 0x21040080, 0x01040000, 0x00000000, 0x20040000, 0x21000000,
	0x00040080, 0x01000080, 0x20000080, 0x00040000, 0x00000000, 0x20040000, 0x01040080, 0x20000080
};

const u32 GDCartridge::DES_SBOX6[] = {
	0x10000008, 0x10200000, 0x00002000, 0x10202008, 0x10200000, 0x00000008, 0x10202008, 0x00200000,
	0x10002000, 0x00202008, 0x00200000, 0x10000008, 0x00200008, 0x10002000, 0x10000000, 0x00002008,
	0x00000000, 0x00200008, 0x10002008, 0x00002000, 0x00202000, 0x10002008, 0x00000008, 0x10200008,
	0x10200008, 0x00000000, 0x00202008, 0x10202000, 0x00002008, 0x00202000, 0x10202000, 0x10000000,
	0x10002000, 0x00000008, 0x10200008, 0x00202000, 0x10202008, 0x00200000, 0x00002008, 0x10000008,
	0x00200000, 0x10002000, 0x10000000, 0x00002008, 0x10000008, 0x10202008, 0x00202000, 0x10200000,
	0x00202008, 0x10202000, 0x00000000, 0x10200008, 0x00000008, 0x00002000, 0x10200000, 0x00202008,
	0x00002000, 0x00200008, 0x10002008, 0x00000000, 0x10202000, 0x10000000, 0x00200008, 0x10002008
};

const u32 GDCartridge::DES_SBOX7[] = {
	0x00100000, 0x02100001, 0x02000401, 0x00000000, 0x00000400, 0x02000401, 0x00100401, 0x02100400,
	0x02100401, 0x00100000, 0x00000000, 0x02000001, 0x00000001, 0x02000000, 0x02100001, 0x00000401,
	0x02000400, 0x00100401, 0x00100001, 0x02000400, 0x02000001, 0x02100000, 0x02100400, 0x00100001,
	0x02100000, 0x00000400, 0x00000401, 0x02100401, 0x00100400, 0x00000001, 0x02000000, 0x00100400,
	0x02000000, 0x00100400, 0x00100000, 0x02000401, 0x02000401, 0x02100001, 0x02100001, 0x00000001,
	0x00100001, 0x02000000, 0x02000400, 0x00100000, 0x02100400, 0x00000401, 0x00100401, 0x02100400,
	0x00000401, 0x02000001, 0x02100401, 0x02100000, 0x00100400, 0x00000000, 0x00000001, 0x02100401,
	0x00000000, 0x00100401, 0x02100000, 0x00000400, 0x02000001, 0x02000400, 0x00000400, 0x00100001
};

const u32 GDCartridge::DES_SBOX8[] = {
	0x08000820, 0x00000800, 0x00020000, 0x08020820, 0x08000000, 0x08000820, 0x00000020, 0x08000000,
	0x00020020, 0x08020000, 0x08020820, 0x00020800, 0x08020800, 0x00020820, 0x00000800, 0x00000020,
	0x08020000, 0x08000020, 0x08000800, 0x00000820, 0x00020800, 0x00020020, 0x08020020, 0x08020800,
	0x00000820, 0x00000000, 0x00000000, 0x08020020, 0x08000020, 0x08000800, 0x00020820, 0x00020000,
	0x00020820, 0x00020000, 0x08020800, 0x00000800, 0x00000020, 0x08020020, 0x00000800, 0x00020820,
	0x08000800, 0x00000020, 0x08000020, 0x08020000, 0x08020020, 0x08000000, 0x00020000, 0x08000820,
	0x00000000, 0x08020820, 0x00020020, 0x08000020, 0x08020000, 0x08000800, 0x08000820, 0x00000000,
	0x08020820, 0x00020800, 0x00020800, 0x00000820, 0x00000820, 0x00020020, 0x08000000, 0x08020800
};

const u32 GDCartridge::DES_MASK_TABLE[] = {
	0x24000000, 0x10000000, 0x08000000, 0x02080000, 0x01000000,
	0x00200000, 0x00100000, 0x00040000, 0x00020000, 0x00010000,
	0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200,
	0x00000100, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
	0x00000002, 0x00000001, 0x20000000, 0x10000000, 0x08000000,
	0x04000000, 0x02000000, 0x01000000, 0x00200000, 0x00100000,
	0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00002000,
	0x00001000, 0x00000808, 0x00000400, 0x00000200, 0x00000100,
	0x00000020, 0x00000011, 0x00000004, 0x00000002
};

const u8 GDCartridge::DES_ROTATE_TABLE[16] = {
	1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

void GDCartridge::permutate(u32 &a, u32 &b, u32 m, int shift)
{
	u32 temp = ((a>>shift) ^ b) & m;
	a ^= temp<<shift;
	b ^= temp;
}

void GDCartridge::des_generate_subkeys(const u64 key, u32 *subkeys)
{
	u32 l = key >> 32;
	u32 r = key;

	permutate(r, l, 0x0f0f0f0f, 4);
	permutate(r, l, 0x10101010, 0);

	l = (DES_LEFTSWAP[(l >> 0)  & 0xf] << 3) |
		(DES_LEFTSWAP[(l >> 8)  & 0xf] << 2) |
		(DES_LEFTSWAP[(l >> 16) & 0xf] << 1) |
		(DES_LEFTSWAP[(l >> 24) & 0xf] << 0) |
		(DES_LEFTSWAP[(l >> 5)  & 0xf] << 7) |
		(DES_LEFTSWAP[(l >> 13) & 0xf] << 6) |
		(DES_LEFTSWAP[(l >> 21) & 0xf] << 5) |
		(DES_LEFTSWAP[(l >> 29) & 0xf] << 4);

	r = (DES_RIGHTSWAP[(r >> 1)  & 0xf] << 3) |
		(DES_RIGHTSWAP[(r >> 9)  & 0xf] << 2) |
		(DES_RIGHTSWAP[(r >> 17) & 0xf] << 1) |
		(DES_RIGHTSWAP[(r >> 25) & 0xf] << 0) |
		(DES_RIGHTSWAP[(r >> 4)  & 0xf] << 7) |
		(DES_RIGHTSWAP[(r >> 12) & 0xf] << 6) |
		(DES_RIGHTSWAP[(r >> 20) & 0xf] << 5) |
		(DES_RIGHTSWAP[(r >> 28) & 0xf] << 4);

	l &= 0x0fffffff;
	r &= 0x0fffffff;


	for(int round = 0; round < 16; round++) {
		l = ((l << DES_ROTATE_TABLE[round]) | (l >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;
		r = ((r << DES_ROTATE_TABLE[round]) | (r >> (28 - DES_ROTATE_TABLE[round]))) & 0x0fffffff;

		subkeys[round*2] =
			((l << 4)  & DES_MASK_TABLE[0]) |
			((l << 28) & DES_MASK_TABLE[1]) |
			((l << 14) & DES_MASK_TABLE[2]) |
			((l << 18) & DES_MASK_TABLE[3]) |
			((l << 6)  & DES_MASK_TABLE[4]) |
			((l << 9)  & DES_MASK_TABLE[5]) |
			((l >> 1)  & DES_MASK_TABLE[6]) |
			((l << 10) & DES_MASK_TABLE[7]) |
			((l << 2)  & DES_MASK_TABLE[8]) |
			((l >> 10) & DES_MASK_TABLE[9]) |
			((r >> 13) & DES_MASK_TABLE[10])|
			((r >> 4)  & DES_MASK_TABLE[11])|
			((r << 6)  & DES_MASK_TABLE[12])|
			((r >> 1)  & DES_MASK_TABLE[13])|
			((r >> 14) & DES_MASK_TABLE[14])|
			((r >> 0)  & DES_MASK_TABLE[15])|
			((r >> 5)  & DES_MASK_TABLE[16])|
			((r >> 10) & DES_MASK_TABLE[17])|
			((r >> 3)  & DES_MASK_TABLE[18])|
			((r >> 18) & DES_MASK_TABLE[19])|
			((r >> 26) & DES_MASK_TABLE[20])|
			((r >> 24) & DES_MASK_TABLE[21]);

		subkeys[round*2+1] =
			((l << 15) & DES_MASK_TABLE[22])|
			((l << 17) & DES_MASK_TABLE[23])|
			((l << 10) & DES_MASK_TABLE[24])|
			((l << 22) & DES_MASK_TABLE[25])|
			((l >> 2)  & DES_MASK_TABLE[26])|
			((l << 1)  & DES_MASK_TABLE[27])|
			((l << 16) & DES_MASK_TABLE[28])|
			((l << 11) & DES_MASK_TABLE[29])|
			((l << 3)  & DES_MASK_TABLE[30])|
			((l >> 6)  & DES_MASK_TABLE[31])|
			((l << 15) & DES_MASK_TABLE[32])|
			((l >> 4)  & DES_MASK_TABLE[33])|
			((r >> 2)  & DES_MASK_TABLE[34])|
			((r << 8)  & DES_MASK_TABLE[35])|
			((r >> 14) & DES_MASK_TABLE[36])|
			((r >> 9)  & DES_MASK_TABLE[37])|
			((r >> 0)  & DES_MASK_TABLE[38])|
			((r << 7)  & DES_MASK_TABLE[39])|
			((r >> 7)  & DES_MASK_TABLE[40])|
			((r >> 3)  & DES_MASK_TABLE[41])|
			((r << 2)  & DES_MASK_TABLE[42])|
			((r >> 21) & DES_MASK_TABLE[43]);
	}
}

template<bool decrypt>
u64 GDCartridge::des_encrypt_decrypt(u64 src, const u32 *des_subkeys)
{
	u32 r = (src & 0x00000000ffffffffULL) >> 0;
	u32 l = (src & 0xffffffff00000000ULL) >> 32;

	permutate(l, r, 0x0f0f0f0f, 4);
	permutate(l, r, 0x0000ffff, 16);
	permutate(r, l, 0x33333333, 2);
	permutate(r, l, 0x00ff00ff, 8);
	permutate(l, r, 0x55555555, 1);

	int subkey;
	if(decrypt)
		subkey = 30;
	else
		subkey = 0;

	for(int i = 0; i < 32 ; i+=4) {
		u32 temp;

		temp = ((r<<1) | (r>>31)) ^ des_subkeys[subkey];
		l ^= DES_SBOX8[ (temp>>0)  & 0x3f ];
		l ^= DES_SBOX6[ (temp>>8)  & 0x3f ];
		l ^= DES_SBOX4[ (temp>>16) & 0x3f ];
		l ^= DES_SBOX2[ (temp>>24) & 0x3f ];
		subkey++;

		temp = ((r>>3) | (r<<29)) ^ des_subkeys[subkey];
		l ^= DES_SBOX7[ (temp>>0)  & 0x3f ];
		l ^= DES_SBOX5[ (temp>>8)  & 0x3f ];
		l ^= DES_SBOX3[ (temp>>16) & 0x3f ];
		l ^= DES_SBOX1[ (temp>>24) & 0x3f ];
		subkey++;
		if(decrypt)
			subkey -= 4;

		temp = ((l<<1) | (l>>31)) ^ des_subkeys[subkey];
		r ^= DES_SBOX8[ (temp>>0)  & 0x3f ];
		r ^= DES_SBOX6[ (temp>>8)  & 0x3f ];
		r ^= DES_SBOX4[ (temp>>16) & 0x3f ];
		r ^= DES_SBOX2[ (temp>>24) & 0x3f ];
		subkey++;

		temp = ((l>>3) | (l<<29)) ^ des_subkeys[subkey];
		r ^= DES_SBOX7[ (temp>>0)  & 0x3f ];
		r ^= DES_SBOX5[ (temp>>8)  & 0x3f ];
		r ^= DES_SBOX3[ (temp>>16) & 0x3f ];
		r ^= DES_SBOX1[ (temp>>24) & 0x3f ];
		subkey++;
		if(decrypt)
			subkey -= 4;
	}

	permutate(r, l, 0x55555555, 1);
	permutate(l, r, 0x00ff00ff, 8);
	permutate(l, r, 0x33333333, 2);
	permutate(r, l, 0x0000ffff, 16);
	permutate(r, l, 0x0f0f0f0f, 4);

	return (u64(r) << 32) | u64(l);
}

u64 GDCartridge::rev64(u64 src)
{
	u64 ret;

	ret = ((src & 0x00000000000000ffULL) << 56)
		| ((src & 0x000000000000ff00ULL) << 40)
		| ((src & 0x0000000000ff0000ULL) << 24)
		| ((src & 0x00000000ff000000ULL) << 8 )
		| ((src & 0x000000ff00000000ULL) >> 8 )
		| ((src & 0x0000ff0000000000ULL) >> 24)
		| ((src & 0x00ff000000000000ULL) >> 40)
		| ((src & 0xff00000000000000ULL) >> 56);

	return ret;
}

void GDCartridge::find_file(const char *name, const u8 *dir_sector, u32 &file_start, u32 &file_size)
{
	file_start = 0;
	file_size = 0;
	DEBUG_LOG(NAOMI, "Looking for file [%s]", name);
	for(u32 pos = 0; pos < 2048; pos += dir_sector[pos]) {
		int fnlen = 0;
		// A record starts with its length, and is whole inside the sector:
		// 33 bytes and a name. One that is not ends the list (as the
		// padding at the end of the sector does, whose length is 0).
		const u32 reclen = dir_sector[pos];
		if (reclen < 34 || reclen > 2048 - pos)
			break;
		if(!(dir_sector[pos+25] & 2)) {
			int len = dir_sector[pos+32];
//          printf("file: [%s]\n", &dir_sector[pos+33+fnlen]);
			for(fnlen=0; fnlen < FILENAME_LENGTH; fnlen++) {
				// (the name is compared up to one byte past its end, which
				// may be past the sector's: nothing follows there)
				const u8 c = pos+33+fnlen < 2048 ? dir_sector[pos+33+fnlen] : 0;
				if((c == ';') && (name[fnlen] == 0)) {
					fnlen = FILENAME_LENGTH+1;
					break;
				}
				if(c != name[fnlen])
					break;
				if(fnlen == len) {
					if(name[fnlen] == 0)
						fnlen = FILENAME_LENGTH+1;
					else
						fnlen = FILENAME_LENGTH;
				}
			}
		}
		if(fnlen == FILENAME_LENGTH+1) {
			// start sector and size of file
			file_start = ((dir_sector[pos+2] << 0) |
							(dir_sector[pos+3] << 8) |
							(dir_sector[pos+4] << 16) |
							(dir_sector[pos+5] << 24));
			file_size =  ((dir_sector[pos+10] << 0) |
							(dir_sector[pos+11] << 8) |
							(dir_sector[pos+12] << 16) |
							(dir_sector[pos+13] << 24));

			DEBUG_LOG(NAOMI, "start %08x size %08x", file_start, file_size);
			break;
		}
	}
}

void GDCartridge::read_gdrom(Disc *gdrom, u32 sector, u8* dst, u32 count)
{
	gdrom->ReadSectors(sector + 150, count, dst, 2048);
}

bool GDCartridge::device_start()
{
	if (dimm_data != NULL)
	{
		free(dimm_data);
		dimm_data = NULL;
	}
	dimm_data_size = 0;

	char name[128];
	memset(name,'\0',128);

	u64 key;
	u8 netpic = 0;

	const u8 *picdata = this->RomPtr;

	if (RomSize > 0 && gdrom_name != NULL)
	{
		if (RomSize >= 0x4000) {
			DEBUG_LOG(NAOMI, "Real PIC binary found");
			for(int i=0;i<7;i++)
				name[i] = picdata[0x7c0+i*2];
			for(int i=0;i<7;i++)
				name[i+7] = picdata[0x7e0+i*2];

			key = 0;
			for(int i=0;i<7;i++)
				key |= u64(picdata[0x780+i*2]) << (56 - i*8);

			key |= picdata[0x7a0];

			netpic = picdata[0x6ee];
		} else {
			// use extracted pic data
			//printf("This PIC key hasn't been converted to a proper PIC binary yet!\n");
			memcpy(name, picdata+33, 7);
			memcpy(name+7, picdata+25, 7);

			key =((u64(picdata[0x31]) << 56) |
					(u64(picdata[0x32]) << 48) |
					(u64(picdata[0x33]) << 40) |
					(u64(picdata[0x34]) << 32) |
					(u64(picdata[0x35]) << 24) |
					(u64(picdata[0x36]) << 16) |
					(u64(picdata[0x37]) << 8)  |
					(u64(picdata[0x29]) << 0));
		}

		DEBUG_LOG(NAOMI, "key is %08x%08x", (u32)((key & 0xffffffff00000000ULL)>>32), (u32)(key & 0x00000000ffffffffULL));

		// (as dir_sector below: nothing in it if a sector cannot be read)
		u8 buffer[2048];
		memset(buffer, 0, sizeof(buffer));
		std::string gdrom_dir;
		char *pdot = strrchr(g_base_name, '.');
		if (pdot != NULL)
		   gdrom_dir = std::string(g_base_name, pdot - g_base_name);
		else
		   gdrom_dir = g_base_name;
		std::string gdrom_path = std::string(g_roms_dir) + "/" + gdrom_dir + "/" + gdrom_name;
		Disc *gdrom = OpenDisc((gdrom_path + ".chd").c_str());
		if (gdrom == NULL)
			gdrom = OpenDisc((gdrom_path + ".gdi").c_str());
		if (gdrom == NULL && g_parent_name[0] != '\0')
			gdrom = OpenDisc((std::string(g_roms_dir) + "/" + g_parent_name + "/" + gdrom_name + ".chd").c_str());
		if (gdrom == NULL)
		{
		   // The game is on the disc and nowhere else: with nothing in the
		   // board's memory neither the BIOS nor the built-in boot has
		   // anything to start.
		   ERROR_LOG(NAOMI, "Naomi GD-ROM: cannot open the disc image %s.chd (or .gdi): the game cannot start without it", gdrom_path.c_str());
		   return false;
		}
		// primary volume descriptor
		// read frame 0xb06e (frame=sector+150)
		// dimm board firmware starts straight from this frame
		read_gdrom(gdrom, (netpic ? 0 : 45000) + 16, buffer);
		u32 path_table = ((buffer[0x8c+0] << 0) |
								(buffer[0x8c+1] << 8) |
								(buffer[0x8c+2] << 16) |
								(buffer[0x8c+3] << 24));
		// path table
		read_gdrom(gdrom, path_table, buffer);

		// directory (empty until one is read: a disc whose path table
		// has no ROM directory, or a sector that cannot be read)
		u8 dir_sector[2048];
		memset(dir_sector, 0, sizeof(dir_sector));
		// find data of file
		u32 file_start, file_size;

		if (netpic == 0) {
			u32 dir = ((buffer[0x2 + 0] << 0) |
				(buffer[0x2 + 1] << 8) |
				(buffer[0x2 + 2] << 16) |
				(buffer[0x2 + 3] << 24));

			read_gdrom(gdrom, dir, dir_sector);
			find_file(name, dir_sector, file_start, file_size);

			if (file_start && (file_size == 0x100)) {
				// read file
				read_gdrom(gdrom, file_start, buffer);
				// get "rom" file name
				memset(name, '\0', 128);
				memcpy(name, buffer + 0xc0, FILENAME_LENGTH - 1);
			}
		} else {
			u32 i = 0;
			// (an entry is 8 bytes and its name, whole inside the sector)
			while (i < 2048 && buffer[i] != 0 && 8u + buffer[i] <= 2048 - i)
			{
				if (buffer[i] == 3 && buffer[i + 8] == 'R' && buffer[i + 9] == 'O' && buffer[i + 10] == 'M')    // find ROM dir
				{
					u32 dir = ((buffer[i + 2] << 0) |
						(buffer[i + 3] << 8) |
						(buffer[i + 4] << 16) |
						(buffer[i + 5] << 24));
					memcpy(name, "ROM.BIN", 7);
					read_gdrom(gdrom, dir, dir_sector);
					break;
				}
				i += buffer[i] + 8 + (buffer[i] & 1);
			}
		}

		find_file(name, dir_sector, file_start, file_size);

		if (file_start) {
			u32 file_rounded_size = (file_size + 2047) & -2048;
			// (a size the disc gives: one that no power of two holds is not
			// a file of this board's, and would never end the loop below)
			if (file_rounded_size <= 0x80000000u)
			{
				for (dimm_data_size = 4096; dimm_data_size < file_rounded_size; dimm_data_size <<= 1)
					;
				dimm_data = (u8 *)malloc(dimm_data_size);
			}
			if (dimm_data == NULL)
			{
				ERROR_LOG(NAOMI, "Naomi GDROM: no memory for a file of %u bytes", file_size);
				dimm_data_size = 0;
				delete gdrom;
				return false;
			}
			if (dimm_data_size != file_rounded_size)
				memset(dimm_data + file_rounded_size, 0, dimm_data_size - file_rounded_size);

			// read encrypted data into dimm_data
			u32 sectors = file_rounded_size / 2048;
			read_gdrom(gdrom, file_start, dimm_data, sectors);

			// decrypt loaded data
			u32 des_subkeys[32];
			des_generate_subkeys(rev64(key), des_subkeys);

			for (u32 i = 0; i < file_rounded_size; i += 8)
				*(u64 *)(dimm_data + i) = des_encrypt_decrypt<true>(*(u64 *)(dimm_data + i), des_subkeys);
		}

		delete gdrom;

		if (!dimm_data)
		{
			ERROR_LOG(NAOMI, "Naomi GDROM: Could not find the file to decrypt on the disc image %s", gdrom_path.c_str());
			return false;
		}
	}
	return true;
}

void GDCartridge::device_reset()
{
	dimm_cur_address = 0;
}
void *GDCartridge::GetDmaPtr(u32 &size)
{
	if (dimm_data == NULL)
	{
		size = 0;
		return NULL;
	}

	dimm_cur_address = DmaOffset & (dimm_data_size-1);
	size = std::min(size, dimm_data_size - dimm_cur_address);
	return dimm_data + dimm_cur_address;
}

bool GDCartridge::Read(u32 offset, u32 size, void *dst)
{
	if (dimm_data == NULL)
	{
		*(u32 *)dst = 0;
		return true;
	}
	u32 addr = offset & (dimm_data_size-1);
	memcpy(dst, &dimm_data[addr], std::min(size, dimm_data_size - addr));
	return true;
}

std::string GDCartridge::GetGameId()
{
	if (dimm_data_size < 0x30 + 0x20)
		return "(ROM too small)";
	std::string game_id((char *)(dimm_data + 0x30), 0x20);
	while (!game_id.empty() && game_id.back() == ' ')
		game_id.pop_back();
	return game_id;
}

/* The DIMM board, as its firmware 1.02 without the network answers the
 * NAOMI: the BIOS finds a GD-ROM system, and the board puts its memory
 * size, firmware version and the ROM board's serial number where the
 * BIOS and the games look for them (a game that finds no serial number
 * there stops with error 04). The registers are reg_dimm_command,
 * reg_dimm_offsetl, reg_dimm_parameterl and reg_dimm_parameterh, which
 * the save state has always held. */

extern std::vector<sched_list> sch_list;

// the cartridge with a DIMM board that answers (at most one at a time)
static GDCartridge *dimm_cart;
static int dimm_sched_id = -1;
// bit 0: no command for the board; bit 4: always set; bit 8, cleared while
// the board's interrupt to the NAOMI is waiting
static const u32 DIMM_STATUS = 0x111;

GDCartridge::~GDCartridge()
{
	SetDimmBoard(false);
	free(dimm_data);
}

void GDCartridge::SetDimmBoard(bool on)
{
	static const u8 serial_data[0x84] = {
		// response to reset
		0x19, 0x00, 0xaa, 0x55,
		// write password, read password
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x69, 0x79, 0x68, 0x6b, 0x74, 0x6d, 0x68, 0x6d,
		// the data: the serial number is its last 16 bytes
		0xa1, 0x09, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
		' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
		'0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
	};

	if (on)
	{
		dimm_cart = this;
		dimm_sched_id = sh4_sched_register(0, &GDCartridge::dimm_sched);
		serial_id.setData(serial_data);
	}
	else if (dimm_cart == this)
	{
		sh4_sched_request(dimm_sched_id, -1);
		dimm_cart = NULL;
	}
	dimm_board = on;
	dimm_on = on;
	serial_id.reset();
}

void GDCartridge::dimm_reset()
{
	dimm_on = dimm_board;
	serial_id.reset();
	if (dimm_cart == this)
		sh4_sched_request(dimm_sched_id, -1);
}

u32 GDCartridge::ReadMem(u32 address, u32 size)
{
	if (!dimm_on)
		return NaomiCartridge::ReadMem(address, size);
	switch (address & 255)
	{
	case 0x3c:	// 5f703c: DIMM COMMAND
		return reg_dimm_command & 0xffff;
	case 0x40:	// 5f7040: DIMM OFFSETL
		return reg_dimm_offsetl & 0xffff;
	case 0x44:	// 5f7044: DIMM PARAMETERL
		return reg_dimm_parameterl & 0xffff;
	case 0x48:	// 5f7048: DIMM PARAMETERH
		return reg_dimm_parameterh & 0xffff;
	case 0x4c:	// 5f704c: DIMM STATUS
		return DIMM_STATUS & ~(((SB_ISTEXT >> 3) & 1) << 8);
	case NAOMI_BOARDID_READ_addr & 255:
		return serial_id.readSDA() ? 0x8000 : 0;
	default:
		return NaomiCartridge::ReadMem(address, size);
	}
}

void GDCartridge::WriteMem(u32 address, u32 data, u32 size)
{
	if (!dimm_on)
	{
		NaomiCartridge::WriteMem(address, data, size);
		return;
	}
	switch (address & 255)
	{
	case 0x3c:	// 5f703c: DIMM COMMAND
		reg_dimm_command = data & 0xffff;
		return;
	case 0x40:	// 5f7040: DIMM OFFSETL
		reg_dimm_offsetl = data & 0xffff;
		return;
	case 0x44:	// 5f7044: DIMM PARAMETERL
		reg_dimm_parameterl = data & 0xffff;
		return;
	case 0x48:	// 5f7048: DIMM PARAMETERH
		reg_dimm_parameterh = data & 0xffff;
		return;
	case 0x4c:	// 5f704c: DIMM STATUS
		// bit 8 set: the board's interrupt is taken (a 0 there does nothing);
		// bit 0 clear: a command for the board
		if (data & 0x100)
			asic_CancelInterrupt(holly_EXP_PCI);
		if ((data & 1) == 0)
			dimm_process();
		return;
	case NAOMI_BOARDID_WRITE_addr & 255:
		serial_id.writeCS(data & 4);
		serial_id.writeRST(data & 8);
		serial_id.writeSCL(data & 2);
		serial_id.writeSDA(data & 1);
		return;
	default:
		NaomiCartridge::WriteMem(address, data, size);
		return;
	}
}

/* A command from the NAOMI: bits 14-13 its group, 12-9 the command. */
void GDCartridge::dimm_process()
{
	const u32 group = (reg_dimm_command >> 13) & 3;
	const u32 cmd = (reg_dimm_command >> 9) & 0xf;

	DEBUG_LOG(NAOMI, "DIMM command %04x offset %04x param %04x%04x", reg_dimm_command,
			reg_dimm_offsetl, reg_dimm_parameterh, reg_dimm_parameterl);
	if (group == 0)
		dimm_system_command(cmd);
	else
	{
		// (network commands: this board has none)
		WARN_LOG(NAOMI, "DIMM command group %d command %x not handled", group, cmd);
		dimm_return(true, 0, 0xffffffff);
	}
}

void GDCartridge::dimm_return(bool failed, u16 offsetl, u32 parameter)
{
	reg_dimm_command = ((reg_dimm_command & 0x7e00) + 0x400) | (failed ? 0xff : 0x4);
	reg_dimm_offsetl = offsetl;
	reg_dimm_parameterh = parameter >> 16;
	reg_dimm_parameterl = parameter & 0xffff;
	asic_RaiseInterrupt(holly_EXP_PCI);
}

void GDCartridge::dimm_system_command(u32 cmd)
{
	switch (cmd)
	{
	case 0xf:	// start up
		{
			/* At 0c01fc04, the board's configuration: bits 16-17 the size
			 * of DIMM 0 (128, 256 or 512 MB), bit 29 set, bit 30 a GD-ROM
			 * drive, the low bits the size in MB of the memory the game
			 * takes. At 0c01fc0c the firmware version, at 0c01fc40 the
			 * serial number. */
			u32 dimm_size = 1 << 16;
			if (dimm_data_size == 0x20000000)
				dimm_size = 3 << 16;
			else if (dimm_data_size == 0x10000000)
				dimm_size = 2 << 16;
			WriteMem32_nommu(0x0c01fc04, dimm_size | 0x60000000 | (dimm_data_size >> 20));
			WriteMem32_nommu(0x0c01fc0c, 0x01020264);
			const u8 *serial = serial_id.getProtectedData() + 0x20;
			for (u32 i = 0; i < 16; i += 4)
			{
				u32 v;
				memcpy(&v, serial + i, 4);
				WriteMem32_nommu(0x0c01fc40 + i, v);
			}
			// the board asks the NAOMI to set its base address to 0c000000
			reg_dimm_command = 0x8600;
			reg_dimm_offsetl = 0;
			reg_dimm_parameterl = 0;
			reg_dimm_parameterh = 0x0c00;
			asic_RaiseInterrupt(holly_EXP_PCI);
			if (dimm_cart == this)
				sh4_sched_request(dimm_sched_id, SH4_MAIN_CLOCK);
		}
		break;

	case 0:		// nothing
	case 1:		// control read
	case 3:		// set base address
	case 4:		// peek8
	case 5:		// peek16
	case 6:		// peek32
	case 8:		// poke8
	case 9:		// poke16
	case 10:	// poke32
		// the NAOMI's answer to a command of the board's
		DEBUG_LOG(NAOMI, "DIMM: answer to command %x", cmd);
		break;

	default:
		WARN_LOG(NAOMI, "DIMM: system command %x not handled", cmd);
		break;
	}
}

int GDCartridge::dimm_sched(int tag, int cycles, int jitter)
{
	return dimm_cart != NULL ? dimm_cart->dimm_poll() : 0;
}

/* Once a second the board asks the NAOMI for the word at 0c01fc08, where the
 * BIOS's test menu leaves its requests, and answers them. */
int GDCartridge::dimm_poll()
{
	if (!dimm_on)
		return 0;
	if (SB_ISTEXT & 8)	// the last interrupt is not taken yet
		return SH4_MAIN_CLOCK;

	// peek32(0c01fc08)
	reg_dimm_command = 0x8000 | (6 << 9) | ((0x0c01fc08 >> 16) & 0x1ff);
	reg_dimm_offsetl = 0x0c01fc08 & 0xffff;
	reg_dimm_parameterl = 0;
	reg_dimm_parameterh = 0;
	asic_RaiseInterrupt(holly_EXP_PCI);

	const u32 request = ReadMem32_nommu(0x0c01fc08);
	if (request & 1)
	{
		// the DIMM test of the BIOS
		static const char lines[] = "CHECKING DIMM BD" "DIMM0 - GOOD    " "DIMM1 - GOOD    " "--- COMPLETED---";
		WriteMem32_nommu(0x0c01fc08, request & ~1);
		for (u32 i = 0; i < 64; i++)
			WriteMem8_nommu(0x0c01fd00 + i, lines[i]);
		WriteMem32_nommu(0x0c01fc0c, 0x0102a264);
	}
	else if (request != 0)
	{
		WriteMem32_nommu(0x0c01fc08, 0);
		WriteMem32_nommu(0x0c01fc0c, 0x03170100);
		INFO_LOG(NAOMI, "DIMM: test request %x", request);
	}
	return SH4_MAIN_CLOCK;
}

/* In the save state from V25, on a NAOMI with a DIMM board only (151
 * bytes): whether it answers, when it next polls, and its serial EEPROM. */
void GDCartridge::SerializeV25(void **data, unsigned int *total_size)
{
	u8 on = dimm_on;
	s32 start = sch_list[dimm_sched_id].start;
	s32 end = sch_list[dimm_sched_id].end;

	LIBRETRO_S(on);
	LIBRETRO_S(start);
	LIBRETRO_S(end);
	serial_id.serialize(data, total_size);
}

void GDCartridge::UnserializeV25(void **data, unsigned int *total_size)
{
	u8 on = 0;
	s32 start = -1, end = -1;

	LIBRETRO_US(on);
	LIBRETRO_US(start);
	LIBRETRO_US(end);
	serial_id.unserialize(data, total_size);
	dimm_on = on != 0;
	periph_sched_restore(dimm_sched_id, start, end);
}

/* A state from before: the board did not answer in it, and it goes on not
 * answering till the machine is reset. */
void GDCartridge::StateBeforeV25()
{
	dimm_on = false;
	serial_id.reset();
	sh4_sched_request(dimm_sched_id, -1);
}

void naomi_gd_serialize_v25(void **data, unsigned int *total_size)
{
	if (dimm_cart != NULL && dimm_cart == CurrentCartridge)
		dimm_cart->SerializeV25(data, total_size);
}

bool naomi_gd_unserialize_v25(void **data, unsigned int *total_size)
{
	if (dimm_cart != NULL && dimm_cart == CurrentCartridge)
		dimm_cart->UnserializeV25(data, total_size);
	return true;
}

void naomi_gd_state_before_v25(void)
{
	if (dimm_cart != NULL && dimm_cart == CurrentCartridge)
		dimm_cart->StateBeforeV25();
}

/* The X76F100. Each write to 5f7078 sets its four lines at once, chip
 * select and reset first, then the clock, then the data: a data line that
 * changes in the same write as the clock is not a start or a stop. */

void GDCartridge::X76F100::setData(const u8 *src)
{
	memcpy(responseToReset, src, sizeof(responseToReset));
	src += sizeof(responseToReset);
	memcpy(writePassword, src, sizeof(writePassword));
	src += sizeof(writePassword);
	memcpy(readPassword, src, sizeof(readPassword));
	src += sizeof(readPassword);
	memcpy(data, src, sizeof(data));
}

void GDCartridge::X76F100::reset()
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

int GDCartridge::X76F100::dataOffset() const
{
	// (blocks 14 and 15 are past the 112 bytes)
	const int offset = ((command >> 1) & 0x0f) * 8 + byteOffset;
	return offset < (int)sizeof(data) ? offset : -1;
}

void GDCartridge::X76F100::writeCS(bool v)
{
	if (lastCS != v)
		state = STOP;
	if (!lastCS && v)
		sda = false;
	lastCS = v;
}

void GDCartridge::X76F100::writeRST(bool v)
{
	if (v && !lastRST && !lastCS)
	{
		state = RESPONSE_TO_RESET;
		bit = 0;
		byteOffset = 0;
	}
	lastRST = v;
}

void GDCartridge::X76F100::writeSCL(bool v)
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

void GDCartridge::X76F100::writeSDA(bool v)
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

void GDCartridge::X76F100::serialize(void **data, unsigned int *total_size)
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

void GDCartridge::X76F100::unserialize(void **data, unsigned int *total_size)
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
