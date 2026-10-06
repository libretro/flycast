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
#include <stdlib.h>
#include <string.h>

#include <7z/r7z_archive.h>
#include <encodings/crc32.h>
#include <encodings/deflate.h>
#include <encodings/utf.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <formats/data_transfer.h>

#include "archive.h"
#include "deps/coreio/coreio.h"

#define ZIP_SIG_LOCAL       0x04034b50u
#define ZIP_SIG_CENTRAL     0x02014b50u
#define ZIP_SIG_EOCD        0x06054b50u
#define ZIP_SIG_EOCD64      0x06064b50u
#define ZIP_SIG_EOCD64_LOC  0x07064b50u
#define ZIP_METHOD_STORED   0
#define ZIP_METHOD_DEFLATE  8
#define ZIP_EOCD_LEN        22
#define ZIP_EOCD_MAX_TAIL   (ZIP_EOCD_LEN + 0xffff)
#define ZIP_INPUT_CHUNK     (64 * 1024)

struct archive
{
   core_file       *f;
   const uint8_t   *map;      /* the whole file, when addressable      */
   r7z_archive_t   *sz;
   uint8_t         *sz_buf;   /* 7z read into memory when not mapped   */
   uint32_t         sz_folders;
   archive_entry_t *entries;
   uint8_t        **cache;    /* decoded members, one slot per entry   */
   const uint8_t  **view;     /* members borrowed from a mapping or a
                                 decoded folder, one slot per entry    */
   char            *names;
   char            *path;
   size_t           map_len;
   unsigned         num_entries;
};

static uint32_t rd_le16(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
   return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

/* Bytes [off, off + len) of the archive file: a pointer into the
 * mapping, or read into @buf. */
static const uint8_t *archive_bytes(archive_t *a, uint64_t off,
      size_t len, uint8_t *buf)
{
   if (a->map)
   {
      if (off > a->map_len || len > a->map_len - (size_t)off)
         return NULL;
      return a->map + (size_t)off;
   }
   if (core_fread_at(a->f, off, buf, len) != len)
      return NULL;
   return buf;
}

static int archive_alloc_tables(archive_t *a, unsigned n, size_t name_bytes)
{
   if (!(a->entries = (archive_entry_t*)calloc(n ? n : 1, sizeof(*a->entries))))
      return 0;
   if (!(a->cache = (uint8_t**)calloc(n ? n : 1, sizeof(*a->cache))))
      return 0;
   if (!(a->view = (const uint8_t**)calloc(n ? n : 1, sizeof(*a->view))))
      return 0;
   if (!(a->names = (char*)malloc(name_bytes ? name_bytes : 1)))
      return 0;
   a->num_entries = n;
   return 1;
}

/* ---------------------------------------------------------------- zip */

/* Finds the end-of-central-directory record in the file's tail and
 * returns the directory's offset, size and entry count. */
static int zip_find_directory(archive_t *a, uint64_t *cd_off,
      uint64_t *cd_size, uint64_t *cd_count)
{
   uint8_t       *tail_buf = NULL;
   const uint8_t *tail;
   uint64_t       file_size = core_fsize(a->f);
   uint64_t       tail_off;
   size_t         tail_len;
   size_t         i;
   int            ok = 0;

   if (file_size < ZIP_EOCD_LEN)
      return 0;
   tail_len = file_size < ZIP_EOCD_MAX_TAIL ? (size_t)file_size : ZIP_EOCD_MAX_TAIL;
   tail_off = file_size - tail_len;

   if (!a->map && !(tail_buf = (uint8_t*)malloc(tail_len)))
      return 0;
   if (!(tail = archive_bytes(a, tail_off, tail_len, tail_buf)))
      goto out;

   for (i = tail_len - ZIP_EOCD_LEN + 1; i-- > 0; )
   {
      const uint8_t *e = tail + i;
      uint64_t       eocd_off;

      if (rd_le32(e) != ZIP_SIG_EOCD)
         continue;
      if (rd_le16(e + 20) > tail_len - ZIP_EOCD_LEN - i)
         continue;   /* comment length does not fit: not the record */

      eocd_off  = tail_off + i;
      *cd_count = rd_le16(e + 10);
      *cd_size  = rd_le32(e + 12);
      *cd_off   = rd_le32(e + 16);

      /* zip64: the locator sits right before the record */
      if ((*cd_count == 0xffff || *cd_size == 0xffffffffu
               || *cd_off == 0xffffffffu) && eocd_off >= 20)
      {
         uint8_t        loc_buf[20];
         uint8_t        rec_buf[56];
         const uint8_t *loc;
         const uint8_t *rec;

         if (!(loc = archive_bytes(a, eocd_off - 20, 20, loc_buf))
               || rd_le32(loc) != ZIP_SIG_EOCD64_LOC)
            goto out;
         if (!(rec = archive_bytes(a, rd_le64(loc + 8), 56, rec_buf))
               || rd_le32(rec) != ZIP_SIG_EOCD64)
            goto out;
         *cd_count = rd_le64(rec + 32);
         *cd_size  = rd_le64(rec + 40);
         *cd_off   = rd_le64(rec + 48);
      }
      ok = 1;
      break;
   }

out:
   free(tail_buf);
   return ok;
}

/* The member's data starts after its local header, whose name and
 * extra fields need not match the central directory's. */
static int zip_locate_data(archive_t *a, archive_entry_t *e, uint64_t local_off)
{
   uint8_t        hdr_buf[30];
   const uint8_t *hdr;

   if (!(hdr = archive_bytes(a, local_off, 30, hdr_buf)))
      return 0;
   if (rd_le32(hdr) != ZIP_SIG_LOCAL)
      return 0;
   e->data_off = local_off + 30 + rd_le16(hdr + 26) + rd_le16(hdr + 28);
   return 1;
}

static int zip_open(archive_t *a)
{
   uint8_t       *cd_buf = NULL;
   const uint8_t *cd;
   const uint8_t *p;
   uint64_t       cd_off, cd_size, cd_count;
   size_t         name_bytes = 0;
   unsigned       n          = 0;
   unsigned       i;
   char          *name_out;
   int            ok = 0;

   if (!zip_find_directory(a, &cd_off, &cd_size, &cd_count))
      return 0;
   if (cd_size > (uint64_t)((size_t)-1) || cd_count > 0x7fffffffu)
      return 0;

   if (!a->map && !(cd_buf = (uint8_t*)malloc((size_t)cd_size + 1)))
      return 0;
   if (!(cd = archive_bytes(a, cd_off, (size_t)cd_size, cd_buf)))
      goto out;

   /* First pass: count and size the names. */
   for (p = cd; (size_t)(p - cd) + 46 <= (size_t)cd_size && n < cd_count; n++)
   {
      size_t vlen;

      if (rd_le32(p) != ZIP_SIG_CENTRAL)
         break;
      vlen = rd_le16(p + 28) + rd_le16(p + 30) + rd_le16(p + 32);
      if ((size_t)(p - cd) + 46 + vlen > (size_t)cd_size)
         break;
      name_bytes += rd_le16(p + 28) + 1;
      p          += 46 + vlen;
   }

   if (!archive_alloc_tables(a, n, name_bytes))
      goto out;

   name_out = a->names;
   for (i = 0, p = cd; i < n; i++)
   {
      archive_entry_t *e        = &a->entries[i];
      uint32_t         method   = rd_le16(p + 10);
      uint64_t         csize    = rd_le32(p + 20);
      uint64_t         size     = rd_le32(p + 24);
      uint64_t         local    = rd_le32(p + 42);
      size_t           name_len = rd_le16(p + 28);
      size_t           extra_len = rd_le16(p + 30);
      const uint8_t   *extra    = p + 46 + name_len;
      size_t           extra_left = extra_len;

      memcpy(name_out, p + 46, name_len);
      name_out[name_len] = '\0';
      e->name   = name_out;
      name_out += name_len + 1;

      /* zip64 extra field: 64-bit sizes and offset, in that order,
       * only for the fields the 32-bit record could not hold */
      while (extra_left >= 4)
      {
         size_t flen = rd_le16(extra + 2);
         if (flen > extra_left - 4)
            break;
         if (rd_le16(extra) == 0x0001)
         {
            const uint8_t *q = extra + 4;
            if (size == 0xffffffffu && flen >= 8)
            {
               size = rd_le64(q);
               q   += 8;
            }
            if (csize == 0xffffffffu && q + 8 <= extra + 4 + flen)
            {
               csize = rd_le64(q);
               q    += 8;
            }
            if (local == 0xffffffffu && q + 8 <= extra + 4 + flen)
               local = rd_le64(q);
            break;
         }
         extra      += 4 + flen;
         extra_left -= 4 + flen;
      }

      e->size   = size;
      e->csize  = csize;
      e->crc    = rd_le32(p + 16);
      e->is_dir = name_len > 0 && (name_out[-2] == '/' || name_out[-2] == '\\');
      e->stored = method == ZIP_METHOD_STORED;
      e->usable = !e->is_dir
         && (method == ZIP_METHOD_STORED || method == ZIP_METHOD_DEFLATE)
         && csize <= (uint64_t)((size_t)-1)
         && zip_locate_data(a, e, local);

      p += 46 + name_len + extra_len + rd_le16(p + 32);
   }
   ok = 1;

out:
   free(cd_buf);
   return ok;
}

/* The deflate producer behind data_transfer: input comes straight from
 * the mapping or in chunks from the file; output goes to the exact
 * buffer the transfer owns. */
typedef struct zip_inflate_src
{
   archive_t *a;
   void      *inf;
   uint8_t   *in_buf;
   uint64_t   in_off;      /* next compressed byte to read from the file */
   uint64_t   in_left;     /* compressed bytes not yet handed in         */
   size_t     out_left;
   int        pending_in;  /* input handed in that is not yet consumed   */
} zip_inflate_src_t;

static int64_t zip_inflate_read(void *ud, uint8_t *dst, size_t n)
{
   zip_inflate_src_t *s = (zip_inflate_src_t*)ud;
   size_t             wrote;
   int                st;

   (void)n;
   if (!s->out_left)
      return 0;
   rinflate_set_out(s->inf, dst, s->out_left);

   for (;;)
   {
      size_t rd;

      if (!s->pending_in)
      {
         size_t take;

         if (!s->in_left)
            return -1;   /* stream ended before the declared size */
         if (s->a->map)
         {
            take = (size_t)s->in_left;
            rinflate_set_in(s->inf, s->a->map + (size_t)s->in_off, take);
         }
         else
         {
            take = s->in_left < ZIP_INPUT_CHUNK ? (size_t)s->in_left : ZIP_INPUT_CHUNK;
            if (core_fread_at(s->a->f, s->in_off, s->in_buf, take) != take)
               return -1;
            rinflate_set_in(s->inf, s->in_buf, take);
         }
         s->in_off     += take;
         s->in_left    -= take;
         s->pending_in  = 1;
      }

      st = rinflate_process(s->inf, &rd, &wrote);
      if (st == RDEFLATE_PROCESS_ERROR)
         return -1;
      if (wrote)
      {
         s->out_left -= wrote;
         return (int64_t)wrote;
      }
      if (st == RDEFLATE_PROCESS_END)
         return 0;
      /* no output: the decoder wants more input */
      s->pending_in = 0;
   }
}

static uint8_t *zip_inflate_entry(archive_t *a, const archive_entry_t *e)
{
   zip_inflate_src_t s;
   data_transfer_t  *dt;
   uint8_t          *out = NULL;

   s.a          = a;
   s.in_buf     = NULL;
   s.in_off     = e->data_off;
   s.in_left    = e->csize;
   s.out_left   = (size_t)e->size;
   s.pending_in = 0;

   if (!(s.inf = rinflate_new(-15)))
      return NULL;
   if (!a->map && !(s.in_buf = (uint8_t*)malloc(ZIP_INPUT_CHUNK)))
      goto out;
   if (!(dt = data_transfer_open_source((size_t)e->size, zip_inflate_read, &s)))
      goto out;

   while (!data_transfer_complete(dt) && !data_transfer_failed(dt))
      if (!data_transfer_iterate(dt, 0))
         break;

   if (!(out = data_transfer_source_detach(dt, NULL)))
      data_transfer_free(dt);

   if (out && encoding_crc32(0, out, (size_t)e->size) != e->crc)
   {
      free(out);
      out = NULL;
   }
out:
   free(s.in_buf);
   rinflate_free(s.inf);
   return out;
}

/* A stored member of an unmapped archive: one read into an exact
 * buffer. */
static int64_t zip_stored_read(void *ud, uint8_t *dst, size_t n)
{
   zip_inflate_src_t *s = (zip_inflate_src_t*)ud;
   size_t             got;

   (void)n;
   if (!s->in_left)
      return 0;
   got = core_fread_at(s->a->f, s->in_off, dst, (size_t)s->in_left);
   if (!got)
      return -1;
   s->in_off  += got;
   s->in_left -= got;
   return (int64_t)got;
}

static uint8_t *zip_read_stored(archive_t *a, const archive_entry_t *e)
{
   zip_inflate_src_t s;
   data_transfer_t  *dt;
   uint8_t          *out;

   s.a       = a;
   s.in_off  = e->data_off;
   s.in_left = e->size;

   if (!(dt = data_transfer_open_source((size_t)e->size, zip_stored_read, &s)))
      return NULL;
   while (!data_transfer_complete(dt) && !data_transfer_failed(dt))
      if (!data_transfer_iterate(dt, 0))
         break;
   if (!(out = data_transfer_source_detach(dt, NULL)))
      data_transfer_free(dt);
   return out;
}

static const uint8_t *zip_entry_data(archive_t *a, unsigned index, size_t *len)
{
   archive_entry_t *e = &a->entries[index];

   if (e->stored)
   {
      if (a->map)
      {
         if (e->data_off > a->map_len || e->size > a->map_len - (size_t)e->data_off)
            return NULL;
         *len = (size_t)e->size;
         return a->map + (size_t)e->data_off;
      }
      a->cache[index] = zip_read_stored(a, e);
   }
   else
      a->cache[index] = zip_inflate_entry(a, e);

   if (!a->cache[index])
      return NULL;
   *len = (size_t)e->size;
   return a->cache[index];
}

/* ----------------------------------------------------------------- 7z */

static int sz_open(archive_t *a)
{
   const uint8_t *data;
   size_t         len = 0;
   size_t         name_bytes = 0;
   unsigned       n, i;
   char          *name_out;

   if (a->map)
   {
      data = a->map;
      len  = a->map_len;
   }
   else
   {
      len = core_fsize(a->f);
      if (!(a->sz_buf = (uint8_t*)malloc(len ? len : 1)))
         return 0;
      if (core_fread_at(a->f, 0, a->sz_buf, len) != len)
         return 0;
      data = a->sz_buf;
   }

   if (r7z_archive_open(&a->sz, data, len) != R7Z_OK)
      return 0;

   n = r7z_archive_num_entries(a->sz);
   for (i = 0; i < n; i++)
   {
      const uint16_t *w = r7z_archive_entry(a->sz, i)->name;
      size_t          wl = 0;
      while (w[wl])
         wl++;
      name_bytes += wl * 3 + 1;
   }

   if (!archive_alloc_tables(a, n, name_bytes))
      return 0;

   name_out = a->names;
   for (i = 0; i < n; i++)
   {
      const r7z_entry_t *se = r7z_archive_entry(a->sz, i);
      archive_entry_t   *e  = &a->entries[i];
      size_t             wl = 0;
      size_t             out_chars;

      if (!se->is_dir && se->size && se->folder + 1 > a->sz_folders)
         a->sz_folders = se->folder + 1;

      while (se->name[wl])
         wl++;
      out_chars = wl * 3 + 1;
      if (!utf16_conv_utf8((uint8_t*)name_out, &out_chars, se->name, wl))
         out_chars = 0;
      name_out[out_chars] = '\0';
      e->name   = name_out;
      name_out += out_chars + 1;
      e->size   = se->size;
      e->csize  = se->size;
      e->crc    = se->has_crc ? se->crc : 0;
      e->is_dir = se->is_dir != 0;
      e->stored = 0;
      e->usable = !e->is_dir;
   }
   return 1;
}

/* A solid archive is one folder: its members are borrowed out of the
 * decoded folder, which stays cached for the archive's lifetime. In any
 * other archive a member is its own folder, or shares one that a later
 * decode would replace, so it is handed over or copied out instead. */
static const uint8_t *sz_entry_data(archive_t *a, unsigned index, size_t *len)
{
   uint8_t *out;
   size_t   out_len;

   if (a->sz_folders == 1)
   {
      const uint8_t *view;

      if (r7z_archive_entry_borrow(a->sz, index, &view, &out_len) != R7Z_OK)
         return NULL;
      a->view[index] = view;
      *len           = out_len;
      return view;
   }

   if (r7z_archive_extract_detach(a->sz, index, &out, &out_len) != R7Z_OK)
      return NULL;
   a->cache[index] = out;
   *len            = out_len;
   return out;
}

/* ------------------------------------------------------------ common */

static archive_t *archive_open_one(const char *path)
{
   archive_t     *a;
   uint8_t        sig_buf[6];
   const uint8_t *sig;
   size_t         path_len;
   int            ok = 0;

   if (!(a = (archive_t*)calloc(1, sizeof(*a))))
      return NULL;
   if (!(a->f = core_fopen(path)))
   {
      free(a);
      return NULL;
   }
   a->map = core_fmap(a->f, &a->map_len);

   path_len = strlen(path);
   if ((a->path = (char*)malloc(path_len + 1)))
      memcpy(a->path, path, path_len + 1);

   if (a->path && (sig = archive_bytes(a, 0, sizeof(sig_buf), sig_buf)))
   {
      if (!memcmp(sig, "7z\xBC\xAF\x27\x1C", 6))
         ok = sz_open(a);
      else if (sig[0] == 'P' && sig[1] == 'K')
         ok = zip_open(a);
   }

   if (!ok)
   {
      archive_close(a);
      return NULL;
   }
   return a;
}

archive_t *archive_open(const char *path)
{
   static const char *const suffixes[] = { "", ".zip", ".ZIP", ".7z", ".7Z" };
   archive_t *a   = NULL;
   size_t     len = strlen(path);
   char      *buf;
   unsigned   i;

   if (!(buf = (char*)malloc(len + 5)))
      return NULL;
   for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]) && !a; i++)
   {
      memcpy(buf, path, len);
      strcpy(buf + len, suffixes[i]);
      a = archive_open_one(buf);
   }
   free(buf);
   return a;
}

void archive_close(archive_t *a)
{
   unsigned i;

   if (!a)
      return;
   for (i = 0; i < a->num_entries && a->cache; i++)
      free(a->cache[i]);
   free(a->cache);
   free(a->view);
   free(a->entries);
   free(a->names);
   free(a->path);
   if (a->sz)
      r7z_archive_close(a->sz);
   free(a->sz_buf);
   if (a->f)
      core_fclose(a->f);
   free(a);
}

const char *archive_path(const archive_t *a)
{
   return a->path;
}

unsigned archive_num_entries(const archive_t *a)
{
   return a->num_entries;
}

const archive_entry_t *archive_entry(const archive_t *a, unsigned index)
{
   return index < a->num_entries ? &a->entries[index] : NULL;
}

int archive_find(const archive_t *a, const char *name)
{
   unsigned i;
   for (i = 0; i < a->num_entries; i++)
      if (!a->entries[i].is_dir && !strcmp(a->entries[i].name, name))
         return (int)i;
   return -1;
}

int archive_find_crc(const archive_t *a, uint32_t crc)
{
   unsigned i;
   if (!crc)
      return -1;
   for (i = 0; i < a->num_entries; i++)
      if (!a->entries[i].is_dir && a->entries[i].crc == crc)
         return (int)i;
   return -1;
}

const uint8_t *archive_entry_data(archive_t *a, unsigned index, size_t *len)
{
   const archive_entry_t *e;

   if (index >= a->num_entries)
      return NULL;
   e = &a->entries[index];
   if (!e->usable || e->size > (uint64_t)((size_t)-1))
      return NULL;
   if (a->cache[index] || a->view[index])
   {
      *len = (size_t)e->size;
      return a->cache[index] ? a->cache[index] : a->view[index];
   }
   if (a->sz)
      return sz_entry_data(a, index, len);
   return zip_entry_data(a, index, len);
}

const uint8_t *archive_entry_map(archive_t *a, unsigned index, size_t *len)
{
   const archive_entry_t *e;

   if (index >= a->num_entries)
      return NULL;
   e = &a->entries[index];
   if (!e->usable || e->size > (uint64_t)((size_t)-1))
      return NULL;
   if (a->cache[index] || a->view[index])
   {
      *len = (size_t)e->size;
      return a->cache[index] ? a->cache[index] : a->view[index];
   }
   if (e->stored && a->map)
      return zip_entry_data(a, index, len);
   return NULL;
}

int archive_resolve_disc(const char *path, char *out, size_t out_len)
{
   static const char *const exts[] = { "gdi", "cue", "chd", "cdi" };
   archive_t *a;
   int        best = -1;
   unsigned   best_rank = sizeof(exts) / sizeof(exts[0]);
   unsigned   i, r;
   size_t     path_len, name_len;

   const char *delim = path_get_archive_delim(path);
   char       *arc   = NULL;

   /* A member already named: kept when it is a disc image itself, else
    * the image is looked for in the same archive. */
   if (delim)
   {
      const char *ext = path_get_extension(delim + 1);
      size_t      len = strlen(path);

      for (r = 0; ext && r < sizeof(exts) / sizeof(exts[0]); r++)
      {
         if (string_is_equal_case_insensitive(ext, exts[r]))
         {
            if (len >= out_len)
               return 0;
            memcpy(out, path, len + 1);
            return 1;
         }
      }
      if (!(arc = (char*)malloc((size_t)(delim - path) + 1)))
         return 0;
      memcpy(arc, path, (size_t)(delim - path));
      arc[delim - path] = '\0';
      path = arc;
   }
   if (!(a = archive_open_one(path)))
   {
      free(arc);
      return 0;
   }

   for (i = 0; i < a->num_entries; i++)
   {
      const archive_entry_t *e   = &a->entries[i];
      const char            *ext = path_get_extension(e->name);

      if (!e->usable || !ext)
         continue;
      for (r = 0; r < best_rank; r++)
      {
         if (string_is_equal_case_insensitive(ext, exts[r]))
         {
            best      = (int)i;
            best_rank = r;
            break;
         }
      }
   }

   path_len = strlen(path);
   name_len = best < 0 ? 0 : strlen(a->entries[best].name);
   if (best < 0 || path_len + 1 + name_len >= out_len)
   {
      archive_close(a);
      free(arc);
      return 0;
   }
   memcpy(out, path, path_len);
   out[path_len] = '#';
   memcpy(out + path_len + 1, a->entries[best].name, name_len + 1);
   archive_close(a);
   free(arc);
   return 1;
}
