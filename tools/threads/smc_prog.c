/* Bare-metal SH4 program for headless.sh: code that is written over while
 * it is in use, against a recompiler that keeps what it has compiled.
 *
 * A recompiler compiles a block of code once and runs the compilation from
 * then on, so it has to find out when the code has been written over. It
 * protects the pages it has compiled from (the first write to one faults,
 * and its blocks are thrown away) and, in a page it has had to give up
 * protecting, has each block look at its own code before it runs. Code a
 * game keeps rewriting is compiled into a small cache of its own, the
 * temporary one, which is emptied whenever it is full and filled again
 * from the start. And a recompiler that links its blocks - one jumping
 * straight into the next - has to take those jumps back whenever a block
 * goes, by any of these roads.
 *
 * The program writes its test code into four pages of memory:
 *
 *   A  P    bra H2 ; nop                  never rewritten
 *      H2   rts ; mov #v,r0               rewritten at the start, then left
 *      T    bra Q ; mov #k,r1             rewritten forty thousand times
 *      G0 to G63                          sixty-four functions that return
 *                                         their number, rewritten at the
 *                                         start
 *   B  Q    rts ; mov r1,r0               never rewritten
 *   C  W    a loop of two blocks that stores to a word of page C itself
 *   D  X    mov.w r4,@r5 ; bra Y ; nop    writes Y's second instruction
 *      Y    rts ; mov #v,r0               and goes on into it
 *
 * Everything in A but P is rewritten often enough at the start to be
 * compiled into the temporary cache from then on. P is then linked to H2
 * and T to Q. T is rewritten and called until the temporary cache has been
 * emptied several times over: each time T is compiled anew, linked to Q
 * anew, and what it returns is looked at. Then:
 *
 *   - P is called: H2 has to answer, with what it was last rewritten to
 *     return - a block that was linked to one of the temporary cache must
 *     not go on jumping to where that was (step 6);
 *   - the Gs are called, which compiles them into the room that old
 *     compilations of T were in; a word of page B is written, at which
 *     every block of that page goes, Q among them, and whatever was linked
 *     to Q is unlinked; and the Gs have to return their numbers still -
 *     nothing may be written where a block used to be (step 8);
 *   - W is run: its first store is to the page it is running from, so the
 *     block that is running is thrown away under itself, and has to get to
 *     its end and on to the next block all the same (step 10);
 *   - X is run twenty times, each time writing over the code it then jumps
 *     to, which has to do what was just written (step 11).
 *
 * Then the other roads by which a write gets to code. For each a function
 * is written into a page of its own and run three times, so that it is
 * compiled and its page protected; it is rewritten, and has to do what was
 * written; and again, the page now being one that is no longer protected:
 *
 *   - main memory is there four times over in area 3, and that in each of
 *     the SH4's regions: the function is rewritten at 0x8D000000,
 *     0x8F000000, 0xAD000000, 0x0D000000 and 0xCC000000 and on, and run
 *     from one of those (step 13);
 *   - it is rewritten a byte at a time, by the SH4's DMA controller and
 *     through a store queue (step 14);
 *   - it is entered at two places, and it has the instruction that goes
 *     with its RTS in the next page (step 15);
 *   - it is rewritten after the recompiler has thrown away everything it
 *     had, which starts the protecting of pages again (step 16);
 *   - with the MMU on: its page is at two addresses, written at one and
 *     run at the other; a word is read at one of two and written at the
 *     other; and it is in a page of 64K, not in the first 4K of it (step
 *     17);
 *   - it is rewritten at 0x2C000000 and on, where the host has nothing
 *     mapped and the write goes the long way round (step 18: the last,
 *     because the recompiled write is not the same one afterwards).
 *
 * After that it goes on for ever, a hundred or so times a frame: T, one of
 * the Gs and Y rewritten and run and W run, each looked at. That is for a
 * state saved at one moment and loaded at a later one: memory is then the
 * code of the earlier moment again, and nothing compiled from the later
 * one may be left (step 12).
 *
 * All of it is done twice over if the MMU is there to be turned on (the
 * core option "Force Windows CE Mode"): the program maps main memory at
 * other addresses and writes and runs the four pages there, as a Windows
 * CE game's code is - the recompilers do nearly everything another way
 * then.
 *
 * The word at 0x8c00f800 says how far it has got. It is 0x600D5AC0 once
 * the steps are done and the going on for ever has begun - 0x600D5ACE if
 * that was with the MMU on - or 0xBAD0ssnn: the step, and which function
 * or turn it was.
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
        "1: .long 0x8c00f000\n2: .long cmain\n"
        /* Where VBR points when the MMU is on. 0x400 on is the handler for a
         * TLB miss: any page that is asked for is mapped to main memory at
         * the address's low 24 bits (0x10200000 is 0x0C200000, and so is
         * 0x11200000), to be read and written, in the TLB entry that the
         * page's number chooses. Pages are of 4K, but from 0x12000000 on,
         * where they are of 64K. */
        ".text\n.align 2\n.global smc_vbr\nsmc_vbr:\n  .fill 0x400, 1, 0\n"
        "  mov.l 1f, r0\n  mov.l @r0, r1\n  mov.l 2f, r2\n  and r2, r1\n"
        "  mov.l @r0, r4\n  shlr16 r4\n  shlr8 r4\n"
        "  mov.l 3f, r3\n  mov #0x12, r6\n  cmp/eq r6, r4\n  bf 5f\n"
        "  mov.l 6f, r3\n"
        "5: or r3, r1\n  mov.l r1, @(4,r0)\n"                                /* PTEL */
        "  mov.l @r0, r1\n  shlr8 r1\n  shlr2 r1\n  shlr2 r1\n"
        "  mov #0x3F, r2\n  and r2, r1\n  shll8 r1\n  shll2 r1\n"            /* the entry */
        "  mov.l @(16,r0), r2\n  mov.l 4f, r3\n  and r3, r2\n  or r1, r2\n"
        "  mov.l r2, @(16,r0)\n"                                             /* MMUCR.URC */
        "  .word 0x0038\n  rte\n  nop\n"                                     /* ldtlb */
        "  .align 2\n1: .long 0xFF000000\n2: .long 0x00FFF000\n"
        /* main memory; valid, read and write in any mode, dirty, shared: of 4K, of 64K */
        "3: .long 0x0C000176\n4: .long 0xFFFF03FF\n6: .long 0x0C0001E6\n");
extern char smc_vbr[];

#define VERDICT  ((volatile u32 *)0x8c00f800)
#define CODE(at) ((volatile u16 *)(at))

/* The four pages: in main memory, and where they are run from - the same
 * place, or where the MMU has them. */
#define PAGES_IN_MEMORY  0x8c200000u
#define PAGES_MAPPED     0x10200000u
static u32 pages;

#define PAGE_A   (pages + 0x0000)
#define PAGE_B   (pages + 0x1000)
#define PAGE_C   (pages + 0x2000)
#define PAGE_D   (pages + 0x3000)

#define P_AT     (PAGE_A + 0x000)
#define H2_AT    (PAGE_A + 0x010)
#define G_AT(j)  (PAGE_A + 0x100 + (j) * 0x20)
#define T_AT     (PAGE_A + 0xF00)
#define Q_AT     (PAGE_B + 0x000)
#define SCRATCH  ((volatile u32 *)(PAGE_B + 0x800))
#define W_AT     (PAGE_C + 0x000)
#define W_WORD   ((volatile u32 *)(PAGE_C + 0x800))
#define X_AT     (PAGE_D + 0x000)
#define Y_AT     (PAGE_D + 0x010)

#define GS       64
#ifndef TURNS
#define TURNS    40000
#endif

static u32 call(u32 at)
{
   u32 (* volatile f)(void) = (u32 (*)(void))at;
   return f();
}

static u32 call2(u32 at, u32 a, u32 b)
{
   u32 (* volatile f)(u32, u32) = (u32 (*)(u32, u32))at;
   return f(a, b);
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

/* W(n, p): stores n, n-1 ... 1 to the word at p and returns 0. Two blocks
 * that go round by way of each other. */
static void write_w(void)
{
   volatile u16 *w = CODE(W_AT);

   w[0] = 0x2542;                                   /* mov.l r4,@r5 */
   w[1] = bra(W_AT + 2, W_AT + 8);                  /* bra 1f */
   w[2] = 0x0009;                                   /* nop */
   w[3] = 0x0009;                                   /* nop */
   w[4] = 0x4410;                                   /* 1: dt r4 */
   w[5] = (u16)(0x8B00 | ((0 - 7) & 0xFF));         /* bf W */
   w[6] = 0x000B;                                   /* rts */
   w[7] = 0x6043;                                   /* mov r4,r0 */
}

/* (A function of its own: what is compiled before the MMU is turned on is
 * compiled for a machine without one, and a recompiler finishes the block
 * it is in.) */
static u32 __attribute__((noinline)) mmu_probe(void)
{
   return *(volatile u32 *)PAGES_MAPPED == 0x11223344;
}

/* The MMU, if it can be turned on: the four pages are then somewhere else
 * as well. 1 if it is on. */
static u32 mmu_on(void)
{
   __asm__ volatile ("ldc %0, vbr" : : "r" (smc_vbr));
   *(volatile u32 *)PAGES_IN_MEMORY = 0x11223344;
   *(volatile u32 *)0xFF000000 = 0;                 /* PTEH: address space 0 */
   *(volatile u32 *)0xFF000010 = 0x00000005;        /* MMUCR: on, and the TLB emptied */
   if (mmu_probe())
      return 1;
   *(volatile u32 *)0xFF000010 = 0;                 /* nobody translates: as it was */
   return 0;
}

static void delay(void)
{
   volatile u32 n;
   for (n = 0; n < 3000; n++)
      ;
}

/* Pages for the functions of step 13 on: one each, with one between. */
static u32 next_page;

static u32 page(void)
{
   const u32 at = 0x00300000 + next_page * 0x2000;

   next_page++;
   return at;
}

/* A function of twenty-two instructions, written at @w and on, run at @x
 * and on and rewritten at @r and on: the three are the same memory. 0, or
 * what it was that did not return what had been written. */
static u32 __attribute__((noinline)) rewritten(u32 w, u32 x, u32 r)
{
   const u32 at = page();
   u32 i;

   for (i = 0; i < 20; i++)
      CODE(w + at)[i] = 0x0009;                     /* nop */
   CODE(w + at)[20] = 0x000B;                       /* rts */
   CODE(w + at)[21] = 0xE011;                       /* mov #0x11,r0 */
   for (i = 0; i < 3; i++)
      if (call(x + at) != 0x11)
         return 1;
   CODE(r + at)[21] = 0xE022;
   if (call(x + at) != 0x22)
      return 2;
   CODE(r + at)[21] = 0xE033;
   if (call(x + at) != 0x33)
      return 3;
   return 0;
}

/* RTS ; MOV #0x11,r0 in a page of its own, run three times. Where it is. */
static u32 fresh(void)
{
   const u32 at = 0x8c000000 + page();
   u32 i;

   CODE(at)[0] = 0x000B;
   CODE(at)[1] = 0xE011;
   for (i = 0; i < 3; i++)
      if (call(at) != 0x11)
         fail(14, 0);
   return at;
}

/* ...rewritten a byte at a time */
static u32 __attribute__((noinline)) by_bytes(void)
{
   const u32 at = fresh();
   u32 k;

   for (k = 0; k < 2; k++)
   {
      *(volatile unsigned char *)(at + 2) = (unsigned char)(0x22 + k * 0x11);
      if (call(at) != 0x22 + k * 0x11)
         return 1 + k;
   }
   return 0;
}

/* ...by the DMA controller: channel 1, two bytes */
static u32 __attribute__((noinline)) by_dma(void)
{
   static u16 from[2];
   volatile u32 *sar = (volatile u32 *)0xFFA00010, *dar = (volatile u32 *)0xFFA00014;
   volatile u32 *count = (volatile u32 *)0xFFA00018, *chcr = (volatile u32 *)0xFFA0001C;
   const u32 at = fresh();
   u32 k;

   for (k = 0; k < 2; k++)
   {
      from[0] = (u16)(0xE022 + k * 0x11);
      *(volatile u32 *)0xFFA00040 = 0x8201;         /* DMAOR */
      *chcr = 0;
      *sar = (u32)from;
      *dar = at + 2;
      *count = 2;
      *chcr = (1 << 14) | (1 << 12) | (4 << 8) | (1 << 4) | 1;   /* both up, by the program, bytes, go */
      if (call(at) != 0x22 + k * 0x11)
         return 1 + k;
   }
   return 0;
}

/* ...through a store queue: thirty-two bytes at once */
static u32 __attribute__((noinline)) by_store_queue(void)
{
   const u32 at = fresh();
   volatile u32 *sq = (volatile u32 *)(0xE0000000 | (at & 0x03FFFFE0));
   u32 i, k, bad = 0;

   (*(volatile u32 *)0xFF000038) = 0x0C;            /* QACR0, QACR1: main memory */
   (*(volatile u32 *)0xFF00003C) = 0x0C;
   for (k = 0; k < 2 && !bad; k++)
   {
      sq[0] = 0x000B | ((0xE022 + k * 0x11) << 16);
      for (i = 1; i < 8; i++)
         sq[i] = 0x00090009;
      __asm__ volatile ("pref @%0" : : "r" (sq));
      if (call(at) != 0x22 + k * 0x11)
         bad = 1 + k;
   }
   (*(volatile u32 *)0xFF000038) = 0x10;
   (*(volatile u32 *)0xFF00003C) = 0x10;
   return bad;
}

/* A function entered at its start and at its third instruction: two
 * blocks of the same code. From the start it returns 0x40 and what it
 * adds; from the third instruction, what it is given and what it adds
 * from there. */
static u32 __attribute__((noinline)) two_entries(void)
{
   const u32 at = 0x8c000000 + page();
   volatile u16 *c = CODE(at);
   u32 i;

   c[0] = 0xE440;                                   /* mov #0x40,r4 */
   c[1] = 0x7401;                                   /* add #1,r4 */
   c[2] = 0x7402;                                   /* add #2,r4 */
   c[3] = 0x7404;                                   /* add #4,r4 */
   c[4] = 0x000B;                                   /* rts */
   c[5] = 0x6043;                                   /* mov r4,r0 */
   for (i = 0; i < 3; i++)
      if (call(at) != 0x47 || call2(at + 4, 0x10, 0) != 0x16)
         return 1;
   c[3] = 0x7408;
   if (call(at) != 0x4B)
      return 2;
   if (call2(at + 4, 0x10, 0) != 0x1A)
      return 3;
   c[2] = 0x7403;
   if (call2(at + 4, 0x10, 0) != 0x1B)
      return 4;
   if (call(at) != 0x4C)
      return 5;
   return 0;
}

/* A function of @nops NOPs and an RTS that end a page, with what goes with
 * the RTS at the start of the next: that one is rewritten, and then the
 * first NOP if there is one. */
static u32 __attribute__((noinline)) two_pages(u32 nops)
{
   u32 at, i;

   page();
   at = 0x8c000000 + page();                        /* the second page */
   for (i = 1; i <= nops; i++)
      CODE(at)[-1 - (int)i] = 0x0009;
   CODE(at)[-1] = 0x000B;
   CODE(at)[0] = 0xE011;
   for (i = 0; i < 3; i++)
      if (call(at - 2 - nops * 2) != 0x11)
         return 1;
   CODE(at)[0] = 0xE022;
   if (call(at - 2 - nops * 2) != 0x22)
      return 2;
   CODE(at)[0] = 0xE033;
   if (call(at - 2 - nops * 2) != 0x33)
      return 3;
   if (nops)
   {
      CODE(at)[-1 - (int)nops] = 0x0008;            /* clrt */
      if (call(at - 2 - nops * 2) != 0x33)
         return 4;
   }
   return 0;
}

/* The recompiler made to throw away everything it has: these cores do
 * when they are asked to compile what is at 0x8c0000e0 (it is where a
 * game goes back to the BIOS through), so something new is put there
 * each time. (A core that does not is none the worse for it: what follows
 * is then the test that came before, again.) */
static void __attribute__((noinline)) forget(void)
{
   static u32 turn;

   CODE(0x8c0000e0)[0] = 0x000B;
   CODE(0x8c0000e0)[1] = (u16)(0xE100 | (turn++ & 0x7F));   /* mov #turn,r1 */
   call(0x8c0000e0);
}

/* As rewritten(), with everything thrown away once the page is one that
 * is no longer protected. */
static u32 __attribute__((noinline)) rewritten_after_forgetting(u32 w, u32 x, u32 r)
{
   const u32 at = page();
   u32 i;

   CODE(w + at)[0] = 0x000B;
   CODE(w + at)[1] = 0xE011;
   for (i = 0; i < 3; i++)
      if (call(x + at) != 0x11)
         return 1;
   CODE(r + at)[1] = 0xE012;
   for (i = 0; i < 3; i++)
      if (call(x + at) != 0x12)
         return 2;
   forget();
   for (i = 0; i < 3; i++)
      if (call(x + at) != 0x12)
         return 3;
   CODE(r + at)[1] = 0xE022;
   if (call(x + at) != 0x22)
      return 4;
   CODE(r + at)[1] = 0xE033;
   if (call(x + at) != 0x33)
      return 5;
   return 0;
}

/* With the MMU on: a page that is at two addresses, read at the first and
 * then written at the second. */
static u32 __attribute__((noinline)) read_here_written_there(void)
{
   const u32 at = page();

   *(volatile u32 *)(0x8c000000 + at) = 0xAAAA0001;
   *(volatile u32 *)(0x8c000000 + at + 4) = 0xAAAA0002;
   if (*(volatile u32 *)(0x10000000 + at) != 0xAAAA0001)
      return 1;
   *(volatile u32 *)(0x11000000 + at) = 0xBBBB0001;
   if (*(volatile u32 *)(0x10000000 + at) != 0xBBBB0001)
      return 2;
   if (*(volatile u32 *)(0x8c000000 + at) != 0xBBBB0001)
      return 3;
   if (*(volatile u32 *)(0x11000000 + at + 4) != 0xAAAA0002)
      return 4;
   return 0;
}

/* With the MMU on: a function in the sixth 4K of a page of 64K, rewritten
 * at the address the MMU has the page at. @first: the first 4K of the page
 * has been written to before. Then another in the tenth 4K, rewritten
 * after the TLB has been emptied and the page read again: what stands for
 * the page in the host is then made anew, some of its 4Ks code that has
 * been written to and some code that has not. */
static u32 __attribute__((noinline)) in_a_large_page(u32 first)
{
   u32 at, i;

   do
      at = page();
   while (at & 0xFFFF);
   next_page += 7;
   if (first)
      *(volatile u32 *)(0x12000000 + at) = 1;
   CODE(0x8c000000 + at + 0x5000)[0] = 0x000B;
   CODE(0x8c000000 + at + 0x5000)[1] = 0xE011;
   for (i = 0; i < 3; i++)
      if (call(0x12000000 + at + 0x5000) != 0x11)
         return 1;
   CODE(0x12000000 + at + 0x5000)[1] = 0xE022;
   if (call(0x12000000 + at + 0x5000) != 0x22)
      return 2;
   CODE(0x12000000 + at + 0x5000)[1] = 0xE033;
   if (call(0x12000000 + at + 0x5000) != 0x33)
      return 3;

   CODE(0x8c000000 + at + 0x9000)[0] = 0x000B;
   CODE(0x8c000000 + at + 0x9000)[1] = 0xE044;
   for (i = 0; i < 3; i++)
      if (call(0x12000000 + at + 0x9000) != 0x44)
         return 4;
   *(volatile u32 *)0xFF000010 = 0x00000005;        /* MMUCR: on, and the TLB emptied */
   if (*(volatile u16 *)(0x12000000 + at + 0x5000) != 0x000B)
      return 5;
   CODE(0x12000000 + at + 0x9000)[1] = 0xE055;
   if (call(0x12000000 + at + 0x9000) != 0x55)
      return 6;
   CODE(0x12000000 + at + 0x5000)[1] = 0xE066;
   if (call(0x12000000 + at + 0x5000) != 0x66)
      return 7;
   return 0;
}

void cmain(void)
{
   /* where main memory is, beside 0x8c000000 */
   static const u32 others[] = { 0x8d000000, 0x8f000000, 0xac000000, 0xad000000, 0x0c000000, 0x0d000000, 0xcc000000 };
   u32 i, j, mmu, r;

   *VERDICT = 1;
   mmu = mmu_on();
   pages = mmu ? PAGES_MAPPED : PAGES_IN_MEMORY;

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
   write_w();
   CODE(X_AT)[0]  = 0x2541;                         /* mov.w r4,@r5 */
   CODE(X_AT)[1]  = bra(X_AT + 2, Y_AT);
   CODE(X_AT)[2]  = 0x0009;                         /* nop */
   CODE(Y_AT)[0]  = 0x000B;                         /* rts */
   CODE(Y_AT)[1]  = 0xE07F;                         /* mov #127,r0 */

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

   /* P was linked to H2, which has been emptied out since */
   *VERDICT = 6;
   if (call(P_AT) != 29)
      fail(6, 0);

   /* the Gs, compiled into the room old Ts were in; then Q goes */
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

   /* W: the first store is to the page it runs from */
   *VERDICT = 10;
   for (i = 0; i < 3; i++)
   {
      *W_WORD = 0;
      if (call2(W_AT, 50 + i, (u32)W_WORD) != 0)
         fail(10, i);
      if (*W_WORD != 1)
         fail(10, 0x10 + i);
   }

   /* X: writes over what it then jumps to */
   *VERDICT = 11;
   for (i = 0; i < 20; i++)
      if (call2(X_AT, 0xE000 | (40 + i), Y_AT + 2) != 40 + i)
         fail(11, i);

   /* Rewritten at another address of the same memory. (0xcc000000 is not
    * one with the MMU on: P3 is then what the MMU says it is.) */
   *VERDICT = 13;
   for (i = 0; i < sizeof(others) / sizeof(others[0]) - (mmu ? 1 : 0); i++)
      if ((r = rewritten(0x8c000000, 0x8c000000, others[i])) != 0)
         fail(13, (i << 4) | r);
   /* ...and run at another: written at the first, or all of it there */
   if ((r = rewritten(0x8c000000, 0x8d000000, 0x8c000000)) != 0)
      fail(13, 0x80 | r);
   if ((r = rewritten(0x8d000000, 0x8d000000, 0x8d000000)) != 0)
      fail(13, 0x90 | r);
   if ((r = rewritten(0x8c000000, 0xac000000, 0x8c000000)) != 0)
      fail(13, 0xA0 | r);
   if ((r = rewritten(0x0c000000, 0x0c000000, 0x8c000000)) != 0)
      fail(13, 0xB0 | r);
   if ((r = rewritten(0x8c000000, 0x0c000000, 0x0c000000)) != 0)
      fail(13, 0xC0 | r);

   /* Rewritten by something other than an instruction that stores a word.
    * (The store queues are the MMU's with it on.) */
   *VERDICT = 14;
   if ((r = by_bytes()) != 0)
      fail(14, 0x10 | r);
   if ((r = by_dma()) != 0)
      fail(14, 0x20 | r);
   if (!mmu && (r = by_store_queue()) != 0)
      fail(14, 0x30 | r);

   *VERDICT = 15;
   if ((r = two_entries()) != 0)
      fail(15, 0x10 | r);
   if ((r = two_pages(0)) != 0)
      fail(15, 0x20 | r);
   if ((r = two_pages(3)) != 0)
      fail(15, 0x30 | r);

   *VERDICT = 16;
   if ((r = rewritten_after_forgetting(0x8c000000, 0x8c000000, 0x8c000000)) != 0)
      fail(16, 0x10 | r);
   if (mmu && (r = rewritten_after_forgetting(0x10000000, 0x10000000, 0x10000000)) != 0)
      fail(16, 0x20 | r);

   *VERDICT = 17;
   if (mmu)
   {
      static const u32 where[][3] =
      {
         { 0x10000000, 0x10000000, 0x10000000 }, { 0x10000000, 0x10000000, 0x11000000 },
         { 0x10000000, 0x11000000, 0x10000000 }, { 0x11000000, 0x10000000, 0x11000000 },
         { 0x10000000, 0x10000000, 0x8c000000 }, { 0x8c000000, 0x8c000000, 0x10000000 },
         { 0x10000000, 0x10000000, 0x8d000000 }, { 0x8c000000, 0x10000000, 0x8c000000 },
      };

      for (i = 0; i < sizeof(where) / sizeof(where[0]); i++)
         if ((r = rewritten(where[i][0], where[i][1], where[i][2])) != 0)
            fail(17, (i << 4) | r);
      if ((r = read_here_written_there()) != 0)
         fail(17, 0x80 | r);
      if ((r = in_a_large_page(0)) != 0)
         fail(17, 0x90 | r);
      if ((r = in_a_large_page(1)) != 0)
         fail(17, 0xA0 | r);
   }

   *VERDICT = 18;
   if ((r = rewritten(0x8c000000, 0x8c000000, 0x2c000000)) != 0)
      fail(18, r);

   /* And for ever, for whoever saves a state and loads it later. */
   *VERDICT = mmu ? 0x600D5ACE : 0x600D5AC0;
   for (i = 0; ; i++)
   {
      const u32 k = i & 0x7F;

      delay();
      CODE(T_AT)[1] = (u16)(0xE100 | k);
      if (call(T_AT) != k)
         fail(12, 0);
      write_g(i % GS, i % 7);
      if (call(G_AT(i % GS)) != i % GS)
         fail(12, 1);
      if (call2(X_AT, 0xE000 | k, Y_AT + 2) != k)
         fail(12, 2);
      *W_WORD = 0;
      if (call2(W_AT, 3 + (i & 3), (u32)W_WORD) != 0 || *W_WORD != 1)
         fail(12, 3);
      if (call(P_AT) != 0x31)
         fail(12, 4);
   }
}
