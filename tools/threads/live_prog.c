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
static void ta_volume(u32 x0, u32 y0, u32 x1, u32 y1, u32 near)
{
   ta_send(TA_POLYGON | TA_MODIFIER, 0, 0, 0, 0, 0, 0, 0);
   ta_volume_triangle(x0, y0, x0, y1, x1, y0, near);
   ta_volume_triangle(x1, y0, x0, y1, x1, y1, near);
   ta_volume_triangle(x0, y0, x0, y1, x1, y0, F(0.25f));
   ta_send(TA_POLYGON | TA_MODIFIER, (1u << 29) | (1u << 26), 0, 0, 0, 0, 0, 0);
   ta_volume_triangle(x1, y0, x0, y1, x1, y1, F(0.25f));
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
   ta_send(0, 0, 0, 0, 0, 0, 0, 0);                    /* end of the opaque list */

   ta_volume(x0, y0, x1, y1, F(0.75f));

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
   PVR(0x11C) = 0x80;                                  /* PT_ALPHA_REF */
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
   gd_bad = gd_test();
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
            total += took;
         }
         tick = now;
         if (frame == 250 && longest - shortest > 200)
            set_palette(4, 0x7C00);                    /* red: uneven frames */
         else if (frame == 250 && (total < FRAMES_150 - 100 || total > FRAMES_150 + 100))
            set_palette(4, 0x03FF);                    /* cyan: frames of the wrong length */
         else if (frame == 250 && stale)
            set_palette(4, 0x7C1F);                    /* magenta: a register with junk in it */
         else if (frame == 250 && gd_bad)
            set_palette(4, 0x001F | (gd_bad << 7));     /* blue: the disc read back wrong */
      }

      /* ...and shown at the next vblank, the way a game flips buffers */
      if (frame >= 64)
         PVR(0x50) = (frame & 1) ? 0x400000 : 0x200000;      /* FB_R_SOF1 */
      frame++;
   }
}
