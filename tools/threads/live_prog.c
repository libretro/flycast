/* Bare-metal Dreamcast program for tools/threads/live.sh.
 *
 * It keeps the PowerVR busy so the core's emulation thread and render
 * thread hand frames to each other: three frames out of four start a
 * render through the Tile Accelerator, the fourth writes to the
 * framebuffer directly and lets the core render that, for the first 64
 * frames. The renders draw a background with a paletted texture. The
 * program repaints that texture four times in its first second and then
 * changes the palette entry the last repaint uses to yellow, which is
 * what live.sh looks for in a screenshot. It also times its frames
 * against the CPU's timer and, if they are not all the same length, paints
 * the screen red instead; and magenta if a read-only system bus register
 * that nothing has written yet reads as anything but zero. Every frame it
 * polls the controller in port A, which is how the emulation thread comes
 * to read input.
 *
 * Built with -DNO_REGION_ARRAY it never writes a region array, and the
 * core has to render that without getting stuck.
 *
 * live_disc.py carries the compiled program. To rebuild it:
 *
 *   sh4-linux-gnu-gcc -ml -m4-nofpu -O1 -nostdlib -ffreestanding -fno-pic \
 *      -fno-pie -static -Wl,-T,live_prog.ld -Wl,--build-id=none \
 *      -o live_prog.elf live_prog.c
 *   sh4-linux-gnu-objcopy -O binary live_prog.elf live_prog.bin
 *
 * and paste the bytes of live_prog.bin into PROGRAM; the same again with
 * -DNO_REGION_ARRAY for PROGRAM_NO_REGION_ARRAY. */
typedef unsigned int u32;
typedef unsigned short u16;
#define PVR(off) (*(volatile u32 *)(0xA05F8000 + (off)))
#define VRAM16   ((volatile u16 *)0xA5000000)

__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        " mov.l 1f,r15\n mov.l 2f,r0\n jmp @r0\n nop\n .align 2\n"
        "1: .long 0x8c00f000\n2: .long cmain\n");

#define SB(off)  (*(volatile u32 *)(0xA05F6000 + (off)))

/* Ask the controller in port A for its state, as a game does every frame:
 * one Maple "get condition" frame, sent by DMA. */
static void poll_controller(void)
{
   volatile u32 *frame = (volatile u32 *)0xAC00E000;

   frame[0] = 0x80000001;                 /* last frame, port A, 2 words */
   frame[1] = 0x0C00E100;                 /* where the reply goes */
   frame[2] = 0x09 | (0x20 << 8) | (1 << 24);   /* get condition, to A0 */
   frame[3] = 0x01000000;                 /* function: controller */
   SB(0xCE8) = 1;                         /* SB_MMSEL */
   SB(0xC10) = 0;                         /* SB_MDTSEL: start by software */
   SB(0xC14) = 1;                         /* SB_MDEN */
   SB(0xC04) = 0x0C00E000;                /* SB_MDSTAR */
   SB(0xC18) = 1;                         /* SB_MDST: go */
}

/* The background plane's texture is 8x8 with 8-bit palette indices, at the
 * start of video memory. Fill it with one index. The top bit of every word
 * stays clear: see NO_REGION_ARRAY. */
static void paint_texture(u32 index)
{
   volatile u32 *texel = (volatile u32 *)0xA4000000;
   u32 four = index | (index << 8) | (index << 16) | (index << 24);
   u32 i;

   for (i = 0; i < 16; i++)
      texel[i] = four;
}

/* Palette entry @index, in the 1555 the palette is left in at reset. */
static void set_palette(u32 index, u16 colour)
{
   PVR(0x1000 + index * 4) = colour;
}

/* Wait for the beam to start a new frame: the scanline counter in
 * SPG_STATUS going back down. That holds whatever video mode the BIOS
 * left behind, which the vsync bit does not. */
static void wait_vblank(void)
{
   u32 guard, now, last = PVR(0x10C) & 0x3FF;

   for (guard = 0; guard < 2000000; guard++)
   {
      now = PVR(0x10C) & 0x3FF;
      if (now < last)
         break;
      last = now;
   }
}

/* TMU channel 0, counting down at a 16th of a microsecond (12.5 MHz). */
#define TMU_TSTR  (*(volatile unsigned char *)0xFFD80004)
#define TMU_TCOR0 (*(volatile u32 *)0xFFD80008)
#define TMU_TCNT0 (*(volatile u32 *)0xFFD8000C)
#define TMU_TCR0  (*(volatile u16 *)0xFFD80010)

void cmain(void)
{
   u32 frame = 0, i;
   u32 tick = 0, shortest = 0xFFFFFFFF, longest = 0;
   /* Read-only system bus registers nothing has written yet: the Maple
    * status and its address counters, and the AICA DMA counters. They
    * read as zero, not as whatever the emulator had lying there. */
   u32 stale = SB(0xC84) | SB(0xCF4) | SB(0xCF8) | SB(0xCFC)
             | (*(volatile u32 *)0xA05F78C0) | (*(volatile u32 *)0xA05F78C4);

   TMU_TSTR &= ~1;
   TMU_TCOR0 = 0xFFFFFFFF;
   TMU_TCNT0 = 0xFFFFFFFF;
   TMU_TCR0 = 0;
   TMU_TSTR |= 1;

   PVR(0x44) = (PVR(0x44) & 0x00800000) | 0x5;         /* FB_R_CTRL: enable, RGB565 */
   PVR(0x5C) = (1u << 20) | (479u << 10) | 319u;       /* FB_R_SIZE: 640x480 */
   PVR(0x50) = 0;                                      /* FB_R_SOF1 */
   PVR(0x60) = 0x200000;                               /* FB_W_SOF1 */
   PVR(0xCC) = 0x00150104;                             /* SPG_VBLANK_INT: in 260, out 21 */
   PVR(0x68) = 639 << 16;                              /* FB_X_CLIP: 0..639 */
   PVR(0x6C) = 479 << 16;                              /* FB_Y_CLIP: 0..479 */
   PVR(0x7C) = 0x0027DF77;                             /* FPU_PARAM_CFG: 6-word region entries */
   PVR(0x20) = 0x100000;                               /* PARAM_BASE */
   PVR(0x128) = 0x100000;                              /* TA_ISP_BASE */
   /* A textured background plane, so every render has something to draw
    * and the core walks the region array for its clipping. */
   PVR(0x8C) = 0;                                      /* ISP_BACKGND_T */
   PVR(0x88) = 0x38D1B717;                             /* ISP_BACKGND_D: 0.0001, far away */
   (*(volatile u32 *)0xA5100000) = 0x02000000;         /* ISP word: textured */
   /* TSP word: source x 1 + destination x 0. Opaque polygons are blended
    * by these instructions like any others, and with the word left at
    * zero that is everything times zero: a black screen, whatever the
    * texture holds. */
   (*(volatile u32 *)0xA5100004) = 1u << 29;
   (*(volatile u32 *)0xA5100008) = 6u << 27;           /* TCW: 8-bit palette, at address 0 */
   set_palette(1, 0x7C00);                             /* red */
   set_palette(2, 0x03E0);                             /* green */
   set_palette(3, 0x001F);                             /* blue */
   set_palette(4, 0x7FFF);                             /* white, until frame 70 */
#ifdef NO_REGION_ARRAY
   /* No region array: REGION_BASE points at empty video memory, as it does
    * for a program that starts a render before it has written one. Nothing
    * this build writes to video memory has its top bit set, so there is no
    * "last region" mark anywhere for the core to stop at. */
   PVR(0x2C) = 0x300000;                               /* REGION_BASE */
#else
   /* A region array of two entries, the top left and the bottom right
    * tile, which is what the core takes the screen's extent from. The
    * first one's opaque list is marked in use so that it counts. */
   for (i = 0; i < 12; i++)
      (*(volatile u32 *)(0xA5180000 + i * 4)) = 0x80000000;
   (*(volatile u32 *)0xA5180000) = 0;                  /* tile 0,0 */
   (*(volatile u32 *)0xA5180004) = 0;                  /* its opaque list */
   (*(volatile u32 *)0xA5180018) = 0x80000000 | (14 << 8) | (19 << 2);   /* tile 19,14, last */
   PVR(0x2C) = 0x180000;                               /* REGION_BASE */
#endif

   for (;;)
   {
      /* Repaint the texture four times, once each and never again: every
       * one of these has to reach the screen through the core's texture
       * cache. The last one stays, and then its palette entry is changed,
       * once: what the screen shows from then on takes both the texture
       * write and the palette write having arrived. */
      if (frame == 15)
         paint_texture(1);
      else if (frame == 30)
         paint_texture(2);
      else if (frame == 45)
         paint_texture(3);
      else if (frame == 60)
         paint_texture(4);
      else if (frame == 70)
         set_palette(4, 0x7FE0);                       /* yellow, for good */

      if ((frame & 3) == 3 && frame < 64)
      {
         /* direct framebuffer write: the core renders the framebuffer */
         u16 c = (u16)(frame * 2113) & 0x7FFF;         /* top bit clear: see NO_REGION_ARRAY */
         for (i = 0; i < 640 * 64; i++)
            VRAM16[i + (frame & 63) * 640] = c;
      }
      else
      {
         /* a (empty) Tile Accelerator frame and a render start; once the
          * framebuffer frames are over, into one of two buffers in turn */
         if (frame >= 64)
            PVR(0x60) = (frame & 1) ? 0x400000 : 0x200000;   /* FB_W_SOF1 */
         PVR(0x144) = 0x80000000;                      /* TA_LIST_INIT */
         PVR(0x14) = 0xFFFFFFFF;                       /* STARTRENDER */
      }
      poll_controller();
      wait_vblank();
#ifdef HALF_RATE
      /* A 30 fps game: a render every other vblank, nothing in between. */
      wait_vblank();
#endif
      /* Time every frame against the CPU's own timer. They all have to be
       * the same length; if one came out a quarter of a scanline off, say
       * so in red. */
      {
         u32 now = TMU_TCNT0;

         if (frame >= 100 && frame < 250)
         {
            u32 took = tick - now;
            if (took < shortest)
               shortest = took;
            if (took > longest)
               longest = took;
         }
         tick = now;
         if (frame == 250 && longest - shortest > 200)
            set_palette(4, 0x7C00);                    /* red: uneven frames */
         else if (frame == 250 && stale)
            set_palette(4, 0x7C1F);                    /* magenta: a register with junk in it */
      }

      /* ...and shown at the next vblank, the way a game flips buffers */
      if (frame >= 64)
         PVR(0x50) = (frame & 1) ? 0x400000 : 0x200000;      /* FB_R_SOF1 */
      frame++;
   }
}
