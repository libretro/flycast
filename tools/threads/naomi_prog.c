/* A NAOMI cartridge's program, for headless.sh: the built-in BIOS's system
 * menu routine (core/reios/reios.cpp, nb_sys_menu()), which is what the
 * cabinet's TEST switch leads to in a game.
 *
 * One program is the cartridge's game and its test program both, as in
 * some games: the header's two load lists name it, and the word the BIOS
 * keeps for the game (routine 2) says which it was started as. It asks for
 * the system menu (routine 17) twice - as a game does when TEST is pressed,
 * and as a test program does to leave - and looks at how it is started each
 * time:
 *
 *   from power-on        the game: word 0, r1 at the header copy's first
 *                        entry point
 *   after the first ask  the test program: word 1, r1 at the second entry
 *                        point - and no interrupt allowed through, though
 *                        the game left one allowed (the BIOS hands over
 *                        with the interrupt controller's masks at 0; a
 *                        program started with the last one's would get
 *                        interrupts it has no handlers for)
 *   after the second     the game again
 *
 * A word of main memory, which such a restart leaves alone, says how far
 * it has come. The verdict is at 0x8c300010: 0x600d7e57, or 0xbad000nn
 * with the check that failed.
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

#define STEP        ((volatile u32 *)0x8c300000)
#define VERDICT     ((volatile u32 *)0x8c300010)
#define PROGRAM     ((volatile u32 *)0x8c01ff00)   /* 1 the game, 2 its test program */
#define SB_IML6NRM  ((volatile u32 *)0xa05f6930)
#define ROUTINE(n)  (*(u32 (* volatile *)(void))(0x8c018000 + (n) * 4))

__asm__ (
   ".section .text.start\n"
   ".global _start\n"
   "_start:\n"
   "  mov.l 1f, r0\n"
   "  jmp @r0\n"
   "  mov r1, r4\n"            /* where the BIOS says the entry point taken is */
   "  .align 2\n"
   "1: .long naomi_main\n"
   ".text\n");

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

void naomi_main(u32 entry_at)
{
   const u32 step = *STEP;
   const u32 word = ROUTINE(2)();
   const int test = step == 1;

   if (step > 2)
      fail(0x10);
   if (word != (test ? 1u : 0u))
      fail(0x11 + step * 0x10);
   if ((entry_at & 0x1fffffff) != (test ? 0x0c01f824u : 0x0c01f820u))
      fail(0x12 + step * 0x10);
   if (*PROGRAM != (test ? 2u : 1u))
      fail(0x13 + step * 0x10);
   if (step != 0 && *SB_IML6NRM != 0)
      fail(0x14 + step * 0x10);
   if (step == 2)
   {
      *VERDICT = 0x600d7e57;
      for (;;)
         ;
   }
   /* an interrupt allowed (the vertical blank's), none taken */
   interrupts_off();
   *SB_IML6NRM = 8;
   *STEP = step + 1;
   ROUTINE(17)();
   fail(0x1f + step * 0x10);       /* it does not return */
}
