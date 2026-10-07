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

/* What the program has found, for a test that has no picture to look at
 * (headless.c reads it out of the machine's memory): 0 until frame 250,
 * then 0x600D600D if every check the background colour reports came out
 * right, or 0xBAD0000n for the first that did not, in the order they are
 * tested below - 0xBAD001nn for the instruction tests, nn being what
 * cpu_test() returned. 0x600D4D4D where all of it was done with the MMU
 * on (mmu_test()). */
volatile u32 live_verdict;

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

/* The scanline the beam is on. Not to be folded into its caller: see
 * wait_line(). */
static u32 __attribute__((noinline)) scanline(void)
{
   return PVR(0x10C) & 0x3FF;
}

/* Wait for the beam to get down to @line, the way many games wait: round
 * and round a loop that calls a function, with no count kept and nothing
 * written, until what the function reads has changed. Under the accurate
 * SH4 timing the x86-64 recompiler tells such a loop for what it is by
 * watching it and gives up the rest of each time slice (WaitSite, in
 * rec_x64.cpp); this is here so that the live test runs one. wait_vblank()
 * above keeps a count, and is rightly not taken for one. */
static void __attribute__((noinline)) wait_line(u32 line)
{
   while (scanline() < line)
      ;
}

/* The beam. The scanline in SPG_STATUS counts every line of the frame, not
 * only the ones something happens on; and the interrupt asked for on a line
 * (SPG_HBLANK_INT, mode 0) is there when the beam is on that line, not when
 * it gets to the next vertical blank. Not zero if either is not so. */
static u32 spg_test(void)
{
   u32 guard, seen = 0, last, now, at, old;

   wait_vblank();
   last = scanline();
   for (guard = 0; guard < 2000000; guard++)
   {
      now = scanline();
      if (now < last)
         break;
      if (now != last)
         seen++;
      last = now;
   }
   if (seen < 100)
      return 1;

   old = PVR(0xC8);
   wait_vblank();
   PVR(0xC8) = 100;                                    /* SPG_HBLANK_INT: on line 100 */
   SB(0x900) = 1 << 5;                                 /* SB_ISTNRM: not yet */
   for (guard = 0; guard < 2000000 && !(SB(0x900) & (1 << 5)); guard++)
      ;
   at = scanline();
   PVR(0xC8) = old;
   SB(0x900) = 1 << 5;
   return at < 100 || at > 102 ? 2 : 0;
}

/* A Maple transfer takes time. The controller's answer is not in memory
 * when the transfer has only just been started, and is there when the
 * transfer is over. Not zero if not so. (With nothing in port A the bus
 * says that nobody answered, and there is nothing to tell by.) */
static u32 maple_test(void)
{
   volatile u32 *reply = (volatile u32 *)0xAC00E100;
   u32 guard, early;

   reply[0] = 0x12345678;
   poll_controller();
   early = reply[0];
   for (guard = 0; guard < 2000000 && (SB(0xC18) & 1); guard++)
      ;
   if (SB(0xC18) & 1)
      return 2;                                        /* never over */
   if (reply[0] == 0x12345678)
      return 3;                                        /* over, and no answer */
   if (reply[0] != 0xFFFFFFFF && early != 0x12345678)
      return 1;                                        /* the answer was there at once */
   if (reply[0] == 0xFFFFFFFF)
      return 0;

   /* Asked for all of its status, a controller answers with its 28 words
    * and 20 more of its maker's notes, which begin "Version". */
   {
      volatile u32 *frame = (volatile u32 *)0xAC00E000;

      reply[0] = 0x12345678;
      frame[0] = 0x80000000;                           /* last frame, port A, 1 word */
      frame[1] = 0x0C00E100;
      frame[2] = 0x02 | (0x20 << 8);                   /* all status, to A0 */
      SB(0xC04) = 0x0C00E000;
      SB(0xC18) = 1;
      for (guard = 0; guard < 2000000 && (SB(0xC18) & 1); guard++)
         ;
      if ((reply[0] & 0xFF) != 6 || (reply[0] >> 24) != 48)
         return 4;
      if (reply[29] != ('V' | ('e' << 8) | ('r' << 16) | ('s' << 24)))
         return 5;
   }
   return 0;
}

/* The SH4's own DMA controller, asked by the program for a transfer from
 * memory to memory (channel 1, which nothing on a Dreamcast uses). Sixteen
 * single bytes with both addresses going up; then four longwords from an
 * address that stays where it is. Afterwards the addresses are where they
 * got to, the count is 0 and the channel says it is done. Not zero if not. */
static u32 dmac_test(void)
{
   static unsigned char from[32] __attribute__((aligned(4))), to[32] __attribute__((aligned(4)));
   static u32 word[2], fill[6];
   volatile u32 *sar = (volatile u32 *)0xFFA00010, *dar = (volatile u32 *)0xFFA00014;
   volatile u32 *count = (volatile u32 *)0xFFA00018, *chcr = (volatile u32 *)0xFFA0001C;
   volatile u32 *dmaor = (volatile u32 *)0xFFA00040;
   u32 i, bad = 0, old = *dmaor;

   for (i = 0; i < 32; i++)
   {
      from[i] = 0x40 + i;
      to[i] = 0;
   }
   *dmaor = 0x8201;
   *chcr = 0;
   *sar = (u32)from;
   *dar = (u32)to;
   *count = 16;
   *chcr = (1 << 14) | (1 << 12) | (4 << 8) | (1 << 4) | 1;   /* both up, by the program, bytes, go */
   for (i = 0; i < 16; i++)
      if (to[i] != from[i])
         bad = 1;
   if (to[16] != 0)
      bad = 2;
   if (*sar != (u32)from + 16 || *dar != (u32)to + 16 || *count != 0 || !(*chcr & 2))
      bad = bad ? bad : 3;

   word[0] = 0xCAFE0001;
   word[1] = 0xCAFE0002;
   for (i = 0; i < 6; i++)
      fill[i] = 0;
   *chcr = 0;
   *sar = (u32)word;
   *dar = (u32)fill;
   *count = 4;
   *chcr = (1 << 14) | (4 << 8) | (3 << 4) | 1;               /* source stays, longwords */
   for (i = 0; i < 4; i++)
      if (fill[i] != 0xCAFE0001)
         bad = bad ? bad : 4;
   if (fill[4] != 0 || *sar != (u32)word || *dar != (u32)fill + 16)
      bad = bad ? bad : 5;
   *chcr = 0;
   *dmaor = old;
   return bad;
}

/* The top eighth of the address space. Only part of it is the SH4's own:
 * through the rest an address reaches the same memory as it does from
 * anywhere else. A word of video memory and a word of main memory, written
 * through there and read back the usual way. Not zero if they did not get
 * there. */
static u32 p4_test(void)
{
   static volatile u32 word;
   volatile u32 *vram_usual = (volatile u32 *)0xA5140000, *vram_top = (volatile u32 *)0xE5140000;
   volatile u32 *ram_top = (volatile u32 *)((u32)&word | 0xE0000000);
   u32 bad = 0;

   *vram_usual = 0;
   *vram_top = 0x0BADF00D;
   if (*vram_usual != 0x0BADF00D || *vram_top != 0x0BADF00D)
      bad = 1;
   *vram_usual = 0;
   word = 0;
   *ram_top = 0x12344321;
   if (word != 0x12344321 || *ram_top != 0x12344321)
      bad = bad ? bad : 2;
   return bad;
}

/* The two store queues each have a register saying which area their 32
 * bytes go to. Nearly every game sets the two alike, and so does this
 * program for its rendering; here, once, they differ: the second queue is
 * aimed at main memory and then the first at the Tile Accelerator, and 32
 * bytes are sent through the second. They have to arrive in memory. An
 * emulator that lets the register written last decide for both queues sends
 * them to the TA instead. (Which queue is used is bit 5 of the address, so
 * the second queue's bytes always land on an odd 32-byte block.) Not zero
 * if they did not arrive. */
static u32 sq_test(void)
{
   static u32 block[24] __attribute__((aligned(64)));
   u32 *target = block + 8;                            /* the odd block */
   volatile u32 *sq = (volatile u32 *)(0xE0000000 | ((u32)target & 0x03FFFFE0));
   u32 i, bad = 0;

   for (i = 0; i < 24; i++)
      block[i] = 0x11111111;
   (*(volatile u32 *)0xFF00003C) = 0x0C;               /* QACR1: main memory */
   (*(volatile u32 *)0xFF000038) = 0x10;               /* QACR0: the TA */
   /* The top three bits of the first word are 0, so that where this does
    * go to the TA it is taken for the end of a list and nothing else. */
   for (i = 0; i < 8; i++)
      sq[i] = 0x00C0FFE0 + i;
   __asm__ volatile ("pref @%0" : : "r" (sq));
   for (i = 0; i < 8; i++)
      if (target[i] != 0x00C0FFE0 + i)
         bad = 1;
   /* and nothing either side of it */
   if (block[7] != 0x11111111 || block[16] != 0x11111111)
      bad = 1;
   return bad;
}

/* A float's bits, worked out by the compiler: the program has no FPU code. */
#define F(x) (((union { float f; u32 u; }){ x }).u)

/* 32 bytes for the Tile Accelerator, sent the way games send them: through
 * a store queue aimed at the TA's FIFO. */
static void ta_send(u32 w0, u32 w1, u32 w2, u32 w3, u32 w4, u32 w5, u32 w6, u32 w7)
{
   volatile u32 *sq = (volatile u32 *)0xE0000000;

   sq[0] = w0; sq[1] = w1; sq[2] = w2; sq[3] = w3;
   sq[4] = w4; sq[5] = w5; sq[6] = w6; sq[7] = w7;
   __asm__ volatile ("pref @%0" : : "r" (sq));
}

#define TA_POLYGON   0x80000000u     /* global parameter: polygon or modifier volume */
#define TA_VERTEX    0xE0000000u
#define TA_LAST      0x10000000u     /* vertex: end of strip */
#define TA_SHADOW    0x00000080u     /* polygon: modifier volumes affect it */
#define TA_TEXTURED  0x00000008u
#define TA_MODIFIER  0x01000000u     /* list: opaque modifier volumes */
#define TA_TRANSLUCENT 0x02000000u   /* list: translucent polygons */
#define TA_PUNCH     0x04000000u     /* list: punch-through polygons */
#define ISP_GEQUAL   (6u << 29)
#define TSP_PLAIN    ((1u << 29) | (2u << 22))   /* source x 1, destination x 0; no fog; decal */
#define TSP_BY_ALPHA ((4u << 29) | (2u << 22) | (1u << 20))   /* source x its alpha, destination x 0 */
#define TSP_FOGGED   (1u << 29)                  /* as plain, with fog from the table */
#define TSP_CUTOUT   ((1u << 29) | (2u << 22) | (1u << 20))   /* as plain, with the texture's alpha */
#define TSP_BLEND    ((4u << 29) | (5u << 26) | (2u << 22) | (1u << 20))   /* source x alpha + destination x (1 - alpha) */

/* A screen-aligned rectangle at depth 1/w = z, as a strip of four. */
static void ta_quad(u32 pcw, u32 tsp, u32 tcw, u32 white, u32 z, u32 x0, u32 y0, u32 x1, u32 y1)
{
   ta_send(TA_POLYGON | pcw, ISP_GEQUAL, tsp, tcw, 0, 0, 0, 0);
   /* x, y, z, u, v, base colour, offset colour. Without a texture the
    * colour is in the same place and the words before it are ignored. */
   ta_send(TA_VERTEX | pcw, x0, y0, z, 0, 0, white, 0);
   ta_send(TA_VERTEX | pcw, x0, y1, z, 0, F(1.0f), white, 0);
   ta_send(TA_VERTEX | pcw, x1, y0, z, F(1.0f), 0, white, 0);
   ta_send(TA_VERTEX | pcw | TA_LAST, x1, y1, z, F(1.0f), F(1.0f), white, 0);
}

/* A screen-aligned rectangle with two textures: the first is what it is
 * drawn with, the second what it is drawn with inside a modifier volume
 * ("two volumes"). Its parameter has a second pair of texture words, and
 * each vertex, 64 bytes, a second set of texture coordinates and colours. */
static void ta_quad_two(u32 tsp0, u32 tcw0, u32 tsp1, u32 tcw1, u32 z, u32 x0, u32 y0, u32 x1, u32 y1)
{
   const u32 pcw = TA_SHADOW | 0x40 | TA_TEXTURED;
   u32 i;

   ta_send(TA_POLYGON | pcw, ISP_GEQUAL, tsp0, tcw0, tsp1, tcw1, 0, 0);
   for (i = 0; i < 4; i++)
   {
      const u32 x = (i & 2) ? x1 : x0, y = (i & 1) ? y1 : y0, last = i == 3 ? TA_LAST : 0;
      const u32 u = (i & 2) ? F(1.0f) : 0, v = (i & 1) ? F(1.0f) : 0;

      ta_send(TA_VERTEX | pcw | last, x, y, z, u, v, 0xFFFFFFFF, 0);
      ta_send(u, v, 0xFFFFFFFF, 0, 0, 0, 0, 0);
   }
}

/* One triangle of a modifier volume: 64 bytes. */
static void ta_volume_triangle(u32 x0, u32 y0, u32 x1, u32 y1, u32 x2, u32 y2, u32 z)
{
   ta_send(TA_VERTEX, x0, y0, z, x1, y1, z, x2);
   ta_send(y2, z, 0, 0, 0, 0, 0, 0);
}

/* Where in video memory the render to a texture goes: see render_to_texture(). */
#define RTT_ADDRESS 0x700000

/* A screen-aligned rectangle at depth 0.5 whose colour is given the other
 * two ways the Tile Accelerator takes one: as four floats with every
 * vertex (@intensity 0), or as a face colour in the polygon's header and an
 * intensity with every vertex that it is multiplied by (@intensity 1). */
static void ta_float_quad(int intensity, u32 colour, u32 scale, u32 x0, u32 y0, u32 x1, u32 y1)
{
   const u32 pcw = intensity ? 0x20 : 0x10, z = F(0.5f), one = F(1.0f);
   u32 i;

   if (intensity)
      ta_send(TA_POLYGON | pcw, ISP_GEQUAL, TSP_PLAIN, 0, one, colour, colour, colour);
   else
      ta_send(TA_POLYGON | pcw, ISP_GEQUAL, TSP_PLAIN, 0, 0, 0, 0, 0);
   for (i = 0; i < 4; i++)
   {
      const u32 x = (i & 2) ? x1 : x0, y = (i & 1) ? y1 : y0, last = i == 3 ? TA_LAST : 0;

      if (intensity)
         ta_send(TA_VERTEX | last, x, y, z, 0, 0, scale, 0);
      else
         ta_send(TA_VERTEX | last, x, y, z, one, colour, colour, colour);
   }
}

/* A modifier volume, as its own list: a slab over a rectangle of the
 * screen, from depth 0.25 up to @near. Its near face and its far face, two
 * triangles each; the last triangle of a volume comes under a parameter of
 * its own that says so ("inside last polygon"). */
static void ta_volume_part(u32 pcw, u32 x0, u32 y0, u32 x1, u32 y1, u32 near)
{
   ta_send(TA_POLYGON | TA_MODIFIER | pcw, 0, 0, 0, 0, 0, 0, 0);
   ta_volume_triangle(x0, y0, x0, y1, x1, y0, near);
   ta_volume_triangle(x1, y0, x0, y1, x1, y1, near);
   ta_volume_triangle(x0, y0, x0, y1, x1, y0, F(0.25f));
   ta_send(TA_POLYGON | TA_MODIFIER | pcw, (1u << 29) | (1u << 26), 0, 0, 0, 0, 0, 0);
   ta_volume_triangle(x1, y0, x0, y1, x1, y1, F(0.25f));
}

static void ta_volume(u32 x0, u32 y0, u32 x1, u32 y1, u32 near)
{
   ta_volume_part(0, x0, y0, x1, y1, near);
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the volume list */
}

/* What every render draws on top of the background: three polygons that
 * take shadows and a modifier volume that crosses them all.
 *
 *   A, on the left, has no texture and is white.
 *   C, in the middle, is white too, and its blend instructions say
 *   "source times its alpha", with an alpha of a half. That would make it
 *   grey, and it has to be white: an opaque polygon of the first render
 *   pass is not blended, whatever its instructions say. Games leave all
 *   sorts in there.
 *   B, on the right, has a decal texture, all blue.
 *   The volume is a slab in front of and behind a band across the three,
 *   from the middle of A to the middle of B, and the background between.
 *
 * In the volume the PowerVR2 halves what a polygon is shaded with, its base
 * and offset colours (FPU_SHAD_SCALE says by how much). So A and C are
 * grey there. B is not changed: a decal texture replaces the base colour,
 * and the texture is not scaled. Nor is the background: it does not take
 * shadows. */
static void ta_scene(void)
{
   /* The screen is 320 by 240 in the video mode the program is started in. */
   const u32 x0 = F(80.0f), x1 = F(240.0f), y0 = F(80.0f), y1 = F(100.0f);

   ta_quad(TA_SHADOW, TSP_PLAIN, 0, 0xFFFFFFFF, F(0.5f), F(32.0f), F(70.0f), F(128.0f), F(110.0f));
   ta_quad(TA_SHADOW, TSP_BY_ALPHA, 0, 0x80FFFFFF, F(0.5f), F(144.0f), F(70.0f), F(176.0f), F(110.0f));
   ta_quad(TA_SHADOW | TA_TEXTURED, TSP_PLAIN, (6u << 27) | (0x1000 >> 3), 0xFFFFFFFF, F(0.5f),
         F(192.0f), F(70.0f), F(288.0f), F(110.0f));
   /* Above them, on the left: a white polygon in fog. The fog table holds
    * one value throughout, a half, and the fog is green, so whatever the
    * depth it comes out half white and half green. */
   ta_quad(0, TSP_FOGGED, 0, 0xFFFFFFFF, F(0.5f), F(32.0f), F(30.0f), F(96.0f), F(55.0f));
   /* On the right, below B: what render_to_texture() drew, as a texture.
    * RGB565, not twiddled, 128 by 128. */
   ta_quad(TA_TEXTURED, TSP_PLAIN | (4u << 3) | 4u, (1u << 27) | (1u << 26) | (RTT_ADDRESS >> 3),
         0xFFFFFFFF, F(0.5f), F(250.0f), F(125.0f), F(300.0f), F(175.0f));
   /* Bottom middle: a white polygon whose colour is given as an intensity, 1,
    * and a face colour, 1. The two are multiplied and made eight bits, and
    * that is 255: not the 254 that 255 * 255 / 256 makes of it. */
   ta_float_quad(1, F(1.0f), F(1.0f), F(175.0f), F(215.0f), F(195.0f), F(235.0f));
   /* Bottom left: one 8-bit texture drawn twice, with the third and the
    * fourth of the four palette banks an 8-bit texture can pick, the first
    * with bilinear filtering and the second without. Both banks start out
    * blue and are changed while running, each on its own and with nothing
    * else in the palette changing after: green, and magenta. A renderer
    * that keeps a decoded copy of a paletted texture has to notice which
    * bank changed. */
   ta_quad(TA_TEXTURED, TSP_PLAIN | (1u << 13), (6u << 27) | (0x20u << 21) | (0x60000 >> 3), 0xFFFFFFFF, F(0.5f),
         F(10.0f), F(215.0f), F(30.0f), F(235.0f));
   ta_quad(TA_TEXTURED, TSP_PLAIN, (6u << 27) | (0x30u << 21) | (0x60000 >> 3), 0xFFFFFFFF, F(0.5f),
         F(35.0f), F(215.0f), F(55.0f), F(235.0f));
   /* Top middle: M, the texture with mipmaps, repeated 128 times across 40
    * pixels: far too small for any level but the smallest, the white one,
    * at any resolution it is drawn at. That is mipmapping, which the
    * console does. With Force Texture LOD0 the largest level is drawn
    * whatever the size, and M is red. */
   {
      const u32 tsp = TSP_PLAIN | (1u << 13) | (4u << 8);          /* bilinear, mipmap D of 1 */
      const u32 tcw = (1u << 31) | (1u << 27) | (0x64000 >> 3);    /* mipmapped, RGB565, twiddled */
      const u32 mx0 = F(120.0f), mx1 = F(160.0f), my0 = F(28.0f), my1 = F(52.0f), z = F(0.5f), far = F(128.0f);

      ta_send(TA_POLYGON | TA_TEXTURED, ISP_GEQUAL, tsp, tcw, 0, 0, 0, 0);
      ta_send(TA_VERTEX | TA_TEXTURED, mx0, my0, z, 0, 0, 0xFFFFFFFF, 0);
      ta_send(TA_VERTEX | TA_TEXTURED, mx0, my1, z, 0, far, 0xFFFFFFFF, 0);
      ta_send(TA_VERTEX | TA_TEXTURED, mx1, my0, z, far, 0, 0xFFFFFFFF, 0);
      ta_send(TA_VERTEX | TA_TEXTURED | TA_LAST, mx1, my1, z, far, far, 0xFFFFFFFF, 0);
   }
   /* Bottom, left of the middle: E, white, taking shadows, under a volume
    * that covers its whole width - but the volume is sent under a clipping
    * rectangle, the tile of E's left half, and asks to be kept inside it.
    * E is grey on the left and white on the right. */
   ta_quad(TA_SHADOW, TSP_PLAIN, 0, 0xFFFFFFFF, F(0.5f), F(64.0f), F(192.0f), F(128.0f), F(224.0f));
   /* Bottom, right of the middle: F, with two textures, both of palette
    * indices. Outside a volume it is the blue one, of the first palette
    * bank. In the volume over its right half it is the other, of the
    * fourth bank, which is magenta by the time the picture is taken - where
    * the renderer does two volumes at all, which the per-pixel ones do. A
    * renderer that looks the second texture's indices up in the first
    * one's bank has it green. */
   ta_quad_two(TSP_PLAIN, (6u << 27) | (0x1000 >> 3), TSP_PLAIN, (6u << 27) | (0x30u << 21) | (0x60000 >> 3),
         F(0.5f), F(200.0f), F(214.0f), F(240.0f), F(236.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the opaque list */

   ta_volume_part(0, x0, y0, x1, y1, F(0.75f));
   ta_volume_part(0, F(220.0f), F(210.0f), F(244.0f), F(239.0f), F(0.75f));
   ta_send(1u << 29, 0, 0, 0, 2, 6, 2, 6);             /* the clipping rectangle: tile column 2, row 6 */
   ta_volume_part(2u << 16, F(64.0f), F(196.0f), F(128.0f), F(220.0f), F(0.75f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the volume list */

   /* Above them, on the right: two punch-through polygons, which are drawn
    * or not texel by texel, by the texture's alpha. One has a texture that
    * is all opaque magenta, the other one that is all transparent: the
    * first is there and the second is not. */
   ta_quad(TA_PUNCH | TA_TEXTURED, TSP_CUTOUT, (6u << 27) | (0x62000 >> 3), 0xFFFFFFFF, F(0.5f),
         F(200.0f), F(30.0f), F(232.0f), F(55.0f));
   ta_quad(TA_PUNCH | TA_TEXTURED, TSP_CUTOUT, (6u << 27) | (0x63000 >> 3), 0xFFFFFFFF, F(0.5f),
         F(250.0f), F(30.0f), F(282.0f), F(55.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the punch-through list */

   /* Below those, two translucent polygons that overlap, each half
    * transparent: a blue one near and a red one far, sent nearest first.
    * They have to be blended farthest first all the same, the red over the
    * background and the blue over that, which is the renderer's sorting to
    * do. The other way round the overlap comes out reddish, not purple. */
   ta_quad(TA_TRANSLUCENT, TSP_BLEND, 0, 0x800000FF, F(0.6f), F(140.0f), F(130.0f), F(200.0f), F(170.0f));
   ta_quad(TA_TRANSLUCENT, TSP_BLEND, 0, 0x80FF0000, F(0.3f), F(120.0f), F(130.0f), F(180.0f), F(170.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the translucent list */
}

/* A render to a texture: 128 by 128, at 0x700000 in video memory, orange
 * with its top left quarter blue. The screen's scene then has a polygon
 * with that texture, which has to show it the same way up. */

static void render_to_texture(void)
{
   const u32 fb_w_ctrl = PVR(0x48), linestride = PVR(0x4C), fb_w_sof1 = PVR(0x60);

   PVR(0x48) = 1;                                      /* FB_W_CTRL: RGB565 */
   PVR(0x4C) = 128 * 2 / 8;                            /* FB_W_LINESTRIDE */
   PVR(0x60) = 0x01000000 | RTT_ADDRESS;               /* FB_W_SOF1: a texture */
   PVR(0x68) = 127 << 16;                              /* FB_X_CLIP: 0..127 */
   PVR(0x6C) = 127 << 16;                              /* FB_Y_CLIP: 0..127 */
   PVR(0x144) = 0x80000000;                            /* TA_LIST_INIT */
   ta_quad(0, TSP_PLAIN, 0, 0xFFFF8000, F(0.5f), F(0.0f), F(0.0f), F(128.0f), F(128.0f));
   ta_quad(0, TSP_PLAIN, 0, 0xFF0000FF, F(0.6f), F(0.0f), F(0.0f), F(64.0f), F(64.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the opaque list */
   PVR(0x14) = 0xFFFFFFFF;                             /* STARTRENDER */

   PVR(0x48) = fb_w_ctrl;
   PVR(0x4C) = linestride;
   PVR(0x60) = fb_w_sof1;
   PVR(0x68) = 639 << 16;
   PVR(0x6C) = 479 << 16;
}

/* A second render pass, drawn over the first: the lists are opened again
 * (TA_LIST_CONT) and the region array's second entry says this pass's
 * translucent polygons are already in order ("pre-sort").
 *
 *   P is cyan, opaque, does not take shadows, and lies in front of a
 *   corner of A, which does. A modifier volume of this pass covers the
 *   middle of P. P has to stay cyan there: a renderer that still has A
 *   down as what is showing at those pixels darkens it.
 *
 *   D, lower right, is C over again: white, takes shadows, "source times
 *   its alpha" with an alpha of a half. In this pass, a continuation,
 *   an opaque polygon's blend instructions do count, so D is grey, and
 *   darker grey under the volume that covers its middle. (It is also the
 *   one polygon here that the per-triangle renderers cannot draw a second
 *   time for its shadow, being blended, and darken the old way.)
 *
 *   The translucent pair again, lower down, blue near and red far, sent
 *   nearest first. Not being sorted, the blue is drawn first and writes
 *   its depth, and the red, behind it, is then not drawn where they
 *   overlap: blue over the background there, with no red in it. */
static void ta_scene_pass2(void)
{
   ta_quad(0, TSP_PLAIN, 0, 0xFF00FFFF, F(0.7f), F(40.0f), F(95.0f), F(70.0f), F(108.0f));
   ta_quad(TA_SHADOW, TSP_BY_ALPHA, 0, 0x80FFFFFF, F(0.5f), F(256.0f), F(180.0f), F(300.0f), F(210.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the opaque list */
   ta_volume(F(45.0f), F(97.0f), F(65.0f), F(106.0f), F(0.9f));
   ta_volume(F(270.0f), F(185.0f), F(290.0f), F(205.0f), F(0.9f));
   ta_quad(TA_TRANSLUCENT, TSP_BLEND, 0, 0x800000FF, F(0.6f), F(140.0f), F(180.0f), F(200.0f), F(210.0f));
   ta_quad(TA_TRANSLUCENT, TSP_BLEND, 0, 0x80FF0000, F(0.3f), F(120.0f), F(180.0f), F(180.0f), F(210.0f));
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the translucent list */
}

/* How long 150 frames take, in ticks of that timer, in the video mode the
 * program is started in: 263 lines of 858 pixels at 13.5 MHz, which is
 * 3343022.2 CPU cycles a frame at 200 MHz, and a tick is 16 cycles. The
 * frames timed have to add up to this, give or take 100 ticks. A scanline
 * counted as a whole number of cycles, 12711 for 12711.11, makes them 274
 * ticks short. */
#define FRAMES_150 31340833u

/* TMU channel 0, counting down at a 16th of a microsecond (12.5 MHz). */
#define TMU_TSTR  (*(volatile unsigned char *)0xFFD80004)
#define TMU_TCOR0 (*(volatile u32 *)0xFFD80008)
#define TMU_TCNT0 (*(volatile u32 *)0xFFD8000C)
#define TMU_TCR0  (*(volatile u16 *)0xFFD80010)

/* The GD-ROM drive as a game uses it: an ATA packet command that asks for
 * sectors, then a DMA transfer that brings them into memory. */
#define GD8(reg)  (*(volatile unsigned char *)(0xA05F7000 + (reg)))
#define GD16(reg) (*(volatile u16 *)(0xA05F7000 + (reg)))
#define G1(reg)   (*(volatile u32 *)(0xA05F7000 + (reg)))

/* Send a CD read for @sectors sectors of 2048 bytes from @fad, to be
 * fetched by DMA. 0 if the drive never asked for the packet. */
static int gd_read(u32 fad, u32 sectors)
{
   u16 packet[6];
   u32 i;

   packet[0] = 0x30 | (0x20 << 8);                     /* CD read, user data */
   packet[1] = ((fad >> 16) & 0xFF) | (((fad >> 8) & 0xFF) << 8);
   packet[2] = fad & 0xFF;
   packet[3] = 0;
   packet[4] = ((sectors >> 16) & 0xFF) | (((sectors >> 8) & 0xFF) << 8);
   packet[5] = sectors & 0xFF;
   GD8(0x84) = 1;                                      /* features: by DMA */
   GD8(0x9C) = 0xA0;                                   /* the packet command */
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0;
   for (i = 0; i < 6; i++)
      GD16(0x80) = packet[i];
   return 1;
}

/* What the drive has to say for itself: the REQ_ERROR command, its ten
 * bytes read back a word at a time. The sense key in the high byte and the
 * additional sense code in the low one; 0xFFFF if the drive never answered. */
static u32 gd_sense(void)
{
   static const u16 packet[6] = { 0x13, 0, 10, 0, 0, 0 };
   u16 word[5];
   u32 i, n;

   GD8(0x84) = 0;                                      /* features: not by DMA */
   GD8(0x90) = 10;                                     /* at most ten bytes */
   GD8(0x94) = 0;
   GD8(0x9C) = 0xA0;
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0xFFFF;
   for (i = 0; i < 6; i++)
      GD16(0x80) = packet[i];
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0xFFFF;
   for (n = 0; n < 5; n++)
      word[n] = GD16(0x80);
   (void)GD8(0x9C);                                    /* the status, read: that is the interrupt seen to */
   return ((word[1] & 0x0F) << 8) | (word[4] & 0xFF);
}

/* A packet command whose answer is read back a word at a time: @bytes of
 * it into @out. 0 if the drive never asked for the packet or never had
 * the answer. */
static int gd_pio(const u16 *packet, unsigned char *out, u32 bytes)
{
   u32 i;

   GD8(0x84) = 0;                                      /* features: not by DMA */
   GD8(0x90) = bytes & 0xFF;                           /* at most so many bytes */
   GD8(0x94) = bytes >> 8;
   GD8(0x9C) = 0xA0;
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0;
   for (i = 0; i < 6; i++)
      GD16(0x80) = packet[i];
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0;
   for (i = 0; i < bytes; i += 2)
   {
      u16 word = GD16(0x80);
      out[i] = word & 0xFF;
      if (i + 1 < bytes)
         out[i + 1] = word >> 8;
   }
   (void)GD8(0x9C);
   return 1;
}

/* What the drive says of itself while the audio track plays (sectors 600
 * to 899, track 2). Its status and its subcode are those of the sector
 * being played - an audio track, though the disc has data on it - and move
 * on with the music; and the Q frame it makes up has a checksum that
 * holds. Not zero if not. */
static u32 gdq_fad;

/* In two goes, some frames apart, from the frame loop: the music has to
 * have moved on in between, and what follows the start of the music is
 * timed to the sample by live_audio.py and cannot wait. */
static u32 gdq_test(u32 second)
{
   static const u16 stat_cmd[6] = { 0x0010, 0, 10, 0, 0, 0 };     /* REQ_STAT, ten bytes */
   static const u16 q_cmd[6]    = { 0x0140, 0, 14, 0, 0, 0 };     /* GET_SCD, Q only */
   static const u16 raw_cmd[6]  = { 0x0040, 0, 100, 0, 0, 0 };    /* GET_SCD, as on the disc */
   unsigned char b[100], q[12];
   u32 i, bit, fad, crc;

   if (!gd_pio(stat_cmd, b, 10))
      return 9;
   fad = (b[5] << 16) | (b[6] << 8) | b[7];
   if ((b[0] & 0x0F) != 3 || b[2] != 0x01 || b[3] != 2 || fad < 600 || fad > 899)
      return 1;
   if (!second)
   {
      gdq_fad = fad;
      return 0;
   }
   if (fad == gdq_fad)
      return 2;

   if (!gd_pio(q_cmd, b, 14))
      return 9;
   fad = (b[11] << 16) | (b[12] << 8) | b[13];
   if (b[1] != 0x11 || b[4] != 0x01 || b[5] != 2 || fad < 600 || fad > 899)
      return 3;

   if (!gd_pio(raw_cmd, b, 100))
      return 9;
   for (i = 0; i < 12; i++)
   {
      q[i] = 0;
      for (bit = 0; bit < 8; bit++)
         if (b[4 + i * 8 + bit] & 0x40)
            q[i] |= 0x80 >> bit;
   }
   crc = 0;
   for (i = 0; i < 10; i++)
   {
      crc ^= q[i] << 8;
      for (bit = 0; bit < 8; bit++)
         crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
   }
   crc ^= 0xFFFF;
   if (q[0] != 0x01 || q[1] != 0x02 || q[10] != (crc >> 8) || q[11] != (crc & 0xFF))
      return 4;
   return 0;
}

/* The drive, looked at once a frame, for the run in which the lid is opened
 * and shut again (headless.sh's last). A drive that has just been given a
 * disc is busy for a second before it has one, and then says, when asked,
 * that the medium may have changed: sense key 6, code 0x28. */
static u32 drive_open, drive_busy, drive_ready, drive_sense, drive_bios_bad;

static void drive_watch(void)
{
   u32 status = GD8(0x8C) & 0x0F;

   if (status == 6)
      drive_open++;
   else if (drive_open && !drive_ready)
   {
      if (status == 0)
      {
         /* and the BIOS, asked how the drive is, says the same: busy,
          * not that the lid is still open */
         u32 answer[2] = { 9, 9 };

         ((int (*)(u32 *, u32, u32, u32))(*(volatile u32 *)0x8C0000BC))(answer, 0, 0, 4);
         if (answer[0] != 0)
            drive_bios_bad = 1;
         drive_busy++;
      }
      else
      {
         drive_ready = 1;
         drive_sense = gd_sense();
      }
   }
}

/* A register of the sound chip, the AICA. */
#define AICA(reg) (*(volatile u32 *)(0xA0700000 + (reg)))

/* Start CDDA playback of [@fad, @end], looping, so the drive hands the
 * sound chip one audio sector every 588 samples for as long as the disc
 * is in. 0 if the drive never asked for the packet. */
static int gd_play(u32 fad, u32 end)
{
   u16 packet[6];
   u32 i;

   packet[0] = 0x20 | (0x01 << 8);                     /* CD play, FAD */
   packet[1] = ((fad >> 16) & 0xFF) | (((fad >> 8) & 0xFF) << 8);
   packet[2] = fad & 0xFF;
   packet[3] = 0x0F;                                   /* byte 6: repeat for ever */
   packet[4] = ((end >> 16) & 0xFF) | (((end >> 8) & 0xFF) << 8);
   packet[5] = end & 0xFF;
   GD8(0x84) = 0;
   GD8(0x9C) = 0xA0;
   for (i = 0; i < 100000 && !(GD8(0x18) & 0x08); i++)
      ;
   if (!(GD8(0x18) & 0x08))
      return 0;
   for (i = 0; i < 6; i++)
      GD16(0x80) = packet[i];
   return 1;
}

/* Transfer @len bytes of what the drive has to @dest. 0 if it never ends. */
static int gd_dma(u32 dest, u32 len)
{
   u32 i;

   G1(0x404) = dest;                                   /* SB_GDSTAR */
   G1(0x408) = len;                                    /* SB_GDLEN */
   G1(0x40C) = 1;                                      /* SB_GDDIR: to memory */
   G1(0x414) = 1;                                      /* SB_GDEN */
   G1(0x418) = 1;                                      /* SB_GDST */
   for (i = 0; i < 4000000 && (G1(0x418) & 1); i++)
      ;
   return !(G1(0x418) & 1);
}

/* Read the start of the disc's boot sector three ways and see that it is
 * the same every time and is what a boot sector starts with:
 *   A, five sectors in one transfer;
 *   B, the same five in ten transfers of half a sector, so that no
 *      transfer has a whole sector to move and every sector is split
 *      across two;
 *   C, eight sectors, more than the drive's DMA moves in one step;
 *   D, two sectors asked of the drive and a transfer of three asked of
 *      the DMA. The two there are arrive, the transfer is left waiting
 *      for a third that is not coming, and can be called off. An emulator
 *      that goes on trying to fill the transfer never comes back.
 * An emulator may well move whole sectors and parts of sectors by
 * different routes, and A and B then check one against the other.
 * Non-zero, saying which, if anything is off. */
/* Six instructions the recompilers used to hand to the interpreter and now
 * do themselves: DIV1, TAS.B, the two that move the status register to and
 * from memory, and the two that load FPSCR. Each is run here on values
 * that take it down every path, and what it leaves is compared with what
 * the SH-4 manual says it leaves, worked out in C below.
 *
 * They are written as .word because the assembler this is built with has
 * the floating-point unit turned off, and for DIV1 with one register for
 * both operands, which nothing would write by hand. */
__asm__(".text\n.align 2\n"
        /* t_div1(rn, rm, qmt, out): Q, M and T from qmt, then DIV1 rm,rn;
         * rn to out[0], SR to out[1] */
        ".global t_div1\nt_div1:\n"
        "  stc sr, r0\n  mov.l 1f, r1\n  and r1, r0\n  or r6, r0\n  ldc r0, sr\n"
        "  .word 0x3454\n"                             /* div1 r5,r4 */
        "  stc sr, r0\n  mov.l r4, @r7\n  mov.l r0, @(4,r7)\n  rts\n  nop\n"
        "  .align 2\n1: .long 0xFFFFFCFE\n"
        /* t_div1_same(rn, qmt, out): DIV1 rn,rn */
        ".global t_div1_same\nt_div1_same:\n"
        "  stc sr, r0\n  mov.l 2f, r1\n  and r1, r0\n  or r5, r0\n  ldc r0, sr\n"
        "  .word 0x3444\n"                             /* div1 r4,r4 */
        "  stc sr, r0\n  mov.l r4, @r6\n  mov.l r0, @(4,r6)\n  rts\n  nop\n"
        "  .align 2\n2: .long 0xFFFFFCFE\n"
        /* t_tas(p): TAS.B @p, returns T */
        ".global t_tas\nt_tas:\n  .word 0x441B\n  movt r0\n  rts\n  nop\n"
        /* t_sr(end, flip, out): STC.L SR,@-end; the stored word has flip's
         * bits turned over and is taken back with LDC.L @..+,SR. out[0]:
         * the pointer after the store; out[1]: SR after the load; out[2]:
         * the pointer after the load */
        ".global t_sr\nt_sr:\n"
        "  .word 0x4403\n"                             /* stc.l sr,@-r4 */
        "  mov.l r4, @r6\n  mov.l @r4, r0\n  xor r5, r0\n  mov.l r0, @r4\n"
        "  .word 0x4407\n"                             /* ldc.l @r4+,sr */
        "  stc sr, r0\n  mov.l r0, @(4,r6)\n  mov.l r4, @(8,r6)\n  rts\n  nop\n"
        ".global t_getsr\nt_getsr:\n  stc sr, r0\n  rts\n  nop\n"
        ".global t_setsr\nt_setsr:\n  ldc r4, sr\n  rts\n  nop\n"
        /* t_fpscr(v): LDS v,FPSCR, returns FPSCR */
        ".global t_fpscr\nt_fpscr:\n  .word 0x446A\n  .word 0x006A\n  rts\n  nop\n"
        /* t_fpscr_mem(p, after): LDS.L @p+,FPSCR, returns FPSCR; p to *after */
        ".global t_fpscr_mem\nt_fpscr_mem:\n  .word 0x4466\n  .word 0x006A\n  mov.l r4, @r5\n  rts\n  nop\n"
        ".global t_getfpscr\nt_getfpscr:\n  .word 0x006A\n  rts\n  nop\n"
        /* t_sr_slot(): returns 1. SR is loaded in the delay slot of the
         * return, with what it already holds. A recompiler that ends its
         * block at a load of SR whatever comes before it loses the return
         * and runs on into the three instructions after, which return 0. */
        ".global t_sr_slot\nt_sr_slot:\n  stc sr, r1\n  mov #1, r0\n  rts\n"
        "  .word 0x410E\n"                             /* ldc r1,sr */
        "  mov #0, r0\n  rts\n  nop\n"
        /* the same with SR taken off the stack, as a function that saved it does */
        ".global t_sr_slot_mem\nt_sr_slot_mem:\n"
        "  .word 0x4F03\n"                             /* stc.l sr,@-r15 */
        "  mov #1, r0\n  rts\n"
        "  .word 0x4F07\n"                             /* ldc.l @r15+,sr */
        "  mov #0, r0\n  rts\n  nop\n"
        /* t_illegal(): an instruction that is none, with VBR pointing at
         * a handler that notes the exception's code in t_expevt_seen and
         * returns to the instruction after. The handler is 0x100 past the
         * address in VBR, where a general exception's is. */
        ".align 2\n.global t_vbr_base\nt_vbr_base:\n  .fill 0x100, 1, 0\n"
        "  mov.l 1f, r1\n  mov.l 2f, r0\n  mov.l @r0, r0\n  mov.l r0, @r1\n"
        "  stc spc, r0\n  add #2, r0\n  ldc r0, spc\n  rte\n  nop\n"
        "  .align 2\n1: .long t_expevt_seen\n2: .long 0xFF000024\n"
        /* 0x400 past the address in VBR, the handler for a TLB miss. It
         * maps the page that was wanted and returns to try again. The
         * pages of t_mmu_map[] (address, then the whole of PTEL) come
         * first, each in a TLB entry of its own; then
         *   0xE0000000              to the Tile Accelerator, which is where
         *                           the program's store queue writes go
         *   the rest of 0xE0000000- to main memory at the same offset
         * and it counts in t_tlb_misses. */
        "  .fill 0x400 - (. - t_vbr_base), 1, 0\n"
        "  mov.l 10f, r0\n  mov.l @r0, r1\n  mov.l 11f, r2\n  and r2, r1\n"   /* the page, from PTEH */
        "  mov.l 16f, r2\n  mov #8, r5\n"
        "7: mov.l @r2, r3\n  cmp/eq r3, r1\n  bt 8f\n  add #8, r2\n  dt r5\n  bf 7b\n"
        "  mov.l 13f, r3\n  cmp/eq r3, r1\n  bt 5f\n"
        "  mov.l 14f, r3\n  and r3, r1\n  mov.l 15f, r3\n  or r3, r1\n  mov #11, r4\n  bra 6f\n  nop\n"
        "5: mov.l 12f, r1\n  mov #10, r4\n"
        "6: mov.l 17f, r3\n  or r3, r1\n  bra 9f\n  nop\n"
        "8: mov.l @(4,r2), r1\n  mov #9, r4\n  sub r5, r4\n"                  /* PTEL as given; entry 1 to 8 */
        "9: mov.l r1, @(4,r0)\n"                                               /* PTEL */
        "  mov.l @(16,r0), r1\n  mov.l 18f, r3\n  and r3, r1\n"                /* MMUCR.URC: which entry */
        "  shll8 r4\n  shll2 r4\n  or r4, r1\n  mov.l r1, @(16,r0)\n"
        "  mov.l 19f, r2\n  mov.l @r2, r3\n  add #1, r3\n  mov.l r3, @r2\n"
        "  .word 0x0038\n  rte\n  nop\n"                                       /* ldtlb */
        "  .align 2\n10: .long 0xFF000000\n11: .long 0xFFFFF000\n12: .long 0x10000000\n"
        "13: .long 0xE0000000\n14: .long 0x00FFF000\n15: .long 0x0C000000\n"
        "16: .long t_mmu_map\n"
        /* valid, 4K, read and write in any mode, dirty, shared */
        "17: .long 0x00000176\n18: .long 0xFFFF03FF\n19: .long t_tlb_misses\n"
        ".global t_illegal\nt_illegal:\n  stc vbr, r2\n  mov.l 3f, r1\n  ldc r1, vbr\n"
        "  .word 0xFFFD\n"
        "  ldc r2, vbr\n  rts\n  nop\n  .align 2\n3: .long t_vbr_base\n"
        /* t_pr_neg(bits): bits into FR2 by way of FPUL, FNEG, and back */
        ".global t_pr_neg\nt_pr_neg:\n"
        "  .word 0x445A\n  .word 0xF20D\n"            /* lds r4,fpul; fsts fpul,fr2 */
        "  .word 0xF24D\n"                             /* fneg fr2 */
        "  .word 0xF21D\n  .word 0x005A\n  rts\n  nop\n"   /* flds fr2,fpul; sts fpul,r0 */
        /* t_pr_mov(from, to): a word from memory to FR4, to FR5, to memory */
        ".global t_pr_mov\nt_pr_mov:\n"
        "  .word 0xF448\n  .word 0xF54C\n  .word 0xF55A\n  rts\n  nop\n"
        /* FR0 from and to memory, to see which bank FPSCR.FR has chosen */
        ".global t_fr0_load\nt_fr0_load:\n  .word 0xF048\n  rts\n  nop\n"
        ".global t_fr0_store\nt_fr0_store:\n  .word 0xF40A\n  rts\n  nop\n");
extern void t_div1(u32 rn, u32 rm, u32 qmt, u32 *out);
extern void t_div1_same(u32 rn, u32 qmt, u32 *out);
extern u32 t_tas(volatile unsigned char *p);
extern void t_sr(u32 *end, u32 flip, u32 *out);
extern u32 t_getsr(void);
extern void t_setsr(u32 sr);
extern u32 t_fpscr(u32 v);
extern u32 t_sr_slot(void);
extern void t_illegal(void);
volatile u32 t_expevt_seen;
extern char t_vbr_base[];
volatile u32 t_tlb_misses;
/* what the TLB miss handler maps: a page's address, and the PTEL for it */
u32 t_mmu_map[8][2];
extern u32 t_sr_slot_mem(void);
extern u32 t_pr_neg(u32 bits);
extern void t_pr_mov(u32 *from, u32 *to);
extern u32 t_fpscr_mem(u32 *p, u32 **after);
extern u32 t_getfpscr(void);
extern void t_fr0_load(u32 *p);
extern void t_fr0_store(u32 *p);

/* One step of DIV1 as the manual has it: rn and the flags in, both out */
static u32 div1_model(u32 a, u32 b, u32 *qmt)
{
   u32 t = *qmt & 1, q = (*qmt >> 8) & 1, m = (*qmt >> 9) & 1;
   const u32 qxm = q ^ m;
   u32 old;

   q = a >> 31;
   a = (a << 1) | t;
   old = a;
   if (qxm)
   {
      a += b;
      q ^= m ^ (a < old);
   }
   else
   {
      a -= b;
      q ^= m ^ (a > old);
   }
   t = !(q ^ m);
   *qmt = t | (q << 8) | (m << 9);
   return a;
}

/* 0 if all six do what they should, or which did not */
static int cpu_test(void)
{
   static const u32 vals[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFF,
                               0x12345678, 0xDEADBEEF, 0x00010000 };
   static const u32 flips[] = { 0x001, 0x002, 0x100, 0x200, 0x303 };
   static const u32 modes[] = { 0x00040001, 0x00040000, 0x00040003, 0x000C0001, 0x00240001, 0x00040001 };
   static const unsigned char bytes[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF };
   u32 out[3], buf[2], sr, fpscr, a, b, x, *after;
   volatile unsigned char byte;
   unsigned i, j, k;

   t_setsr(t_getsr() & ~0x8000u);                     /* the floating-point unit on */

   for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
      for (j = 0; j < sizeof(vals) / sizeof(vals[0]); j++)
         for (k = 0; k < 8; k++)
         {
            u32 qmt = (k & 1) | ((k & 2) << 7) | ((k & 4) << 7), want;

            want = div1_model(vals[i], vals[j], &qmt);
            t_div1(vals[i], vals[j], (k & 1) | ((k & 2) << 7) | ((k & 4) << 7), out);
            if (out[0] != want || (out[1] & 0x301) != qmt)
               return 1;
            if (j == 0)
            {
               qmt = (k & 1) | ((k & 2) << 7) | ((k & 4) << 7);
               want = div1_model(vals[i], vals[i], &qmt);
               t_div1_same(vals[i], (k & 1) | ((k & 2) << 7) | ((k & 4) << 7), out);
               if (out[0] != want || (out[1] & 0x301) != qmt)
                  return 2;
            }
         }

   for (i = 0; i < sizeof(bytes); i++)
   {
      byte = bytes[i];
      x = t_tas(&byte);
      if (x != (u32)(bytes[i] == 0) || byte != (bytes[i] | 0x80))
         return 3;
   }

   for (i = 0; i < sizeof(flips) / sizeof(flips[0]); i++)
   {
      sr = t_getsr();
      t_sr(&buf[1], flips[i], out);
      /* T is anyone's between the two readings; the rest has to be SR,
       * with the bits turned over, and SR afterwards has to be that word */
      if (out[0] != (u32)&buf[0] || out[2] != (u32)&buf[1]
            || ((buf[0] ^ sr ^ flips[i]) & 0x700083F2) || ((out[1] ^ buf[0]) & 0x700083F3))
         return 4;
   }

   fpscr = t_getfpscr();
   for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
   {
      if (t_fpscr(modes[i]) != modes[i])
         return 5;
      x = modes[i] ^ 2;
      if (t_fpscr_mem(&x, &after) != (modes[i] ^ 2) || after != &x + 1)
         return 6;
   }
   /* FPSCR.FR chooses between two banks of registers: FR0 of one is not
    * FR0 of the other, and each keeps what it was given */
   a = 0x3F800000;
   b = 0x40000000;
   t_fpscr(0x00040001);
   t_fr0_load(&a);
   t_fpscr(0x00240001);
   t_fr0_load(&b);
   t_fpscr(0x00040001);
   t_fr0_store(&x);
   if (x != a)
      return 7;
   x = 0x00240001;
   t_fpscr_mem(&x, &after);
   t_fr0_store(&x);
   if (x != b)
      return 8;
   /* A load of SR in the delay slot of a return: the return still happens */
   if (t_sr_slot() != 1)
      return 9;
   if (t_sr_slot_mem() != 1)
      return 10;
   /* With FPSCR.PR set the arithmetic is on doubles, but the moves, the
    * transfers through FPUL and FNEG are what they are without it: a
    * recompiler may do them itself in either mode, and has to get the same
    * as one that leaves the whole mode to the interpreter. */
   t_fpscr(0x000C0001);
   if (t_pr_neg(0x3F800000) != 0xBF800000 || t_pr_neg(0x80000001) != 0x00000001)
      return 11;
   a = 0x12345678;
   x = 0;
   t_pr_mov(&a, &x);
   if (x != a)
      return 12;
   t_fpscr(fpscr);
   /* An instruction that is none raises the general illegal instruction
    * exception, code 0x180, and the program goes on after its handler. (An
    * emulator has been known to end the whole process here.) */
   t_expevt_seen = 0;
   t_illegal();
   if (t_expevt_seen != 0x180)
      return 13;
   return 0;
}

/* The MMU. A page of memory is reached through an address that means
 * nothing without it, by way of the TLB miss handler above. Returns 1 if
 * that works - and the MMU is then left on, so that everything after this
 * runs with it: the recompilers have a different way of doing nearly
 * everything when it is on, which is how the Windows CE games run. Returns
 * 0 if the address is not translated: the emulator's MMU is only on for
 * Windows CE games (the core option "Force Windows CE Mode" makes this
 * disc one), and without it the test has nothing to say and puts things
 * back. Otherwise the step of mmu_probe() that went wrong. */
static u32 mmu_page[3][1024] __attribute__((aligned(4096)));

/* PTEL for a 4K page at @page: valid and shared, with the given protection
 * (0x00 read only, 0x20 read and write, both for privileged mode, which is
 * what the program runs in) and dirty bit (0x04) */
#define PTEL(page, bits) (((u32)(page) & 0x1FFFF000) | 0x112 | (bits))

static void mmu_map(u32 slot, u32 va, u32 ptel)
{
   t_mmu_map[slot][0] = va;
   t_mmu_map[slot][1] = ptel;
}

/* Not zero, and which, if a page is not reached the way its mapping says */
static int __attribute__((noinline)) mmu_probe(void)
{
   volatile u32 *va = (volatile u32 *)0x10000000;
   u32 *a = mmu_page[0], *b = mmu_page[1], *c = mmu_page[2];

   if (va[0] != 0x11223344)
   {
      (*(volatile u32 *)0xFF000010) = 0;
      return 0;
   }
   va[1] = 0x55667788;
   if (a[1] != 0x55667788 || t_tlb_misses != 1)
      return 0x11;

   /* A page that may only be read: reading it works, writing it is a
    * protection violation (0x0C0) and leaves it as it was - also once it
    * has been read, which is when an emulator that keeps translations by
    * has one for it. (The first write to a page whose dirty bit is clear
    * is an exception too, on the SH4. This emulator does not raise it,
    * nor does the one it descends from, so it is not asked for here.) */
   b[0] = 0xAAAA5555;
   mmu_map(1, 0x10001000, PTEL(b, 0x04));
   va = (volatile u32 *)0x10001000;
   if (va[0] != 0xAAAA5555)
      return 0x12;
   t_expevt_seen = 0;
   va[0] = 1;
   if (t_expevt_seen != 0x0C0 || b[0] != 0xAAAA5555)
      return 0x13;

   c[0] = 0x0BADF00D;

   /* The same address mapped somewhere else, after the TLB is emptied */
   b[2] = 0xB0B0B0B0;
   c[2] = 0xC0C0C0C0;
   mmu_map(3, 0x10003000, PTEL(b, 0x24));
   va = (volatile u32 *)0x10003000;
   if (va[2] != 0xB0B0B0B0)
      return 0x16;
   mmu_map(3, 0x10003000, PTEL(c, 0x24));
   (*(volatile u32 *)0xFF000010) = 0x00000005;
   if (va[2] != 0xC0C0C0C0)
      return 0x17;
   va[2] = 0xC1C1C1C1;
   if (c[2] != 0xC1C1C1C1 || b[2] != 0xB0B0B0B0)
      return 0x18;

   /* And for another address space: a page that is not shared belongs to
    * the space it was mapped in, and changing space is all it takes for
    * the address to mean another page. */
   mmu_map(4, 0x10004000, PTEL(b, 0x24) & ~2);
   (*(volatile u32 *)0xFF000000) = 1;
   va = (volatile u32 *)0x10004000;
   if (va[2] != 0xB0B0B0B0)
      return 0x19;
   mmu_map(4, 0x10004000, PTEL(c, 0x24) & ~2);
   (*(volatile u32 *)0xFF000000) = 2;
   if (va[2] != 0xC1C1C1C1)
      return 0x1A;
   (*(volatile u32 *)0xFF000000) = 0;
   return 1;
}

static int mmu_test(void)
{
   u32 i;

   __asm__ volatile ("ldc %0, vbr" : : "r" (t_vbr_base));
   for (i = 0; i < 8; i++)
      mmu_map(i, 0xFFFFFFFF, 0);
   mmu_map(0, 0x10000000, PTEL(mmu_page[0], 0x24));
   mmu_page[0][0] = 0x11223344;
   mmu_page[0][1] = 0;
   t_tlb_misses = 0;
   (*(volatile u32 *)0xFF000000) = 0;                  /* PTEH: address space 0 */
   (*(volatile u32 *)0xFF000010) = 0x00000005;         /* MMUCR: on, and the TLB emptied */
   return mmu_probe();
}

static int gd_test(void)
{
   static const char id[16] = "SEGA SEGAKATANA ";
   volatile unsigned char *a = (volatile unsigned char *)0xAC200000;
   volatile unsigned char *b = (volatile unsigned char *)0xAC210000;
   volatile unsigned char *c = (volatile unsigned char *)0xAC220000;
   volatile unsigned char *d = (volatile unsigned char *)0xAC230000;
   u32 i;

   for (i = 0; i < 9 * 2048; i++)
      a[i] = b[i] = c[i] = d[i] = 0xEE;
   if (!gd_read(45150, 5) || !gd_dma(0x0C200000, 5 * 2048))
      return 1;
   if (!gd_read(45150, 5))
      return 2;
   for (i = 0; i < 10; i++)
      if (!gd_dma(0x0C210000 + i * 1024, 1024))
         return 2;
   if (!gd_read(45150, 8) || !gd_dma(0x0C220000, 8 * 2048))
      return 3;
   for (i = 0; i < 16; i++)
      if (a[i] != (unsigned char)id[i])
         return 4;
   for (i = 0; i < 5 * 2048; i++)
      if (b[i] != a[i])
         return 5;
   for (i = 0; i < 5 * 2048; i++)
      if (c[i] != a[i])
         return 6;
   if (!gd_read(45150, 2))
      return 8;
   G1(0x404) = 0x0C230000;
   G1(0x408) = 3 * 2048;
   G1(0x40C) = 1;
   G1(0x414) = 1;
   G1(0x418) = 1;
   for (i = 0; i < 400000 && (G1(0x418) & 1); i++)
      ;
   if (!(G1(0x418) & 1))
      return 9;                                        /* finished, on two sectors? */
   G1(0x414) = 0;                                      /* called off */
   if (G1(0x418) & 1)
      return 10;
   for (i = 0; i < 2 * 2048; i++)
      if (d[i] != a[i])
         return 11;
   if (d[2 * 2048] != 0xEE)
      return 12;
   /* ...and nothing was written past the end of what was asked for */
   if (a[5 * 2048] != 0xEE || b[5 * 2048] != 0xEE || c[8 * 2048] != 0xEE)
      return 7;
   return 0;
}

void cmain(void)
{
   u32 frame = 0, i;
   u32 tick = 0, shortest = 0xFFFFFFFF, longest = 0; u32 total = 0;
   int gd_bad;
   int sq_bad;
   u32 spg_bad;
   u32 maple_bad;
   u32 dmac_bad;
   u32 p4_bad;
   u32 gdq_bad = 0;
   int mmu_state;
   int cpu_bad;
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
   PVR(0x8C) = 3 << 24;                                /* ISP_BACKGND_T: three more words to a vertex */
   /* Its three vertices: three corners of the screen, and the texture once
    * across and once down. The background is the plane through them, and
    * with none given - all three at one point - the core has to fall back
    * on a guess; this way what every screenshot's background shows is the
    * plane as it is worked out. */
   {
      static const u32 bg_vertices[3][6] = {
         /* x           y           z           u           v           colour */
         { 0x00000000, 0x00000000, 0x38D1B717, 0x00000000, 0x00000000, 0xFFFFFFFF },
         { 0x44200000, 0x00000000, 0x38D1B717, 0x3F800000, 0x00000000, 0xFFFFFFFF },   /* 640, 0 */
         { 0x00000000, 0x43F00000, 0x38D1B717, 0x00000000, 0x3F800000, 0xFFFFFFFF },   /* 0, 480 */
      };
      volatile u32 *v = (volatile u32 *)0xA510000C;
      u32 n;

      for (n = 0; n < 18; n++)
         v[n] = bg_vertices[n / 6][n % 6];
   }
   PVR(0x88) = 0x38D1B717;                             /* ISP_BACKGND_D: 0.0001, far away */
   (*(volatile u32 *)0xA5100000) = 0x02000000;         /* ISP word: textured */
   /* TSP word: source x 1 + destination x 0. Opaque polygons are blended
    * by these instructions like any others, and with the word left at
    * zero that is everything times zero: a black screen, whatever the
    * texture holds. */
   (*(volatile u32 *)0xA5100004) = (1u << 29) | (2u << 22);   /* and no fog */
   (*(volatile u32 *)0xA5100008) = 6u << 27;           /* TCW: 8-bit palette, at address 0 */
   set_palette(1, 0x7C00);                             /* red */
   set_palette(2, 0x03E0);                             /* green */
   set_palette(3, 0x001F);                             /* blue */
   set_palette(4, 0x7FFF);                             /* white, until frame 70 */
   /* The texture of the shadow test's polygon B: 8x8, all palette entry 3. */
   for (i = 0; i < 16; i++)
      (*(volatile u32 *)(0xA4001000 + i * 4)) = 0x03030303;
   PVR(0x74) = 0x100 | 128;                            /* FPU_SHAD_SCALE: shadows halve, by intensity */
   /* The fog: green, and a table that says "half" at every depth. */
   PVR(0xB0) = 0x0000FF00;                             /* FOG_COL_RAM */
   PVR(0xB8) = 0xFF07;                                 /* FOG_DENSITY */
   for (i = 0; i < 128; i++)
      PVR(0x200 + i * 4) = 0x8080;                     /* FOG_TABLE */
   /* The punch-through polygons' textures and their palette entries: 5 is
    * magenta with the alpha bit set, 6 the same without it. */
   for (i = 0; i < 16; i++)
   {
      (*(volatile u32 *)(0xA4062000 + i * 4)) = 0x05050505;
      (*(volatile u32 *)(0xA4063000 + i * 4)) = 0x06060606;
   }
   set_palette(5, 0xFC1F);
   set_palette(6, 0x7C1F);
   /* The texture for the palette bank polygons, all entry 7, and that entry
    * in the third and fourth banks: blue to start with. (These textures are
    * where the framebuffer writes of the first frames do not reach.) */
   for (i = 0; i < 16; i++)
      (*(volatile u32 *)(0xA4060000 + i * 4)) = 0x07070707;
   set_palette(2 * 256 + 7, 0x001F);
   set_palette(3 * 256 + 7, 0x001F);
   /* A texture with mipmaps, 8x8 RGB565: its 1x1 level white, 2x2 blue, 4x4
    * green and the 8x8 one red. The levels come smallest first, after three
    * unused texels. */
   for (i = 0; i < 88; i++)
      (*(volatile u16 *)(0xA4064000 + i * 2)) = i < 3 ? 0 : i == 3 ? 0xFFFF : i < 8 ? 0x001F : i < 24 ? 0x07E0 : 0xF800;
   PVR(0x11C) = 0x80;                                  /* PT_ALPHA_REF */
   sq_bad = sq_test();
   (*(volatile u32 *)0xFF000038) = 0x10;               /* QACR0: store queues go to the TA */
   (*(volatile u32 *)0xFF00003C) = 0x10;               /* QACR1 */
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
   /* tile 19,14, last; and what the second render pass goes by: pre-sorted */
   (*(volatile u32 *)0xA5180018) = 0x80000000 | 0x20000000 | (14 << 8) | (19 << 2);
   PVR(0x2C) = 0x180000;                               /* REGION_BASE */
#endif

   /* The disc, read the way a game reads it, before anything is drawn */
   mmu_state = mmu_test();
   gd_bad = gd_test();
   cpu_bad = cpu_test();
   spg_bad = spg_test();
   maple_bad = maple_test();
   dmac_bad = dmac_test();
   p4_bad = p4_test();
   /* and the audio track playing underneath everything that follows:
    * the sector the sound chip mixes from is lent out of the image, and
    * the save, load, reset and unload below all happen while it is */
   if (!gd_play(600, 899))
      gd_bad = 9;
   /* ...and audible: the sound chip's master volume all the way up, and
    * the CD's left and right channels sent at full level, the left one all
    * the way to the left and the right one to the right. At that setting
    * what comes out of the sound chip is the disc's samples, bit for bit,
    * which is what tools/threads/live_audio.py checks it against. */
   AICA(0x2800) = 0x000F;                              /* MVOL */
   AICA(0x2040) = (0xF << 8) | 0x1F;                   /* CDDA left: level, pan */
   AICA(0x2044) = (0xF << 8) | 0x0F;                   /* CDDA right */
   /* A sound of the sound chip's own to go with it: 256 samples of 16-bit
    * PCM in its memory, which channel 0 will play in a loop at the rate
    * they were made for, at full level and all the way to the left, with
    * its filter and its envelope's attenuation switched off. Then what
    * the channel adds to the left is those samples as they are. */
   for (i = 0; i < 300; i++)
      (*(volatile u16 *)(0xA0810000 + i * 2)) = (u16)(i < 256 ? (int)((i * 97) % 4001) - 2000 : 0x7777);
   AICA(0x04) = 0x0000;                                /* SA: 0x10000, low */
   AICA(0x08) = 0;                                     /* loop start */
   AICA(0x0C) = 256;                                   /* loop end */
   AICA(0x10) = 0x001F;                                /* fastest attack, no decay */
   AICA(0x14) = 0x3C1F;                                /* fastest release, no key scaling */
   AICA(0x18) = 0;                                     /* 44100 Hz */
   AICA(0x1C) = 0;                                     /* no LFO */
   AICA(0x20) = 0;                                     /* nothing to the DSP */
   AICA(0x24) = (0xF << 8) | 0x1F;                     /* full level, left */
   AICA(0x28) = 0x0060;                                /* filter off, attenuation off */
   /* A second sound, for channel 1 and its low-pass filter: 4096 samples,
    * the first half a square wave and the second half silence, looping, at
    * full level and all the way to the right. The filter is on, its cutoff
    * held at one value the whole time and its resonance turned up. What the
    * channel adds to the right is then what the filter makes of the wave,
    * which live_audio.py works out for itself and compares. Its key is
    * down from the start: it sounds when channel 0 is first keyed on, and
    * goes on to the end. */
   for (i = 0; i < 4096; i++)
      (*(volatile u16 *)(0xA0820000 + i * 2)) = (u16)(i >= 2048 ? 0 : (i & 32) ? -6000 : 6000);
   AICA(0x80 + 0x04) = 0x0000;                         /* SA: 0x20000, low */
   AICA(0x80 + 0x08) = 0;
   AICA(0x80 + 0x0C) = 4096;
   AICA(0x80 + 0x10) = 0x001F;
   AICA(0x80 + 0x14) = 0x3C1F;
   AICA(0x80 + 0x18) = 0;
   AICA(0x80 + 0x1C) = 0;
   AICA(0x80 + 0x20) = 0;
   AICA(0x80 + 0x24) = (0xF << 8) | 0x0F;              /* full level, right */
   AICA(0x80 + 0x28) = 0x0048;                         /* filter on, Q 8, attenuation off */
   for (i = 0; i < 5; i++)
      AICA(0x80 + 0x2C + i * 4) = 0x1900;              /* the cutoff, at every stage */
   AICA(0x80 + 0x40) = 0;                              /* and no hurry between them */
   AICA(0x80 + 0x44) = 0;
   AICA(0x80 + 0x00) = 0x4202;                         /* 16-bit, looping, key down */
   /* A third, for channel 2: 4096 samples played an octave down, so that
    * every other sample that comes out lies halfway between two of them
    * and has to be worked out. Full level, all the way to the left, filter
    * and attenuation off, key down from the start like channel 1. The
    * sample after the last is the first again, so that the one worked out
    * across the end of the loop is the same whichever of them the chip
    * takes. */
   for (i = 0; i <= 4096; i++)
      (*(volatile u16 *)(0xA0830000 + i * 2)) = (u16)((int)(((i & 4095) * 131) % 1999) - 999);
   AICA(0x100 + 0x04) = 0x0000;                        /* SA: 0x30000, low */
   AICA(0x100 + 0x08) = 0;
   AICA(0x100 + 0x0C) = 4096;
   AICA(0x100 + 0x10) = 0x001F;
   AICA(0x100 + 0x14) = 0x3C1F;
   AICA(0x100 + 0x18) = 0xF << 11;                     /* one octave down */
   AICA(0x100 + 0x1C) = 0;
   AICA(0x100 + 0x20) = 0;
   AICA(0x100 + 0x24) = (0xF << 8) | 0x1F;             /* full level, left */
   AICA(0x100 + 0x28) = 0x0060;                        /* filter off, attenuation off */
   AICA(0x100 + 0x00) = 0x4203;                        /* 16-bit, looping, key down */

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
      else if (frame == 90)
         set_palette(2 * 256 + 7, 0x03E0);             /* the third bank: green */
      /* The channel is keyed on, off and on again, well apart, so that it
       * starts, stops and starts while the audio track plays on. */
      if (frame == 100 || frame == 200)
         AICA(0x00) = 0xC201;                          /* key on: 16-bit, looping */
      else if (frame == 160)
         AICA(0x00) = 0x8201;                          /* key off */

      if (frame == 100)
         set_palette(3 * 256 + 7, 0x7C1F);             /* the fourth bank: magenta */

      if ((frame & 3) == 3 && frame < 64)
      {
         /* direct framebuffer write: the core renders the framebuffer */
         u16 c = (u16)(frame * 2113) & 0x7FFF;         /* top bit clear: see NO_REGION_ARRAY */
         for (i = 0; i < 640 * 64; i++)
            VRAM16[i + (frame & 63) * 640] = c;
      }
      else
      {
         /* a Tile Accelerator frame and a render start; once the framebuffer
          * frames are over, with the shadow test's scene in it and into one
          * of two buffers in turn */
         if (frame >= 64)
         {
            PVR(0x60) = (frame & 1) ? 0x400000 : 0x200000;   /* FB_W_SOF1 */
            render_to_texture();
         }
         PVR(0x144) = 0x80000000;                      /* TA_LIST_INIT */
         if (frame >= 64)
         {
            ta_scene();
            PVR(0x160) = 0x80000000;                   /* TA_LIST_CONT */
            ta_scene_pass2();
         }
         PVR(0x14) = 0xFFFFFFFF;                       /* STARTRENDER */
      }
      poll_controller();
      drive_watch();
      if (frame == 60)
         gdq_bad = gdq_test(0);
      else if (frame == 66 && !gdq_bad)
         gdq_bad = gdq_test(1);
      wait_vblank();
      wait_line(16);
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
            total += took;
         }
         tick = now;
         if (frame == 250 && cpu_bad)
            set_palette(4, 0x03E0);                    /* green: an instruction came out wrong */
         else if (frame == 250 && longest - shortest > 200)
            set_palette(4, 0x7C00);                    /* red: uneven frames */
         else if (frame == 250 && (total < FRAMES_150 - 100 || total > FRAMES_150 + 100))
            set_palette(4, 0x03FF);                    /* cyan: frames of the wrong length */
         else if (frame == 250 && stale)
            set_palette(4, 0x7C1F);                    /* magenta: a register with junk in it */
         else if (frame == 250 && gd_bad)
            set_palette(4, 0x001F | (gd_bad << 7));     /* blue: the disc read back wrong */
         else if (frame == 250 && sq_bad)
            set_palette(4, 0x7E00);                    /* orange: a store queue went where the other one's register says */
         /* The same verdict where it can be read without a picture: see
          * live_verdict. */
         if (frame == 250)
            live_verdict = cpu_bad ? 0xBAD00100 + cpu_bad
               : longest - shortest > 200 ? 0xBAD00002
               : (total < FRAMES_150 - 100 || total > FRAMES_150 + 100) ? 0xBAD00003
               : stale ? 0xBAD00004
               : gd_bad ? 0xBAD00005
               : sq_bad ? 0xBAD00006
               : spg_bad ? 0xBAD00800 + spg_bad
               : maple_bad ? 0xBAD00900 + maple_bad
               : dmac_bad ? 0xBAD00A00 + dmac_bad
               : p4_bad ? 0xBAD00B00 + p4_bad
               : gdq_bad ? 0xBAD00D00 + gdq_bad
               : mmu_state > 1 ? 0xBAD00700 + mmu_state
               : mmu_state == 1 ? 0x600D4D4D
               : 0x600D600D;
         /* ...and if the lid was opened on the way, how the drive took the
          * disc back is the verdict */
         if (frame == 250 && drive_open && live_verdict == 0x600D600D)
            live_verdict = (drive_busy < 55 || drive_busy > 65) ? 0xBAD00C01
               : !drive_ready ? 0xBAD00C02
               : drive_sense != 0x628 ? 0xBAD00C03
               : drive_bios_bad ? 0xBAD00C04
               : 0x600D5A9D;
      }

      /* ...and shown at the next vblank, the way a game flips buffers */
      if (frame >= 64)
         PVR(0x50) = (frame & 1) ? 0x400000 : 0x200000;      /* FB_R_SOF1 */
      frame++;
   }
}
