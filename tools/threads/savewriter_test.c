/* The save writer (core/savewriter.c) against a file system that can be
 * made slow and made to fail: the libretro-common one behind a VFS
 * interface of the test's, as a frontend's would be.
 *
 *   - writes handed over in any number land in the file as they would
 *     have written straight to it, the later over the earlier;
 *   - handing over does not wait for the disk: with a write that takes
 *     five milliseconds, forty of them are handed over in a fraction of
 *     the time they take to be written;
 *   - a write that fails sets the caller's flag, and what is written
 *     again afterwards is in the file;
 *   - a file made anew each time is what was last handed over, and no
 *     longer than that;
 *   - more than the writer may have waiting is refused, and nothing that
 *     was taken is lost;
 *   - the writer can be stopped and started again.
 *
 * tools/threads/run.sh builds and runs it, under ThreadSanitizer and
 * under AddressSanitizer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

#include "savewriter.h"

#define CARD_SIZE (128 * 1024)

static retro_atomic_int_t write_delay_ms;    /* each write takes this long */
static retro_atomic_int_t writes_to_fail;    /* the next so many fail */
static retro_atomic_int_t writes_seen;

static int64_t RETRO_CALLCONV slow_write(struct retro_vfs_file_handle *stream, const void *s, uint64_t len)
{
   const int delay = retro_atomic_load_acquire_int(&write_delay_ms);

   retro_atomic_fetch_add_int(&writes_seen, 1);
   if (delay > 0)
      retro_sleep((unsigned)delay);
   if (retro_atomic_load_acquire_int(&writes_to_fail) > 0)
   {
      retro_atomic_fetch_sub_int(&writes_to_fail, 1);
      return -1;
   }
   return retro_vfs_file_write_impl((libretro_vfs_implementation_file *)stream, s, len);
}

static struct retro_vfs_interface vfs =
{
   (retro_vfs_get_path_t)retro_vfs_file_get_path_impl,
   (retro_vfs_open_t)retro_vfs_file_open_impl,
   (retro_vfs_close_t)retro_vfs_file_close_impl,
   (retro_vfs_size_t)retro_vfs_file_size_impl,
   (retro_vfs_tell_t)retro_vfs_file_tell_impl,
   (retro_vfs_seek_t)retro_vfs_file_seek_impl,
   (retro_vfs_read_t)retro_vfs_file_read_impl,
   slow_write,
   (retro_vfs_flush_t)retro_vfs_file_flush_impl,
   (retro_vfs_remove_t)retro_vfs_file_remove_impl,
   (retro_vfs_rename_t)retro_vfs_file_rename_impl,
   (retro_vfs_truncate_t)retro_vfs_file_truncate_impl,
};

static int failures;

static void check(int ok, const char *what)
{
   if (!ok)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
   rnd_state = rnd_state * 1103515245u + 12345u;
   return (rnd_state >> 8) & 0xFFFFFF;
}

/* The file's bytes; its length to @len. */
static uint8_t *slurp(const char *path, int64_t *len)
{
   void *buf = NULL;

   *len = 0;
   if (!filestream_read_file(path, &buf, len))
      return NULL;
   return (uint8_t *)buf;
}

int main(int argc, char **argv)
{
   static uint8_t card[CARD_SIZE], chunk[CARD_SIZE];
   struct retro_vfs_interface_info info;
   char card_path[1024], small_path[1024];
   retro_atomic_int_t failed;
   const char *dir = argc > 1 ? argv[1] : ".";
   uint8_t *got;
   int64_t got_len;
   RFILE *file;
   int i, taken, refused;
   retro_time_t t0, t_put, t_all;

   retro_atomic_int_init(&write_delay_ms, 0);
   retro_atomic_int_init(&writes_to_fail, 0);
   retro_atomic_int_init(&writes_seen, 0);
   retro_atomic_int_init(&failed, 0);
   info.required_interface_version = 2;
   info.iface = &vfs;
   filestream_vfs_init(&info);

   snprintf(card_path, sizeof(card_path), "%s/savewriter_card.bin", dir);
   snprintf(small_path, sizeof(small_path), "%s/savewriter_small.bin", dir);

   /* a card's file, as the core makes one: written whole, then patched */
   memset(card, 0xE5, sizeof(card));
   file = filestream_open(card_path, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   check(file != NULL, "the card's file is made");
   if (!file)
      return 1;
   check(filestream_write(file, card, sizeof(card)) == (int64_t)sizeof(card), "the card's file is filled");

   /* many writes, some over others */
   for (i = 0; i < 2000; i++)
   {
      const unsigned at  = rnd() % (CARD_SIZE - 512);
      const unsigned len = 1 + rnd() % 512;
      unsigned k;

      for (k = 0; k < len; k++)
         card[at + k] = (uint8_t)rnd();
      check(save_writer_put(file, at, card + at, len, &failed), "a write is taken");
   }
   save_writer_drain();
   check(!retro_atomic_load_acquire_int(&failed), "none of them failed");
   got = slurp(card_path, &got_len);
   check(got && got_len == CARD_SIZE && !memcmp(got, card, CARD_SIZE), "the file is what was written, the later over the earlier");
   free(got);

   /* a slow disk: handing over does not wait for it */
   retro_atomic_store_release_int(&write_delay_ms, 5);
   t0 = cpu_features_get_time_usec();
   for (i = 0; i < 40; i++)
   {
      const unsigned at = (unsigned)i * 512;

      memset(card + at, i + 1, 512);
      check(save_writer_put(file, at, card + at, 512, &failed), "a write to the slow disk is taken");
   }
   t_put = cpu_features_get_time_usec() - t0;
   save_writer_drain();
   t_all = cpu_features_get_time_usec() - t0;
   retro_atomic_store_release_int(&write_delay_ms, 0);
   printf("slow disk: 40 writes of 5 ms handed over in %.2f ms, written after %.0f ms\n",
         t_put / 1000.0, t_all / 1000.0);
   check(t_all >= 40 * 5000 * 9 / 10, "the writes took the time they take");
   check(t_put < t_all / 4, "handing them over did not wait for the disk");
   got = slurp(card_path, &got_len);
   check(got && got_len == CARD_SIZE && !memcmp(got, card, CARD_SIZE), "the file is what was written to the slow disk");
   free(got);

   /* a write that fails */
   retro_atomic_store_release_int(&writes_to_fail, 1);
   memset(card + 4096, 0x5A, 512);
   check(save_writer_put(file, 4096, card + 4096, 512, &failed), "the write that will fail is taken");
   save_writer_drain();
   check(retro_atomic_load_acquire_int(&failed) == 1, "the caller is told of the write that failed");
   retro_atomic_store_release_int(&failed, 0);
   /* what the core then does: the whole card again */
   check(save_writer_put(file, 0, card, CARD_SIZE, &failed), "the whole card is taken");
   save_writer_drain();
   check(!retro_atomic_load_acquire_int(&failed), "written again, it did not fail");
   got = slurp(card_path, &got_len);
   check(got && got_len == CARD_SIZE && !memcmp(got, card, CARD_SIZE), "the file is the card after the failure");
   free(got);

   /* more than may wait is refused; what was taken is written */
   retro_atomic_store_release_int(&write_delay_ms, 2);
   taken = refused = 0;
   for (i = 0; i < 64; i++)
   {
      memset(chunk, 0x80 + i, sizeof(chunk));
      if (save_writer_put(file, 0, chunk, sizeof(chunk), &failed))
      {
         memcpy(card, chunk, sizeof(chunk));
         taken++;
      }
      else
         refused++;
   }
   save_writer_drain();
   retro_atomic_store_release_int(&write_delay_ms, 0);
   printf("too much at once: %d taken, %d refused\n", taken, refused);
   check(taken >= 16 && refused > 0, "more than may wait was refused");
   got = slurp(card_path, &got_len);
   check(got && got_len == CARD_SIZE && !memcmp(got, card, CARD_SIZE), "the file is the last card that was taken");
   free(got);

   /* a file made anew each time; the writer stopped and started between */
   check(save_writer_put_file(small_path, "0123456789abcdef", 16, &failed), "a whole file is taken");
   save_writer_stop();
   got = slurp(small_path, &got_len);
   check(got && got_len == 16 && !memcmp(got, "0123456789abcdef", 16), "the whole file is there once the writer is stopped");
   free(got);
   check(save_writer_put_file(small_path, "EEPROM", 6, &failed), "a whole file is taken by a writer started again");
   save_writer_drain();
   got = slurp(small_path, &got_len);
   check(got && got_len == 6 && !memcmp(got, "EEPROM", 6), "the file is the shorter one that was last handed over");
   free(got);
   /* where there is nowhere to write it */
   check(save_writer_put_file("/nonexistent-directory/savewriter.bin", "x", 1, &failed), "a file that cannot be made is taken");
   save_writer_drain();
   check(retro_atomic_load_acquire_int(&failed) == 1, "the caller is told the file could not be made");

   save_writer_stop();
   filestream_close(file);
   filestream_delete(card_path);
   filestream_delete(small_path);

   if (failures)
      return 1;
   printf("save writer test passed (%d writes reached the file system)\n",
         retro_atomic_load_acquire_int(&writes_seen));
   return 0;
}
