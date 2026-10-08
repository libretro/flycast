/*
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
// Naomi comm board emulation from mame
// https://github.com/mamedev/mame/blob/master/src/mame/machine/m3comm.cpp
// license:BSD-3-Clause
// copyright-holders:MetalliC

#include <memory>
#include <string>
#include <streams/file_stream.h>
#include "naomi_cart.h"
#include "naomi_regs.h"
#include "naomi.h"
#include "decrypt.h"
#include "naomi_roms.h"
#include "hw/flashrom/flashrom.h"
#include "hw/holly/holly_intc.h"
#include "m1cartridge.h"
#include "m4cartridge.h"
#include "awcartridge.h"
#include "gdcartridge.h"
#include "archive/archive.h"
#include "file/file_path.h"
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

Cartridge *CurrentCartridge;
bool bios_loaded = false;

#ifdef _WIN32
#include <windows.h>
typedef HANDLE fd_t;
#define INVALID_FD INVALID_HANDLE_VALUE
#ifndef FILE_READ_ACCESS
#define FILE_READ_ACCESS        0x0001
#endif

#ifndef FILE_WRITE_ACCESS
#define FILE_WRITE_ACCESS       0x0002
#endif
#else
typedef int fd_t;
#define INVALID_FD -1

#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#endif
#include <unistd.h>

fd_t *RomCacheMap;
u32 RomCacheMapCount;

char naomi_game_id[33];
char g_parent_name[128];

extern RomChip sys_rom;
extern DCFlashChip sys_nvmem_flash;		// AtomisWave BIOS is loaded there

extern char game_dir_no_slash[1024];
extern char g_roms_dir[PATH_MAX];

InputDescriptors *naomi_game_inputs;
u8 *naomi_default_eeprom;
static RotationType game_rotation = ROT0;

/* Where the blob is: the one with CRC @crc in the first of @archives
 * that has one, else the one named @filename in the first that has it.
 * Its index, with its archive in @in; -1 if there is none. */
static int naomi_find_member(archive_t *const *archives, int count,
      u32 crc, const char *filename, archive_t **in)
{
   int i;
   int idx;

   for (i = 0; i < count; i++)
   {
      if (archives[i] && (idx = archive_find_crc(archives[i], crc)) >= 0)
      {
         *in = archives[i];
         return idx;
      }
   }
   for (i = 0; i < count; i++)
   {
      if (archives[i] && (idx = archive_find(archives[i], filename)) >= 0)
      {
         *in = archives[i];
         return idx;
      }
   }
   return -1;
}

/* Which game of the table the romset at @path is; -1 if none.
 *
 * By its name, when that is a set's name: the MAME short name the table
 * goes by. A set is not always called that - named after the game by its
 * owner, or by another collection's rules - so otherwise by what is in it:
 * the game most of whose ROMs the archive has, by checksum where the table
 * has one and by file name where it has not. A set that has under half of
 * a game's ROMs has to have its first one, the program ROM, which is what
 * tells a game from the others that share its data ROMs. */
static std::string found_path;
static int found_game = -1;

static int naomi_find_game(const char *path)
{
	char game_name[128];
	int gameid;

	if (found_path == path)
		return found_game;
	strncpy(game_name, path_basename(path), sizeof(game_name) - 1);
	game_name[sizeof(game_name) - 1] = '\0';
	path_remove_extension(game_name);

	for (gameid = 0; Games[gameid].name != NULL; gameid++)
		if (!stricmp(Games[gameid].name, game_name))
			break;
	if (Games[gameid].name == NULL)
	{
		archive_t *archive = archive_open(path);
		int best = -1, best_found = 0;

		gameid = -1;
		if (archive != NULL)
		{
			for (int g = 0; Games[g].name != NULL; g++)
			{
				int files = 0, found = 0;
				bool first = false;

				for (int romid = 0; Games[g].blobs[romid].filename != NULL; romid++)
				{
					const u32 crc = Games[g].blobs[romid].crc;

					if (Games[g].blobs[romid].blob_type == Copy)
						continue;
					files++;
					if (crc != 0 ? archive_find_crc(archive, crc) >= 0
							: archive_find(archive, Games[g].blobs[romid].filename) >= 0)
					{
						found++;
						if (romid == 0)
							first = true;
					}
				}
				if (found > best_found && (first || found * 2 >= files))
				{
					best = g;
					best_found = found;
				}
			}
			archive_close(archive);
		}
		gameid = best;
		if (gameid >= 0)
			NOTICE_LOG(NAOMI, "%s is not a set's name: by its %d ROMs it is %s (%s)", game_name, best_found,
					Games[gameid].name, Games[gameid].description);
	}
	found_path = path;
	found_game = gameid;
	return gameid;
}

/* That blob's bytes, which belong to its archive. */
static const u8 *naomi_find_blob(archive_t *const *archives, int count,
      u32 crc, const char *filename, size_t *len)
{
   archive_t *in = NULL;
   int idx = naomi_find_member(archives, count, crc, filename, &in);

   return idx >= 0 ? archive_entry_data(in, (unsigned)idx, len) : NULL;
}

/* The 16-bit words of @blob (@len bytes of them) to every other word from
 * @to on: one of the two ROMs that share an address range, a word each in
 * turn. The words in between are the other ROM's and are left as they
 * are. */
static void naomi_copy_interleaved(u16 *to, const u8 *blob, u32 len)
{
   const u16 *from = (const u16 *)blob;
   int i = len / 2;

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
   /* eight words at a time: spread out with nothing between them, and
    * put over what is there with the other ROM's words kept */
   {
      const __m128i zero  = _mm_setzero_si128();
      const __m128i other = _mm_set1_epi32((int)0xFFFF0000u);

      for (; i >= 8; i -= 8, from += 8, to += 16)
      {
         const __m128i words = _mm_loadu_si128((const __m128i *)from);
         const __m128i low   = _mm_unpacklo_epi16(words, zero);
         const __m128i high  = _mm_unpackhi_epi16(words, zero);
         __m128i a = _mm_loadu_si128((const __m128i *)to);
         __m128i b = _mm_loadu_si128((const __m128i *)(to + 8));

         a = _mm_or_si128(_mm_and_si128(a, other), low);
         b = _mm_or_si128(_mm_and_si128(b, other), high);
         _mm_storeu_si128((__m128i *)to, a);
         _mm_storeu_si128((__m128i *)(to + 8), b);
      }
   }
#endif
   for (; --i >= 0; to++)
      *to++ = *from++;
}

static bool naomi_LoadBios(const char *filename, archive_t *child_archive, archive_t *parent_archive, int region)
{
	int biosid = 0;
	for (; BIOS[biosid].name != NULL; biosid++)
		if (!stricmp(BIOS[biosid].name, filename))
			break;
	if (BIOS[biosid].name == NULL)
	{
		WARN_LOG(NAOMI, "Unknown BIOS %s", filename);
		return false;
	}

	MemChip *rom_chip;
	if (SYSTEM_IS_NAOMI())
	   rom_chip = &sys_rom;
	else
	   rom_chip = &sys_nvmem_flash;

	struct BIOS_t *bios = &BIOS[biosid];

	std::string basepath(game_dir_no_slash);
	basepath += PATH_DEFAULT_SLASH();

	archive_t *bios_archive = archive_open((basepath + filename).c_str());
	archive_t *archives[3];

	archives[0] = child_archive;
	archives[1] = parent_archive;
	archives[2] = bios_archive;

	bool found_region = false;

	for (int romid = 0; bios->blobs[romid].filename != NULL; romid++)
	{
	   if (region == -1)
		  region = bios->blobs[romid].region;
	   else
	   {
		  if (bios->blobs[romid].region != region)
			 continue;
	   }
	   found_region = true;

	    if (bios->blobs[romid].blob_type == Copy)
		{
			verify(bios->blobs[romid].offset + bios->blobs[romid].length <= rom_chip->size);
			verify(bios->blobs[romid].src_offset + bios->blobs[romid].length <= rom_chip->size);
			memcpy(rom_chip->data + bios->blobs[romid].offset, rom_chip->data + bios->blobs[romid].src_offset, bios->blobs[romid].length);
		}
		else
		{
			size_t blob_len = 0;
			const u8 *blob = naomi_find_blob(archives, 3, bios->blobs[romid].crc,
					bios->blobs[romid].filename, &blob_len);
			if (!blob) {
				WARN_LOG(NAOMI, "%s: Cannot open %s", filename, bios->blobs[romid].filename);
				goto error;
			}
			u32 read = bios->blobs[romid].length;
			if (read > blob_len)
				read = (u32)blob_len;
			verify(bios->blobs[romid].offset + bios->blobs[romid].length <= rom_chip->size);
			if (bios->blobs[romid].blob_type == Normal)
				memcpy(rom_chip->data + bios->blobs[romid].offset, blob, read);
			else if (bios->blobs[romid].blob_type == InterleavedWord)
				naomi_copy_interleaved((u16 *)(rom_chip->data + bios->blobs[romid].offset), blob, read);
			else
				die("Unknown blob type");
		}
	}

	archive_close(bios_archive);

	if (settings.System == DC_PLATFORM_ATOMISWAVE)
	   // Reload the writeable portion of the FlashROM
	   sys_nvmem_flash.Reload();

	return found_region;

error:
	archive_close(bios_archive);
	return false;
}

static bool naomi_cart_LoadZip(const char *filename)
{
	u8 *scratch = NULL;			// for the ROMs that are spread out: see below
	size_t scratch_size = 0;

	const int gameid = naomi_find_game(filename);
	if (gameid < 0)
	{
		WARN_LOG(NAOMI, "Unknown game %s", path_basename(filename));
		return false;
	}

	struct Game *game = &Games[gameid];

	archive_t *archive = archive_open(filename);
	if (archive != NULL)
		INFO_LOG(NAOMI, "Opened %s", filename);

	archive_t *parent_archive = NULL;
	if (game->parent_name != NULL)
	{
	   strncpy(g_parent_name, game->parent_name, sizeof(g_parent_name));
	   std::string parent_path(g_roms_dir);
	   parent_path += PATH_DEFAULT_SLASH();
	   parent_path += game->parent_name;
	   parent_archive = archive_open(parent_path.c_str());
	   if (parent_archive != NULL)
		  INFO_LOG(NAOMI, "Opened %s", game->parent_name);
	}

	if (archive == NULL && parent_archive == NULL)
	{
		if (game->parent_name != NULL)
			WARN_LOG(NAOMI, "Cannot open %s or %s", filename, game->parent_name);
		else
			WARN_LOG(NAOMI, "Cannot open %s", filename);
		return false;
	}

	const char *bios = "naomi";
	if (game->bios != NULL)
	   bios = game->bios;
	u32 region_flag = settings.dreamcast.region;
	if (region_flag > game->region_flag)
	   region_flag = game->region_flag;
	if (game->region_flag == REGION_EXPORT_ONLY)
	   region_flag = REGION_EXPORT;
	/* A NAOMI or NAOMI 2 game can be started without its BIOS (reios_boot()
	 * does what the BIOS does to hand over to it), so the BIOS is used if it
	 * is there and "Use Real BIOS (If Available)" is on, and is not required.
	 * An Atomiswave game cannot: that BIOS has to be found. */
	if (game->cart_type != AW && !settings.bios.UseRealBios)
	{
	   bios_loaded = false;
	   NOTICE_LOG(NAOMI, "The BIOS is not to be used: the game is started without one");
	}
	else if (naomi_LoadBios(bios, archive, parent_archive, region_flag))
	   bios_loaded = true;
	else
	{
	   WARN_LOG(NAOMI, "Warning: Region %d bios not found in %s", settings.dreamcast.region, bios);
	   if (naomi_LoadBios(bios, archive, parent_archive, -1))
		  bios_loaded = true;
	   else if (game->cart_type == AW)
	   {
		  if (!bios_loaded)
		  {
			 ERROR_LOG(NAOMI, "Error: cannot load BIOS. Exiting");
			 goto error;
		  }
		  // otherwise use the default BIOS
	   }
	   else if (!bios_loaded)
		  NOTICE_LOG(NAOMI, "No BIOS in %s.zip or %s.7z: the game is started without one", bios, bios);
	}

	switch (game->cart_type)
	{
	case M1:
		CurrentCartridge = new M1Cartridge(game->size);
		break;
	case M2:
		CurrentCartridge = new M2Cartridge(game->size);
		break;
	case M4:
		CurrentCartridge = new M4Cartridge(game->size);
		break;
	case AW:
		CurrentCartridge = new AWCartridge(game->size);
		break;
	case GD:
	    {
	       GDCartridge *gdcart = new GDCartridge(game->size);
	       gdcart->SetGDRomName(game->gdrom_name);
	       CurrentCartridge = gdcart;
	    }
	    break;
	default:
		die("Unsupported cartridge type");
		break;
	}
	CurrentCartridge->SetKey(game->key);
	naomi_game_inputs = game->inputs;

	for (int romid = 0; game->blobs[romid].filename != NULL; romid++)
	{
		u32 len = game->blobs[romid].length;

		if (game->blobs[romid].blob_type == Copy)
		{
			u8 *dst = (u8 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);
			u8 *src = (u8 *)CurrentCartridge->GetPtr(game->blobs[romid].src_offset, len);
			memcpy(dst, src, game->blobs[romid].length);
			DEBUG_LOG(NAOMI, "Copied: %x bytes from %07x to %07x", game->blobs[romid].length, game->blobs[romid].src_offset, game->blobs[romid].offset);
		}
		else
		{
			archive_t *archives[2];
			size_t blob_len = 0;
			archives[0] = archive;
			archives[1] = parent_archive;
			if (game->blobs[romid].blob_type == Normal)
			{
				/* A ROM of the cartridge: decoded straight into the
				 * cartridge's memory when it is no longer than its place
				 * there. It used to be decoded into a buffer of the
				 * archive's - kept, with every other ROM's, until the set
				 * was loaded: the whole set twice over in memory - and
				 * copied across. */
				archive_t *in = NULL;
				const int idx = naomi_find_member(archives, 2, game->blobs[romid].crc,
						game->blobs[romid].filename, &in);
				const archive_entry_t *entry = idx >= 0 ? archive_entry(in, (unsigned)idx) : NULL;

				if (entry && entry->usable && entry->size <= len)
				{
					u8 *dst = (u8 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);

					if (!archive_entry_read(in, (unsigned)idx, dst, len, &blob_len))
					{
						WARN_LOG(NAOMI, "%s: Cannot open %s", filename, game->blobs[romid].filename);
						goto error;
					}
					DEBUG_LOG(NAOMI, "Mapped %s: %x bytes at %07x", game->blobs[romid].filename, (u32)blob_len, game->blobs[romid].offset);
					continue;
				}
			}
			if (game->blobs[romid].blob_type == InterleavedWord)
			{
				/* One of two ROMs that share a range: decoded into a buffer
				 * that the next such ROM uses again - not one the archive
				 * keeps for each until the set is loaded - and spread from
				 * there. */
				archive_t *in = NULL;
				const int idx = naomi_find_member(archives, 2, game->blobs[romid].crc,
						game->blobs[romid].filename, &in);
				const archive_entry_t *entry = idx >= 0 ? archive_entry(in, (unsigned)idx) : NULL;

				if (entry && entry->usable && entry->size <= len)
				{
					if (len > scratch_size)
					{
						free(scratch);
						scratch = (u8 *)malloc(len);
						scratch_size = scratch ? len : 0;
					}
					if (!scratch || !archive_entry_read(in, (unsigned)idx, scratch, len, &blob_len))
					{
						WARN_LOG(NAOMI, "%s: Cannot open %s", filename, game->blobs[romid].filename);
						goto error;
					}
					u16 *to = (u16 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);
					naomi_copy_interleaved(to, scratch, (u32)blob_len);
					DEBUG_LOG(NAOMI, "Mapped %s: %x bytes (interleaved word) at %07x", game->blobs[romid].filename, (u32)blob_len, game->blobs[romid].offset);
					continue;
				}
			}
			const u8 *blob = naomi_find_blob(archives, 2, game->blobs[romid].crc,
					game->blobs[romid].filename, &blob_len);
			if (!blob) {
				WARN_LOG(NAOMI, "%s: Cannot open %s", filename, game->blobs[romid].filename);
				if (game->blobs[romid].blob_type != Eeprom)
				   // Default eeprom file is optional
				   goto error;
				else
				   continue;
			}
			u32 read = len;
			if (read > blob_len)
				read = (u32)blob_len;
			if (game->blobs[romid].blob_type == Normal)
			{
				u8 *dst = (u8 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);
				memcpy(dst, blob, read);
				DEBUG_LOG(NAOMI, "Mapped %s: %x bytes at %07x", game->blobs[romid].filename, read, game->blobs[romid].offset);
			}
			else if (game->blobs[romid].blob_type == InterleavedWord)
			{
				u16 *to = (u16 *)CurrentCartridge->GetPtr(game->blobs[romid].offset, len);
				naomi_copy_interleaved(to, blob, read);
				DEBUG_LOG(NAOMI, "Mapped %s: %x bytes (interleaved word) at %07x", game->blobs[romid].filename, read, game->blobs[romid].offset);
			}
			else if (game->blobs[romid].blob_type == Key)
			{
				u8 *buf = (u8 *)malloc(len);
				if (buf == NULL)
				{
					ERROR_LOG(NAOMI, "malloc failed");
					goto error;
				}
				memcpy(buf, blob, read);
				CurrentCartridge->SetKeyData(buf);
				DEBUG_LOG(NAOMI, "Loaded %s: %x bytes cart key", game->blobs[romid].filename, read);
			}
			else if (game->blobs[romid].blob_type == Eeprom)
			{
			    naomi_default_eeprom = (u8 *)malloc(len);
			    if (naomi_default_eeprom == NULL)
			    {
					ERROR_LOG(NAOMI, "malloc failed");
			       goto error;
			    }
				memcpy(naomi_default_eeprom, blob, read);
				DEBUG_LOG(NAOMI, "Loaded %s: %x bytes default eeprom", game->blobs[romid].filename, read);
			}
			else
				die("Unknown blob type");
		}
	}
	if (naomi_default_eeprom == NULL && game->eeprom_dump != NULL)
		naomi_default_eeprom = game->eeprom_dump;
	game_rotation = game->rotation_flag;
	free(scratch);
	archive_close(archive);
	archive_close(parent_archive);

	CurrentCartridge->Init();

	strcpy(naomi_game_id, CurrentCartridge->GetGameId().c_str());
	if (naomi_game_id[0] == '\0')
		strcpy(naomi_game_id, game->name);
	NOTICE_LOG(NAOMI, "NAOMI GAME ID [%s]", naomi_game_id);

	return true;

error:
	free(scratch);
	archive_close(archive);
	archive_close(parent_archive);
	delete CurrentCartridge;
	CurrentCartridge = NULL;
	return false;
}

/* An archive that is no romset may hold a flat image, the kind a .bin
 * file is: one member named .bin or .dat, whose first bytes say which
 * board it is for. It is found when the archive is looked at to tell the
 * machine, and kept until the loader takes it - 180 MB of RAR take
 * seconds to decode, and are decoded once. */
static archive_t *flat_archive;
static const u8 *flat_data;
static size_t flat_len;
static std::string flat_path;

static void naomi_FlatArchiveDrop()
{
	archive_close(flat_archive);
	flat_archive = NULL;
	flat_data = NULL;
	flat_len = 0;
	flat_path.clear();
}

static bool naomi_FlatArchiveOpen(const char *path)
{
	if (flat_archive && flat_path == path)
		return true;
	naomi_FlatArchiveDrop();

	archive_t *a = archive_open(path);
	if (!a)
		return false;
	int found = -1;
	for (unsigned i = 0; i < archive_num_entries(a); i++)
	{
		const archive_entry_t *e = archive_entry(a, i);
		const char *ext = strrchr(e->name, '.');
		if (!e->usable || !ext || (strcasecmp(ext, ".bin") && strcasecmp(ext, ".dat")))
			continue;
		if (found >= 0)
		{
			// more than one: not an image by itself
			found = -1;
			break;
		}
		found = (int)i;
	}
	size_t len = 0;
	const u8 *data = found >= 0 ? archive_entry_data(a, found, &len) : NULL;
	if (!data || len < 0x500 || len > 0xFFFFFFFFu
			|| (memcmp(data, "NAOMI", 5) && memcmp(data, "Naomi2", 6)))
	{
		archive_close(a);
		return false;
	}
	flat_archive = a;
	flat_data = data;
	flat_len = len;
	flat_path = path;
	return true;
}

static bool naomi_IsArchive(const char *ext)
{
	return !strcasecmp(ext, "zip") || !strcasecmp(ext, "7z") || !strcasecmp(ext, "rar");
}

/* The region of the BIOS for a flat image: the one asked for if the image
 * runs there, or else the first it does run in. Its header says where
 * (0x428: Japan 1, USA 2, export 4, Korea 8), and a board of any other
 * region answers "this game is not acceptable by main board". */
static int naomi_FlatRegion(const u8 *rom, size_t size)
{
	int region = settings.dreamcast.region;
	if (size > 0x428 && region >= 0 && region < 4 && !(rom[0x428] & (1 << region)))
		for (int r = 0; r < 4; r++)
			if (rom[0x428] & (1 << r))
			{
				region = r;
				break;
			}
	return region;
}

static bool naomi_FlatBios(const u8 *rom, size_t size)
{
	// From naomi.zip, or naomi2.zip for a NAOMI 2
	const char *bios = settings.System == DC_PLATFORM_NAOMI2 ? "naomi2" : "naomi";
	int region = naomi_FlatRegion(rom, size);
	if (!settings.bios.UseRealBios)
	{
		// "Use Real BIOS (If Available)" is off: not the one in the
		// archive, and not a naomi_boot.bin that was loaded already
		bios_loaded = false;
		NOTICE_LOG(NAOMI, "The BIOS is not to be used: the game is started without one");
		return true;
	}
	if (naomi_LoadBios(bios, NULL, NULL, region))
		bios_loaded = true;
	else
	{
		WARN_LOG(NAOMI, "Warning: Region %d bios not found in %s.zip", region, bios);
		if (naomi_LoadBios(bios, NULL, NULL, -1))
			bios_loaded = true;
		else if (!bios_loaded)
		{
			// A flat image says where it is loaded and where it starts:
			// it is started without one (reios_boot())
			NOTICE_LOG(NAOMI, "No BIOS in %s.zip or %s.7z: the game is started without one", bios, bios);
		}
	}
	return true;
}

int naomi_cart_GetSystemType(const char* file)
{
	const char *ext = path_get_extension(file);

   if (!naomi_IsArchive(ext))
   {
	  // Not a ZIP or 7z file so it has to be a Naomi game: the board it
	  // is for is the first thing in its header
	  char board[8];
	  int system = DC_PLATFORM_NAOMI;
	  RFILE *fp = filestream_open(file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	  if (fp)
	  {
		 if (filestream_read(fp, board, 6) == 6 && !memcmp(board, "Naomi2", 6))
			system = DC_PLATFORM_NAOMI2;
		 filestream_close(fp);
	  }
	  return system;
   }

   const int gameid = naomi_find_game(file);
   if (gameid < 0)
   {
	  // Not a romset. A flat image, archived?
	  if (!naomi_FlatArchiveOpen(file))
		 return -1;
	  return memcmp(flat_data, "Naomi2", 6) ? DC_PLATFORM_NAOMI : DC_PLATFORM_NAOMI2;
   }

   if (Games[gameid].cart_type == AW)
	  return DC_PLATFORM_ATOMISWAVE;
   // a NAOMI 2 game is one whose BIOS is the NAOMI 2's
   if (Games[gameid].bios != NULL && !strcmp(Games[gameid].bios, "naomi2"))
	  return DC_PLATFORM_NAOMI2;
   return DC_PLATFORM_NAOMI;
}

int naomi_cart_GetRotation()
{
	return game_rotation;
}

#ifdef _WIN32
#define CloseFile(f)	CloseHandle(f)
#else
#define CloseFile(f)	close(f)
#endif

void naomi_cart_Close()
{
	found_path.clear();		// the set there may be another by the next time
	if (CurrentCartridge != NULL)
	{
		delete CurrentCartridge;
		CurrentCartridge = NULL;
	}
	if (RomCacheMap != NULL)
	{
		for (int i = 0; i < RomCacheMapCount; i++)
			if (RomCacheMap[i] != INVALID_FD)
				CloseFile(RomCacheMap[i]);
		RomCacheMapCount = 0;
		delete[] RomCacheMap;
		RomCacheMap = NULL;
	}
	bios_loaded = false;
}

static bool naomi_cart_LoadRom(const char* file)
{
	INFO_LOG(NAOMI, "nullDC-Naomi rom loader v1.2");

	naomi_cart_Close();

	size_t folder_pos = strlen(file) - 1;
	while (folder_pos>1 && (file[folder_pos] != '\\' && file[folder_pos] != '/'))
		folder_pos--;

	folder_pos++;

   std::vector<std::string> files;
   std::vector<u32> fstart;
   std::vector<u32> fsize;

	u32 setsize = 0;
	bool raw_bin_file = false;

	char t[512];
	strcpy(t, file);

	const char *ext = path_get_extension(file);

	if (flat_archive && flat_path != file)
		naomi_FlatArchiveDrop();	// one that was looked at and not loaded

	if (naomi_IsArchive(ext))
	{
		// A flat image in an archive (found when the machine was told), or a romset
		if (flat_archive && flat_path == file)
		{
			if (!naomi_FlatBios(flat_data, flat_len))
			{
				naomi_FlatArchiveDrop();
				return false;
			}
			// The cartridge has the archive from here, and closes it
			CurrentCartridge = new ArchivedCartridge(flat_data, (u32)flat_len, flat_archive);
			flat_archive = NULL;
			naomi_FlatArchiveDrop();
			strcpy(naomi_game_id, CurrentCartridge->GetGameId().c_str());
			NOTICE_LOG(NAOMI, "NAOMI GAME ID [%s]", naomi_game_id);
			return true;
		}
		return naomi_cart_LoadZip(file);
	}

	u8* RomPtr;
	u32 RomSize;

	if (!strcasecmp(ext, "lst"))
	{
	   RFILE* fl = filestream_open(t, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	   if (!fl)
		   return false;

	   char* line = filestream_gets(fl, t, 512);
	   if (!line)
	   {
		   filestream_close(fl);
		   return false;
	   }

	   char* eon = strstr(line, "\n");
	   if (!eon)
	   {
		   ERROR_LOG(NAOMI, "+Parsing was unsuccessful, there is something wrong with your lst file");
		   filestream_close(fl);
		   return false;
	   }
	   else
	   {
		   *eon = 0;
	   }
	   eon = strstr(line, "\r");
	   if (eon)
		   *eon = 0;

	   DEBUG_LOG(NAOMI, "+Loading naomi rom : %s", line);

	   line = filestream_gets(fl, t, 512);
	   if (!line)
	   {
		   filestream_close(fl);
		   return false;
	   }

	   RomSize = 0;

	   while (line)
	   {
		   char filename[512];
		   u32 addr, sz;
		   if (sscanf(line, "\"%[^\"]\",%x,%x", filename, &addr, &sz) == 3)
		   {
			  files.push_back(filename);
			  fstart.push_back(addr);
			  fsize.push_back(sz);
			  setsize += sz;
			  RomSize = std::max(RomSize, (addr + sz));
		   }
		   else if (line[0] != 0 && line[0] != '\n' && line[0] != '\r')
				WARN_LOG(NAOMI, "Warning: invalid line in .lst file: %s", line);

		   line = filestream_gets(fl, t, 512);
	   }
	   filestream_close(fl);
	}
	else
	{
	   // BIN loading
	   RFILE* fp = filestream_open(t, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	   if (fp == NULL)
		  return false;
	   u32 file_size = (u32)filestream_get_size(fp);
	   filestream_close(fp);
	   files.push_back(t);
	   fstart.push_back(0);
	   fsize.push_back(file_size);
	   setsize = file_size;
	   RomSize = file_size;
	   raw_bin_file = true;
	}

	INFO_LOG(NAOMI, "+%zd romfiles, %.2f MB set size, %.2f MB set address space", files.size(), setsize / 1024.f / 1024.f, RomSize / 1024.f / 1024.f);

	if (RomCacheMap)
	{
		for (int i = 0; i < RomCacheMapCount; i++)
			if (RomCacheMap[i] != INVALID_FD)
				CloseFile(RomCacheMap[i]);
		RomCacheMapCount = 0;
		delete[] RomCacheMap;
	}

	RomCacheMapCount = (u32)files.size();
	RomCacheMap = new fd_t[files.size()]();

	//Allocate space for the ram, so we are sure we have a segment of continuous ram
	RomPtr = (u8*)mem_region_reserve(NULL, RomSize);
	verify(RomPtr != NULL);
	strcpy(t, file);

	bool load_error = false;

	//Create File Mapping Objects
	for (size_t i = 0; i<files.size(); i++)
	{
		if (!raw_bin_file)
		{
		   strncpy(t, file, sizeof(t));
		   t[sizeof(t) - 1] = '\0';
		   t[folder_pos] = 0;
		   strcat(t, files[i].c_str());
		}
		else
		{
		   strncpy(t, files[i].c_str(), sizeof(t));
		   t[sizeof(t) - 1] = '\0';
		}
		fd_t RomCache;

		if (strcmp(files[i].c_str(), "null") == 0)
		{
			RomCacheMap[i] = INVALID_FD;
			continue;
		}
#ifdef _WIN32
		RomCache = CreateFile(t, FILE_READ_ACCESS, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
#else
		RomCache = open(t, O_RDONLY);
#endif
		if (RomCache == INVALID_FD)
		{
		   ERROR_LOG(NAOMI, "-Unable to read file %s: error %d", t, errno);
		   RomCacheMap[i] = INVALID_FD;
		   load_error = true;
		   break;
		}

#ifdef _WIN32
		RomCacheMap[i] = CreateFileMapping(RomCache, 0, PAGE_READONLY, 0, fsize[i], 0);
		verify(CloseHandle(RomCache));
#else
		RomCacheMap[i] = RomCache;
#endif

		verify(RomCacheMap[i] != INVALID_FD);
		//printf("-Preparing \"%s\" at 0x%08X, size 0x%08X\n", files[i].c_str(), fstart[i], fsize[i]);
	}

	//Release the segment we reserved so we can map the files there
	mem_region_release(RomPtr, RomSize);

	if (load_error)
	{
	   for (size_t i = 0; i < files.size(); i++)
		  if (RomCacheMap[i] != INVALID_FD)
			 CloseFile(RomCacheMap[i]);
	   return false;
	}
	//We have all file mapping objects, we start to map the ram

	//Map the files into the segment of the ram that was reserved
	for (size_t i = 0; i<RomCacheMapCount; i++)
	{
		u8* RomDest = RomPtr + fstart[i];

		if (RomCacheMap[i] == INVALID_FD)
		{
			//printf("-Reserving ram at 0x%08X, size 0x%08X\n", fstart[i], fsize[i]);
			
			bool mapped = RomDest == (u8 *)mem_region_reserve(RomDest, fsize[i]);
			if (!mapped)
			{
			   ERROR_LOG(NAOMI, "-Mapping RAM FAILED @ %08x size %x", fstart[i], fsize[i]);
			   return false;
			}
		}
		else
		{
			//printf("-Mapping \"%s\" at 0x%08X, size 0x%08X\n", files[i].c_str(), fstart[i], fsize[i]);
			bool mapped = RomDest == (u8 *)mem_region_map_file((void *)(uintptr_t)RomCacheMap[i], RomDest, fsize[i], 0, false);
			if (!mapped)
			{
			   ERROR_LOG(NAOMI, "-Mapping ROM FAILED: %s @ %08x size %x", files[i].c_str(), fstart[i], fsize[i]);
			   return false;
			}
		}
	}

	//done :)
	INFO_LOG(NAOMI, "Mapped ROM Successfully !");

	// (the BIOS goes by the image's header: after the image is there)
	if (!naomi_FlatBios(RomPtr, RomSize))
	{
		mem_region_release(RomPtr, RomSize);
		return false;
	}

	CurrentCartridge = new DecryptedCartridge(RomPtr, RomSize);
	strcpy(naomi_game_id, CurrentCartridge->GetGameId().c_str());
	NOTICE_LOG(NAOMI, "NAOMI GAME ID [%s]", naomi_game_id);

	return true;
}

extern char *game_data;
extern char eeprom_file[PATH_MAX];

bool naomi_cart_SelectFile()
{
	if (!naomi_cart_LoadRom(game_data))
	{
	   ERROR_LOG(NAOMI, "Cannot load %s: error %d", game_data, errno);
	   return false;
	}

	INFO_LOG(NAOMI, "EEPROM file : %s", eeprom_file);

	return true;
}

Cartridge::Cartridge(u32 size)
{
	RomPtr = (u8 *)malloc(size);
	RomSize = size;
	memset(RomPtr, 0xFF, RomSize);
}

Cartridge::~Cartridge()
{
	if (RomPtr != NULL)
		free(RomPtr);
}

bool Cartridge::Read(u32 offset, u32 size, void* dst)
{
	offset &= 0x1FFFFFFF;
	if (offset >= RomSize || (offset + size) > RomSize)
	{
		static u32 ones = 0xffffffff;

		// Makes Outtrigger boot
		INFO_LOG(NAOMI, "offset %d > %d", offset, RomSize);
		memcpy(dst, &ones, size);
	}
	else
	{
		memcpy(dst, &RomPtr[offset], size);
	}

	return true;
}

bool Cartridge::Write(u32 offset, u32 size, u32 data)
{
	INFO_LOG(NAOMI, "Invalid write @ %08x data %x", offset, data);
	return false;
}

void* Cartridge::GetPtr(u32 offset, u32& size)
{
	offset &= 0x1FFFffff;

	verify(offset < RomSize);
	verify((offset + size) <= RomSize);

	return &RomPtr[offset];
}

std::string Cartridge::GetGameId() {
	if (RomSize < 0x30 + 0x20)
		return "(ROM too small)";

	std::string game_id((char *)RomPtr + 0x30, 0x20);
	if (game_id == "AWNAOMI                         " && RomSize >= 0xFF50)
	{
		game_id = std::string((char *)RomPtr + 0xFF30, 0x20);
	}
	while (!game_id.empty() && game_id.back() == ' ')
		game_id.pop_back();
	return game_id;
}

void* NaomiCartridge::GetDmaPtr(u32& size)
{
	if ((DmaOffset & 0x1fffffff) >= RomSize)
	{
		INFO_LOG(NAOMI, "Error: DmaOffset >= RomSize");
		size = 0;
		return NULL;
	}
	size = std::min(size, RomSize - (DmaOffset & 0x1fffffff));
	return GetPtr(DmaOffset, size);
}

void NaomiCartridge::AdvancePtr(u32 size) {
}

u32 NaomiCartridge::ReadMem(u32 address, u32 size)
{
	verify(size!=1);
	//printf("+naomi?WTF? ReadMem: %X, %d\n", address, size);
	switch(address & 255)
	{
	case 0x3c:	// 5f703c: DIMM COMMAND
		DEBUG_LOG(NAOMI, "DIMM COMMAND read<%d>", size);
		return 0xffff; // reg_dimm_command;
	case 0x40:	// 5f7040: DIMM OFFSETL
		DEBUG_LOG(NAOMI, "DIMM OFFSETL read<%d>", size);
		return reg_dimm_offsetl;
	case 0x44:	// 5f7044: DIMM PARAMETERL
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL read<%d>", size);
		return reg_dimm_parameterl;
	case 0x48:	// 5f7048: DIMM PARAMETERH
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH read<%d>", size);
		return reg_dimm_parameterh;
	case 0x04C:	// 5f704c: DIMM STATUS
		DEBUG_LOG(NAOMI, "DIMM STATUS read<%d>", size);
		return reg_dimm_status;

	case NAOMI_ROM_OFFSETH_addr&255:
		return RomPioOffset>>16 | (RomPioAutoIncrement << 15);

	case NAOMI_ROM_OFFSETL_addr&255:
		return RomPioOffset&0xFFFF;

	case NAOMI_ROM_DATA_addr & 255:
		{
			u32 rv = 0;
			Read(RomPioOffset, 2, &rv);
			if (RomPioAutoIncrement)
				RomPioOffset += 2;

			return rv;
		}

	case NAOMI_DMA_COUNT_addr&255:
		return (u16) DmaCount;

	case NAOMI_BOARDID_READ_addr&255:
		return NaomiGameIDRead()?0x8000:0x0000;

		//What should i do to emulate 'nothing' ?
	case NAOMI_COMM_OFFSET_addr&255:
		#ifdef NAOMI_COMM
		DEBUG_LOG(NAOMI, "naomi COMM offs READ: %X, %d", address, size);
		return CommOffset;
		#endif
	case NAOMI_COMM_DATA_addr&255:
		#ifdef NAOMI_COMM
		DEBUG_LOG(NAOMI, "naomi COMM data read: %X, %d", CommOffset, size);
		if (CommSharedMem)
		{
			return CommSharedMem[CommOffset&0xF];
		}
		#endif
		return 1;


	case NAOMI_DMA_OFFSETH_addr&255:
		return DmaOffset>>16;
	case NAOMI_DMA_OFFSETL_addr&255:
		return DmaOffset&0xFFFF;

	case NAOMI_BOARDID_WRITE_addr&255:
		DEBUG_LOG(NAOMI, "naomi ReadBoardId: %X, %d", address, size);
		return 1;

	case NAOMI_COMM2_CTRL_addr & 255:
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_CTRL read");
		return comm_ctrl;

	case NAOMI_COMM2_OFFSET_addr & 255:
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_OFFSET read");
		return comm_offset;

	case NAOMI_COMM2_DATA_addr & 255:
		{
			DEBUG_LOG(NAOMI, "NAOMI_COMM2_DATA read @ %04x", comm_offset);
			u16 value;
			if (comm_ctrl & 1)
				value = m68k_ram[comm_offset / 2];
			else {
				// TODO u16 *commram = (u16*)membank("comm_ram")->base();
				value = comm_ram[comm_offset / 2];
			}
			comm_offset += 2;
			return value;
		}

	case NAOMI_COMM2_STATUS0_addr & 255:
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS0 read");
		return comm_status0;

	case NAOMI_COMM2_STATUS1_addr & 255:
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS1 read");
		return comm_status1;

	default: break;
	}
	DEBUG_LOG(NAOMI, "naomi?WTF? ReadMem: %X, %d", address, size);

	return 0xFFFF;
}

void NaomiCartridge::WriteMem(u32 address, u32 data, u32 size)
{
	switch(address & 255)
	{
	case 0x3c:	// 5f703c: DIMM COMMAND
		 if (0x1E03==data)
		 {
			 /*
			 if (!(reg_dimm_status & 0x100))
				asic_RaiseInterrupt(holly_EXP_PCI);
			 reg_dimm_status |= 1;*/
		 }
		 reg_dimm_command = data;
		 DEBUG_LOG(NAOMI, "DIMM COMMAND Write: %X <= %X, %d", address, data, size);
		 return;

	case 0x40:	// 5f7040: DIMM OFFSETL
		reg_dimm_offsetl = data;
		DEBUG_LOG(NAOMI, "DIMM OFFSETL Write: %X <= %X, %d", address, data, size);
		return;
	case 0x44:	// 5f7044: DIMM PARAMETERL
		reg_dimm_parameterl = data;
		DEBUG_LOG(NAOMI, "DIMM PARAMETERL Write: %X <= %X, %d", address, data, size);
		return;
	case 0x48:	// 5f7048: DIMM PARAMETERH
		reg_dimm_parameterh = data;
		DEBUG_LOG(NAOMI, "DIMM PARAMETERH Write: %X <= %X, %d", address, data, size);
		return;

	case 0x4C:	// 5f704c: DIMM STATUS
		if (data&0x100)
		{
			asic_CancelInterrupt(holly_EXP_PCI);
		}
		else if ((data&1)==0)
		{
			/*FILE* ramd=fopen("c:\\ndc.ram.bin","wb");
			fwrite(mem_b.data,1,RAM_SIZE,ramd);
			fclose(ramd);*/
			naomi_process(reg_dimm_command, reg_dimm_offsetl, reg_dimm_parameterl, reg_dimm_parameterh);
		}
		reg_dimm_status = data & ~0x100;
		DEBUG_LOG(NAOMI, "DIMM STATUS Write: %X <= %X, %d", address, data, size);
		return;

		//These are known to be valid on normal ROMs and DIMM board
	case NAOMI_ROM_OFFSETH_addr&255:
		RomPioAutoIncrement = (data & 0x8000) != 0;
		RomPioOffset&=0x0000ffff;
		RomPioOffset|=(data<<16)&0x7fff0000;
		PioOffsetChanged(RomPioOffset);
		return;

	case NAOMI_ROM_OFFSETL_addr&255:
		RomPioOffset&=0xffff0000;
		RomPioOffset|=data;
		PioOffsetChanged(RomPioOffset);
		return;

	case NAOMI_ROM_DATA_addr&255:
		Write(RomPioOffset, size, data);
		if (RomPioAutoIncrement)
			RomPioOffset += 2;

		return;

	case NAOMI_DMA_OFFSETH_addr&255:
		DmaOffset&=0x0000ffff;
		DmaOffset|=(data&0x7fff)<<16;
		DmaOffsetChanged(DmaOffset);
		return;

	case NAOMI_DMA_OFFSETL_addr&255:
		DmaOffset&=0xffff0000;
		DmaOffset|=data;
		DmaOffsetChanged(DmaOffset);
		return;

	case NAOMI_DMA_COUNT_addr&255:
		DmaCount=data;
		return;

	case NAOMI_STATUS_LEDS_addr & 255:
		return;

	case NAOMI_BOARDID_WRITE_addr&255:
		NaomiGameIDWrite((u16)data);
		return;

		//What should i do to emulate 'nothing' ?
	case NAOMI_COMM_OFFSET_addr&255:
#ifdef NAOMI_COMM
		DEBUG_LOG(NAOMI, "naomi COMM ofset Write: %X <= %X, %d", address, data, size);
		CommOffset=data&0xFFFF;
#endif
		return;

	case NAOMI_COMM_DATA_addr&255:
		#ifdef NAOMI_COMM
		DEBUG_LOG(NAOMI, "naomi COMM data Write: %X <= %X, %d", CommOffset, data, size);
		if (CommSharedMem)
		{
			CommSharedMem[CommOffset&0xF]=data;
		}
		#endif
		return;

		//This should be valid
	case NAOMI_BOARDID_READ_addr&255:
		DEBUG_LOG(NAOMI, "naomi WriteMem: %X <= %X, %d", address, data, size);
		return;

	case NAOMI_COMM2_CTRL_addr & 255:
		comm_ctrl = (u16)data;
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_CTRL set to %x", comm_ctrl);
		return;

	case NAOMI_COMM2_OFFSET_addr & 255:
		comm_offset = (u16)data;
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_OFFSET set to %x", comm_offset);
		return;

	case NAOMI_COMM2_DATA_addr & 255:
		if (comm_ctrl & 1)
			m68k_ram[comm_offset / 2] = (u16)data;
		else {
			// TODO u16 *commram = (u16*)membank("comm_ram")->base();
			comm_ram[comm_offset / 2] = (u16)data;
		}
		comm_offset += 2;
		return;

	case NAOMI_COMM2_STATUS0_addr & 255:
		comm_status0 = (u16)data;
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS0 set to %x", comm_status0);
		return;

	case NAOMI_COMM2_STATUS1_addr & 255:
		comm_status1 = (u16)data;
		DEBUG_LOG(NAOMI, "NAOMI_COMM2_STATUS1 set to %x", comm_status1);
		return;

	default:
		break;
	}
	DEBUG_LOG(NAOMI, "naomi?WTF? WriteMem: %X <= %X, %d", address, data, size);
}

void NaomiCartridge::Serialize(void** data, unsigned int* total_size)
{
   LIBRETRO_S(RomPioOffset);
   LIBRETRO_S(RomPioAutoIncrement);
   LIBRETRO_S(DmaOffset);
   LIBRETRO_S(DmaCount);
   Cartridge::Serialize(data, total_size);
}

void NaomiCartridge::Unserialize(void** data, unsigned int* total_size)
{
   LIBRETRO_US(RomPioOffset);
   LIBRETRO_US(RomPioAutoIncrement);
   LIBRETRO_US(DmaOffset);
   LIBRETRO_US(DmaCount);
   Cartridge::Unserialize(data, total_size);
}

bool M2Cartridge::Read(u32 offset, u32 size, void* dst)
{
	if (offset & 0x40000000)
	{
		if (offset == 0x4001fffe)
		{
			//printf("NAOMI CART DECRYPT read: %08x sz %d\n", offset, size);
			cyptoSetKey(key);
			u16 data = cryptoDecrypt();
			*(u16 *)dst = data;
			return true;
		}
		INFO_LOG(NAOMI, "Invalid read @ %08x", offset);
		return false;
	}
	else if (!(RomPioOffset & 0x20000000))
	{
		// 4MB mode
		offset = (offset & 0x103fffff) | ((offset & 0x07c00000) << 1);
	}
	return NaomiCartridge::Read(offset, size, dst);
}

void* M2Cartridge::GetDmaPtr(u32& size)
{
	if (RomPioOffset & 0x20000000)
		return NaomiCartridge::GetDmaPtr(size);

	// 4MB mode
	u32 offset4mb = (DmaOffset & 0x103fffff) | ((DmaOffset & 0x07c00000) << 1);
	size = std::min(std::min(size, 0x400000 - (offset4mb & 0x3FFFFF)), RomSize - offset4mb);

	return GetPtr(offset4mb, size);
}

bool M2Cartridge::Write(u32 offset, u32 size, u32 data)
{
	if (offset & 0x40000000)
	{
		//printf("NAOMI CART CRYPT write: %08x data %x sz %d\n", offset, data, size);
		if (offset & 0x00020000)
		{
			offset &= sizeof(naomi_cart_ram) - 1;
			naomi_cart_ram[offset] = data;
			naomi_cart_ram[offset + 1] = data >> 8;
			return true;
		}
		switch (offset & 0x1ffff)
		{
			case 0x1fff8:
				cyptoSetLowAddr(data);
				return true;
			case 0x1fffa:
				cyptoSetHighAddr(data);
				return true;
			case 0x1fffc:
				cyptoSetSubkey(data);
				return true;
		}
	}
	return NaomiCartridge::Write(offset, size, data);
}

u16 M2Cartridge::ReadCipheredData(u32 offset)
{
	if ((offset & 0xffff0000) == 0x01000000)
	{
		int base = 2 * (offset & 0x7fff);
		return naomi_cart_ram[base + 1] | (naomi_cart_ram[base] << 8);
	}
	verify(2 * offset + 1 < RomSize);
	return RomPtr[2 * offset + 1] | (RomPtr[2 * offset] << 8);

}

std::string M2Cartridge::GetGameId()
{
	std::string game_id = NaomiCartridge::GetGameId();
	if ((game_id.size() < 2 || ((u8)game_id[0] == 0xff && (u8)game_id[1] == 0xff)) && RomSize >= 0x800050)
	{
		game_id = std::string((char *)RomPtr + 0x800030, 0x20);
		while (!game_id.empty() && game_id.back() == ' ')
			game_id.pop_back();
	}
	return game_id;
}

void M2Cartridge::Serialize(void** data, unsigned int* total_size) {
   LIBRETRO_S(naomi_cart_ram);
   NaomiCartridge::Serialize(data, total_size);
}

void M2Cartridge::Unserialize(void** data, unsigned int* total_size) {
   LIBRETRO_US(naomi_cart_ram);
   NaomiCartridge::Unserialize(data, total_size);
}

ArchivedCartridge::~ArchivedCartridge()
{
	// the image is the archive's
	archive_close(archive);
	RomPtr = NULL;
}

DecryptedCartridge::~DecryptedCartridge()
{
	// TODO this won't work on windows -> need to unmap each file first
	mem_region_release(RomPtr, RomSize);
	// Avoid crash when freeing vmem
	RomPtr = NULL;
}
