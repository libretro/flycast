/* ta_gpu_test.c's device in another program (built with TGT_REPLAY): one
 * that runs where this one cannot - ta_gpu_test_d3d12.c, Direct3D 12's,
 * under wine - with files in $TAG_DIR between the two. The frames are run
 * twice, the same way (see ta_gpu_test_d3d12.sh):
 *
 *   TAG_REPLAY=dump  each decode asked for written to cNNNNNN.in (its data,
 *                    its tables, the sorted indices of its prepass), and
 *                    answered with the model's, which is what a decode that
 *                    is right gives; the sizes and the layout to init
 *   (ta_gpu_test_d3d12.exe decodes every cNNNNNN.in, one after the other in
 *    the same buffers, and writes what each decode made, as far as
 *    ta_gpu_test_cpu.cpp looks at it, to cNNNNNN.out)
 *   TAG_REPLAY=read  each decode answered with its cNNNNNN.out, in buffers
 *                    that keep what a decode did not write, as the
 *                    device's do; a decode asked for that is not the one
 *                    dumped in its place (the run went another way: what
 *                    the device made differed) fails the test
 *
 * Included by ta_gpu_test.c. */
#include <stdint.h>

enum { R_VTX, R_MOD, R_IDX, R_WORK, R_REC, R_CMD, R_FRM, R_TRD, R_TRP, R_SRT, R_COUNT };
static unsigned *r_buf[R_COUNT];
static size_t r_words[R_COUNT];
static int r_read;
static const char *r_dir;
static unsigned r_call;
static int r_errors;

static FILE *r_open(const char *name, const char *mode)
{
   char path[1024];
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s", r_dir, name);
   f = fopen(path, mode);
   if (!f && mode[0] == 'w')
   {
      printf("ta_gpu_test: cannot write %s\n", path);
      exit(2);
   }
   return f;
}

int tgt_validation_errors(void)
{
   return r_errors;
}

void tgt_init(const char *spv_dir, int validate, const struct tgt_sizes *sz, unsigned *layout)
{
   unsigned strips;
   (void)spv_dir;
   (void)validate;
   r_dir = getenv("TAG_DIR") ? getenv("TAG_DIR") : ".";
   r_read = getenv("TAG_REPLAY") && !strcmp(getenv("TAG_REPLAY"), "read");
   model_init(sz, layout);
   strips = sz->list[0] + sz->list[1] + sz->list[2];
   r_words[R_VTX] = (size_t)sz->verts * TAG_V_WORDS;
   r_words[R_MOD] = (size_t)sz->modtrig * 9 + 1;
   r_words[R_IDX] = sz->idx;
   r_words[R_WORK] = work_words;
   r_words[R_REC] = (size_t)strips * TAG_C_WORDS;
   r_words[R_CMD] = (size_t)sz->headers * 2 * TAG_C_WORDS;
   r_words[R_FRM] = TAG_O_WORDS;
   r_words[R_TRD] = (size_t)(sz->verts + sz->headers) * TAG_C_WORDS;
   r_words[R_TRP] = (size_t)sz->list[2] * TAG_P_WORDS;
   r_words[R_SRT] = (size_t)sz->idx * 6;
   if (r_read)
   {
      int b;
      FILE *f = r_open("errors", "r");
      for (b = 0; b < R_COUNT; b++)
         r_buf[b] = (unsigned *)calloc(r_words[b], 4);
      if (!f || fscanf(f, "%d", &r_errors) != 1)
         r_errors = 1;
      if (f)
         fclose(f);
      printf("device: the decodes of ta_gpu_test_d3d12 (%s)\n", r_dir);
   }
   else
   {
      FILE *f = r_open("init", "wb");
      fwrite(sz, sizeof(*sz), 1, f);
      fwrite(layout_words, sizeof(layout_words), 1, f);
      fwrite(&work_words, 4, 1, f);
      fclose(f);
      printf("device: none, the decodes written to %s\n", r_dir);
   }
}

void tgt_gpu(const void *ta, unsigned ta_bytes, const unsigned *table, unsigned table_words, unsigned trd, struct tgt_out *o, double *ms)
{
   const unsigned checks = srt_gpu != NULL ? srt_gpu_checks : 0;
   const unsigned srt_words = checks != 0 ? srt_gpu_words : 0;
   unsigned head[5];
   char name[32];
   FILE *f;

   head[0] = ta_bytes;
   head[1] = table_words;
   head[2] = trd;
   head[3] = srt_words;
   head[4] = checks;
   *ms = 0;
   snprintf(name, sizeof(name), "c%06u.%s", r_call++, r_read ? "out" : "in");
   if (!r_read)
   {
      f = r_open(name, "wb");
      fwrite(head, sizeof(head), 1, f);
      fwrite(ta, 1, ta_bytes, f);
      fwrite(table, 4, table_words, f);
      if (srt_words != 0)
         fwrite(srt_gpu, 4, srt_words, f);
      fclose(f);
      /* answered with the model's: the next tgt_model() does the prepass's
       * checks on the device's sorted indices, as the device would */
      if (srt_gpu != NULL)
      {
         srt_model = srt_gpu;
         srt_model_checks = srt_gpu_checks;
      }
      srt_gpu = NULL;
      tgt_model(ta, table, trd, o);
      return;
   }
   f = r_open(name, "rb");
   if (f)
   {
      unsigned had[5], rec[3];
      size_t n = fread(had, sizeof(had), 1, f);
      if (n != 1 || memcmp(had, head, sizeof(head)) != 0)
      {
         printf("ta_gpu_test: decode %u is not the one dumped in its place\n", r_call - 1);
         r_errors++;
      }
      while (fread(rec, sizeof(rec), 1, f) == 1 && rec[0] < R_COUNT)
      {
         if ((size_t)rec[1] + rec[2] > r_words[rec[0]] || fread(r_buf[rec[0]] + rec[1], 4, rec[2], f) != rec[2])
         {
            printf("ta_gpu_test: %s is cut short\n", name);
            r_errors++;
            break;
         }
      }
      fclose(f);
   }
   else
   {
      printf("ta_gpu_test: no %s/%s\n", r_dir, name);
      r_errors++;
   }
   if (srt_gpu != NULL && srt_words != 0)
      memcpy(srt_gpu, r_buf[R_SRT], (size_t)srt_words * 4);
   srt_gpu = NULL;
   o->vtx = r_buf[R_VTX];
   o->mod = r_buf[R_MOD];
   o->idx = r_buf[R_IDX];
   o->rec = r_buf[R_REC];
   o->cmd = r_buf[R_CMD];
   o->frm = r_buf[R_FRM];
   o->work = r_buf[R_WORK];
   o->trd = r_buf[R_TRD];
   o->trp = r_buf[R_TRP];
}
