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
#include <encodings/utf.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <zip/rzip_archive.h>

#include "archive.h"
#include "deps/coreio/coreio.h"

struct archive
{
   core_file       *f;
   const uint8_t   *map;      /* the whole file, when addressable      */
   r7z_archive_t   *sz;
   rzip_archive_t  *zip;
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

static int64_t zip_read(void *ud, uint64_t off, void *dst, size_t len)
{
   return (int64_t)core_fread_at((core_file*)ud, off, dst, len);
}

static int zip_open(archive_t *a)
{
   uint32_t n, i;
   size_t   name_bytes = 0;
   char    *name_out;

   if (rzip_archive_open(&a->zip, a->map, core_fsize(a->f),
            a->map ? NULL : zip_read, a->f) != RZIP_OK)
      return 0;

   n = rzip_archive_num_entries(a->zip);
   for (i = 0; i < n; i++)
      name_bytes += strlen(rzip_archive_entry(a->zip, i)->name) + 1;
   if (!archive_alloc_tables(a, n, name_bytes))
      return 0;

   name_out = a->names;
   for (i = 0; i < n; i++)
   {
      const rzip_entry_t *ze = rzip_archive_entry(a->zip, i);
      archive_entry_t    *e  = &a->entries[i];
      size_t              nl = strlen(ze->name);

      memcpy(name_out, ze->name, nl + 1);
      e->name     = name_out;
      name_out   += nl + 1;
      e->size     = ze->size;
      e->csize    = ze->csize;
      e->data_off = ze->data_off;
      e->crc      = ze->crc;
      e->is_dir   = ze->is_dir != 0;
      e->stored   = ze->method == RZIP_METHOD_STORED;
      e->usable   = !e->is_dir
         && (ze->method == RZIP_METHOD_STORED || ze->method == RZIP_METHOD_DEFLATE)
         && ze->csize <= (uint64_t)((size_t)-1)
         && ze->data_off < core_fsize(a->f);
   }
   return 1;
}

static const uint8_t *zip_entry_data(archive_t *a, unsigned index, size_t *len)
{
   const uint8_t *view;
   uint8_t       *out;
   size_t         out_len;

   if ((view = rzip_archive_entry_view(a->zip, index, &out_len)))
   {
      a->view[index] = view;
      *len           = out_len;
      return view;
   }
   if (rzip_archive_extract(a->zip, index, &out, &out_len) != RZIP_OK)
      return NULL;
   a->cache[index] = out;
   *len            = out_len;
   return out;
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
   if (a->zip)
      rzip_archive_close(a->zip);
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
   if (a->zip && e->stored && a->map)
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
