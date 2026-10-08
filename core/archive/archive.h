/*
	Copyright 2018 flyinghead

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
#ifndef CORE_ARCHIVE_ARCHIVE_H_
#define CORE_ARCHIVE_ARCHIVE_H_

#include <stddef.h>
#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Read-only view of a zip, 7z or rar archive. Members are handed out as
 * borrowed byte ranges: a stored member of a mapped zip is the mapping
 * itself, anything else is decoded once and kept for the archive's
 * lifetime. */
typedef struct archive archive_t;

typedef struct archive_entry
{
   const char *name;      /* UTF-8, '/' separated, owned by the archive */
   uint64_t    size;
   uint64_t    csize;     /* bytes in the archive; equals size when stored */
   uint64_t    data_off;  /* where those bytes are in the archive file */
   uint32_t    crc;
   uint8_t     stored;    /* the bytes in the file are the member itself */
   uint8_t     is_dir;
   uint8_t     usable;    /* 0: unsupported method or unreadable header */
} archive_entry_t;

/* Opens @path, then @path with .zip/.ZIP/.7z/.7Z/.rar/.RAR appended. */
archive_t *archive_open(const char *path);
void archive_close(archive_t *a);
const char *archive_path(const archive_t *a);
unsigned archive_num_entries(const archive_t *a);
const archive_entry_t *archive_entry(const archive_t *a, unsigned index);
/* Index of the member named @name (exact match), or -1. */
int archive_find(const archive_t *a, const char *name);
/* Index of a member whose CRC32 is @crc (never 0), or -1. */
int archive_find_crc(const archive_t *a, uint32_t crc);
/* The member's bytes, valid until archive_close(). NULL on a decode
 * failure or an unsupported method. */
const uint8_t *archive_entry_data(archive_t *a, unsigned index, size_t *len);
/* The same bytes when they are already addressable - a stored member
 * of a mapped archive, or one decoded earlier - else NULL without
 * reading anything: a stored member of an unmapped archive is better
 * read in place at data_off than pulled into memory. */
const uint8_t *archive_entry_map(archive_t *a, unsigned index, size_t *len);
/* The member's bytes written to @dst, which has room for @dst_size, and
 * their length to @len: for a member whose bytes have their own place to
 * go to. A deflated zip member is decoded straight there - not into a
 * buffer the archive keeps until it is closed, to be copied from. Returns
 * 0 if the member does not fit (nothing is written), cannot be decoded or
 * fails its checksum (@dst may then hold part of it). */
int archive_entry_read(archive_t *a, unsigned index, uint8_t *dst,
      size_t dst_size, size_t *len);
/* Read-ahead hint for @len bytes at @p, when @p lies in the archive's
 * mapping; bytes already decoded into memory need none. */
void archive_prefetch(archive_t *a, const uint8_t *p, size_t len);

/* For a zip or 7z at @path holding a disc image, writes "path#member"
 * into @out, preferring a .gdi, then .cue, .chd, .cdi. A @path that
 * already names a member is written out unchanged when that member is
 * a disc image, else resolved within its archive. Returns 0 when @path
 * is not an archive, holds no disc image, or @out is too small. */
int archive_resolve_disc(const char *path, char *out, size_t out_len);

RETRO_END_DECLS

#endif /* CORE_ARCHIVE_ARCHIVE_H_ */
