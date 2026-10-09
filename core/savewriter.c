/* See savewriter.h. */
#include "savewriter.h"

#include <stdlib.h>
#include <string.h>

#ifndef TARGET_NO_THREADS
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#endif

/* Never more than this waiting to be written: eight memory cards written
 * whole are a quarter of it. */
#define SAVE_WRITER_MAX_WAITING (4u * 1024u * 1024u)

typedef struct save_job
{
   struct save_job    *next;
   RFILE              *file;      /* NULL: the file at path, made anew */
   int64_t             offset;
   size_t              len;
   retro_atomic_int_t *failed;
   /* then the path with its 0, if there is one, and the bytes */
   size_t              path_size;
} save_job_t;

#define SAVE_JOB_PATH(job)  ((char *)((job) + 1))
#define SAVE_JOB_DATA(job)  ((uint8_t *)((job) + 1) + (job)->path_size)

static int save_job_run(const save_job_t *job)
{
   int ok = 1;

   if (job->file)
   {
      if (filestream_seek(job->file, job->offset, RETRO_VFS_SEEK_POSITION_START) < 0)
         return 0;
      if (filestream_write(job->file, SAVE_JOB_DATA(job), (int64_t)job->len) != (int64_t)job->len)
         ok = 0;
      if (filestream_flush(job->file) != 0)
         ok = 0;
   }
   else
   {
      RFILE *f = filestream_open(SAVE_JOB_PATH(job), RETRO_VFS_FILE_ACCESS_WRITE,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);

      if (!f)
         return 0;
      if (filestream_write(f, SAVE_JOB_DATA(job), (int64_t)job->len) != (int64_t)job->len)
         ok = 0;
      if (filestream_close(f) != 0)
         ok = 0;
   }
   return ok;
}

static save_job_t *save_job_new(RFILE *file, const char *path, int64_t offset,
      const void *data, size_t len, retro_atomic_int_t *failed)
{
   const size_t path_size = path ? strlen(path) + 1 : 0;
   save_job_t *job        = (save_job_t *)malloc(sizeof(*job) + path_size + len);

   if (!job)
      return NULL;
   job->next      = NULL;
   job->file      = file;
   job->offset    = offset;
   job->len       = len;
   job->failed    = failed;
   job->path_size = path_size;
   if (path)
      memcpy(SAVE_JOB_PATH(job), path, path_size);
   memcpy(SAVE_JOB_DATA(job), data, len);
   return job;
}

#ifdef TARGET_NO_THREADS

static int save_writer_take(save_job_t *job)
{
   if (!job)
      return 0;
   if (!save_job_run(job) && job->failed)
      retro_atomic_store_release_int(job->failed, 1);
   free(job);
   return 1;
}

void save_writer_drain(void) { }
void save_writer_stop(void) { }

#else

static struct
{
   sthread_t          *thread;
   retro_eventcount_t  work;      /* the writer sleeps here */
   retro_eventcount_t  idle;      /* and who drains, here */
   retro_atomic_ptr_t  head;      /* what was handed over, newest first */
   retro_atomic_size_t waiting;   /* bytes handed over and not yet written */
   retro_atomic_int_t  stop;
   int                 ready;     /* the two above it are made */
} writer;

static void save_writer_thread(void *unused)
{
   (void)unused;
   for (;;)
   {
      save_job_t *list = (save_job_t *)retro_atomic_exchange_ptr(&writer.head, NULL);
      save_job_t *job  = NULL;

      if (!list)
      {
         int key;

         if (retro_atomic_load_acquire_int(&writer.stop))
            break;
         key = retro_eventcount_prepare_wait(&writer.work);
         if (retro_atomic_load_acquire_ptr(&writer.head) != NULL
               || retro_atomic_load_acquire_int(&writer.stop))
            retro_eventcount_cancel_wait(&writer.work);
         else
            retro_eventcount_commit_wait(&writer.work, key);
         continue;
      }

      /* oldest first: a later write to the same bytes has to land last */
      while (list)
      {
         save_job_t *next = list->next;

         list->next = job;
         job        = list;
         list       = next;
      }
      while (job)
      {
         save_job_t *next = job->next;

         if (!save_job_run(job) && job->failed)
            retro_atomic_store_release_int(job->failed, 1);
         retro_atomic_fetch_sub_size(&writer.waiting, sizeof(*job) + job->path_size + job->len);
         free(job);
         job = next;
      }
      retro_eventcount_notify(&writer.idle);
   }
}

static int save_writer_start(void)
{
   if (writer.thread)
      return 1;
   if (!writer.ready)
   {
      if (!retro_eventcount_init(&writer.work))
         return 0;
      if (!retro_eventcount_init(&writer.idle))
      {
         retro_eventcount_free(&writer.work);
         return 0;
      }
      retro_atomic_ptr_init(&writer.head, NULL);
      retro_atomic_size_init(&writer.waiting, 0);
      writer.ready = 1;
   }
   retro_atomic_int_init(&writer.stop, 0);
   writer.thread = sthread_create(save_writer_thread, NULL);
   return writer.thread != NULL;
}

static int save_writer_take(save_job_t *job)
{
   const size_t size = job ? sizeof(*job) + job->path_size + job->len : 0;

   if (!job)
      return 0;
   if (!save_writer_start())
   {
      /* no thread to be had: written here, as without threads */
      if (!save_job_run(job) && job->failed)
         retro_atomic_store_release_int(job->failed, 1);
      free(job);
      return 1;
   }
   if (retro_atomic_load_acquire_size(&writer.waiting) + size > SAVE_WRITER_MAX_WAITING)
   {
      free(job);
      return 0;
   }
   retro_atomic_fetch_add_size(&writer.waiting, size);
   for (;;)
   {
      void *old = retro_atomic_load_relaxed_ptr(&writer.head);

      job->next = (save_job_t *)old;
      if (retro_atomic_cas_ptr(&writer.head, old, job))
         break;
   }
   retro_eventcount_notify(&writer.work);
   return 1;
}

void save_writer_drain(void)
{
   if (!writer.thread)
      return;
   for (;;)
   {
      int key;

      if (retro_atomic_load_acquire_size(&writer.waiting) == 0)
         return;
      key = retro_eventcount_prepare_wait(&writer.idle);
      if (retro_atomic_load_acquire_size(&writer.waiting) == 0)
      {
         retro_eventcount_cancel_wait(&writer.idle);
         return;
      }
      retro_eventcount_commit_wait(&writer.idle, key);
   }
}

void save_writer_stop(void)
{
   if (!writer.thread)
      return;
   save_writer_drain();
   retro_atomic_store_release_int(&writer.stop, 1);
   retro_eventcount_notify(&writer.work);
   sthread_join(writer.thread);
   writer.thread = NULL;
}

#endif

int save_writer_put(RFILE *file, int64_t offset, const void *data,
      size_t len, retro_atomic_int_t *failed)
{
   if (!file)
      return 0;
   return save_writer_take(save_job_new(file, NULL, offset, data, len, failed));
}

int save_writer_put_file(const char *path, const void *data, size_t len,
      retro_atomic_int_t *failed)
{
   if (!path)
      return 0;
   return save_writer_take(save_job_new(NULL, path, 0, data, len, failed));
}
