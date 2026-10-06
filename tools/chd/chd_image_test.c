/* chd_image check against the track files its CHD was made from.
 *
 *    chd_image_test <image.chd> <source file>...
 *
 * The sources are the files tools/chd/make_fixture.py writes, in track
 * order: the one BIN of a CUE fixture, or the three track files of a GDI
 * fixture. Each track in the CHD's metadata ('CHT2' for the CUE, 'CHGD'
 * for the GDI) takes its frames from the sources in turn, moving to the
 * next file when one runs out, and the CHD lays them out as chd.cpp does:
 * each track padded to a multiple of four frames. Every frame must hold
 * that sector -- data verbatim, audio byte-swapped (a version 5 CHD stores
 * it big-endian) -- with an all-zero subchannel, and the PAD frames a
 * GD-ROM track ends with must be zero.
 *
 * run.sh feeds this CHDs compressed with one CD codec family each (cdzs,
 * cdlz, cdzl, cdfl), a GD-ROM image, and a child that differences against
 * a parent, so a codec the build cannot decode, a misread GD-ROM track
 * table, or a hunk taken from the wrong image of the chain shows up as a
 * failed or mismatched frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/imgread/chd_image.h"

typedef struct
{
   char  **paths;
   int     count;
   int     next;
   FILE   *fp;
} sources_t;

/* Reads the next 2352-byte sector, moving on to the next source file
 * when the current one is exhausted. */
static bool next_sector(sources_t *src, uint8_t *sector)
{
   for (;;)
   {
      if (src->fp && fread(sector, 1, 2352, src->fp) == 2352)
         return true;
      if (src->fp)
         fclose(src->fp);
      src->fp = NULL;
      if (src->next >= src->count)
         return false;
      if (!(src->fp = fopen(src->paths[src->next++], "rb")))
         return false;
   }
}

int main(int argc, char **argv)
{
   chd_image_t      *img;
   chd_image_track_t t;
   sources_t         src;
   uint8_t          *hunk;
   uint8_t           sector[2352];
   uint8_t           want[CHD_IMAGE_FRAME_BYTES];
   char              err[512];
   uint32_t          index;
   uint32_t          hunk_bytes;
   uint32_t          per_hunk;
   uint32_t          held      = 0xFFFFFFFFu;
   uint32_t          chd_frame = 0;
   unsigned          frames    = 0;
   unsigned          bad       = 0;

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s image.chd source...\n", argv[0]);
      return 2;
   }

   err[0] = '\0';
   if (!(img = chd_image_open(argv[1], err, sizeof(err))))
   {
      printf("%s\nRESULT: FAIL\n", err[0] ? err : "open failed");
      return 1;
   }
   hunk_bytes = chd_image_hunk_bytes(img);
   per_hunk   = hunk_bytes / CHD_IMAGE_FRAME_BYTES;
   if (!per_hunk || !(hunk = (uint8_t *)malloc(hunk_bytes)))
   {
      printf("%s: not a disc image\nRESULT: FAIL\n", argv[1]);
      chd_image_close(img);
      return 1;
   }

   src.paths = argv + 2;
   src.count = argc - 2;
   src.next  = 0;
   src.fp    = NULL;

   /* The read-ahead hint in every shape, before the reads it is for:
    * a hint changes no bytes, and one past the end hints nothing. */
   chd_image_prefetch(img, 0, chd_image_hunk_count(img));
   chd_image_prefetch(img, chd_image_hunk_count(img) / 2, 3);
   chd_image_prefetch(img, chd_image_hunk_count(img), 1);
   chd_image_prefetch(img, 0, 0);

   for (index = 0; chd_image_track(img, index, &t); index++)
   {
      int32_t i;
      bool    audio = !strcmp(t.type, "AUDIO");

      if (t.track != (int32_t)index + 1 || t.frames <= 0 || t.pad < 0
            || t.pad > t.frames || (strcmp(t.type, "MODE1_RAW") && !audio))
      {
         printf("track %u: unexpected metadata (track %d, type %s, "
               "frames %d, pad %d)\n", (unsigned)index + 1, (int)t.track,
               t.type, (int)t.frames, (int)t.pad);
         bad++;
         break;
      }

      for (i = 0; i < t.frames; i++)
      {
         uint32_t       f = chd_frame + (uint32_t)i;
         const uint8_t *got;
         unsigned       k;

         memset(want, 0, sizeof(want));
         if (i < t.frames - t.pad)
         {
            if (!next_sector(&src, sector))
            {
               printf("sources end before track %u frame %d\n",
                     (unsigned)index + 1, (int)i);
               bad++;
               break;
            }
            if (audio && chd_image_version(img) >= 5)
               for (k = 0; k < sizeof(sector); k += 2)
               {
                  want[k]     = sector[k + 1];
                  want[k + 1] = sector[k];
               }
            else
               memcpy(want, sector, sizeof(sector));
         }

         frames++;
         if (f / per_hunk != held)
         {
            held = f / per_hunk;
            if (!chd_image_read_hunk(img, held, hunk))
            {
               if (bad < 8)
                  printf("hunk %u: decode failed\n", (unsigned)held);
               bad++;
               held = 0xFFFFFFFFu;
               continue;
            }
         }
         got = hunk + (size_t)(f % per_hunk) * CHD_IMAGE_FRAME_BYTES;
         if (memcmp(got, want, sizeof(want)))
         {
            if (bad < 8)
               printf("track %u frame %d differs\n", (unsigned)index + 1,
                     (int)i);
            bad++;
         }
      }
      chd_frame += (uint32_t)((t.frames + 3) & ~3);
   }

   if (index == 0)
   {
      printf("no track metadata\n");
      bad++;
   }
   if (next_sector(&src, sector))
   {
      printf("sources hold sectors the CHD does not\n");
      bad++;
   }

   if (src.fp)
      fclose(src.fp);
   free(hunk);
   chd_image_close(img);

   printf("%s: %u tracks, %u frames, %u mismatches\nRESULT: %s\n", argv[1],
         (unsigned)index, frames, bad, bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}
