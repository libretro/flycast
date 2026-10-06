#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

#include "coreio.h"

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
} core_file_impl;

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
   return cf;
}

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
   core_file_impl *cf = core_file_alloc();
   if (!cf)
      return NULL;
   if (!core_file_open_path(cf, filename))
   {
      free(cf);
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

   if (cf->stream_pos != (size_t)offset)
   {
      if (filestream_seek(cf->f, (int64_t)offset,
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
   free(cf);
   return 0;
}
