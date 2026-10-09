/* What the machine saves - a memory card's blocks, a cabinet's EEPROM -
 * goes to its file from a thread of its own.
 *
 * A game saves in the middle of a frame; the bytes used to be written to
 * the file between two frames, by the thread that runs them, which then
 * waited for the write: nothing on a disk with its cache in front of it,
 * a frame or several where a write goes through to a memory card or to
 * another process. Here the bytes are copied and handed over, which takes
 * no lock and makes no system call unless the writer is asleep, and the
 * writer does the waiting.
 *
 * Whoever saves is one thread at a time: the one that runs the frames, or
 * the emulation thread while that one waits for it. The hand-over would
 * take more than one at once (it is a list any thread may push onto), the
 * writer's start would not.
 *
 * A write can fail, and is found to have failed after the caller has gone
 * on. The caller gives a flag for that, which the writer sets; what the
 * caller does when it finds it set - write everything again, later - is
 * its own business. Until now a write that failed was forgotten.
 *
 * Built without threads (TARGET_NO_THREADS) the calls write before they
 * return, and report through the same flag. */
#ifndef CORE_SAVEWRITER_H_
#define CORE_SAVEWRITER_H_

#include <stddef.h>
#include <stdint.h>
#include <retro_common_api.h>
#include <retro_atomic.h>
#include <streams/file_stream.h>

RETRO_BEGIN_DECLS

/* @len bytes of @data, copied, to be written at @offset of @file and the
 * file flushed. @file is open for writing and is the writer's from here
 * until save_writer_drain() has returned: nobody else seeks, reads or
 * closes it in between.
 *
 * Returns 0 if they were not taken - no memory, or too much is waiting
 * already (the writer cannot keep up) - and are still the caller's to
 * save; 1 if they were. @failed is set to 1 if the write then fails; it
 * has to outlive the write, which it does if the caller drains before it
 * goes. */
int save_writer_put(RFILE *file, int64_t offset, const void *data,
      size_t len, retro_atomic_int_t *failed);

/* The same for a file that is made anew each time: @len bytes of @data to
 * be the whole of the file at @path. */
int save_writer_put_file(const char *path, const void *data, size_t len,
      retro_atomic_int_t *failed);

/* Returns when everything handed over has been written, or has failed. */
void save_writer_drain(void);

/* The same, and the writer's thread is ended: at the end of the content.
 * The next hand-over starts it again. */
void save_writer_stop(void);

RETRO_END_DECLS

#endif
