/* Runs the x86-64 recompiler's code for the SH4's two vector instructions,
 * FIPR and FTRV (core/rec-x64/x64_vector.h), against the reference
 * implementations the recompiler would otherwise call and the interpreter
 * agrees with, on random operands: every result has to be the same bits.
 * tools/sh4/run.sh builds and runs this. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "deps/xbyak/xbyak.h"
#include "rec-x64/x64_vector.h"

using namespace Xbyak::util;

// as shil_canonical.h has them
static float fipr_reference(const float *fn, const float *fm)
{
   double idp = (double)fn[0] * fm[0];
   idp += (double)fn[1] * fm[1];
   idp += (double)fn[2] * fm[2];
   idp += (double)fn[3] * fm[3];
   return (float)idp;
}

static void ftrv_reference(float *fd, const float *fn, const float *fm)
{
   for (int row = 0; row < 4; row++)
      fd[row] = (float)((double)fm[row] * fn[0] + (double)fm[row + 4] * fn[1]
            + (double)fm[row + 8] * fn[2] + (double)fm[row + 12] * fn[3]);
}

struct Code : Xbyak::CodeGenerator
{
   float (*fipr)(const float *, const float *);
   void (*ftrv)(float *, const float *, const float *);

   Code()
   {
      // the host's own calling convention in, the recompiler's registers out
      fipr = getCurr<float (*)(const float *, const float *)>();
#ifdef _WIN32
      mov(rax, rcx); mov(rcx, rdx);
#else
      mov(rax, rdi); mov(rcx, rsi);
#endif
      x64_emit_fipr(*this, xmm0);
      ret();

      ftrv = getCurr<void (*)(float *, const float *, const float *)>();
#ifdef _WIN32
      mov(r9, rcx); mov(rax, rdx); mov(rcx, r8);
      sub(rsp, 24); movups(ptr[rsp], xmm6);
#else
      mov(r9, rdi); mov(rax, rsi); mov(rcx, rdx);
#endif
      x64_emit_ftrv(*this);
      movups(xword[r9], xmm0);
#ifdef _WIN32
      movups(xmm6, ptr[rsp]); add(rsp, 24);
#endif
      ret();
   }
};

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
   seed = seed * 6364136223846793005ull + 1442695040888963407ull;
   return (uint32_t)(seed >> 32);
}

// A float of any size, or one of a few million in size, as coordinates
// are: the second kind is where sums lose their low bits.
static float value(void)
{
   uint32_t bits = rnd();
   float f;
   if (rnd() & 1)
      bits = (bits & 0x807fffff) | ((100 + rnd() % 60) << 23);
   else if (((bits >> 23) & 0xff) == 0xff)
      bits &= ~0x00800000u;         // not an infinity or a NaN: their bits are not the sum's to settle
   memcpy(&f, &bits, 4);
   return f;
}

int main(int argc, char **argv)
{
   const long count = argc > 1 ? atol(argv[1]) : 5000000;
   Code code;
   long bad = 0;

   for (long i = 0; i < count; i++)
   {
      float v[4], w[4], m[16], want[4], got[4];
      for (float &f : v) f = value();
      for (float &f : w) f = value();
      for (float &f : m) f = value();

      float a = code.fipr(v, w), b = fipr_reference(v, w);
      if (memcmp(&a, &b, 4) && bad++ < 5)
         printf("FIPR: recompiled %a, reference %a\n", a, b);
      code.ftrv(got, v, m);
      ftrv_reference(want, v, m);
      if (memcmp(got, want, sizeof(got)) && bad++ < 5)
         printf("FTRV: recompiled %a %a %a %a, reference %a %a %a %a\n", got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
   }
   if (bad)
   {
      printf("FAIL: %ld of %ld differ\n", bad, 2 * count);
      return 1;
   }
   printf("sh4 vectors: ok (FIPR and FTRV as recompiled are the reference's, on %ld random operands each)\n", count);
   return 0;
}
