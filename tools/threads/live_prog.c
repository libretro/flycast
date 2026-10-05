/* Bare-metal Dreamcast program for tools/threads/live.sh.
 *
 * It keeps the PowerVR busy so the core's emulation thread and render
 * thread hand frames to each other: three frames out of four start a
 * render through the Tile Accelerator, the fourth writes to the
 * framebuffer directly and lets the core render that. Every frame it
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

static void wait_vblank(void)
{
   u32 guard;
   for (guard = 0; guard < 2000000 && (PVR(0x10C) & 0x2000); guard++) ;
   for (guard = 0; guard < 2000000 && !(PVR(0x10C) & 0x2000); guard++) ;
}

void cmain(void)
{
   u32 frame = 0, i;

   PVR(0x44) = (PVR(0x44) & 0x00800000) | 0x5;         /* FB_R_CTRL: enable, RGB565 */
   PVR(0x5C) = (1u << 20) | (479u << 10) | 319u;       /* FB_R_SIZE: 640x480 */
   PVR(0x50) = 0;                                      /* FB_R_SOF1 */
   PVR(0x60) = 0x200000;                               /* FB_W_SOF1 */
   PVR(0xCC) = 0x00150104;                             /* SPG_VBLANK_INT: in 260, out 21 */
   PVR(0x20) = 0x100000;                               /* PARAM_BASE */
   PVR(0x128) = 0x100000;                              /* TA_ISP_BASE */
   /* A textured background plane, so every render has something to draw
    * and the core walks the region array for its clipping. */
   PVR(0x8C) = 0;                                      /* ISP_BACKGND_T */
   (*(volatile u32 *)0xA5100000) = 0x02000000;         /* ISP word: textured */
#ifdef NO_REGION_ARRAY
   /* No region array: REGION_BASE points at empty video memory, as it does
    * for a program that starts a render before it has written one. Nothing
    * this build writes to video memory has its top bit set, so there is no
    * "last region" mark anywhere for the core to stop at. */
   PVR(0x2C) = 0x300000;                               /* REGION_BASE */
#else
   /* a one-entry region array: last region, all five lists empty */
   for (i = 0; i < 6; i++)
      (*(volatile u32 *)(0xA5180000 + i * 4)) = 0x80000000;
   PVR(0x2C) = 0x180000;                               /* REGION_BASE */
#endif

   for (;;)
   {
      if ((frame & 3) == 3)
      {
         /* direct framebuffer write: the core renders the framebuffer */
         u16 c = (u16)(frame * 2113) & 0x7FFF;         /* top bit clear: see NO_REGION_ARRAY */
         for (i = 0; i < 640 * 64; i++)
            VRAM16[i + (frame & 63) * 640] = c;
      }
      else
      {
         /* a (empty) Tile Accelerator frame and a render start */
         PVR(0x144) = 0x80000000;                      /* TA_LIST_INIT */
         PVR(0x14) = 0xFFFFFFFF;                       /* STARTRENDER */
      }
      poll_controller();
      wait_vblank();
      frame++;
   }
}
