/* Bare-metal SH4 benchmark, to compare recompilers on the same work.
 *
 * Five kernels, each a fixed amount of work: integer arithmetic, memory
 * loads and stores, single-precision floating point, calls and returns,
 * and small loops with branches. The word at 0x8c00f800 counts the stages
 * and is 0x600DBE4C at the end; the five words after it are the kernels'
 * checksums, and words 8 to 13 are what the SH4's own timer (TMU0, at the
 * peripheral clock / 1024) read before the first kernel and after each.
 * So one run gives, per kernel: whether the answer is right, how long the
 * host took, and how long the emulated machine thinks it took.
 *
 * To build (the code is on its own page, so no store lands on a page of
 * code):
 *
 *   sh4-linux-gnu-gcc -ml -m4 -O2 -nostdlib -ffreestanding -fno-pic \
 *      -fno-pie -static -Wl,-T,bench_prog.ld -Wl,--build-id=none \
 *      -o bench.elf bench_prog.c
 *
 * To run, with the frontend and the OpenGL that does nothing that
 * headless.sh builds (in its work directory):
 *
 *   HEADLESS_DIR=dir HEADLESS_STAGE=0x8c00f800 LD_PRELOAD=libGLESv2.so.2 \
 *      ./headless flycast_libretro.so bench.elf 30000 \
 *      reicast_hle_bios=enabled reicast_threaded_rendering=disabled \
 *      reicast_sh4_timing=accurate
 *
 * The same command runs upstream's libretro core, whose options have the
 * same names. Integer checksums, for the iteration counts below:
 * d974596f b0997ce5 (float) 01709e79 01013a00. */
typedef unsigned int u32;
typedef unsigned char u8;
#define RES ((volatile u32 *)0x8c00f800)

__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        " mov.l 1f,r15\n mov.l 3f,r0\n lds r0,fpscr\n mov.l 2f,r0\n jmp @r0\n nop\n .align 2\n"
        "1: .long 0x8c00f000\n2: .long cmain\n3: .long 0x000C0001\n");

static u32 buf_a[65536], buf_b[65536];
static float verts[4096 * 4], outv[4096 * 4];
static u8 sieve[32768];

__attribute__((noinline)) static u32 k_int(u32 n)
{
   u32 x = 0x12345678, sum = 0, i;
   for (i = 0; i < n; i++)
   {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      sum += x + (i & 0xff);
   }
   return sum;
}

__attribute__((noinline)) static u32 k_mem(u32 rounds)
{
   u32 r, i, sum = 0;
   for (r = 0; r < rounds; r++)
   {
      for (i = 0; i < 65536; i++)
         buf_b[i] = buf_a[i] + r;
      for (i = 0; i < 65536; i++)
         sum += buf_b[i] ^ (sum >> 3);
      for (i = 0; i < 32768; i++)
         sum += ((signed char *)buf_b)[i * 3];
   }
   return sum;
}

__attribute__((noinline)) static u32 k_float(u32 rounds)
{
   static const float m[16] = { 0.9f, 0.1f, 0.0f, 0.0f, -0.1f, 0.9f, 0.2f, 0.0f,
                                0.0f, -0.2f, 0.9f, 0.0f, 1.0f, 2.0f, 3.0f, 1.0f };
   u32 r, i;
   float acc = 0.0f;
   for (r = 0; r < rounds; r++)
   {
      for (i = 0; i < 4096; i++)
      {
         const float *v = &verts[i * 4];
         float x = v[0], y = v[1], z = v[2];
         float ox = x * m[0] + y * m[4] + z * m[8] + m[12];
         float oy = x * m[1] + y * m[5] + z * m[9] + m[13];
         float oz = x * m[2] + y * m[6] + z * m[10] + m[14];
         float w = 1.0f / (oz * 0.01f + 2.0f);
         outv[i * 4] = ox * w; outv[i * 4 + 1] = oy * w; outv[i * 4 + 2] = oz; outv[i * 4 + 3] = w;
         acc += ox * w - oy * w;
      }
   }
   return (u32)(int)acc ^ (u32)(int)(outv[100] * 1000.0f);
}

__attribute__((noinline)) static u32 fib(u32 n)
{
   return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

__attribute__((noinline)) static u32 k_branch(u32 rounds)
{
   u32 r, i, j, count = 0;
   for (r = 0; r < rounds; r++)
   {
      for (i = 0; i < 32768; i++) sieve[i] = 1;
      for (i = 2; i < 32768; i++)
         if (sieve[i])
         {
            count++;
            for (j = i + i; j < 32768; j += i) sieve[j] = 0;
         }
   }
   return count;
}

void cmain(void)
{
   u32 i;
   for (i = 0; i < 65536; i++) buf_a[i] = i * 2654435761u;
   for (i = 0; i < 4096 * 4; i++) verts[i] = (float)(int)((i * 37) & 1023) * 0.125f - 60.0f;
   *(volatile unsigned char *)0xFFD80004 = 0;           /* TSTR: stop */
   *(volatile u32 *)0xFFD80008 = 0xFFFFFFFF;            /* TCOR0 */
   *(volatile u32 *)0xFFD8000C = 0xFFFFFFFF;            /* TCNT0 */
   *(volatile unsigned short *)0xFFD80010 = 4;          /* TCR0: peripheral clock / 1024 */
   *(volatile unsigned char *)0xFFD80004 = 1;           /* go */
#define T() (~*(volatile u32 *)0xFFD8000C)
   RES[8] = T();
   RES[0] = 1; RES[1] = k_int(240000000); RES[9] = T();
   RES[0] = 2; RES[2] = k_mem(1000); RES[10] = T();
   RES[0] = 3; RES[3] = k_float(6000); RES[11] = T();
   RES[0] = 4; RES[4] = fib(37); RES[12] = T();
   RES[0] = 5; RES[5] = k_branch(4800); RES[13] = T();
   RES[0] = 0x600DBE4C;
   for (;;) ;
}
