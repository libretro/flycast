/* What ta_gpu_test.c gives ta_gpu_test_cpu.cpp: the decode on the device
 * and as the model, and the softfloat check */
#ifndef TA_GPU_TEST_H
#define TA_GPU_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* The room of each buffer: words of data and tables, and entries */
struct tgt_sizes
{
   unsigned ta_words, table_words;
   unsigned verts, idx, list[3], modtrig, headers;
};

/* What a decode made (the device's buffers, mapped, or the model's): and
 * the translucent draw records and polygon parameter words */
struct tgt_out
{
   const unsigned *vtx, *mod, *idx, *rec, *cmd, *frm, *work, *trd, *trp;
};

/* The device and its kernels from @spv_dir (k1.spv to k13.spv), buffers
 * for @sz; @layout[13] gets the places, as TaGpuLayout has them */
void tgt_init(const char *spv_dir, int validate, const struct tgt_sizes *sz, unsigned *layout);
int tgt_validation_errors(void);
/* A frame: its data and tables, decoded on the device (@ms: submit to
 * fence); @trd: the translucent draw record slots it takes (TaGpuFrame) */
void tgt_gpu(const void *ta, unsigned ta_bytes, const unsigned *table, unsigned table_words, unsigned trd, struct tgt_out *o, double *ms);
/* ...and by the model; 0 if its scan's elements and its states differ */
int tgt_model(const void *ta, const unsigned *table, unsigned trd, struct tgt_out *o);
/* 1 if the header's single precision is this CPU's */
int tgt_softfloat_test(void);

#ifdef __cplusplus
}
#endif

#endif
