/* chd_image: a CD or GD-ROM image in a CHD file, read through
 * libretro-common's rchd. See chd_image.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/rchd.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <retro_dirent.h>
#include <string/stdstring.h>
#include <compat/strl.h>

#include "chd_image.h"

/* Every CD codec chdman writes. rchd treats a codec it was built without
 * as unsupported only when a hunk needs it, so a build that dropped one
 * would open such images and then fail every read; this makes it fail to
 * build instead. */
#if !defined(HAVE_RCHD_DEFLATE) || !defined(HAVE_RCHD_LZMA) \
 || !defined(HAVE_RCHD_FLAC) || !defined(HAVE_RCHD_ZSTD)
#error "rchd must be built with all four CD codecs: see HAVE_RCHD_* in Makefile.common"
#endif

#define CHD_PATH_BUF 4096

/* GD-ROM track metadata as the earliest GD-ROM images record it; rchd.h
 * names only the current 'CHGD' spelling. */
#define CHD_META_GDROM_OLD 0x43484754U /* 'CHGT' */

/* A child CHD references unchanged data in a parent file and a parent
 * can itself be a child; this bounds how many images one chain holds. */
#define CHD_MAX_PARENTS 8

/* Largest single read handed to the decoder; rchd accepts short
 * supplies and asks again. */
#define CHD_IO_CHUNK 65536

/* One image of a chain: the decoder and the file it is fed from. Every
 * request the decoder makes is read through the filestream. */
typedef struct
{
   rchd_t *chd;
   RFILE  *fp;
} chd_src;

struct chd_image
{
   /* chain[0] is the image itself, chain[i + 1] the parent of chain[i] */
   chd_src  chain[CHD_MAX_PARENTS + 1];
   uint8_t *io_buf;
   uint32_t chain_len;
};

static void chd_set_err(char *err, size_t err_len, const char *what,
      const char *path)
{
   if (!err || !err_len)
      return;
   strlcpy(err, "CHD: ", err_len);
   strlcat(err, what, err_len);
   strlcat(err, ": \"", err_len);
   strlcat(err, path, err_len);
   strlcat(err, "\"", err_len);
}

static void chd_src_close(chd_src *src)
{
   if (src->chd)
      rchd_free(src->chd);
   if (src->fp)
      filestream_close(src->fp);
   src->chd = NULL;
   src->fp  = NULL;
}

/* Satisfies one request from @src, or the first CHD_IO_CHUNK bytes of
 * it; the decoder asks again for the rest. @reading selects the
 * read-time feed, which names the range it is supplying. */
static bool chd_src_supply(chd_src *src, const rchd_request_t *rq,
      uint8_t *io_buf, bool reading)
{
   int64_t got;

   if (filestream_seek(src->fp, (int64_t)rq->offset,
            RETRO_VFS_SEEK_POSITION_START) < 0)
      return false;
   got = filestream_read(src->fp, io_buf,
         rq->length < CHD_IO_CHUNK ? rq->length : CHD_IO_CHUNK);
   if (got <= 0)
      return false;
   if (reading)
      return rchd_feed_at(src->chd, rq->offset, rq->source,
            io_buf, (size_t)got) == RCHD_OK;
   return rchd_feed(src->chd, io_buf, (size_t)got) == RCHD_OK;
}

/* Opens @path into @src and runs the decoder's open sequence (header,
 * map, metadata). On failure @src is left closed. */
static bool chd_src_open(chd_src *src, const char *path, uint8_t *io_buf)
{
   rchd_request_t rq;
   int            err;

   src->fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!src->fp)
      return false;

   if (!(src->chd = rchd_new()))
   {
      chd_src_close(src);
      return false;
   }

   while ((err = rchd_open_step(src->chd, &rq)) == RCHD_PENDING)
   {
      if (!chd_src_supply(src, &rq, io_buf, false))
         break;
   }

   if (err != RCHD_OK)
   {
      chd_src_close(src);
      return false;
   }
   return true;
}

/* Combined SHA-1 of the CHD at @path, from its header alone. That is the
 * hash a child names its parent by, so this is all a parent search has
 * to read of each candidate. Versions 1 and 2 carry no SHA-1. */
static bool chd_peek_sha1(const char *path, uint8_t *sha1)
{
   uint8_t  h[124];
   uint32_t version;
   size_t   at;
   int64_t  got;
   RFILE   *fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!fp)
      return false;
   got = filestream_read(fp, h, sizeof(h));
   filestream_close(fp);

   if (got < 16 || memcmp(h, "MComprHD", 8))
      return false;

   version = ((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16)
           | ((uint32_t)h[14] <<  8) |  (uint32_t)h[15];
   switch (version)
   {
      case 3:  at = 80; break;
      case 4:  at = 48; break;
      case 5:  at = 84; break;
      default: return false;
   }
   if ((size_t)got < at + 20)
      return false;
   memcpy(sha1, h + at, 20);
   return true;
}

/* Search @dir for the parent of @child, matching each candidate's
 * combined SHA-1 against the parent SHA-1 the child records, and open
 * the match into @out. The child's own hash never equals the parent
 * hash it records, so the child is passed over without a path check. */
static bool chd_find_parent_in_dir(const char *dir, const rchd_t *child,
      chd_src *out, uint8_t *io_buf)
{
   struct RDIR *rdir = retro_opendir(dir);
   bool         ok   = false;
   char        *cand;

   if (!rdir)
      return false;
   if (!(cand = (char *)malloc(CHD_PATH_BUF)))
   {
      retro_closedir(rdir);
      return false;
   }

   while (retro_readdir(rdir))
   {
      const char *name = retro_dirent_get_name(rdir);
      const char *ext;
      uint8_t     sha1[20];

      if (!name || retro_dirent_is_dir(rdir, NULL))
         continue;

      ext = path_get_extension(name);
      if (!ext || !string_is_equal_noncase(ext, "chd"))
         continue;

      fill_pathname_join(cand, dir, name, CHD_PATH_BUF);

      if (!chd_peek_sha1(cand, sha1)
            || !rchd_parent_sha1_matches(child, sha1))
         continue;

      ok = chd_src_open(out, cand, io_buf);
      break;
   }

   free(cand);
   retro_closedir(rdir);
   return ok;
}

void chd_image_close(chd_image_t *img)
{
   uint32_t i;

   if (!img)
      return;
   /* A child holds its parent, so the chain closes child first. */
   for (i = 0; i <= CHD_MAX_PARENTS; i++)
      chd_src_close(&img->chain[i]);
   free(img->io_buf);
   free(img);
}

chd_image_t *chd_image_open(const char *path, char *err, size_t err_len)
{
   chd_image_t *img;
   char        *base_dir;

   if (!(img = (chd_image_t *)calloc(1, sizeof(*img))))
      return NULL;
   if (!(img->io_buf = (uint8_t *)malloc(CHD_IO_CHUNK))
         || !(base_dir = (char *)malloc(CHD_PATH_BUF)))
   {
      chd_image_close(img);
      return NULL;
   }

   if (!chd_src_open(&img->chain[0], path, img->io_buf))
   {
      chd_set_err(err, err_len, "failed to open", path);
      free(base_dir);
      chd_image_close(img);
      return NULL;
   }
   img->chain_len = 1;

   base_dir[0] = '\0';
   fill_pathname_basedir(base_dir, path, CHD_PATH_BUF);

   while (rchd_info(img->chain[img->chain_len - 1].chd)->has_parent)
   {
      rchd_t *child = img->chain[img->chain_len - 1].chd;

      if (img->chain_len > CHD_MAX_PARENTS
            || !chd_find_parent_in_dir(base_dir, child,
               &img->chain[img->chain_len], img->io_buf)
            || rchd_set_parent(child,
               img->chain[img->chain_len].chd) != RCHD_OK)
      {
         chd_set_err(err, err_len,
               "parent CHD not found in the same directory", path);
         free(base_dir);
         chd_image_close(img);
         return NULL;
      }
      img->chain_len++;
   }
   free(base_dir);
   return img;
}

uint32_t chd_image_hunk_bytes(const chd_image_t *img)
{
   return rchd_info(img->chain[0].chd)->hunk_bytes;
}

uint32_t chd_image_hunk_count(const chd_image_t *img)
{
   return rchd_info(img->chain[0].chd)->hunk_count;
}

uint32_t chd_image_version(const chd_image_t *img)
{
   return rchd_info(img->chain[0].chd)->version;
}

/* Copies the @n'th @tag metadata entry, NUL-terminated, into @out. */
static bool chd_meta_text(const rchd_t *chd, uint32_t tag, uint32_t n,
      char *out, size_t out_size)
{
   const rchd_metadata_t *m = rchd_metadata_find(chd, tag, n);
   size_t                 len;

   if (!m)
      return false;
   len = m->length < out_size - 1 ? m->length : out_size - 1;
   memcpy(out, m->data, len);
   out[len] = '\0';
   return true;
}

bool chd_image_track(const chd_image_t *img, uint32_t index,
      chd_image_track_t *out)
{
   const rchd_t *chd = img->chain[0].chd;
   char          meta[256];

   memset(out, 0, sizeof(*out));

   /* Field widths are sizeof(dest) - 1, so no metadata string, however
    * long, writes past the buffers. */
   if (chd_meta_text(chd, RCHD_META_CDROM_TRACK2, index, meta, sizeof(meta)))
      sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%31s FRAMES:%d PREGAP:%d "
            "PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
            &out->track, out->type, out->subtype, &out->frames,
            &out->pregap, out->pgtype, out->pgsub, &out->postgap);
   else if (chd_meta_text(chd, RCHD_META_CDROM_TRACK, index, meta,
            sizeof(meta)))
      sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%31s FRAMES:%d",
            &out->track, out->type, out->subtype, &out->frames);
   else if (chd_meta_text(chd, CHD_META_GDROM_OLD, index, meta, sizeof(meta))
         || chd_meta_text(chd, RCHD_META_GDROM_TRACK, index, meta,
            sizeof(meta)))
      sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%31s FRAMES:%d PAD:%d "
            "PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
            &out->track, out->type, out->subtype, &out->frames, &out->pad,
            &out->pregap, out->pgtype, out->pgsub, &out->postgap);
   else
      return false;
   return true;
}

/* Requests for a hunk that a child shares with its parent are made by
 * the parent's decoder, so the level whose request is outstanding is the
 * one whose file is read. */
bool chd_image_read_hunk(chd_image_t *img, uint32_t hunk, uint8_t *dst)
{
   rchd_request_t rq;
   int            err = rchd_read_hunk_begin(img->chain[0].chd, hunk, dst);

   while (err == RCHD_OK
         && (err = rchd_read_step(img->chain[0].chd, &rq)) == RCHD_PENDING)
   {
      uint32_t lvl;

      for (lvl = 0; lvl < img->chain_len; lvl++)
         if (rchd_read_pending(img->chain[lvl].chd, &rq, 1))
            break;

      if (lvl == img->chain_len)
         err = RCHD_ERROR_STATE;
      else if (!chd_src_supply(&img->chain[lvl], &rq, img->io_buf, true))
         err = RCHD_ERROR_DATA;
      else
         err = RCHD_OK;
   }
   return err == RCHD_OK;
}
