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
 *     mapped and the write goes the long way round (step 23: the last,
 *     because the recompiled write is not the same one afterwards).
 *
 * And with the MMU on, what is not rewritten code but goes wrong the same
 * way - something kept of how things were, that is used after they have
 * changed:
 *
 *   - a page is taken out of the TLB and mapped again for other memory:
 *     taken out by its address (the write to the TLB that finds the entry
 *     for it), by the number of its entry, by a write of the other memory
 *     into its entry, or with everything else; while its entry is still
 *     one of the TLB's 64, and after another page has taken that entry's
 *     place. Data, and code (step 18);
 *   - the same in P3, 0xC0000000 and on, which the MMU translates like
 *     the addresses below 0x80000000: where the same address without the
 *     MMU is main memory (step 19), where it is nothing, and where it
 *     is video memory (step 20).
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
 * And the MMU itself, where an emulator made for Windows CE cuts corners
 * that another program walks into:
 *
 *   - pages of 1K, each mapped where its neighbour would be; a page that
 *     may be read and not written; the TLB emptied and then one of its
 *     entries written to; and more pages than an emulator that keeps what
 *     the TLB is given may have room for (step 21);
 *   - if the program was started with a TLB of its own making (smc_elf.py
 *     --own-tlb: it puts a page into the TLB by hand and only then turns
 *     translation on, which is how a core knows such a program): the
 *     exception for the first write to a page; a page that has to be
 *     asked for again once another has taken its entry; a page for
 *     privileged mode only, read from user mode; and translation turned
 *     off and on again a thousand times (step 22).
 *
 * The word at 0x8c00f800 says how far it has got. It is 0x600D5AC0 once
 * the steps are done and the going on for ever has begun - 0x600D5ACE if
 * that was with the MMU on, 0x600D5AC5 if with a TLB of its own - or
 * 0xBAD0ssnn: the step, and which function or turn it was.
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
        /* 16 bytes into the program: not 0 if it is to bring a TLB of its own
         * (smc_elf.py --own-tlb sets it) */
        ".global smc_own_tlb\nsmc_own_tlb: .long 0\n"
        /* Where VBR points when the MMU is on: 0x100 on is where an exception
         * goes and 0x400 on where a TLB miss does. Both go to smc_fault(),
         * with what an exception does not put aside put aside. (r0 to r7 are
         * another set in there.) */
        ".text\n.align 2\n.global smc_vbr\nsmc_vbr:\n  .space 0x100\n"
        "  mov.l 1f, r0\n  jmp @r0\n  nop\n  .align 2\n1: .long smc_enter\n"
        "  .space smc_vbr + 0x400 - .\n"
        "  mov.l 1f, r0\n  jmp @r0\n  nop\n  .align 2\n1: .long smc_enter\n"
        "smc_enter:\n"
        "  sts.l pr, @-r15\n  sts.l mach, @-r15\n  sts.l macl, @-r15\n"
        "  mov.l 1f, r0\n  jsr @r0\n  nop\n"
        "  lds.l @r15+, macl\n  lds.l @r15+, mach\n  lds.l @r15+, pr\n"
        "  rte\n  nop\n  .align 2\n1: .long smc_fault\n"
        /* smc_enter_user(where, what): the function at @where - an address
         * the MMU maps - is run in user mode and given @what. It has to get
         * back into privileged mode (TRAPA) before it returns. */
        ".global smc_enter_user\nsmc_enter_user:\n"
        "  sts.l pr, @-r15\n  mova 1f, r0\n  lds r0, pr\n"
        "  stc sr, r1\n  mov.l 2f, r2\n  or r1, r2\n  ldc r2, sr\n"           /* nothing may come between */
        "  mov.l 3f, r2\n  and r2, r1\n  ldc r1, ssr\n  ldc r4, spc\n"
        "  mov r5, r4\n  rte\n  nop\n  .align 2\n"
        "1: lds.l @r15+, pr\n  rts\n  nop\n  .align 2\n"
        "2: .long 0x10000000\n3: .long 0xBFFFFFFF\n");
/* smc_move64(from, to): 64 bits from one place to another, the way a game
 * moves its vertices: with FPSCR.SZ on, which makes fmov take two words
 * at a time. (By number: this is compiled for an SH4 without an FPU.) */
__asm__(".text\n.align 2\n.global smc_move64\nsmc_move64:\n"
        "  .word 0x026A\n"                           /* sts fpscr, r2 */
        "  mov.l 1f, r3\n"
        "  .word 0x436A\n"                           /* lds r3, fpscr */
        "  .word 0xF048\n"                           /* fmov @r4, dr0 */
        "  .word 0xF50A\n"                           /* fmov dr0, @r5 */
        "  .word 0x426A\n"                           /* lds r2, fpscr */
        "  rts\n  nop\n  .align 2\n"
        "1: .long 0x00140001\n");
extern char smc_vbr[];
extern const u32 smc_own_tlb;
extern void smc_move64(u32 from, u32 to);
extern u32 smc_enter_user(u32 where, u32 what);

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

/* The TLB, written to as memory: an entry's address half and its data half
 * by the entry's number, and the address that finds the entry for a page. */
#define TLB_ADDRESS(n)  ((volatile u32 *)(0xF6000000 | ((n) << 8)))
#define TLB_DATA(n)     ((volatile u32 *)(0xF7000000 | ((n) << 8)))
#define TLB_FIND        ((volatile u32 *)0xF6000080)
#define MMUCR           ((volatile u32 *)0xFF000010)
#define WORD(at)        ((volatile u32 *)(at))

/* What smc_fault() counts, and how far up from its usual place it maps a
 * page from 0x13000000, 0xC9000000 or 0xCD000000 on. */
/* (volatile: they change under the code that reads them) */
volatile u32 smc_shift;
volatile u32 smc_misses, smc_first_writes, smc_reads_refused, smc_writes_refused, smc_traps;

/* Every exception there is with the MMU on, and the TLB miss.
 *
 * A page that is asked for is mapped to main memory at the address's low
 * 24 bits (0x10200000 is 0x0C200000, and so is 0x11200000), in the TLB
 * entry that the page's number chooses, to be read and written in any
 * mode and as one that has been written to. But:
 *
 *   0x12000000 on   pages of 64K
 *   0x13000000 on   smc_shift further up than that (0xC9000000 and
 *                   0xCD000000 on as well)
 *   0x15000000 on   as pages that have not been written to yet
 *   0x16000000 on   as pages that may not be written to
 *   0x17000000 on   pages of 1K, each where its neighbour would be: the
 *                   first and second of a 4K change places, and the third
 *                   and fourth
 *   0x18000000 on   for privileged mode only
 *   0x19000000 on   each page where its neighbour would be: the first and
 *                   second of an 8K change places
 *
 * A page that was not to be touched the way it was - the exceptions for a
 * first write and for the two refusals - is counted and given again with
 * nothing in the way, and a TRAPA is answered by going back in privileged
 * mode. */
void smc_fault(void)
{
   const u32 event = *WORD(0xFF000024);             /* EXPEVT */
   const u32 va = *WORD(0xFF000000) & 0xFFFFFC00;   /* PTEH: the page */
   const u32 top = va >> 24;
   u32 flags = 0x176, to = va & 0x00FFF000, entry = (va >> 12) & 0x3F, ssr;

   switch (event)
   {
      case 0x160:
         smc_traps++;
         __asm__ volatile ("stc ssr, %0" : "=r" (ssr));
         ssr |= 0x40000000;
         __asm__ volatile ("ldc %0, ssr" : : "r" (ssr));
         return;
      case 0x40: case 0x60:
         smc_misses++;
         if (top == 0x15)
            flags = 0x172;
         else if (top == 0x16)
            flags = 0x156;
         else if (top == 0x18)
            flags = 0x136;
         break;
      case 0x80:
         smc_first_writes++;
         break;
      case 0xA0:
         smc_reads_refused++;
         break;
      case 0xC0:
         smc_writes_refused++;
         break;
      default:
         *((volatile u32 *)0x8c00f800) = 0xBAD0FF00 | (event >> 5);
         for (;;)
            ;
   }
   if (top == 0x12)
      flags = 0x1E6;
   if (top == 0x17)
   {
      flags = 0x166;
      to = (va & 0x00FFFC00) ^ 0x400;
      entry = (va >> 10) & 0x3F;
   }
   if (top == 0x19)
      to ^= 0x1000;
   if (top == 0x13 || top == 0xC9 || top == 0xCD)
      to += smc_shift;
   *WORD(0xFF000004) = 0x0C000000 | to | flags;     /* PTEL */
   *MMUCR = (*MMUCR & 0xFFFF03FF) | (entry << 10);  /* MMUCR.URC */
   __asm__ volatile (".word 0x0038");               /* ldtlb */
}

/* (A function of its own: what is compiled before the MMU is turned on is
 * compiled for a machine without one, and a recompiler finishes the block
 * it is in.) */
static u32 __attribute__((noinline)) mmu_probe(void)
{
   return *(volatile u32 *)PAGES_MAPPED == 0x11223344;
}

/* The MMU, if it can be turned on: the four pages are then somewhere else
 * as well. 1 if it is on; 2 if that was with a TLB of the program's own,
 * which is the page the probe reads put into its entry by hand and
 * translation turned on after that - what tells an emulator that this is
 * a program it has to do the whole MMU for, Windows CE or not. */
static u32 mmu_on(void)
{
   __asm__ volatile ("ldc %0, vbr" : : "r" (smc_vbr));
   *(volatile u32 *)PAGES_IN_MEMORY = 0x11223344;
   *(volatile u32 *)0xFF000000 = 0;                 /* PTEH: address space 0 */
   if (smc_own_tlb)
   {
      *MMUCR = 0x00000004;                          /* the TLB emptied */
      *TLB_ADDRESS((PAGES_MAPPED >> 12) & 0x3F) = PAGES_MAPPED | 0x300;
      *TLB_DATA((PAGES_MAPPED >> 12) & 0x3F) = (PAGES_IN_MEMORY & 0x00FFF000) | 0x0C000176;
      *MMUCR = 0x00000001;                          /* on */
   }
   else
      *MMUCR = 0x00000005;                          /* on, and the TLB emptied */
   if (mmu_probe())
      return smc_own_tlb ? 2 : 1;
   *MMUCR = 0;                                      /* nobody translates: as it was */
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

enum { BY_ADDRESS, BY_NUMBER, BY_REWRITING, WITH_ALL, HOW_MASK = 3, FALLEN_OUT = 4 };

/* The page at @va is taken out of the TLB, @how. (@va + 0x40000 goes into
 * the same entry: that is how the handler chooses.) */
static void __attribute__((noinline)) take_out(u32 va, u32 how, u32 other)
{
   const u32 entry = (va >> 12) & 0x3F;

   if (how & FALLEN_OUT)
      (void)*WORD(va + 0x40000);
   switch (how & HOW_MASK)
   {
      case BY_ADDRESS:
         *TLB_FIND = va & 0xFFFFF000;               /* and not valid */
         break;
      case BY_NUMBER:
         *TLB_ADDRESS(entry) = 0;
         *TLB_DATA(entry) = 0;
         break;
      case BY_REWRITING:
         /* the page's address as well, in case the entry is another's by now */
         *TLB_ADDRESS(entry) = (va & 0xFFFFF000) | 0x300;   /* written to, valid */
         *TLB_DATA(entry) = (other & 0x00FFF000) | 0x0C000176;
         break;
      default:
         *MMUCR = 0x00000005;                       /* on, and the TLB emptied */
         break;
   }
}

/* Two pages of main memory, one after the other, that say which they are;
 * an address that is the first, and after it has been taken out of the TLB
 * the second, and after that the first again. @code: they say it as
 * functions. */
static u32 __attribute__((noinline)) moved(u32 to, u32 how, u32 code)
{
   const u32 at = page(), va = to + at;
   const u32 first = code ? 0xE011000B : 0xAAAA0001, second = code ? 0xE022000B : 0xBBBB0001;
   u32 turn;

   *WORD(0x8c000000 + at) = first;                  /* rts ; mov #0x11,r0 */
   *WORD(0x8c000000 + at + 0x1000) = second;
   smc_shift = 0;
   take_out(va, WITH_ALL, 0);
   for (turn = 0; turn < 4; turn++)
   {
      const u32 now = (turn & 1) ? at + 0x1000 : at;

      if (turn)
      {
         smc_shift = now - at;
         take_out(va, how, now);
      }
      if (*WORD(va) != ((turn & 1) ? second : first))
         return 1 + turn * 3;
      if (code && call(va) != ((turn & 1) ? 0x22u : 0x11u))
         return 2 + turn * 3;
      /* written to as well, and what is written is in the page it is now */
      *WORD(va + 8) = 0xD00D0000 + turn;
      if (*WORD(0x8c000000 + now + 8) != 0xD00D0000 + turn)
         return 3 + turn * 3;
   }
   smc_shift = 0;
   take_out(va, WITH_ALL, 0);
   return 0;
}

/* More than an emulator that keeps what the TLB is given may have room
 * for: one page given seventy thousand times, taken out and asked for
 * again as many times, and seventy thousand pages asked for once. */
static u32 __attribute__((noinline)) many(void)
{
   const u32 at = page(), va = 0x10000000 + at;
   u32 i, sum = 0;

   *WORD(0x8c000000 + at) = 0x5EED0001;
   for (i = 0; i < 70000; i++)
   {
      *WORD(0xFF000000) = va;                       /* PTEH */
      *WORD(0xFF000004) = (at & 0x00FFF000) | 0x0C000176;   /* PTEL */
      *MMUCR = (*MMUCR & 0xFFFF03FF) | (((va >> 12) & 0x3F) << 10);
      __asm__ volatile (".word 0x0038");            /* ldtlb */
   }
   if (*WORD(va) != 0x5EED0001)
      return 1;
   for (i = 0; i < 70000; i++)
   {
      *TLB_FIND = va;
      sum += *WORD(va);
   }
   if (sum != 0x5EED0001u * 70000u)
      return 2;
   /* (from 0x14000000 to 0x25170000: all of main memory, four times over) */
   for (i = 0, sum = 0; i < 70000; i++)
      sum += *(volatile unsigned char *)(0x14000000 + i * 0x1000 + (at & 0xFFF));
   (void)sum;
   if (*WORD(va) != 0x5EED0001)
      return 3;
   *MMUCR = 0x00000005;
   return 0;
}

/* Pages of 1K, from 0x17000000 on: the four of a 4K are each where a
 * neighbour would be, which a table of addresses by the 4K - an
 * emulator's, a host's - cannot say. */
static u32 __attribute__((noinline)) small_pages(void)
{
   const u32 at = page();
   u32 k;

   for (k = 0; k < 4; k++)
      *WORD(0x8c000000 + at + k * 0x400) = 0x1000 + k;
   for (k = 0; k < 4; k++)
      if (*WORD(0x17000000 + at + k * 0x400) != 0x1000 + (k ^ 1))
         return 1 + k;
   *WORD(0x17000000 + at + 0x404) = 0x7777;
   if (*WORD(0x8c000000 + at + 0x004) != 0x7777)
      return 5;
   for (k = 4; k-- > 0; )
      if (*WORD(0x17000000 + at + k * 0x400) != 0x1000 + (k ^ 1))
         return 6 + k;
   return 0;
}

/* 64 bits at a time, from 0x19000000 on: the last four bytes of one page
 * and the first four of the next, which are nowhere near each other in
 * memory - the pages are each where the other would be. An emulator that
 * translates the address and takes eight bytes from there has the second
 * four from what comes next in memory instead. More than once: what is
 * done about a page the first time it is touched need not be what is done
 * after. */
static u32 __attribute__((noinline)) in_two_pages(void)
{
   const u32 at = page(), va = 0x19000000 + at, mem = 0x8c000000 + at;
   u32 k;

   for (k = 0; k < 4; k++)
   {
      /* (@va + 0xFFC is at @mem + 0x1FFC, and @va + 0x1000 at @mem) */
      *WORD(mem + 0x1FFC) = 0x11110000 + k;
      *WORD(mem + 0x0000) = 0x22220000 + k;
      *WORD(mem + 0x2000) = 0x33330000 + k;
      *WORD(mem + 0x800) = 0;
      *WORD(mem + 0x804) = 0;
      smc_move64(va + 0xFFC, mem + 0x800);
      if (*WORD(mem + 0x800) != 0x11110000 + k)
         return 1;
      if (*WORD(mem + 0x804) != 0x22220000 + k)
         return 2;

      *WORD(mem + 0x810) = 0x44440000 + k;
      *WORD(mem + 0x814) = 0x55550000 + k;
      smc_move64(mem + 0x810, va + 0xFFC);
      if (*WORD(mem + 0x1FFC) != 0x44440000 + k)
         return 3;
      if (*WORD(mem + 0x0000) != 0x55550000 + k)
         return 4;
      if (*WORD(mem + 0x2000) != 0x33330000 + k)
         return 5;

      /* and in one page, at an address that is a multiple of 4 and not of 8 */
      *WORD(mem + 0x1104) = 0x66660000 + k;
      *WORD(mem + 0x1108) = 0x77770000 + k;
      smc_move64(va + 0x104, va + 0x20C);
      if (*WORD(mem + 0x120C) != 0x66660000 + k || *WORD(mem + 0x1210) != 0x77770000 + k)
         return 6;
   }
   return 0;
}

/* A page that may be read and not written, from 0x16000000 on: the write
 * is refused, once (smc_fault() then gives the page with nothing in the
 * way), and is made. */
static u32 __attribute__((noinline)) not_to_be_written(void)
{
   const u32 at = page(), va = 0x16000000 + at, before = smc_writes_refused;

   *WORD(0x8c000000 + at) = 0x1111;
   if (*WORD(va) != 0x1111 || smc_writes_refused != before)
      return 1;
   *WORD(va) = 0x2222;
   if (smc_writes_refused != before + 1)
      return 2;
   if (*WORD(0x8c000000 + at) != 0x2222)
      return 3;
   *WORD(va + 4) = 0x3333;
   if (smc_writes_refused != before + 1 || *WORD(0x8c000000 + at + 4) != 0x3333)
      return 4;
   return 0;
}

/* The TLB is emptied, and then another entry than the page's is written
 * to by its number: the page was gone with the emptying and stays gone.
 * (An emulator that keeps more than the TLB's 64 and goes back to those
 * 64 when one is written to by hand has to know that they were emptied.) */
static u32 __attribute__((noinline)) emptied_then_written(void)
{
   const u32 at = page(), va = 0x13000000 + at, entry = (va >> 12) & 0x3F;

   *WORD(0x8c000000 + at) = 0xAAAA0001;
   *WORD(0x8c000000 + at + 0x1000) = 0xBBBB0001;
   smc_shift = 0;
   *MMUCR = 0x00000005;
   if (*WORD(va) != 0xAAAA0001)
      return 1;
   smc_shift = 0x1000;
   *MMUCR = 0x00000005;                             /* on, and the TLB emptied */
   *TLB_ADDRESS(entry ^ 1) = 0;
   *TLB_DATA(entry ^ 1) = 0;
   if (*WORD(va) != 0xBBBB0001)
      return 2;
   smc_shift = 0;
   *MMUCR = 0x00000005;
   return 0;
}

/* What follows is the MMU as it is, which an emulator made for Windows CE
 * does not do and one that does the whole of it has to. */

/* A page that has not been written to, from 0x15000000 on: reading it is
 * nothing, and the first write is an exception, once. */
static u32 __attribute__((noinline)) first_write(void)
{
   const u32 at = page(), va = 0x15000000 + at, before = smc_first_writes;

   *WORD(0x8c000000 + at) = 0x1111;
   if (*WORD(va) != 0x1111 || smc_first_writes != before)
      return 1;
   *WORD(va) = 0x2222;
   if (smc_first_writes != before + 1)
      return 2;
   if (*WORD(0x8c000000 + at) != 0x2222)
      return 3;
   *WORD(va + 4) = 0x3333;
   if (smc_first_writes != before + 1 || *WORD(0x8c000000 + at + 4) != 0x3333)
      return 4;
   return 0;
}

/* A page is mapped while its entry is one of the TLB's 64: once another
 * page has taken the entry, it has to be asked for again. */
static u32 __attribute__((noinline)) only_while_in_the_tlb(void)
{
   const u32 at = page(), va = 0x10000000 + at;
   u32 before;

   (void)*WORD(va);
   before = smc_misses;
   (void)*WORD(va);
   if (smc_misses != before)
      return 1;
   (void)*WORD(va + 0x40000);                       /* the same entry */
   if (smc_misses != before + 1)
      return 2;
   (void)*WORD(va);
   if (smc_misses != before + 2)
      return 3;
   return 0;
}

/* A page for privileged mode only, from 0x18000000 on, read by a function
 * in user mode: refused, once. The function is
 *    mov.l @r4,r0 ; trapa #0 ; rts ; nop
 * and comes back in privileged mode by way of the TRAPA. Then the page
 * again, taken out of the TLB first and read from privileged mode: that is
 * not refused. */
static u32 __attribute__((noinline)) user_mode(void)
{
   const u32 code = page(), data = page();
   const u32 refused = smc_reads_refused, traps = smc_traps;

   *WORD(0x8c000000 + code) = 0xC3006042;
   *WORD(0x8c000000 + code + 4) = 0x0009000B;
   *WORD(0x8c000000 + data) = 0x4444;
   if (smc_enter_user(0x10000000 + code, 0x18000000 + data) != 0x4444)
      return 1;
   if (smc_traps != traps + 1)
      return 2;
   if (smc_reads_refused != refused + 1)
      return 3;
   *TLB_FIND = 0x18000000 + data;
   if (*WORD(0x18000000 + data) != 0x4444 || smc_reads_refused != refused + 1)
      return 4;
   return 0;
}

/* Translation turned off and on again, a thousand times, as such programs
 * do all the time: while it is off an address is itself, and when it is
 * on again the TLB is what it was. */
static u32 __attribute__((noinline)) off_and_on(void)
{
   const u32 at = page();
   u32 i, misses;

   *WORD(0x8c000000 + at) = 0x5555;
   (void)*WORD(0x10000000 + at);
   misses = smc_misses;
   for (i = 0; i < 1000; i++)
   {
      *MMUCR = 0;
      if (*WORD(0x0c000000 + at) != 0x5555)
         return 1;
      *WORD(0x0c000000 + at + 4) = i;
      *MMUCR = 1;
      if (*WORD(0x10000000 + at + 4) != i)
         return 2;
   }
   if (smc_misses != misses)
      return 3;
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

   /* Pages taken out of the TLB and mapped again for other memory. */
   *VERDICT = 18;
   if (mmu)
   {
      for (i = 0; i < 8; i++)
         for (j = 0; j < 2; j++)
            if ((r = moved(0x13000000, i, j)) != 0)
               fail(18, (i << 5) | (j << 4) | r);
   }

   /* The same in P3: where the address without the MMU is main memory
    * (0xCD000000), where it is nothing (0xC9000000) - and at 0xC4000000,
    * where it is video memory, which has to be main memory now, as the
    * handler maps it. Then a function rewritten there. */
   *VERDICT = 19;
   if (mmu)
      for (i = 0; i < 8; i++)
         for (j = 0; j < 2; j++)
            if ((r = moved(0xCD000000, i, j)) != 0)
               fail(19, (i << 5) | (j << 4) | r);
   *VERDICT = 20;
   if (mmu)
   {
      const u32 at = page();

      for (i = 0; i < 8; i++)
         for (j = 0; j < 2; j++)
            if ((r = moved(0xC9000000, i, j)) != 0)
               fail(20, (i << 5) | (j << 4) | r);
      *WORD(0xa4000000 + at) = 0xCCCC0001;
      *WORD(0x8c000000 + at) = 0xAAAA0001;
      if (*WORD(0xC4000000 + at) != 0xAAAA0001)
         fail(20, 0x0D);
      *WORD(0xC4000000 + at + 4) = 0xAAAA0002;
      if (*WORD(0x8c000000 + at + 4) != 0xAAAA0002)
         fail(20, 0x0E);
      if ((r = rewritten(0x8c000000, 0x8c000000, 0xC9000000)) != 0)
         fail(20, 0x1C | r);
      if ((r = rewritten(0xCD000000, 0xCD000000, 0xCD000000)) != 0)
         fail(20, 0x3C | r);
   }

   /* Pages of 1K; 64 bits in two pages; a page that may not be written
    * to; the TLB emptied and then written to; more pages than there may
    * be room for. */
   *VERDICT = 21;
   if (mmu)
   {
      if ((r = small_pages()) != 0)
         fail(21, 0x10 | r);
      if ((r = in_two_pages()) != 0)
         fail(21, 0x50 | r);
      if ((r = not_to_be_written()) != 0)
         fail(21, 0x20 | r);
      if ((r = emptied_then_written()) != 0)
         fail(21, 0x30 | r);
      if ((r = many()) != 0)
         fail(21, 0x40 | r);
   }

   /* The MMU as it is, for a program with a TLB of its own. */
   *VERDICT = 22;
   if (mmu == 2)
   {
      if ((r = first_write()) != 0)
         fail(22, 0x10 | r);
      if ((r = only_while_in_the_tlb()) != 0)
         fail(22, 0x20 | r);
      if ((r = user_mode()) != 0)
         fail(22, 0x30 | r);
      if ((r = off_and_on()) != 0)
         fail(22, 0x40 | r);
   }

   *VERDICT = 23;
   if ((r = rewritten(0x8c000000, 0x8c000000, 0x2c000000)) != 0)
      fail(23, r);

   /* And for ever, for whoever saves a state and loads it later. */
   *VERDICT = mmu == 2 ? 0x600D5AC5 : mmu ? 0x600D5ACE : 0x600D5AC0;
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
