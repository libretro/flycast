/* A NAOMI cartridge's program, for headless.sh: the built-in BIOS
 * (core/reios/reios.cpp) as a game meets it - how it is started, and the
 * system routines that start a cartridge's programs, the last of which is
 * what the cabinet's TEST switch leads to in a game (nb_sys_menu()).
 *
 * One program is the cartridge's game and its test program both, as in
 * some games: the header's two load lists name it, and the word the BIOS
 * keeps for the game (routine 2) says which it was started as. It asks to
 * be started again five times and looks at how it is started each time:
 *
 *   from power-on        the game: word 0, r1 at the header copy's first
 *                        entry point
 *   after routine 17     the system menu, as a game asks for it when TEST
 *                        is pressed: the test program - word 1, r1 at the
 *                        second entry point - and no interrupt allowed
 *                        through, though the game left one allowed (the
 *                        BIOS hands over with the interrupt controller's
 *                        masks at 0; a program started with the last one's
 *                        would get interrupts it has no handlers for)
 *   after routine 17     as a test program asks for it to leave: the game
 *   after routine 16     "start the game": the game
 *   after routine 18     "start the test program": the test program
 *   after routine 16     the game again
 *
 * Each time r7 has to be what the BIOS's loader leaves there - the
 * header's eight places for a piece to load, less those used: 7 - and r9
 * the ROM board's addressing the header asks for at 0x138: 0x2000.
 *
 * And from power-on, three of the other routines:
 *
 *   0    hands back a floating point 1.0 - in fr0, where one is handed
 *        back, not in r0
 *   11   the cabinet's settings: the ones the cartridge's header asks for
 *        (naomi_cart.py puts them there: a coin chute to each player, coin
 *        setting 12 - two coins to a credit - two credits to start and
 *        three for the third of the eight things credits are for)
 *   8    what each player can do, for settings that say there are six
 *        players: the fifth and sixth are not written. (The BIOS's own
 *        routine goes on for as many as it is told. The number is the
 *        game's, and there are four players.)
 *
 * A word of main memory, which such a restart leaves alone, says how far
 * it has come. The verdict is at 0x8c300010: 0x600d7e57, or 0xbad000nn
 * with the check that failed. The first digit is the start, 1 for the one
 * from power-on to 6 for the last; the second is 1 for the BIOS's word, 2
 * the entry point, 3 the BIOS's note of which program runs, 4 the
 * interrupt masks, 5 r7, 6 r9, and f a routine that came back. 18, 19 and
 * 1a are routines 0, 11 and 8.
 *
 * naomi_cart.py carries the compiled program. To rebuild it:
 *
 *   sh4-linux-gnu-gcc -ml -m4-nofpu -O1 -nostdlib -ffreestanding -fno-pic \
 *      -fno-pie -static -Wl,-T,naomi_prog.ld -Wl,--build-id=none \
 *      -o naomi_prog.elf naomi_prog.c
 *   sh4-linux-gnu-objcopy -O binary naomi_prog.elf naomi_prog.bin
 *
 * and paste the bytes of naomi_prog.bin into PROGRAM. */
typedef unsigned int u32;
typedef unsigned char u8;
typedef u32 (*routine)(u32, u32, u32, u32);

#define STEP        ((volatile u32 *)0x8c300000)
#define VERDICT     ((volatile u32 *)0x8c300010)
#define PROGRAM     ((volatile u32 *)0x8c01ff00)   /* 1 the game, 2 its test program */
#define SB_IML6NRM  ((volatile u32 *)0xa05f6930)
#define ROUTINE(n)  (*(routine volatile *)(0x8c018000 + (n) * 4))

u32 routine0_bits(void);

__asm__ (
   ".section .text.start\n"
   ".global _start\n"
   "_start:\n"
   "  mov.l 1f, r0\n"
   "  mov r7, r5\n"           /* what the BIOS's loader left */
   "  mov r9, r6\n"
   "  jmp @r0\n"
   "  mov r1, r4\n"           /* where the BIOS says the entry point taken is */
   "  .align 2\n"
   "1: .long naomi_main\n"
   ".text\n"
   /* fr0 as routine 0 leaves it, having been 0. (In numbers: the program is
    * built for no FPU.) */
   "  .align 1\n"
   "routine0_bits:\n"
   "  sts.l pr, @-r15\n"
   "  mov #0, r0\n"
   "  .word 0x405a\n"         /* lds r0, fpul */
   "  .word 0xf00d\n"         /* fsts fpul, fr0 */
   "  mov.l 2f, r0\n"
   "  mov.l @r0, r0\n"
   "  jsr @r0\n"
   "  nop\n"
   "  .word 0xf01d\n"         /* flds fr0, fpul */
   "  .word 0x005a\n"         /* sts fpul, r0 */
   "  lds.l @r15+, pr\n"
   "  rts\n"
   "  nop\n"
   "  .align 2\n"
   "2: .long 0x8c018000\n");

static void fail(u32 code)
{
   *VERDICT = 0xbad00000 | code;
   for (;;)
      ;
}

static void interrupts_off(void)
{
   u32 sr;
   __asm__ volatile ("stc sr, %0" : "=r" (sr));
   sr |= 0xf0;
   __asm__ volatile ("ldc %0, sr" : : "r" (sr));
}

/* Routines 0, 11 and 8: see above. */
static void routines(void)
{
   volatile u32 set[17], coin[64];
   volatile u8 can[24];
   u32 i;

   if (routine0_bits() != 0x3f800000)
      fail(0x18);

   for (i = 0; i < 17; i++)
      set[i] = 0xffffffff;
   ROUTINE(11)((u32)set, 0, 0, 0);
   if (set[0] != 9 || set[1] != 1 || set[2] != 1 || set[4] != 11
         || set[5] != 2 || set[6] != 1 || set[7] != 3
         || set[13] != 1 || set[14] != 1 || set[15] != 0 || set[16] != 2)
      fail(0x19);

   for (i = 0; i < 64; i++)
      coin[i] = 0;
   for (i = 0; i < 24; i++)
      can[i] = 0x55;
   set[1] = 5;
   ROUTINE(8)((u32)set, (u32)coin, (u32)can, 0);
   if (can[0] != 0 || can[12] != 0 || can[16] != 0x55 || can[20] != 0x55)
      fail(0x1a);
}

void naomi_main(u32 entry_at, u32 left, u32 mode)
{
   const u32 step = *STEP;
   const u32 word = ROUTINE(2)(0, 0, 0, 0);
   const int test = step == 1 || step == 4;

   if (step > 5)
      fail(0x10);
   if (word != (test ? 1u : 0u))
      fail(0x11 + step * 0x10);
   if ((entry_at & 0x1fffffff) != (test ? 0x0c01f824u : 0x0c01f820u))
      fail(0x12 + step * 0x10);
   if (*PROGRAM != (test ? 2u : 1u))
      fail(0x13 + step * 0x10);
   if (step != 0 && *SB_IML6NRM != 0)
      fail(0x14 + step * 0x10);
   if (left != 7)
      fail(0x15 + step * 0x10);
   if (mode != 0x2000)
      fail(0x16 + step * 0x10);
   if (step == 0)
      routines();
   if (step == 5)
   {
      *VERDICT = 0x600d7e57;
      for (;;)
         ;
   }
   /* an interrupt allowed (the vertical blank's), none taken */
   interrupts_off();
   *SB_IML6NRM = 8;
   *STEP = step + 1;
   ROUTINE(step < 2 ? 17 : step == 3 ? 18 : 16)(0, 0, 0, 0);
   fail(0x1f + step * 0x10);       /* it does not return */
}
