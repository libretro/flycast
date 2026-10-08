/* Archive and coreio check against the files an archive was made from.
 *
 *    archive_test <workdir>
 *
 * <workdir> is what tools/archive/make_fixture.py wrote. Every member of
 * every archive is compared byte for byte with its source, through the
 * archive API and through core_fopen("archive#member"): whole-member
 * reads, positioned reads at odd offsets and sizes, reads straddling the
 * member's end, and the cursor-based read. A stored member of a mapped
 * zip must come back as a pointer into the mapping, a decoded member
 * must not, and the archive's recorded CRC must match the bytes. The
 * disc pick must find the .gdi behind a decoy .cue, and a path missing
 * its .zip must still open.
 *
 * Built with and without HAVE_MMAP by run.sh, so both the mapped path
 * and the read-in-place path are exercised.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <encodings/crc32.h>
#include <file/file_path.h>

#include "../../core/archive/archive.h"
#include "../../core/deps/coreio/coreio.h"

static const char *const members[] =
{ "disc.gdi", "track01.bin", "track02.raw", "track03.bin" };
#define NUM_MEMBERS (sizeof(members) / sizeof(members[0]))

static int failures;

/* a + b + c into dst */
static void join3(char *dst, size_t len, const char *a, const char *b,
      const char *c)
{
   strlcpy(dst, a, len);
   strlcat(dst, b, len);
   strlcat(dst, c, len);
}

#define CHECK(cond, what) \
   do { if (!(cond)) { printf("FAIL %s: %s\n", what, #cond); failures++; } } while (0)

static uint8_t *read_file(const char *path, size_t *len)
{
   FILE    *f = fopen(path, "rb");
   uint8_t *buf;
   long     n;

   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
   if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      free(buf);
      buf = NULL;
   }
   fclose(f);
   *len = (size_t)n;
   return buf;
}

/* Positioned reads at sizes and offsets that do not line up with
 * anything: sector-sized, tiny, crossing the end. */
static void check_reads(core_file *cf, const uint8_t *want, size_t len,
      const char *what)
{
   uint8_t *buf = (uint8_t*)malloc(70000);
   size_t   got, off;
   size_t   i;

   CHECK(core_fsize(cf) == len, what);

   /* the read-ahead hint, in every shape: a hint changes no bytes */
   core_fprefetch(cf, 0, len);
   core_fprefetch(cf, len / 2, len);
   core_fprefetch(cf, len, 4096);
   core_fprefetch(cf, 0, 0);

   for (i = 0; i < 64 && len; i++)
   {
      size_t n = (i * 2351 + 7) % 70000 + 1;
      off = (i * 1000003u) % len;
      got = core_fread_at(cf, off, buf, n);
      CHECK(got == (n < len - off ? n : len - off), what);
      CHECK(!memcmp(buf, want + off, got), what);
   }

   /* past the end */
   got = core_fread_at(cf, len, buf, 16);
   CHECK(got == 0, what);
   got = core_fread_at(cf, len + 100, buf, 16);
   CHECK(got == 0, what);

   /* the cursor form */
   core_fseek(cf, 0, SEEK_SET);
   for (off = 0; off < len; off += got)
   {
      got = (size_t)core_fread(cf, buf, 65536);
      if (!got)
         break;
      CHECK(!memcmp(buf, want + off, got), what);
      CHECK(core_ftell(cf) == off + got, what);
   }
   CHECK(off == len, what);
   core_fseek(cf, (size_t)-12, SEEK_END);
   CHECK(core_ftell(cf) == len - 12, what);
   free(buf);
}

static void check_archive(const char *work, const char *arc_name,
      const char *prefix, int expect_mapped, int mapped_build,
      unsigned count)
{
   char       path[512];
   char       member_path[512];
   char       src_path[512];
   archive_t *a;
   unsigned   i;

   join3(path, sizeof(path), work, "/", arc_name);
   a = archive_open(path);
   CHECK(a != NULL, arc_name);
   if (!a)
      return;

   for (i = 0; i < count; i++)
   {
      const archive_entry_t *e;
      const uint8_t         *data;
      const uint8_t         *map;
      uint8_t               *want;
      size_t                 want_len = 0, len = 0, map_len = 0;
      int                    idx;
      core_file             *cf;
      char                   name[256];

      join3(name, sizeof(name), prefix, members[i], "");
      join3(src_path, sizeof(src_path), work, "/src/", members[i]);
      want = read_file(src_path, &want_len);
      CHECK(want != NULL, src_path);
      if (!want)
         continue;

      idx = archive_find(a, name);
      CHECK(idx >= 0, name);
      if (idx < 0)
      {
         free(want);
         continue;
      }
      e = archive_entry(a, (unsigned)idx);
      CHECK(e->usable, name);
      CHECK(e->size == want_len, name);
      CHECK(e->crc == encoding_crc32(0, want, want_len), name);
      CHECK(archive_find_crc(a, e->crc) == idx, name);

      /* in place or decoded: either way the bytes */
      map  = archive_entry_map(a, (unsigned)idx, &map_len);
      if (expect_mapped && mapped_build)
         CHECK(map != NULL && map_len == want_len, name);
      else if (!e->stored)
         CHECK(map == NULL, name);
      /* Read to a place of the caller's, before the archive has decoded
       * it for anyone: the same bytes, nothing past them touched, and a
       * place too small for them left alone. */
      {
         uint8_t *own = (uint8_t*)malloc(want_len + 16);
         size_t   own_len = 0;

         CHECK(own != NULL, name);
         if (own)
         {
            memset(own, 0xA5, want_len + 16);
            CHECK(archive_entry_read(a, (unsigned)idx, own, want_len, &own_len), name);
            CHECK(own_len == want_len && !memcmp(own, want, want_len), name);
            CHECK(own[want_len] == 0xA5 && own[want_len + 15] == 0xA5, name);
            if (want_len)
            {
               memset(own, 0xA5, want_len + 16);
               CHECK(!archive_entry_read(a, (unsigned)idx, own, want_len - 1, &own_len), name);
               CHECK(own[0] == 0xA5 && own[want_len - 1] == 0xA5, name);
            }
            free(own);
         }
      }
      data = archive_entry_data(a, (unsigned)idx, &len);
      CHECK(data != NULL && len == want_len, name);
      /* ...and again once it has been decoded */
      {
         uint8_t *own = (uint8_t*)malloc(want_len ? want_len : 1);
         size_t   own_len = 0;

         if (own)
         {
            CHECK(archive_entry_read(a, (unsigned)idx, own, want_len, &own_len), name);
            CHECK(own_len == want_len && !memcmp(own, want, want_len), name);
            free(own);
         }
      }
      if (data)
         CHECK(!memcmp(data, want, want_len), name);
      if (map)
         CHECK(map == data, name);

      /* the same member as a core_file: only a path whose archive
       * extension is visible carries a member delimiter */
      join3(member_path, sizeof(member_path), path, "#", name);
      cf = core_fopen(member_path);
      if (!path_get_archive_delim(member_path))
         CHECK(cf == NULL, member_path);
      else
         CHECK(cf != NULL, member_path);
      if (cf)
      {
         size_t         flen = 0;
         const uint8_t *fmap = core_fmap(cf, &flen);

         if (expect_mapped && mapped_build)
            CHECK(fmap != NULL && flen == want_len, member_path);
         check_reads(cf, want, want_len, member_path);
         core_fclose(cf);
      }
      free(want);
   }

   CHECK(archive_find(a, "nope.bin") < 0, arc_name);
   CHECK(archive_find_crc(a, 0) < 0, arc_name);
   archive_close(a);
   core_archive_release();
}

int main(int argc, char **argv)
{
   const char *work;
   char        path[1024];
   char        out[1024];
   int         mapped_build = 0;
   core_file  *cf;
   size_t      len;
   uint8_t    *want;

   if (argc != 2)
   {
      fprintf(stderr, "usage: archive_test <workdir>\n");
      return 2;
   }
   work = argv[1];

   /* A plain file: mapped where the build can, read either way. */
   join3(path, sizeof(path), work, "/src/track02.raw", "");
   want = read_file(path, &len);
   CHECK(want != NULL, path);
   cf = core_fopen(path);
   CHECK(cf != NULL, path);
   if (cf && want)
   {
      mapped_build = core_fmap(cf, NULL) != NULL;
      check_reads(cf, want, len, path);
      core_fclose(cf);
   }
   free(want);
#ifdef HAVE_MMAP
   CHECK(mapped_build, "plain file is mapped under HAVE_MMAP");
#else
   CHECK(!mapped_build, "plain file is not mapped without HAVE_MMAP");
#endif
   CHECK(core_fopen("/nonexistent/path.bin") == NULL, "missing file");

   check_archive(work, "stored.zip",  "",      1, mapped_build, NUM_MEMBERS);
   check_archive(work, "deflate.zip", "",      0, mapped_build, NUM_MEMBERS);
   check_archive(work, "subdir.zip",  "GAME/", 0, mapped_build, NUM_MEMBERS);
   check_archive(work, "solid.7z",    "",      0, mapped_build, NUM_MEMBERS);
   check_archive(work, "suffix",      "",      1, mapped_build, NUM_MEMBERS);
   /* (a rar's members are reached through the archive only: the file
    * layer has no "archive#member" names for one) */
   check_archive(work, "stored.rar",  "",      0, mapped_build, NUM_MEMBERS);

   /* single.7z holds track03.bin only: the detach path */
   {
      archive_t *a;
      join3(path, sizeof(path), work, "/single.7z", "");
      a = archive_open(path);
      CHECK(a != NULL, "single.7z");
      if (a)
      {
         const uint8_t *data;
         size_t         dlen = 0;
         int            idx = archive_find(a, "track03.bin");

         CHECK(archive_num_entries(a) == 1, "single.7z");
         CHECK(idx == 0, "single.7z");
         join3(path, sizeof(path), work, "/src/track03.bin", "");
         want = read_file(path, &len);
         data = archive_entry_data(a, 0, &dlen);
         CHECK(data != NULL && want != NULL && dlen == len
               && !memcmp(data, want, len), "single.7z");
         free(want);
         archive_close(a);
      }
   }

   /* the disc pick */
   join3(path, sizeof(path), work, "/subdir.zip", "");
   CHECK(archive_resolve_disc(path, out, sizeof(out)), "resolve subdir.zip");
   CHECK(!strcmp(out + strlen(path), "#GAME/disc.gdi"), "resolve prefers gdi");
   /* a member already named: a disc image is kept, a track resolves
    * to the image beside it */
   join3(path, sizeof(path), work, "/subdir.zip#GAME/other.cue", "");
   CHECK(archive_resolve_disc(path, out, sizeof(out)) && !strcmp(out, path),
         "a named .cue member is kept");
   join3(path, sizeof(path), work, "/stored.zip#track01.bin", "");
   CHECK(archive_resolve_disc(path, out, sizeof(out)), "resolve from a track member");
   join3(path, sizeof(path), work, "/stored.zip#disc.gdi", "");
   CHECK(!strcmp(out, path), "track member resolves to the gdi");
   join3(path, sizeof(path), work, "/single.7z#track03.bin", "");
   CHECK(!archive_resolve_disc(path, out, sizeof(out)), "no disc beside the member");
   join3(path, sizeof(path), work, "/single.7z", "");
   CHECK(!archive_resolve_disc(path, out, sizeof(out)), "no disc in single.7z");
   join3(path, sizeof(path), work, "/src/disc.gdi", "");
   CHECK(!archive_resolve_disc(path, out, sizeof(out)), "plain file is not an archive");
   join3(path, sizeof(path), work, "/stored.zip", "");
   CHECK(!archive_resolve_disc(path, out, 20), "too small an out");

   /* a member that is not there, and an archive that is not */
   join3(path, sizeof(path), work, "/stored.zip#missing.bin", "");
   CHECK(core_fopen(path) == NULL, "missing member");
   join3(path, sizeof(path), work, "/src/track01.bin#x", "");
   CHECK(core_fopen(path) == NULL, "member of a non-archive");
   core_archive_release();

   printf(failures ? "archive_test: %d FAIL\n" : "archive_test: PASS\n",
         failures);
   return failures ? 1 : 0;
}
