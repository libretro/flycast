/* Bare-metal SH4 program for headless.sh: code that is written over while
 * it is in use, against a recompiler that links its blocks.
 *
 * A recompiler that jumps from one block of compiled code straight into
 * the next has to take those jumps back when a block goes. Code a game
 * keeps rewriting is compiled into a small cache of its own, the temporary
 * one, which is emptied whenever it is full and filled again from the
 * start - so the blocks in it go often, all at once, and the room they
 * were in is given to others. What must still hold then:
 *
 *   1. A block that was linked to one of them must not go on jumping to
 *      where it was.
 *   2. Nothing may later be written where one of them was - where its own
 *      jump to another block used to be, when that other block goes.
 *
 * The program writes its test code into two pages of memory (A and B):
 *
 *   P    bra H2 ; nop                  never rewritten
 *   H2   rts ; mov #v,r0               rewritten at the start, then left
 *   T    bra Q ; mov #k,r1             rewritten forty thousand times
 *   Q    rts ; mov r1,r0               in page B, never rewritten
 *   G0 to G63                          sixty-four functions that return
 *                                      their number, rewritten at the start
 *
 * Everything but P and Q is rewritten often enough at the start to be
 * compiled into the temporary cache from then on. P is then linked to H2
 * and T to Q. T is rewritten and called until the temporary cache has been
 * emptied several times over: each time T is compiled anew, linked to Q
 * anew, and what it returns is looked at. Then:
 *
 *   - P is called: H2 has to answer, with what it was last rewritten to
 *     return (1);
 *   - the Gs are called, which compiles them into the room that old
 *     compilations of T were in; a word of page B is written, at which
 *     every block of that page goes, Q among them, and whatever was linked
 *     to Q is unlinked; and the Gs have to return their numbers still (2).
 *
 * The word at 0x8c00f800 says how far it has got, and is 0x600D5AC0 at the
 * end or 0xBAD0ssnn: the step, and which function or turn it was.
 *
 * smc_elf.py carries the compiled program. To rebuild it:
 *
 *   sh4-linux-gnu-gcc -ml -m4-nofpu -O1 -nostdlib -ffreestanding -fno-pic \
 *      -fno-pie -static -Wl,-T,live_prog.ld -Wl,--build-id=none \
 *      -o smc_prog.elf smc_prog.c
 *   sh4-linux-gnu-objcopy -O binary smc_prog.elf smc_prog.bin
 *
 * and paste the bytes of smc_prog.bin into PROGRAM. */
typedef unsigned int u32;
typedef unsigned short u16;

__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        " mov.l 1f,r15\n mov.l 2f,r0\n jmp @r0\n nop\n .align 2\n"
        "1: .long 0x8c00f000\n2: .long cmain\n");

#define VERDICT  ((volatile u32 *)0x8c00f800)
#define PAGE_A   0x8c200000u
#define PAGE_B   0x8c201000u
#define CODE(at) ((volatile u16 *)(at))

#define P_AT     (PAGE_A + 0x000)
#define H2_AT    (PAGE_A + 0x010)
#define G_AT(j)  (PAGE_A + 0x100 + (j) * 0x20)
#define T_AT     (PAGE_A + 0xF00)
#define Q_AT     (PAGE_B + 0x000)
#define SCRATCH  ((volatile u32 *)(PAGE_B + 0x800))

#define GS       64
#ifndef TURNS
#define TURNS    40000
#endif

static u32 call(u32 at)
{
   u32 (* volatile f)(void) = (u32 (*)(void))at;
   return f();
}

/* bra to @to from an instruction at @from */
static u16 bra(u32 from, u32 to)
{
   return (u16)(0xA000 | (((to - (from + 4)) >> 1) & 0xFFF));
}

static void fail(u32 step, u32 which)
{
   *VERDICT = 0xBAD00000 | (step << 8) | (which & 0xFF);
   for (;;)
      ;
}

/* G number @j, written so that it returns @j: its two halves are @split
 * and the rest, so that it can be rewritten and still return the same. */
static void write_g(u32 j, u32 split)
{
   volatile u16 *g = CODE(G_AT(j));

   g[0] = (u16)(0xE000 | ((j - split) & 0xFF));     /* mov #j-split,r0 */
   g[1] = (u16)(0x7000 | (split & 0xFF));           /* add #split,r0 */
   g[2] = 0x7001;                                   /* add #1,r0 */
   g[3] = 0x70FF;                                   /* add #-1,r0 */
   g[4] = 0x7002;                                   /* add #2,r0 */
   g[5] = 0x70FE;                                   /* add #-2,r0 */
   g[6] = 0x000B;                                   /* rts */
   g[7] = 0x0009;                                   /* nop */
}

void cmain(void)
{
   u32 i, j;

   *VERDICT = 1;
   CODE(P_AT)[0]  = bra(P_AT, H2_AT);
   CODE(P_AT)[1]  = 0x0009;                         /* nop */
   CODE(H2_AT)[0] = 0x000B;                         /* rts */
   CODE(H2_AT)[1] = 0xE000;                         /* mov #0,r0 */
   CODE(T_AT)[0]  = bra(T_AT, Q_AT);
   CODE(T_AT)[1]  = 0xE100;                         /* mov #0,r1 */
   CODE(Q_AT)[0]  = 0x000B;                         /* rts */
   CODE(Q_AT)[1]  = 0x6013;                         /* mov r1,r0 */
   for (j = 0; j < GS; j++)
      write_g(j, 0);

   /* Rewritten and run, ten times each: from the seventh or so on they
    * are compiled into the temporary cache. */
   *VERDICT = 2;
   for (i = 0; i < 10; i++)
   {
      CODE(T_AT)[1] = (u16)(0xE100 | i);
      if (call(T_AT) != i)
         fail(2, i);
   }
   *VERDICT = 3;
   for (i = 0; i < 10; i++)
   {
      CODE(H2_AT)[1] = (u16)(0xE000 | (20 + i));
      if (call(P_AT) != 20 + i)
         fail(3, i);
   }
   /* (once more: the call after a rewrite finds the old H2 still there,
    * is linked to it, and is unlinked as it is thrown away. It is this
    * call that links P to the H2 that stays.) */
   if (call(P_AT) != 29)
      fail(3, 10);
   *VERDICT = 4;
   for (j = 0; j < GS; j++)
      for (i = 0; i < 10; i++)
      {
         write_g(j, i);
         if (call(G_AT(j)) != j)
            fail(4, j);
      }

   /* T, over and over: the temporary cache is filled and emptied. */
   *VERDICT = 5;
   for (i = 0; i < TURNS; i++)
   {
      CODE(T_AT)[1] = (u16)(0xE100 | (i & 0x7F));
      if (call(T_AT) != (i & 0x7F))
         fail(5, i);
   }

   /* (1) P was linked to H2, which has been emptied out since */
   *VERDICT = 6;
   if (call(P_AT) != 29)
      fail(6, 0);

   /* (2) the Gs, compiled into the room old Ts were in; then Q goes */
   *VERDICT = 7;
   for (j = 0; j < GS; j++)
      if (call(G_AT(j)) != j)
         fail(7, j);
   *VERDICT = 8;
   *SCRATCH = 0x12345678;
   for (j = 0; j < GS; j++)
      if (call(G_AT(j)) != j)
         fail(8, j);

   /* and everything still does what it is */
   *VERDICT = 9;
   CODE(T_AT)[1] = 0xE155;
   if (call(T_AT) != 0x55)
      fail(9, 0);
   if (call(P_AT) != 29)
      fail(9, 1);
   CODE(H2_AT)[1] = 0xE031;
   if (call(P_AT) != 0x31)
      fail(9, 2);

   *VERDICT = 0x600D5AC0;
   for (;;)
      ;
}
