#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>
#include <file/file_path.h>

#include "coreio.h"
#include "archive/archive.h"

/* A mapped file keeps its local VFS handle open for the map's lifetime.
 * A streamed file goes through filestream, which is the frontend's VFS
 * when one was supplied; stream_pos tracks where that stream really is
 * so a sequential read never pays for a seek. */
typedef struct core_file_impl
{
   RFILE                            *f;
   libretro_vfs_implementation_file *lf;
   const uint8_t                    *mem;
   size_t                            size;
   size_t                            pos;
   size_t                            stream_pos;
   size_t                            base;       /* member start in f */
   int                               in_archive; /* holds a ref on it */
   int                               seek_entry; /* the member read through
                                                    the archive's index; -1 */
} core_file_impl;

/* The one archive members are being read from: content loads open a
 * few members of the same archive, so it stays open between them and
 * until the last member file is closed. */
static archive_t *cur_archive;
static unsigned   cur_archive_refs;
static int        cur_archive_release;

static void core_archive_drop(void)
{
   if (cur_archive && !cur_archive_refs)
   {
      archive_close(cur_archive);
      cur_archive = NULL;
   }
}

void core_archive_release(void)
{
   cur_archive_release = 1;
   core_archive_drop();
}

/* Opens "archive#member": the member's bytes in place when the archive
 * can hand them out, else positioned reads of a stored member through
 * a handle of its own. */
static int core_file_open_member(core_file_impl *cf, const char *path,
      const char *delim)
{
   const archive_entry_t *e;
   size_t                 arc_len = (size_t)(delim - path);
   int                    idx;

   if (!cur_archive || strncmp(archive_path(cur_archive), path, arc_len)
         || archive_path(cur_archive)[arc_len])
   {
      char *arc_path;

      if (cur_archive_refs)
         return 0;   /* members of another archive are still open */
      core_archive_drop();
      if (!(arc_path = (char*)malloc(arc_len + 1)))
         return 0;
      memcpy(arc_path, path, arc_len);
      arc_path[arc_len]   = '\0';
      cur_archive         = archive_open(arc_path);
      cur_archive_release = 0;
      free(arc_path);
      if (!cur_archive)
         return 0;
   }

   if ((idx = archive_find(cur_archive, delim + 1)) < 0)
      return 0;
   e = archive_entry(cur_archive, (unsigned)idx);
   if (!e->usable || e->size > (size_t)-1)
      return 0;

   if (!(cf->mem = archive_entry_map(cur_archive, (unsigned)idx, &cf->size)))
   {
      /* A large deflated member is not decoded whole and kept: it is
       * read a piece at a time through an index (archive.h). There is
       * no pointer to all of it, so nothing borrows from it: what reads
       * it gets copies. */
      if (!e->stored && archive_entry_seekable(cur_archive, (unsigned)idx))
      {
         cf->seek_entry = idx;
         cf->size       = (size_t)e->size;
         cf->in_archive = 1;
         cur_archive_refs++;
         return 1;
      }
      if (!e->stored)
      {
         if (!(cf->mem = archive_entry_data(cur_archive, (unsigned)idx, &cf->size)))
            return 0;
         cf->in_archive = 1;
         cur_archive_refs++;
         return 1;
      }
      /* An unmapped archive: read the stored member where it lies. */
      cf->f = filestream_open(archive_path(cur_archive),
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (!cf->f)
         return 0;
      cf->base = (size_t)e->data_off;
      cf->size = (size_t)e->size;
      if (filestream_seek(cf->f, (int64_t)cf->base,
               RETRO_VFS_SEEK_POSITION_START) < 0)
         return 0;
   }
   cf->in_archive = 1;
   cur_archive_refs++;
   return 1;
}

static core_file_impl *core_file_alloc(void)
{
   core_file_impl *cf = (core_file_impl*)malloc(sizeof(*cf));
   if (!cf)
      return NULL;
   cf->f          = NULL;
   cf->lf         = NULL;
   cf->mem        = NULL;
   cf->size       = 0;
   cf->pos        = 0;
   cf->stream_pos = 0;
   cf->base       = 0;
   cf->in_archive = 0;
   cf->seek_entry = -1;
   return cf;
}

/* A file by its path: mapped by this core's own copy of the file layer
 * if that can open it, read through the frontend's (filestream) if not.
 *
 * The order is a choice. A frontend's file layer is asked second, so a
 * frontend that means to serve other bytes for a path that also opens
 * locally is not heard: the local file is mapped. The file layer the
 * frontend hands over (the VFS interface) has no way to ask for a
 * mapping, so going through it first would mean no file is ever mapped
 * under a frontend that supplies one - RetroArch always does - and a
 * disc image would be read and copied where it is now borrowed from the
 * page cache. Every path RetroArch's layer treats specially (cdrom://,
 * smb://, a content URI on Android, a path under UWP) is one this
 * core's copy cannot open, and so goes to the frontend's anyway.
 *
 * What would settle it is a mapping call in the VFS interface: the
 * frontend maps what it means the path to be, and this asks it first.
 * Until there is one, comparing a few bytes of the two would only look
 * like an answer - a patch in the middle of a file would get past it. */
static int core_file_open_path(core_file_impl *cf, const char *path)
{
   int64_t len = 0;

   cf->lf = retro_vfs_file_open_impl(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
   if (cf->lf)
   {
      cf->mem = retro_vfs_file_get_mapped_ptr_impl(cf->lf, &len);
      if (cf->mem && len > 0)
      {
         cf->size = (size_t)len;
         return 1;
      }
      retro_vfs_file_close_impl(cf->lf);
      cf->lf  = NULL;
      cf->mem = NULL;
   }

   cf->f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!cf->f)
      return 0;
   len      = filestream_get_size(cf->f);
   cf->size = len > 0 ? (size_t)len : 0;
   return 1;
}

core_file* core_fopen(const char* filename)
{
   core_file_impl *cf    = core_file_alloc();
   const char     *delim = path_get_archive_delim(filename);
   int             ok;

   if (!cf)
      return NULL;
   if (delim)
      ok = core_file_open_member(cf, filename, delim);
   else
      ok = core_file_open_path(cf, filename);
   if (!ok)
   {
      core_fclose((core_file*)cf);
      return NULL;
   }
   return (core_file*)cf;
}

size_t core_fseek(core_file* fc, size_t offs, size_t origin)
{
   core_file_impl *cf = (core_file_impl*)fc;

   switch (origin)
   {
      case SEEK_SET:
         cf->pos = offs;
         break;
      case SEEK_CUR:
         cf->pos += offs;
         break;
      case SEEK_END:
         cf->pos = cf->size + offs;
         break;
      default:
         return (size_t)-1;
   }
   return 0;
}

size_t core_ftell(core_file* fc)
{
   return ((core_file_impl*)fc)->pos;
}

size_t core_fsize(core_file* fc)
{
   return ((core_file_impl*)fc)->size;
}

const uint8_t* core_fmap(core_file* fc, size_t* len)
{
   core_file_impl *cf = (core_file_impl*)fc;
   if (len)
      *len = cf->mem ? cf->size : 0;
   return cf->mem;
}

void core_fprefetch(core_file* fc, uint64_t offset, size_t len)
{
   core_file_impl *cf = (core_file_impl*)fc;

   if (!cf || offset >= cf->size)
      return;
   if (len > cf->size - (size_t)offset)
      len = cf->size - (size_t)offset;
   if (cf->lf)
      retro_vfs_file_prefetch_impl(cf->lf, offset, len);
   else if (cf->f)
      filestream_prefetch(cf->f, cf->base + offset, len);
   else if (cf->in_archive && cf->mem && cur_archive)
      archive_prefetch(cur_archive, cf->mem + (size_t)offset, len);
}

size_t core_fread_at(core_file* fc, uint64_t offset, void* buff, size_t len)
{
   core_file_impl *cf = (core_file_impl*)fc;
   int64_t got;

   if (offset >= cf->size)
      return 0;
   if (len > cf->size - (size_t)offset)
      len = cf->size - (size_t)offset;

   if (cf->mem)
   {
      memcpy(buff, cf->mem + (size_t)offset, len);
      return len;
   }
   if (cf->seek_entry >= 0)
      return archive_entry_read_at(cur_archive, (unsigned)cf->seek_entry,
            offset, (uint8_t*)buff, len) ? len : 0;

   if (cf->stream_pos != (size_t)offset)
   {
      if (filestream_seek(cf->f, (int64_t)(cf->base + (size_t)offset),
               RETRO_VFS_SEEK_POSITION_START) < 0)
         return 0;
      cf->stream_pos = (size_t)offset;
   }
   got = filestream_read(cf->f, buff, len);
   if (got <= 0)
      return 0;
   cf->stream_pos += (size_t)got;
   return (size_t)got;
}

int core_fread(core_file* fc, void* buff, size_t len)
{
   core_file_impl *cf = (core_file_impl*)fc;
   size_t got         = core_fread_at(fc, cf->pos, buff, len);
   cf->pos           += got;
   return (int)got;
}

int core_fclose(core_file* fc)
{
   core_file_impl *cf = (core_file_impl*)fc;

   if (!cf)
      return 0;
   if (cf->f)
      filestream_close(cf->f);
   if (cf->lf)
      retro_vfs_file_close_impl(cf->lf);
   if (cf->in_archive)
   {
      cur_archive_refs--;
      if (cur_archive_release)
         core_archive_drop();
   }
   free(cf);
   return 0;
}
