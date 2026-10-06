/* Runs the sound chip's DSP on made-up programs and prints what each leaves
 * behind. tools/aica/run.sh builds this twice, once around the DSP's
 * compiler (core/hw/aica/dsp_x64.cpp) and once around its interpreter
 * (core/hw/aica/dsp_interp.cpp), runs both, and compares what they print:
 * the two are separate implementations of the same machine and have to
 * agree on every program, to the last bit.
 *
 * A case is a program of 1 to 64 random steps, random coefficients and
 * memory addresses, and 64 samples of random input; what is printed for it
 * are checksums of the 16 outputs after every sample, and of the DSP's
 * registers and its delay memory at the end. One thing is kept out of the
 * random programs: a step that takes a value read from memory (IWT) only
 * appears two steps after one that reads it (MRD). See run.sh for why.
 * $DSP_TEST_PROGRAM names a file of 512 hex words to run in place of the
 * made-up program, for looking at one case.
 *
 * Every case is run twice: as it is, and with a step added at the very end
 * of the 128 that is not empty but does nothing (it only clears a register
 * nothing reads afterwards). The DSP leaves out the empty steps after a
 * program's last one; the added step makes it run all 128. The two runs
 * have to print the same, which is the check that leaving them out
 * changes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "types.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/dsp.h"

unsigned ARAM_SIZE = 2 * 1024 * 1024;
unsigned ARAM_MASK = 2 * 1024 * 1024 - 1;
DSPData_struct *DSPData;
VArray2 aica_ram;

static u8 regs[sizeof(DSPData_struct)] __attribute__((aligned(4096)));
static u8 ram[2 * 1024 * 1024] __attribute__((aligned(4096)));

void os_DebugBreak()
{
   fprintf(stderr, "the DSP stopped on a check of its own\n");
   abort();
}

bool vmem_platform_prepare_jit_block(void *code_area, unsigned size, void **code_area_rwx)
{
   const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
   const uintptr_t from = (uintptr_t)code_area & ~(page - 1);
   const uintptr_t to   = ((uintptr_t)code_area + size + page - 1) & ~(page - 1);

   if (mprotect((void*)from, to - from, PROT_READ | PROT_WRITE | PROT_EXEC))
      return false;
   *code_area_rwx = code_area;
   return true;
}

static u64 seed;
static u32 rnd(void)
{
   seed = seed * 6364136223846793005ull + 1442695040888963407ull;
   return (u32)(seed >> 33);
}

static u64 fold(u64 h, const void *data, size_t size)
{
   const u8 *p = (const u8*)data;
   size_t i;

   for (i = 0; i + 8 <= size; i += 8)
   {
      u64 v;
      memcpy(&v, p + i, 8);
      h = (h ^ v) * 0x100000001b3ull;
      h ^= h >> 29;
   }
   for (; i < size; i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
   return h;
}

static void run_case(unsigned number, bool all_steps)
{
   u64 out = 0xcbf29ce484222325ull, state = out, memory = out;
   unsigned steps, i, sample;

   seed = 0x9E3779B97F4A7C15ull * (number + 1);
   memset(regs, 0, sizeof(regs));
   memset(ram, 0, sizeof(ram));
   dsp_init();

   steps = 1 + rnd() % 64;
   for (i = 0; i < steps; i++)
   {
      u32 *w = &DSPData->MPRO[i * 4];

      w[0] = rnd() & 0xFFFE;
      w[1] = rnd() & 0xFFFE;
      w[2] = rnd() & 0xFFFF;
      w[3] = rnd() & 0xFF80;
      /* the input select has 50 inputs to choose from */
      w[1] = (w[1] & ~(0x3Fu << 7)) | ((((w[1] >> 7) & 0x3F) % 0x32) << 7);
      /* a value read from memory is taken two steps after it is read */
      if (i < 2 || !(DSPData->MPRO[(i - 2) * 4 + 2] & 0x2000))
         w[1] &= ~0x40u;
   }
   if (getenv("DSP_TEST_PROGRAM"))
   {
      /* a program to run in place of the made-up one: 512 words in hex */
      FILE *f = fopen(getenv("DSP_TEST_PROGRAM"), "r");
      for (i = 0; i < 512; i++)
      {
         unsigned word = 0;
         if (fscanf(f, "%x", &word) != 1)
            word = 0;
         DSPData->MPRO[i] = word;
      }
      fclose(f);
   }
   if (all_steps)
      DSPData->MPRO[127 * 4 + 2] = 2;      /* ZERO: clears B, and that is all */
   for (i = 0; i < 128; i++)
      DSPData->COEF[i] = rnd() & 0xFFFF;
   for (i = 0; i < 64; i++)
      DSPData->MADRS[i] = rnd() & 0xFFFF;
   dsp.RBL = (8192u << (rnd() % 4)) - 1;
   dsp.RBP = ((rnd() % 1024) * 2048) & ARAM_MASK;
   dsp.dyndirty = true;
   dsp_writenmem(0x3400);                  /* the program was written */

   for (sample = 0; sample < 64; sample++)
   {
      for (i = 0; i < 16; i++)
         dsp.MIXS[i] = (s32)(rnd() << 12) >> 12;         /* 20 bits, signed */
      DSPData->EXTS[0] = (u32)(s32)(s16)rnd();
      DSPData->EXTS[1] = (u32)(s32)(s16)rnd();
      dsp_step();
      out = fold(out, DSPData->EFREG, sizeof(DSPData->EFREG));
   }
   state = fold(state, dsp.TEMP, sizeof(dsp.TEMP));
   state = fold(state, dsp.MEMS, sizeof(dsp.MEMS));
   state = fold(state, &dsp.regs.MDEC_CT, sizeof(dsp.regs.MDEC_CT));
   memory = fold(memory, ram, sizeof(ram));
   printf("%u %u %016llx %016llx %016llx\n", number, steps,
         (unsigned long long)out, (unsigned long long)state, (unsigned long long)memory);
}

int main(int argc, char **argv)
{
   const unsigned cases = argc > 1 ? (unsigned)atoi(argv[1]) : 200;
   const bool all_steps = argc > 2 && !strcmp(argv[2], "all");
   unsigned i;

   DSPData = (DSPData_struct*)regs;
   aica_ram.data = ram;
   aica_ram.size = sizeof(ram);
   for (i = 0; i < cases; i++)
      run_case(i, all_steps);
   return 0;
}
