/* The NAOMI 2's geometry processor (core/hw/pvr/elan.c), run by itself:
 * display lists are put in its memory and sent to its command port, and
 * what it writes to the tile accelerator is looked at.
 *
 * The machine around it is here - a tile accelerator that only keeps what
 * it is sent and knows which list is open and whether half a 64-byte
 * parameter is outstanding, which is all the chip asks of it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "elan.h"

static int failures;

#define CHECK(what) do { if (!(what)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #what); failures++; } } while (0)

/* ---- the machine ---- */

#define TA_MAX 65536
static uint32_t ta[TA_MAX][8];
static unsigned ta_count;
static int ta_open = -1;         /* list */
static int ta_vertex_blocks = 1; /* of the polygon that is open */
static int ta_half;
static unsigned list_ends[8];
static unsigned list_end_count;
static struct { uint32_t vram, src, size; int from_eram; } dma[8];
static unsigned dma_count;
static bool dma_refuse;

void elan_host_ta(const uint32_t *blocks, unsigned count)
{
   for (; count; count--, blocks += 8)
   {
      uint32_t pcw = blocks[0];
      if (ta_count < TA_MAX)
         memcpy(ta[ta_count++], blocks, 32);
      if (ta_half)
      {
         ta_half = 0;
         continue;
      }
      switch (pcw >> 29)
      {
         case 0:  /* end of list */
            ta_open = -1;
            break;
         case 4:  /* polygon or modifier volume */
            if (ta_open < 0)
               ta_open = (int)((pcw >> 24) & 7);
            if (ta_open & 1)
               ta_vertex_blocks = 2;
            else
               ta_vertex_blocks = (pcw & 0x48) == 0x48 ? 2 : 1;   /* two volumes, textured */
            break;
         case 5:  /* sprite */
            if (ta_open < 0)
               ta_open = (int)((pcw >> 24) & 7);
            ta_vertex_blocks = 2;
            break;
         case 7:  /* vertex */
            ta_half = ta_vertex_blocks == 2;
            break;
      }
   }
}

int elan_host_ta_list(void)
{
   return ta_open;
}

int elan_host_ta_half(void)
{
   return ta_half;
}

void elan_host_list_end(unsigned bit)
{
   if (list_end_count < 8)
      list_ends[list_end_count++] = bit;
}

bool elan_host_texture_dma(uint32_t vram, uint32_t src, uint32_t size, int from_eram)
{
   if (dma_refuse)
      return false;
   if (dma_count < 8)
   {
      dma[dma_count].vram = vram;
      dma[dma_count].src = src;
      dma[dma_count].size = size;
      dma[dma_count].from_eram = from_eram;
      dma_count++;
   }
   return true;
}

/* ---- writing display lists ---- */

static uint8_t *ram;
static uint32_t at;     /* where the next command goes */

static uint32_t f2u(float f)
{
   uint32_t u;
   memcpy(&u, &f, 4);
   return u;
}

static float u2f(uint32_t u)
{
   float f;
   memcpy(&f, &u, 4);
   return f;
}

static uint32_t *put(unsigned words)
{
   uint32_t *p = (uint32_t *)(ram + at);
   memset(p, 0, words * 4);
   at += words * 4;
   return p;
}

#define N2(cmd) (0x08000000u | ((uint32_t)(cmd) << 8))

static void put_projection(float fx, float tx, float fy, float ty)
{
   uint32_t *p = put(8);
   p[0] = N2(3);
   p[2] = f2u(fx); p[3] = f2u(tx); p[4] = f2u(fy); p[5] = f2u(ty);
}

/* A model @z in front of the camera, facing it or turned by @turn about
 * the y axis; normals go to light space unchanged. */
static void put_instance(float z, float near_plane, float far_plane)
{
   uint32_t *p = put(40);
   p[0] = N2(4);
   p[1] = 0xf;
   p[2] = 0x7f;
   p[10] = f2u(1.0f); p[14] = f2u(1.0f); p[18] = f2u(1.0f);     /* lm */
   p[25] = f2u(near_plane);
   p[26] = f2u(1.0f); p[30] = f2u(1.0f); p[34] = f2u(1.0f);     /* tm */
   p[37] = f2u(z);
   p[38] = f2u(far_plane);
}

static void put_light_model(uint32_t diffuse_mask, uint32_t specular_mask, uint32_t ambient)
{
   uint32_t *p = put(8);
   p[0] = N2(4);
   p[1] = 0x10;
   p[2] = diffuse_mask | specular_mask << 16;
   p[3] = ambient;
}

/* A light shining along +z in light space - at a surface that faces -z. */
static void put_parallel_light(unsigned id, uint32_t rgb)
{
   uint32_t *p = put(8);
   p[0] = N2(4) | (1u << 20) | 0xf;       /* parallel; the low bits of the z direction */
   p[1] = id | rgb << 8;
   p[2] = 0x7f;                           /* direction (0, 0, 1) */
}

static void put_gmp(uint32_t select, uint32_t diffuse)
{
   uint32_t *p = put(16);
   p[0] = N2(5);
   p[2] = select;
   p[3] = diffuse;
}

static void put_end_of_list(void)
{
   put(8);
}

/* Vertices: x, y, z, u, v, with a normal of (0, 0, -1); @flags per vertex
 * are the strip marks. */
struct vtx { float x, y, z, u, v; uint32_t marks; };
#define MARK_STRIP 0x20000000u
#define MARK_FAN   0x40000000u
#define MARK_END   0x80000000u

static void put_polygons(uint32_t pcw_bits, uint32_t isp, uint32_t tsp, uint32_t tcw,
      const struct vtx *v, unsigned count)
{
   uint32_t *p = put(8);
   unsigned i;
   p[0] = N2(7) | pcw_bits;
   p[1] = isp;
   p[2] = tsp;
   p[3] = tcw;
   p[6] = 0x00a;     /* position and texture coordinates */
   p[7] = count;
   for (i = 0; i < count; i++)
   {
      uint32_t *w = put(6);
      w[0] = 0x00810000u | v[i].marks;    /* normal (0, 0, -127) */
      w[1] = f2u(v[i].x); w[2] = f2u(v[i].y); w[3] = f2u(v[i].z);
      w[4] = f2u(v[i].u); w[5] = f2u(v[i].v);
   }
   at = (at + 31) & ~31u;
}

/* Sends the commands from @from to here to the chip, as a link from its
 * command port. */
static void run(uint32_t from)
{
   uint32_t cmd[8];
   unsigned i;
   memset(cmd, 0, sizeof(cmd));
   cmd[0] = N2(0xf);
   cmd[1] = from;
   cmd[2] = 0x09000000;
   cmd[3] = at - from;
   for (i = 0; i < 8; i++)
      elan_cmd_write(0x09000000 + i * 4, cmd[i]);
}

static void start(void)
{
   elan_reset();
   memset(ram, 0, 1 << 20);
   at = 0x1000;
   ta_count = 0;
   ta_open = -1;
   ta_half = 0;
   ta_vertex_blocks = 1;
   list_end_count = 0;
   dma_count = 0;
   dma_refuse = false;
}

static bool near(float a, float b)
{
   return fabs((double)(a - b)) < 1e-3;
}

/* ---- tests ---- */

static const struct vtx quad[4] = {
   { -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0 },
   { -1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 0 },
   {  1.0f, -1.0f, 0.0f, 1.0f, 0.0f, MARK_STRIP | MARK_FAN },
   {  1.0f,  1.0f, 0.0f, 1.0f, 1.0f, MARK_STRIP | MARK_END },
};

static void test_registers(void)
{
   start();
   CHECK(elan_reg_read(0x08800000) == 0xe1ad0000u);
   CHECK(elan_reg_read(0x08800004) == 0x10);
   elan_reg_write(0x08800010, 0x1234);
   CHECK(elan_reg_read(0x08800010) == 0x1234);
}

/* A lit, textured strip: where it lands on the screen, what colour the
 * light makes it, and the form it has for the tile accelerator. */
static void test_strip(void)
{
   uint32_t from;
   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_light_model(1, 0, 0x00202020);
   put_parallel_light(0, 0x404040);
   put_gmp(0, 0);
   put_polygons(0x0a, 0x80000000u, 0x20000000u | 0x24, 0x12345678u, quad, 4);
   run(from);

   CHECK(ta_count == 5);
   if (ta_count == 5)
   {
      /* an opaque polygon, textured, Gouraud shaded, packed colours */
      CHECK(ta[0][0] == (0x80000000u | 0x0a));
      CHECK((ta[0][1] & 0xe0000000u) == 0x80000000u);    /* depth mode kept */
      CHECK(ta[0][1] & (1u << 25));                      /* textured */
      CHECK(ta[0][2] == (0x20000000u | 0x24));
      CHECK(ta[0][3] == 0x12345678u);
      /* vertex 0: (-1, -1, 10) */
      CHECK(ta[1][0] == 0xe0000000u);
      CHECK(near(u2f(ta[1][1]), 320.0f - 32.0f));
      CHECK(near(u2f(ta[1][2]), 240.0f + 24.0f));
      CHECK(near(u2f(ta[1][3]), 0.1f));
      CHECK(u2f(ta[1][4]) == 0.0f && u2f(ta[1][5]) == 0.0f);
      /* twice the light's 0x40 from head on, and 0x20 all round */
      CHECK(ta[1][6] == 0xffa0a0a0u);
      CHECK(ta[1][7] == 0xff000000u || ta[1][7] == 0);
      /* vertex 3: (1, 1, 10), the end of the strip */
      CHECK(ta[4][0] == 0xf0000000u);
      CHECK(near(u2f(ta[4][1]), 320.0f + 32.0f));
      CHECK(near(u2f(ta[4][2]), 240.0f - 24.0f));
      CHECK(u2f(ta[4][4]) == 1.0f && u2f(ta[4][5]) == 1.0f);
   }
   CHECK(elan_reg_read(0x08800074) & 2);      /* command done */
}

/* Light from behind the surface does not light it; no light model at
 * all leaves it black; a model marked constant keeps its own colour. */
static void test_lighting(void)
{
   uint32_t from;
   unsigned i;

   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_light_model(1, 0, 0x00202020);
   put_parallel_light(0, 0x404040);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(from);
   CHECK(ta_count == 5 && ta[1][6] == 0xffa0a0a0u);

   /* normals turned round: only the ambient light is left */
   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_light_model(1, 0, 0x00202020);
   put_parallel_light(0, 0x404040);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   for (i = 0; i < 4; i++)
      *(uint32_t *)(ram + at - 96 + i * 24) = 0x007f0000u | quad[i].marks;   /* normal (0, 0, 127) */
   run(from);
   CHECK(ta_count == 5 && ta[1][6] == 0xff202020u);

   /* nothing to light it */
   start();
   from = at;
   put_instance(10.0f, 1.0f, 100.0f);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(from);
   CHECK(ta_count == 5 && ta[1][6] == 0xff000000u);

   /* constant: the model's diffuse colour, untouched */
   start();
   from = at;
   put_instance(10.0f, 1.0f, 100.0f);
   put_gmp(1 | 1 << 9, 0x80112233u);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(from);
   CHECK(ta_count == 5 && ta[1][6] == 0x80112233u);
}

/* The near plane: a polygon wholly nearer than it is not drawn, one that
 * crosses it is cut, and nothing that comes out is nearer than it. */
static void test_clipping(void)
{
   static const struct vtx crossing[3] = {
      { 0.0f, 0.0f, -9.5f, 0.0f, 0.0f, 0 },     /* 0.5 from the eye: nearer than 1 */
      { 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0 },
      { 0.0f, 1.0f,  0.0f, 0.0f, 1.0f, MARK_STRIP | MARK_FAN | MARK_END },
   };
   uint32_t from;
   unsigned i;

   start();
   from = at;
   put_instance(0.5f, 1.0f, 100.0f);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   put_instance(200.0f, 1.0f, 100.0f);          /* and one beyond the far plane */
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(from);
   CHECK(ta_count == 0);

   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, crossing, 3);
   run(from);
   CHECK(ta_count >= 4);
   for (i = 1; i < ta_count; i++)
   {
      float z = u2f(ta[i][3]);
      CHECK(z > 0.0f && z <= 1.0001f);          /* 1 / distance, the near plane at 1 */
      CHECK(((ta[i][0] >> 28) & 1) == (i == ta_count - 1));
   }
}

/* A fan is sent as strips of its triangles. */
static void test_fan(void)
{
   static const struct vtx fan[5] = {
      { 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0 },
      { 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0 },
      { 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, MARK_STRIP | MARK_FAN },
      { 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, MARK_FAN },
      { -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, MARK_FAN | MARK_END },
   };
   uint32_t from;
   unsigned i, ends = 0;

   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, fan, 5);
   run(from);
   /* three triangles: 0 1 2, then 0 2 3, then 0 3 4 */
   CHECK(ta_count == 1 + 9);
   for (i = 1; i < ta_count; i++)
      ends += (ta[i][0] >> 28) & 1;
   CHECK(ends == 3);
   if (ta_count == 10)
   {
      CHECK(near(u2f(ta[4][1]), 320.0f) && near(u2f(ta[4][2]), 240.0f));   /* the centre again */
      CHECK(near(u2f(ta[5][1]), 352.0f) && near(u2f(ta[5][2]), 216.0f));   /* vertex 2 */
      CHECK(near(u2f(ta[6][1]), 320.0f) && near(u2f(ta[6][2]), 216.0f));   /* vertex 3 */
   }
}

/* What is not for the chip goes to the tile accelerator as it is, to the
 * next command that is: a 64-byte parameter's second half is not looked
 * at for that. And a list that is open is where the chip's polygons go. */
static void test_passthrough(void)
{
   uint32_t from;
   uint32_t *p;

   start();
   from = at;
   p = put(8);                   /* a modifier volume list is opened by hand */
   p[0] = 0x81000000u;
   p = put(16);                  /* one of its triangles: 64 bytes */
   p[0] = 0xe0000000u;
   p[8] = 0x08000300u;           /* what would be a command, were it looked at */
   put_instance(10.0f, 1.0f, 100.0f);
   put_polygons(0x0a, 0x20000000u, 0, 0, quad, 4);    /* drawn into the open list: a volume */
   put_end_of_list();
   run(from);
   CHECK(ta_count == 1 + 2 + 1 + 2 * 2 + 1);
   if (ta_count == 9)
   {
      CHECK(ta[2][0] == 0x08000300u);
      CHECK((ta[3][0] & 0xff000000u) == 0x81000000u);    /* a volume, list 1 */
      CHECK(ta[4][0] == 0xf0000000u);
      CHECK(ta[8][0] == 0);
   }
   CHECK(ta_open == -1);
}

static void test_register_wait(void)
{
   uint32_t from;
   uint32_t *p;

   start();
   from = at;
   put_instance(10.0f, 1.0f, 100.0f);
   p = put(8);
   p[0] = N2(0xe);
   p[1] = 0x005f6900;
   p[3] = 0x200;           /* the translucent list's end */
   /* everything set before is forgotten: this is drawn where the model is, unmoved */
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(from);
   CHECK(list_end_count == 1 && list_ends[0] == 9);
   CHECK(ta_count == 0);   /* at distance 0: nearer than the near plane */

   start();
   from = at;
   p = put(8);
   p[0] = N2(0xe);
   p[1] = 0x005f6900;
   p[3] = 0x12345;         /* no interrupt the chip knows */
   run(from);
   CHECK(list_end_count == 0);
   CHECK(elan_reg_read(0x08800074) & 0x10);
}

static void test_texture_dma(void)
{
   uint32_t cmd[8];
   unsigned i;

   start();
   memset(cmd, 0, sizeof(cmd));
   cmd[0] = N2(0xf);
   cmd[1] = 0x80000000u;
   cmd[2] = 0x11123400;
   cmd[3] = 0x4000;
   for (i = 0; i < 8; i++)
      elan_cmd_write(0x09000000 + i * 4, cmd[i]);
   CHECK(dma_count == 1 && dma[0].vram == 0x11123400 && dma[0].size == 0x4000 && !dma[0].from_eram);
   CHECK(!(elan_reg_read(0x08800074) & 3));     /* neither done yet */
   elan_dma_done();
   CHECK(elan_reg_read(0x08800074) & 1);
   elan_reg_write(0x08800074, 1);
   CHECK(!(elan_reg_read(0x08800074) & 1));

   cmd[1] = 0x20001000u;
   for (i = 0; i < 8; i++)
      elan_cmd_write(0x09000000 + i * 4, cmd[i]);
   CHECK(dma_count == 2 && dma[1].from_eram && dma[1].src == 0x1000);

   dma_refuse = true;
   for (i = 0; i < 8; i++)
      elan_cmd_write(0x09000000 + i * 4, cmd[i]);
   CHECK(elan_reg_read(0x08800074) & 0x10);
}

/* Lists that are not what they say: none of these may read or write
 * outside the chip's memory, or go on for ever. */
static void test_bad_lists(void)
{
   uint32_t from;
   uint32_t *p;

   /* more vertices than the list has room for */
   start();
   from = at;
   put_instance(10.0f, 1.0f, 100.0f);
   p = put(8);
   p[0] = N2(7);
   p[6] = 0x00a;
   p[7] = 0x10000000;
   run(from);
   CHECK(ta_count == 0);
   CHECK(elan_reg_read(0x08800074) & 0x10);

   /* a model that is itself */
   start();
   from = at;
   p = put(8);
   p[0] = N2(8);
   p[4] = from;
   p[6] = 32;
   run(from);
   CHECK(elan_reg_read(0x08800074) & 0x10);

   /* a link that runs off the end of the memory */
   start();
   {
      uint32_t cmd[8];
      unsigned i;
      memset(cmd, 0, sizeof(cmd));
      cmd[0] = N2(0xf);
      cmd[1] = ELAN_RAM_SIZE - 64;
      cmd[3] = 0x7fffffff;
      memset(ram + ELAN_RAM_SIZE - 64, 0, 64);
      for (i = 0; i < 8; i++)
         elan_cmd_write(0x09000000 + i * 4, cmd[i]);
   }
   CHECK(ta_count == 2);         /* the two blocks there are, passed on */

   /* a state with addresses from nowhere */
   {
      elan_state_t st;
      start();
      elan_get_state(&st);
      st.gmp = 0xfffffff0u;
      st.instance = ELAN_RAM_SIZE - 4;
      st.light_model = 0x7fffffff;
      st.lights[3] = ELAN_RAM_SIZE;
      elan_set_state(&st);
      from = at;
      put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
      run(from);
   }
}

/* A frame's lists are chained, each one called from the end of the one
 * before: three hundred deep here, with a polygon in each, and the
 * commands after each link still run once it has come back. */
static void test_deep_links(void)
{
   enum { DEPTH = 300 };
   uint32_t lists[DEPTH + 1];
   uint32_t sizes[DEPTH + 1];
   uint32_t from;
   uint32_t *p;
   int i;

   start();
   from = at;
   put_projection(320.0f, 320.0f, -240.0f, 240.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   run(from);

   /* the innermost list first, so that each can point at the next */
   for (i = DEPTH; i >= 0; i--)
   {
      lists[i] = at;
      put_polygons(0x0a, 0x80000000u, 0x20000000u, (uint32_t)i, quad, 4);
      if (i < DEPTH)
      {
         p = put(8);
         p[0] = N2(0xf);
         p[1] = lists[i + 1];
         p[3] = sizes[i + 1];
      }
      /* and one more polygon after the link */
      put_polygons(0x0a, 0x80000000u, 0x20000000u, 0x1000u + (uint32_t)i, quad, 4);
      sizes[i] = at - lists[i];
   }
   ta_count = 0;
   {
      uint32_t cmd[8];
      unsigned k;
      memset(cmd, 0, sizeof(cmd));
      cmd[0] = N2(0xf);
      cmd[1] = lists[0];
      cmd[3] = sizes[0];
      for (k = 0; k < 8; k++)
         elan_cmd_write(0x09000000 + k * 4, cmd[k]);
   }
   CHECK(!(elan_reg_read(0x08800074) & 0x10));
   /* every list's first polygon on the way in... */
   CHECK(ta_count == (DEPTH + 1) * 10);
   if (ta_count == (DEPTH + 1) * 10)
   {
      CHECK(ta[0][3] == 0);
      CHECK(ta[5][3] == 1);
      CHECK(ta[DEPTH * 5][3] == DEPTH);
      /* ...and the innermost one's second straight after its first */
      CHECK(ta[(DEPTH + 1) * 5][3] == 0x1000u + DEPTH);
      /* ...down to the outermost list's, last of all */
      CHECK(ta[(DEPTH + 1) * 10 - 5][3] == 0x1000u);
   }
}

/* A state taken in the middle of a list and put back: what follows is
 * drawn as it would have been. */
static void test_state(void)
{
   uint32_t from, mid;
   uint32_t first[5][8];
   elan_state_t st;

   start();
   from = at;
   put_projection(100.0f, 50.0f, -100.0f, 60.0f);
   put_instance(10.0f, 1.0f, 100.0f);
   put_light_model(1, 0, 0x00202020);
   put_parallel_light(0, 0x404040);
   run(from);
   mid = at;
   put_polygons(0x0a, 0x80000000u, 0x20000000u, 0, quad, 4);
   run(mid);
   CHECK(ta_count == 5);
   memcpy(first, ta, sizeof(first));

   elan_get_state(&st);
   elan_reset();
   ta_count = 0;
   ta_open = -1;
   elan_set_state(&st);
   /* (the list's end did not come: the state is the one before the polygons) */
   run(mid);
   CHECK(ta_count == 5 && !memcmp(first, ta, sizeof(first)));
}

/* Scenes made up at random - matrices, up to four lights of every kind
 * and routing, materials, strips of every length - and a checksum of all
 * that the chip sends for them. The chip works most vertices out four at
 * a time where the processor can (SSE2, 64-bit NEON) and one by one
 * elsewhere, and the two ways have to give the same bits: run.sh builds
 * this both ways and compares what is printed here. Within one build it
 * checks that the same scene comes out the same when its strips start at
 * a different vertex of the four. */
static uint32_t rnd_state;

static uint32_t rnd(void)
{
   rnd_state = rnd_state * 1664525u + 1013904223u;
   return rnd_state >> 8;
}

static float rnd_float(float lo, float hi)
{
   return lo + (hi - lo) * ((float)(rnd() & 0xffff) * (1.0f / 65535.0f));
}

static uint32_t hash_ta(uint32_t h)
{
   unsigned i, k;
   for (i = 0; i < ta_count; i++)
      for (k = 0; k < 8; k++)
         h = (h ^ ta[i][k]) * 16777619u;
   return h;
}

static void put_random_scene(void)
{
   static struct vtx v[64];
   uint32_t *p;
   uint32_t a, b;       /* (two random numbers in one expression would come in an order of the compiler's choosing) */
   unsigned i, n, polys;

   put_projection(320.0f, 320.0f, -240.0f, 240.0f);

   /* a model turned and scaled any way, in front of the camera - now and
    * then close enough to be cut by the near plane */
   p = put(40);
   p[0] = N2(4);
   p[1] = 0xf;
   p[2] = 0x7f;
   for (i = 0; i < 9; i++)
   {
      p[10 + i] = f2u(rnd_float(-1.0f, 1.0f) + ((i % 4) ? 0.0f : 1.0f));
      p[26 + i] = f2u(rnd_float(-1.0f, 1.0f) + ((i % 4) ? 0.0f : 1.0f));
   }
   p[25] = f2u(1.0f);
   p[35] = f2u(rnd_float(-3.0f, 3.0f));
   p[36] = f2u(rnd_float(-3.0f, 3.0f));
   p[37] = f2u((rnd() & 7) ? rnd_float(20.0f, 60.0f) : rnd_float(2.0f, 8.0f));
   p[38] = f2u(1000.0f);

   /* the light model: which lights, to what, and the light all round */
   p = put(8);
   p[0] = N2(4);
   p[1] = 0x10 | (rnd() & 0x260);
   a = rnd() & 0xf;
   b = rnd() & 0xf;
   p[2] = a | b << 16;
   p[3] = rnd() & 0x00ffffff;
   p[4] = (rnd() & 3) ? 0 : (rnd() & 0x003f3f3f);

   for (i = 0; i < 4; i++)
   {
      static const uint8_t routings[] = { 0, 1, 2, 3, 8, 9, 10, 11, 0, 1, 3, 3, 4, 12 };
      unsigned routing;
      a = (rnd() & 15) ? 12 : 14;                 /* to alpha, rarely */
      routing = routings[rnd() % a];
      p = put(8);
      p[1] = i | (rnd() & 0xffffff) << 8;
      if (rnd() & 7)
      {
         /* parallel */
         p[0] = N2(4) | (1u << 20) | (rnd() & 0xf00ff);
         a = rnd() & 0xffffff;
         b = rnd() % 3;
         p[2] = a | routing << 24 | b << 28;
      }
      else
      {
         /* a point or a spot, with a place and a falling off */
         p[0] = N2(4) | (rnd() & 0xf00ff);
         p[1] |= (rnd() % 3) << 5;
         a = rnd() & 0xffffff;
         b = rnd() % 3;
         p[2] = a | routing << 24 | b << 28;
         p[3] = f2u(rnd_float(-9.0f, 9.0f));
         p[4] = f2u(rnd_float(-9.0f, 9.0f));
         p[5] = f2u(rnd_float(-9.0f, 9.0f));
         p[6] = rnd() & 0x3fff3fff;
         p[7] = (rnd() & 1) ? (rnd() & 0x3fff3fff) : 0;
      }
   }

   polys = 1 + rnd() % 6;
   for (n = 0; n < polys; n++)
   {
      unsigned count = 3 + rnd() % 60;
      uint32_t first;

      /* the material: whose colours, how glossy; constant now and then */
      p = put(16);
      p[0] = N2(5);
      p[1] = rnd() & 0xff;
      a = rnd() & 3;
      b = (rnd() & 31) ? 0 : 0x200;
      p[2] = a | b;
      p[3] = rnd() | 0x80000000u;
      p[4] = rnd();

      for (i = 0; i < count; i++)
      {
         v[i].x = rnd_float(-5.0f, 5.0f);
         v[i].y = rnd_float(-5.0f, 5.0f);
         v[i].z = rnd_float(-5.0f, 5.0f);
         v[i].u = rnd_float(0.0f, 4.0f);
         v[i].v = rnd_float(0.0f, 4.0f);
         v[i].marks = i >= 2 ? ((rnd() & 15) ? MARK_STRIP : MARK_FAN) : 0;
         if (i >= 2 && !(rnd() % 9) && count - i > 3)
         {
            /* the strip ends here, another begins */
            v[i].marks |= MARK_END;
            v[i + 1].marks = v[i + 2].marks = 0;
            v[i + 1].x = v[i + 1].y = v[i + 1].z = 0.5f;
            v[i + 1].u = v[i + 1].v = 0.25f;
            v[i + 2] = v[i + 1];
            v[i + 2].x = -0.75f;
            i += 2;
         }
      }
      v[count - 1].marks |= MARK_END;
      first = at + 32;
      put_polygons((rnd() & 1) ? 0xbe : 0x0a, 0x93800000u, 0x200a246du, 0xc81ab700u, v, count);
      /* (the next command comes straight after the last vertex) */
      at = first + count * 24;
      /* (normals of every direction and length) */
      for (i = 0; i < count; i++)
      {
         uint32_t *w = (uint32_t *)(ram + first + i * 24);
         *w = (*w & 0xff000000u) | (rnd() & 0x00ffffffu);
      }
   }
}

static void test_random_scenes(void)
{
   uint32_t h = 2166136261u;
   unsigned scene;

   for (scene = 0; scene < 400; scene++)
   {
      uint32_t from, seed, once;

      rnd_state = seed = 0x1234567u + scene * 7919u;
      start();
      from = at;
      put_random_scene();
      run(from);
      CHECK(!(elan_reg_read(0x08800074) & 0x10));
      once = hash_ta(2166136261u);
      h    = hash_ta(h);

      /* the same again, the chip's memory an odd number of words along:
       * nothing may depend on where a list lies */
      rnd_state = seed;
      start();
      at += 0x24 * 4;
      from = at;
      put_random_scene();
      run(from);
      CHECK(hash_ta(2166136261u) == once);
   }
   printf("random scenes: %08x\n", h);
   /* What they have come to on every build so far - x86-64 and 64-bit
    * ARM, four at a time and one by one, gcc at every optimisation level
    * (built as run.sh builds it, with -ffp-contract=off: a compiler that
    * fuses a multiplication and an addition rounds once where this
    * rounds twice). A change to the lighting changes it, and is then to
    * be looked at and the number put right. */
   CHECK(h == 0x42b247b4u);
}

int main(void)
{
   ram = (uint8_t *)calloc(1, ELAN_RAM_SIZE);
   if (!ram)
      return 2;
   elan_init(ram);

   test_registers();
   test_strip();
   test_lighting();
   test_clipping();
   test_fan();
   test_passthrough();
   test_register_wait();
   test_texture_dma();
   test_bad_lists();
   test_deep_links();
   test_state();
   test_random_scenes();

   free(ram);
   if (failures)
   {
      printf("%d checks failed\n", failures);
      return 1;
   }
   printf("elan test passed\n");
   return 0;
}
