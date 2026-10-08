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
#include <rar/rrar_archive.h>

#include <retro_atomic.h>
#ifndef TARGET_NO_THREADS
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#endif

#include "archive.h"
#include "deps/coreio/coreio.h"

/* A deflated zip member at least this long is read through an index,
 * not decoded whole (archive_entry_seekable()). Below it the whole
 * member is cheap to have, and can be lent out in place. */
#ifndef ARCHIVE_SEEK_MIN
#define ARCHIVE_SEEK_MIN (16 * 1024 * 1024)
#endif
/* How much the indexing thread does between looks at whether to stop. */
#define ARCHIVE_SEEK_STEP (4 * 1024 * 1024)

/* A 7z folder of at least ARCHIVE_SEEK_MIN that is decoded on a thread of
 * its own, a slice at a time, while the game loads and runs: see
 * archive_bg_get(). */
typedef struct archive_bg
{
   r7z_archive_t      *sz;        /* the thread's own view of the archive */
   uint32_t            folder;
   unsigned            index;     /* an entry of it */
#ifndef TARGET_NO_THREADS
   sthread_t          *thread;
   retro_eventcount_t  progress;  /* notified as more is decoded */
   retro_atomic_int_t  stop;
   retro_atomic_int_t  state;     /* 0 decoding, 1 decoded and held, -1 not to be */
#endif
} archive_bg_t;

/* A compressed rar member of at least ARCHIVE_SEEK_MIN that is decoded
 * on a thread of its own while the game loads and runs: see
 * archive_unrar_get(). */
typedef struct archive_unrar
{
   struct archive     *a;
   unsigned            index;
   uint8_t            *buf;       /* the member, decoded into from its start */
   size_t              size;
#ifndef TARGET_NO_THREADS
   sthread_t          *thread;
   retro_eventcount_t  progress;  /* notified as more of it is final */
   retro_atomic_size_t done;      /* how much of it is */
   retro_atomic_int_t  stop;
   retro_atomic_int_t  state;     /* 0 decoding, 1 decoded and checked, -1 not to be */
#endif
} archive_unrar_t;

typedef struct archive_seek
{
   rzip_seek_t        *seek;
#ifndef TARGET_NO_THREADS
   sthread_t          *thread;    /* indexes the member; NULL: the reader does */
   retro_eventcount_t  progress;  /* notified as the index grows */
   retro_atomic_int_t  stop;
   int                 progress_ok;
#endif
} archive_seek_t;

struct archive
{
   core_file       *f;
   const uint8_t   *map;      /* the whole file, when addressable      */
   r7z_archive_t   *sz;
   rzip_archive_t  *zip;
   rrar_archive_t  *rar;
   uint32_t         sz_folders;
   archive_entry_t *entries;
   uint8_t        **cache;    /* decoded members, one slot per entry   */
   const uint8_t  **view;     /* members borrowed from a mapping or a
                                 decoded folder, one slot per entry    */
   archive_seek_t **seek;     /* members read through an index, one slot
                                 per entry, made when first asked for  */
   archive_bg_t   **bg;       /* 7z folders being decoded on a thread   */
   unsigned         num_bg;
   archive_unrar_t **unrar;   /* rar members being decoded on a thread,
                                 one slot per entry                     */
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

/* ------------------------------------------- 7z folders decoded behind */

#ifndef TARGET_NO_THREADS
static void archive_bg_thread(void *data)
{
   archive_bg_t *bg = (archive_bg_t*)data;

   while (!retro_atomic_load_acquire_int(&bg->stop))
   {
      const int res = r7z_archive_decode_step(bg->sz, bg->index);

      if (res != R7Z_PENDING)
      {
         retro_atomic_store_release_int(&bg->state, res == R7Z_OK ? 1 : -1);
         retro_eventcount_notify(&bg->progress);
         break;
      }
      retro_eventcount_notify(&bg->progress);
   }
}

/* The decode of the folder that holds member @index, started if the
 * folder is large enough to be worth not waiting for: NULL if it is not,
 * or cannot be decoded behind.
 *
 * A solid 7z is decoded from its start to its end and no other way.
 * Done when the first member is asked for, that is the whole of it
 * before the game is loaded - tens of seconds for a disc image, with
 * the frontend stopped. Done on a thread, the members become readable
 * as the decoder passes them: the descriptor and the first tracks at
 * once, the rest while the game starts. The thread has the archive open
 * a second time for itself (the reader decodes one folder at a time),
 * which takes the archive being mapped: the two then share only its
 * bytes. */
static archive_bg_t *archive_bg_get(archive_t *a, unsigned index, int start)
{
   const r7z_entry_t *se;
   archive_bg_t      *bg;
   archive_bg_t     **list;
   unsigned           i;

   if (!a->sz || !(se = r7z_archive_entry(a->sz, index)) || se->is_dir || !se->size)
      return NULL;
   for (i = 0; i < a->num_bg; i++)
   {
      if (a->bg[i]->folder == se->folder)
         return a->bg[i];
   }
   if (     !start || !a->map
         || r7z_archive_folder_size(a->sz, se->folder) < ARCHIVE_SEEK_MIN)
      return NULL;

   if (!(list = (archive_bg_t**)realloc(a->bg, (a->num_bg + 1) * sizeof(*list))))
      return NULL;
   a->bg = list;
   if (!(bg = (archive_bg_t*)calloc(1, sizeof(*bg))))
      return NULL;
   bg->folder = se->folder;
   bg->index  = index;
   retro_atomic_int_init(&bg->stop, 0);
   retro_atomic_int_init(&bg->state, 0);
   if (r7z_archive_open(&bg->sz, a->map, a->map_len) != R7Z_OK)
   {
      free(bg);
      return NULL;
   }
   if (!retro_eventcount_init(&bg->progress))
   {
      r7z_archive_close(bg->sz);
      free(bg);
      return NULL;
   }
   if (!(bg->thread = sthread_create(archive_bg_thread, bg)))
   {
      retro_eventcount_free(&bg->progress);
      r7z_archive_close(bg->sz);
      free(bg);
      return NULL;
   }
   a->bg[a->num_bg++] = bg;
   return bg;
}

/* @len bytes at @offset of member @index, where the folder is being
 * decoded: once the decoder has passed them. NULL if it never will. */
static const uint8_t *archive_bg_reach(archive_bg_t *bg, unsigned index,
      uint64_t offset, size_t len)
{
   for (;;)
   {
      const uint8_t *data  = NULL;
      size_t         avail = 0;
      int            key;

      if (r7z_archive_decode_peek(bg->sz, index, &data, &avail) != R7Z_OK)
         return NULL;
      if (data && offset <= avail && len <= avail - (size_t)offset)
         return data + (size_t)offset;
      if (retro_atomic_load_acquire_int(&bg->state) != 0)
      {
         /* done, or failed: what there is now is all there will be */
         if (     r7z_archive_decode_peek(bg->sz, index, &data, &avail) == R7Z_OK
               && data && offset <= avail && len <= avail - (size_t)offset
               && retro_atomic_load_acquire_int(&bg->state) > 0)
            return data + (size_t)offset;
         return NULL;
      }
      key = retro_eventcount_prepare_wait(&bg->progress);
      if (     r7z_archive_decode_peek(bg->sz, index, &data, &avail) != R7Z_OK
            || (data && offset <= avail && len <= avail - (size_t)offset)
            || retro_atomic_load_acquire_int(&bg->state) != 0)
         retro_eventcount_cancel_wait(&bg->progress);
      else
         retro_eventcount_commit_wait(&bg->progress, key);
   }
}

static void archive_bg_close(archive_bg_t *bg)
{
   if (!bg)
      return;
   retro_atomic_store_release_int(&bg->stop, 1);
   sthread_join(bg->thread);
   retro_eventcount_free(&bg->progress);
   r7z_archive_close(bg->sz);
   free(bg);
}
#endif

/* ------------------------------------------- rar members decoded behind */

#ifndef TARGET_NO_THREADS
/* The decoder says how much of the member is final (on its thread). */
static int archive_unrar_progress(void *ud, size_t done)
{
   archive_unrar_t *k = (archive_unrar_t*)ud;

   retro_atomic_store_release_size(&k->done, done);
   retro_eventcount_notify(&k->progress);
   return retro_atomic_load_acquire_int(&k->stop);
}

static void archive_unrar_thread(void *data);

/* The decode of rar member @index on a thread, started if it is a
 * compressed member large enough to be worth not waiting for: NULL if it
 * is not, or cannot be. As a large 7z folder is decoded
 * (archive_bg_get()): the member becomes readable from its start as the
 * decoder gets on, and the game is loaded and running meanwhile. The
 * rar reader keeps nothing of a decode in the archive, so the thread
 * uses the archive as it is; it has to be mapped, so that the two share
 * only its bytes. */
static archive_unrar_t *archive_unrar_get(archive_t *a, unsigned index, int start)
{
   const archive_entry_t *e;
   archive_unrar_t       *k;

   if (!a->rar || index >= a->num_entries)
      return NULL;
   if (a->unrar && a->unrar[index])
      return a->unrar[index];
   e = &a->entries[index];
   if (     !start || !a->map || !e->usable || e->stored
         || e->size < ARCHIVE_SEEK_MIN || e->size > (uint64_t)((size_t)-1))
      return NULL;
   if (     !a->unrar
         && !(a->unrar = (archive_unrar_t**)calloc(a->num_entries, sizeof(*a->unrar))))
      return NULL;
   if (!(k = (archive_unrar_t*)calloc(1, sizeof(*k))))
      return NULL;
   k->a     = a;
   k->index = index;
   k->size  = (size_t)e->size;
   retro_atomic_size_init(&k->done, 0);
   retro_atomic_int_init(&k->stop, 0);
   retro_atomic_int_init(&k->state, 0);
   if (!(k->buf = (uint8_t*)malloc(k->size)))
   {
      free(k);
      return NULL;
   }
   if (!retro_eventcount_init(&k->progress))
   {
      free(k->buf);
      free(k);
      return NULL;
   }
   if (!(k->thread = sthread_create(archive_unrar_thread, k)))
   {
      retro_eventcount_free(&k->progress);
      free(k->buf);
      free(k);
      return NULL;
   }
   a->unrar[index] = k;
   return k;
}

/* @len bytes at @offset of the member, once they are final. NULL if they
 * never will be. */
static const uint8_t *archive_unrar_reach(archive_unrar_t *k, uint64_t offset,
      size_t len)
{
   if (offset > k->size || len > k->size - (size_t)offset)
      return NULL;
   for (;;)
   {
      const size_t need = (size_t)offset + len;
      int          key;

      if (retro_atomic_load_acquire_size(&k->done) >= need)
         return k->buf + (size_t)offset;
      if (retro_atomic_load_acquire_int(&k->state) != 0)
         /* done, or failed: what is final now is all that will be */
         return (   retro_atomic_load_acquire_int(&k->state) > 0
                 && retro_atomic_load_acquire_size(&k->done) >= need)
            ? k->buf + (size_t)offset : NULL;
      key = retro_eventcount_prepare_wait(&k->progress);
      if (     retro_atomic_load_acquire_size(&k->done) >= need
            || retro_atomic_load_acquire_int(&k->state) != 0)
         retro_eventcount_cancel_wait(&k->progress);
      else
         retro_eventcount_commit_wait(&k->progress, key);
   }
}

static void archive_unrar_close(archive_unrar_t *k)
{
   if (!k)
      return;
   retro_atomic_store_release_int(&k->stop, 1);
   sthread_join(k->thread);
   retro_eventcount_free(&k->progress);
   free(k->buf);
   free(k);
}
#endif

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

   /* In place when the file is mapped. When it is not, it is read
    * through the same positioned reads the zip reader uses: the header
    * at open, a folder's packed bytes when the folder is decoded - not
    * the whole file into memory, to be kept there beside everything
    * decoded from it. */
   if (a->map)
   {
      data = a->map;
      len  = a->map_len;
      if (r7z_archive_open(&a->sz, data, len) != R7Z_OK)
         return 0;
   }
   else if (r7z_archive_open_read(&a->sz, core_fsize(a->f), zip_read,
            a->f) != R7Z_OK)
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

#ifndef TARGET_NO_THREADS
   /* A member of a large folder: the folder is decoded behind, once, and
    * this waits until the decoder has passed the member. */
   {
      archive_bg_t *bg = archive_bg_get(a, index, 1);

      if (bg)
      {
         const uint8_t *view = archive_bg_reach(bg, index, 0,
               (size_t)a->entries[index].size);

         if (!view)
            return NULL;
         a->view[index] = view;
         *len           = (size_t)a->entries[index].size;
         return view;
      }
   }
#endif

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

/* --------------------------------------------------------------- rar */

/* A RAR archive is read through libretro-common's rrar_archive, which is
 * handed the whole file: the mapping, or the file read into memory. A
 * stored member of a mapped archive is the mapping itself; any other is
 * unpacked when it is asked for and kept, like a deflated one. */
static int rar_open(archive_t *a)
{
   const uint8_t *data;
   size_t         len = 0;
   size_t         name_bytes = 0;
   unsigned       n, i;
   char          *name_out;

   /* In place when the file is mapped; read as it is needed when it is
    * not - its headers one at a time now, a member's bytes when the
    * member is asked for - as the zip and the 7z are. */
   if (a->map)
   {
      data = a->map;
      len  = a->map_len;
   }
   else
   {
      data = NULL;
      len  = core_fsize(a->f);
   }

   if ((data
            ? rrar_archive_open(&a->rar, data, len)
            : rrar_archive_open_read(&a->rar, len, zip_read, a->f)) != RRAR_OK)
      return 0;

   n = rrar_archive_num_entries(a->rar);
   for (i = 0; i < n; i++)
      name_bytes += strlen(rrar_archive_entry(a->rar, i)->name) + 1;
   if (!archive_alloc_tables(a, n, name_bytes))
      return 0;

   name_out = a->names;
   for (i = 0; i < n; i++)
   {
      const rrar_entry_t *re = rrar_archive_entry(a->rar, i);
      archive_entry_t    *e  = &a->entries[i];
      size_t              nl = strlen(re->name);

      memcpy(name_out, re->name, nl + 1);
      e->name     = name_out;
      name_out   += nl + 1;
      e->size     = re->size;
      e->csize    = re->packed_size;
      e->data_off = re->data_offset;
      e->crc      = re->crc;
      e->is_dir   = re->is_dir != 0;
      e->stored   = re->method == 0x30;
      e->usable   = re->supported != 0;
   }
   return 1;
}

static const uint8_t *rar_entry_data(archive_t *a, unsigned index, size_t *len)
{
   const archive_entry_t *e = &a->entries[index];
   uint8_t *out;
   size_t   out_len;

   if (e->stored && a->map)
   {
      /* in place (its checksum is the archive's word for it, as a
       * stored zip member's is) */
      a->view[index] = a->map + (size_t)e->data_off;
      *len           = (size_t)e->size;
      return a->view[index];
   }
#ifndef TARGET_NO_THREADS
   /* being decoded behind: all of it is waited for, not decoded again */
   {
      archive_unrar_t *k = archive_unrar_get(a, index, 0);

      if (k)
      {
         const uint8_t *view = archive_unrar_reach(k, 0, k->size);

         if (!view || retro_atomic_load_acquire_int(&k->state) <= 0)
         {
            /* (the end has been decoded, but not yet checked: wait) */
            while (retro_atomic_load_acquire_int(&k->state) == 0)
            {
               const int key = retro_eventcount_prepare_wait(&k->progress);
               if (retro_atomic_load_acquire_int(&k->state) != 0)
                  retro_eventcount_cancel_wait(&k->progress);
               else
                  retro_eventcount_commit_wait(&k->progress, key);
            }
            if (retro_atomic_load_acquire_int(&k->state) <= 0)
               return NULL;
            view = k->buf;
         }
         a->view[index] = view;
         *len           = k->size;
         return view;
      }
   }
#endif
   if (rrar_archive_extract(a->rar, index, &out, &out_len) != RRAR_OK)
      return NULL;
   a->cache[index] = out;
   *len            = out_len;
   return out;
}

#ifndef TARGET_NO_THREADS
static void archive_unrar_thread(void *data)
{
   archive_unrar_t *k = (archive_unrar_t*)data;
   rrar_watch_t     watch;
   int              res;

   watch.progress = archive_unrar_progress;
   watch.ud       = k;
   res = rrar_archive_extract_to(k->a->rar, k->index, k->buf, k->size, &watch);
   /* (a member that fails its checksum at the end is not handed out
    * any more: what was read of it before was not known to be bad) */
   retro_atomic_store_release_int(&k->state, res == RRAR_OK ? 1 : -1);
   retro_eventcount_notify(&k->progress);
}
#endif

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
      else if (!memcmp(sig, "Rar!\x1a\x07", 6))
         ok = rar_open(a);
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
   static const char *const suffixes[] = { "", ".zip", ".ZIP", ".7z", ".7Z", ".rar", ".RAR" };
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

#ifndef TARGET_NO_THREADS
/* The indexing thread: on through the member a step at a time, saying so
 * after each, until it is done, the stream turns out bad, or the archive
 * is being closed. */
static void archive_seek_thread(void *data)
{
   archive_seek_t *k = (archive_seek_t*)data;

   while (!retro_atomic_load_acquire_int(&k->stop))
   {
      const int state = rzip_seek_build(k->seek,
            rzip_seek_covered(k->seek) + ARCHIVE_SEEK_STEP);

      retro_eventcount_notify(&k->progress);
      if (state != 0)
         break;
   }
}
#endif

static void archive_seek_close(archive_seek_t *k)
{
   if (!k)
      return;
#ifndef TARGET_NO_THREADS
   if (k->thread)
   {
      retro_atomic_store_release_int(&k->stop, 1);
      sthread_join(k->thread);
   }
   if (k->progress_ok)
      retro_eventcount_free(&k->progress);
#endif
   rzip_seek_free(k->seek);
   free(k);
}

int archive_entry_seekable(archive_t *a, unsigned index)
{
   const archive_entry_t *e;
   archive_seek_t        *k;

   if (index >= a->num_entries)
      return 0;
#ifndef TARGET_NO_THREADS
   /* a member of a 7z folder that is decoded behind: any size, since it
    * is the folder that takes the time */
   if (a->sz)
      return a->entries[index].usable && !a->cache[index] && !a->view[index]
         && archive_bg_get(a, index, 1) != NULL;
   /* a large compressed rar member */
   if (a->rar)
      return !a->cache[index] && !a->view[index]
         && archive_unrar_get(a, index, 1) != NULL;
#endif
   if (!a->zip)
      return 0;
   e = &a->entries[index];
   if (!e->usable || e->stored || e->size < ARCHIVE_SEEK_MIN)
      return 0;
   /* decoded for someone already: there is nothing left to save */
   if (a->cache[index])
      return 0;
   if (a->seek && a->seek[index])
      return 1;
   if (     !a->seek
         && !(a->seek = (archive_seek_t**)calloc(a->num_entries, sizeof(*a->seek))))
      return 0;
   if (!(k = (archive_seek_t*)calloc(1, sizeof(*k))))
      return 0;
   if (!(k->seek = rzip_seek_new(a->zip, index, 0)))
   {
      free(k);
      return 0;
   }
#ifndef TARGET_NO_THREADS
   /* A thread of its own only for a mapped archive: the index and the
    * reader then share nothing but the mapping. An archive that is read
    * has one file position for both. */
   retro_atomic_int_init(&k->stop, 0);
   if (a->map && retro_eventcount_init(&k->progress))
   {
      k->progress_ok = 1;
      k->thread      = sthread_create(archive_seek_thread, k);
   }
#endif
   a->seek[index] = k;
   return 1;
}

/* The member's index, once it covers @len bytes at @offset: waits for
 * the indexing thread, or does the indexing here when there is none.
 * NULL if the bytes are not the member's or its stream is bad. */
static archive_seek_t *archive_seek_reach(archive_t *a, unsigned index,
      uint64_t offset, size_t len)
{
   archive_seek_t *k;
   uint64_t        need;

   if (index >= a->num_entries || !a->seek || !(k = a->seek[index]))
      return NULL;
   if (offset > rzip_seek_size(k->seek) || len > rzip_seek_size(k->seek) - offset)
      return NULL;
   need = offset + len;

   /* Until the index has got that far. */
   while (rzip_seek_covered(k->seek) < need)
   {
      if (rzip_seek_state(k->seek) < 0)
         return NULL;
#ifndef TARGET_NO_THREADS
      if (k->thread)
      {
         const int key = retro_eventcount_prepare_wait(&k->progress);

         if (     rzip_seek_covered(k->seek) >= need
               || rzip_seek_state(k->seek) != 0)
            retro_eventcount_cancel_wait(&k->progress);
         else
            retro_eventcount_commit_wait(&k->progress, key);
         continue;
      }
#endif
      if (rzip_seek_build(k->seek, need) < 0)
         return NULL;
   }
   return k;
}

int archive_entry_read_at(archive_t *a, unsigned index, uint64_t offset,
      uint8_t *dst, size_t len)
{
   archive_seek_t *k;

#ifndef TARGET_NO_THREADS
   if (a->sz || a->rar)
   {
      const uint8_t *p = archive_entry_view_at(a, index, offset, len);

      if (!p)
         return 0;
      memcpy(dst, p, len);
      return 1;
   }
#endif
   k = archive_seek_reach(a, index, offset, len);

   return k && rzip_seek_read(k->seek, offset, dst, len) == RZIP_OK;
}

const uint8_t *archive_entry_view_at(archive_t *a, unsigned index,
      uint64_t offset, size_t len)
{
   archive_seek_t *k;
   const uint8_t  *data = NULL;

#ifndef TARGET_NO_THREADS
   if (a->sz)
   {
      archive_bg_t *bg = index < a->num_entries ? archive_bg_get(a, index, 0) : NULL;

      if (     !bg || offset > a->entries[index].size
            || len > a->entries[index].size - offset)
         return NULL;
      return archive_bg_reach(bg, index, offset, len);
   }
   if (a->rar)
   {
      archive_unrar_t *k = archive_unrar_get(a, index, 0);

      return k ? archive_unrar_reach(k, offset, len) : NULL;
   }
#endif
   k = archive_seek_reach(a, index, offset, len);

   if (!k || rzip_seek_view(k->seek, offset, len, &data) != RZIP_OK)
      return NULL;
   return data;
}

void archive_close(archive_t *a)
{
   unsigned i;

   if (!a)
      return;
   for (i = 0; i < a->num_entries && a->seek; i++)
      archive_seek_close(a->seek[i]);
   free(a->seek);
#ifndef TARGET_NO_THREADS
   /* (before the mapping they decode from goes) */
   for (i = 0; i < a->num_bg; i++)
      archive_bg_close(a->bg[i]);
   for (i = 0; i < a->num_entries && a->unrar; i++)
      archive_unrar_close(a->unrar[i]);
#endif
   free(a->bg);
   free(a->unrar);
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
   if (a->rar)
      rrar_archive_close(a->rar);
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
   if (a->rar)
      return rar_entry_data(a, index, len);
   return zip_entry_data(a, index, len);
}

int archive_entry_read(archive_t *a, unsigned index, uint8_t *dst,
      size_t dst_size, size_t *len)
{
   const archive_entry_t *e;
   const uint8_t         *src;
   size_t                 src_len = 0;

   if (index >= a->num_entries)
      return 0;
   e = &a->entries[index];
   if (!e->usable || e->size > (uint64_t)dst_size)
      return 0;

   /* Already to hand - in the mapping, or decoded for someone else - or
    * a zip member, which can be decoded where it is wanted. A 7z or rar
    * member is decoded the usual way and copied: their readers have no
    * call for it, and a solid one is decoded with its neighbours. */
   if (!(src = archive_entry_map(a, index, &src_len)))
   {
      if (a->zip)
         return rzip_archive_extract_into(a->zip, index, dst, dst_size, len) == RZIP_OK;
      if (!(src = archive_entry_data(a, index, &src_len)))
         return 0;
   }
   memcpy(dst, src, src_len);
   *len = src_len;
   return 1;
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
   if (a->rar && e->stored && a->map)
      return rar_entry_data(a, index, len);
#ifndef TARGET_NO_THREADS
   /* A rar member that was decoded behind, and is done. */
   if (a->rar)
   {
      archive_unrar_t *k = archive_unrar_get(a, index, 0);

      if (k && retro_atomic_load_acquire_int(&k->state) > 0)
      {
         a->view[index] = k->buf;
         *len           = k->size;
         return k->buf;
      }
   }
   /* A 7z member whose folder is being decoded behind, and which the
    * decoder has passed: it is where it will stay. */
   if (a->sz)
   {
      archive_bg_t  *bg    = archive_bg_get(a, index, 0);
      const uint8_t *data  = NULL;
      size_t         avail = 0;

      if (     bg
            && r7z_archive_decode_peek(bg->sz, index, &data, &avail) == R7Z_OK
            && data && avail == (size_t)e->size)
      {
         a->view[index] = data;
         *len           = avail;
         return data;
      }
   }
#endif
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
   /* (A rar is looked through like the others: a member of one has an
    * "archive#member" name the file layer knows, since libretro-common's
    * path_get_archive_delim took ".rar" in.) */
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

void archive_prefetch(archive_t *a, const uint8_t *p, size_t len)
{
   if (!a || !a->map || p < a->map || p >= a->map + a->map_len)
      return;
   core_fprefetch(a->f, (uint64_t)(p - a->map), len);
}
