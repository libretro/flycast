/* The NAOMI 2's geometry processor. See elan.h.
 *
 * Coordinates. A model's vertices are multiplied by the instance matrix
 * the game gave, which leaves them in front of the camera with z growing
 * away from it. The screen position is then
 *
 *    sx = fx * x / z + tx      sy = fy * y / z + ty      depth = 1 / z
 *
 * with fx, tx, fy, ty from the projection command. Lights are given in a
 * space turned half a turn about the y axis from that one (x and z
 * negated), and normals are taken there by the instance's second, 3x3
 * matrix; so the position used for lighting is (-x, y, -z).
 */
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "elan.h"

/* ---- the chip's command words ---- */

#define PCW_NAOMI2      0x08000000u
#define PCW_CMD(pcw)    (((pcw) >> 8) & 0xf)
#define PCW_LIST(pcw)   (((pcw) >> 24) & 7)

enum
{
   CMD_NULL          = 0,
   CMD_PROJ_MATRIX   = 3,
   CMD_MATRIX_LIGHT  = 4,
   CMD_GMP           = 5,
   CMD_ICH           = 7,
   CMD_MODEL         = 8,
   CMD_REGISTER_WAIT = 0xe,
   CMD_LINK          = 0xf
};

/* What a polygon list's vertices carry besides a position and a packed
 * normal, and the combinations games send. */
#define VTX_NORMAL   0x004     /* a normal as three floats */
#define VTX_UV       0x008
#define VTX_RGB      0x040     /* a colour for each volume */
#define VTX_BUMP     0x100

/* lighting methods (dmode, smode) */
#define LMODE_SINGLE_SIDED 0
#define LMODE_DOUBLE_SIDED 1

/* where a light's contribution goes (routing) */
#define ROUTING_SPEC_TO_OFFSET 1
#define ROUTING_DIFF_TO_OFFSET 2
#define ROUTING_ALPHA          4
#define ROUTING_SUB            8

/* the tile accelerator's parameter words */
#define TA_PARAM_POLYGON   0x80000000u
#define TA_PARAM_VERTEX    0xe0000000u
#define TA_END_OF_STRIP    0x10000000u
#define TA_OBJ_UV16        0x01
#define TA_OBJ_GOURAUD     0x02
#define TA_OBJ_OFFSET      0x04
#define TA_OBJ_TEXTURE     0x08
#define TA_OBJ_VOLUME      0x40
#define TA_OBJ_SHADOW      0x80
#define TSP_USE_ALPHA      (1u << 20)
#define TSP_IGNORE_TEX_A   (1u << 19)
#define PIXEL_BUMP_MAP     4

#define MAX_DEPTH          4096   /* lists within lists: games go hundreds deep */
#define MAX_COMMANDS       (1u << 22)   /* run for one write to the command port */
#define OUT_BLOCKS         2048   /* 32-byte blocks gathered before they are sent */
#define MODVOL_Z_MIN       0.001f

typedef struct elan_light
{
   float color[3];
   float dir[3];           /* towards the light; a spot's axis */
   float pos[3];
   float dist_a, dist_b;
   float angle_a, angle_b;
   uint8_t parallel;
   uint8_t routing;
   uint8_t dmode;
   uint8_t smode;
   uint8_t dist_mode;
   uint8_t dist_attn;      /* the light fades with distance */
   uint8_t angle_attn;     /* and away from its axis */
   uint8_t diffuse[2];     /* per volume */
   uint8_t specular[2];
} elan_light_t;

/* A vertex once transformed and lit. */
typedef struct elan_vtx
{
   float x, y, z;
   float uv[2][2];         /* [volume] */
   float col[2][2][4];     /* [volume][base, offset][r, g, b, a] */
} elan_vtx_t;

static uint8_t *ram;
static elan_state_t st;

/* ---- what the commands so far have set up ---- */

static float mat[12];           /* instance matrix: 3 rows of x, y, z, translation */
static float nmat[9];           /* normals to light space, 3 rows */
static float near_plane = 0.001f;
static float far_plane  = 100000.0f;
static float env_u, env_v;

static const uint32_t *cur_gmp;
static float gmp_col[2][2][4];  /* [volume][diffuse, specular] */
static float gloss[2];

static bool lights_dirty;
static bool have_light_model;
static elan_light_t lights[ELAN_MAX_LIGHTS];
static int light_count;
static int bump_light;          /* index in lights[], or -1 */
static float ambient[2][2][3];  /* [volume][base, offset] */
static uint8_t ambient_material[2][2];
static bool base_over;
static bool any_specular[2];

/* set by the model command a list is drawn under */
static bool culling_reversed;
static bool open_volume;
static bool shadowed_volume;
static uint32_t model_tsp;
static uint32_t model_user_clip;

static unsigned count_in, count_out;

/* ---- output ---- */

/* (The tile accelerator takes its blocks from addresses that are
 * multiples of 32, as the SH4's store queues are.) */
static uint32_t out_space[OUT_BLOCKS * 8 + 8];
static uint32_t *out;
static unsigned out_count;

static void out_flush(void)
{
   if (out_count)
   {
      elan_host_ta(out, out_count);
      out_count = 0;
   }
}

/* Room for @blocks more. */
static INLINE uint32_t *out_alloc(unsigned blocks)
{
   uint32_t *p;
   if (out_count + blocks > OUT_BLOCKS)
      out_flush();
   p = out + out_count * 8;
   out_count += blocks;
   return p;
}

/* ---- small things ---- */

static INLINE float u32_float(uint32_t v)
{
   union { uint32_t u; float f; } c;
   c.u = v;
   return c.f;
}

static INLINE uint32_t float_u32(float f)
{
   union { uint32_t u; float f; } c;
   c.f = f;
   return c.u;
}

static INLINE float clamp01(float v)
{
   return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static INLINE float sqrt_float(float v)
{
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || defined(_MSC_VER) || defined(__cplusplus)
   return sqrtf(v);
#else
   return (float)sqrt((double)v);   /* the same value: the square root of a float, rounded once */
#endif
}

static void unpack_color(uint32_t argb, float *c)
{
   c[0] = (float)((argb >> 16) & 0xff) * (1.0f / 255.0f);
   c[1] = (float)((argb >> 8) & 0xff) * (1.0f / 255.0f);
   c[2] = (float)(argb & 0xff) * (1.0f / 255.0f);
   c[3] = (float)(argb >> 24) * (1.0f / 255.0f);
}

/* To the nearest of the 256 levels a PowerVR vertex colour has. */
static INLINE uint32_t pack_color(const float *c)
{
   return (uint32_t)(clamp01(c[3]) * 255.0f + 0.5f) << 24
        | (uint32_t)(clamp01(c[0]) * 255.0f + 0.5f) << 16
        | (uint32_t)(clamp01(c[1]) * 255.0f + 0.5f) << 8
        | (uint32_t)(clamp01(c[2]) * 255.0f + 0.5f);
}

static INLINE float dot3(const float *a, const float *b)
{
   return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static INLINE void normalize3(float *v)
{
   float l = dot3(v, v);
   if (l > 0.0f)
   {
      l = 1.0f / sqrt_float(l);
      v[0] *= l;
      v[1] *= l;
      v[2] *= l;
   }
}

/* The address in the chip's memory of something a command points at, if
 * that is where it is. */
static uint32_t ram_address(const void *p)
{
   const uint8_t *b = (const uint8_t *)p;
   if (!ram || b < ram || b >= ram + ELAN_RAM_SIZE)
      return ELAN_NONE;
   return (uint32_t)(b - ram);
}

/* ---- model state ---- */

static void update_matrix(void)
{
   const uint32_t *m;
   int i;

   if (st.instance == ELAN_NONE || st.instance > ELAN_RAM_SIZE - 160)
   {
      static const float identity[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0 };
      memcpy(mat, identity, sizeof(mat));
      nmat[0] = 1; nmat[1] = 0; nmat[2] = 0;
      nmat[3] = 0; nmat[4] = 1; nmat[5] = 0;
      nmat[6] = 0; nmat[7] = 0; nmat[8] = 1;
      env_u = env_v = 0.0f;
      return;
   }
   m = (const uint32_t *)(ram + st.instance);
   /* words 26-37: tm00 tm01 tm02  tm10 tm11 tm12  tm20 tm21 tm22  tm30 tm31 tm32,
    * a vector's x, y and z each times a row of three, plus the last row */
   for (i = 0; i < 3; i++)
   {
      mat[i * 4 + 0] = u32_float(m[26 + i]);
      mat[i * 4 + 1] = u32_float(m[29 + i]);
      mat[i * 4 + 2] = u32_float(m[32 + i]);
      mat[i * 4 + 3] = u32_float(m[35 + i]);
      /* words 10-18: lm00 lm10 lm20  lm01 lm11 lm21  lm02 lm12 lm22 */
      nmat[i * 3 + 0] = u32_float(m[10 + i * 3]);
      nmat[i * 3 + 1] = u32_float(m[11 + i * 3]);
      nmat[i * 3 + 2] = u32_float(m[12 + i * 3]);
   }
   near_plane = u32_float(m[25]);
   far_plane  = u32_float(m[38]);
   env_u      = u32_float(m[9]);
   env_v      = u32_float(m[19]);
}

static void update_gmp(void)
{
   uint32_t select;
   int i;

   memset(gmp_col, 0, sizeof(gmp_col));
   gloss[0] = gloss[1] = 0.0f;
   if (st.gmp == ELAN_NONE || st.gmp > ELAN_RAM_SIZE - 64)
   {
      cur_gmp = NULL;
      return;
   }
   cur_gmp = (const uint32_t *)(ram + st.gmp);
   select = cur_gmp[2];
   for (i = 0; i < 2; i++)
   {
      /* a 3-bit exponent and five bits of fraction */
      uint32_t g = cur_gmp[1] >> (i * 8);
      gloss[i] = (float)pow(2.0, (double)((g >> 5) & 7) - 1.0)
               * (1.0f + (float)(g & 31) * (1.0f / 32.0f));
      if (select & (1u << (i * 4)))
         unpack_color(cur_gmp[3 + i * 2], gmp_col[i][0]);
      if (select & (2u << (i * 4)))
         unpack_color(cur_gmp[4 + i * 2], gmp_col[i][1]);
   }
}

static float light_dir(uint32_t pcw, uint32_t w2, int shift, int pcw_shift)
{
   int v = (int)(signed char)((w2 >> shift) & 0xff) * 16 + (int)((pcw >> pcw_shift) & 0xf);
   return (float)v * (1.0f / 2047.0f);
}

/* Lights and the light model, into the form the lighting loop wants. */
static void update_lights(void)
{
   const uint32_t *model;
   unsigned i;
   int vol;
   int bump_id;

   lights_dirty = false;
   light_count = 0;
   bump_light = -1;
   any_specular[0] = any_specular[1] = false;
   have_light_model = st.light_model != ELAN_NONE && st.light_model <= ELAN_RAM_SIZE - 32;
   if (!have_light_model)
   {
      /* nothing lights the model: it comes out black, unless its colours
       * are marked constant */
      memset(ambient, 0, sizeof(ambient));
      memset(ambient_material, 0, sizeof(ambient_material));
      base_over = false;
      return;
   }
   model = (const uint32_t *)(ram + st.light_model);
   ambient_material[0][0] = (model[1] >> 5) & 1;
   ambient_material[0][1] = (model[1] >> 6) & 1;
   ambient_material[1][0] = (model[1] >> 7) & 1;
   ambient_material[1][1] = (model[1] >> 8) & 1;
   base_over = (model[1] >> 9) & 1;
   bump_id = (int)((model[1] >> 12) & 0xf);
   for (vol = 0; vol < 2; vol++)
   {
      float c[4];
      unpack_color(model[3 + vol * 3], c);
      memcpy(ambient[vol][0], c, sizeof(ambient[vol][0]));
      unpack_color(model[4 + vol * 3], c);
      memcpy(ambient[vol][1], c, sizeof(ambient[vol][1]));
   }

   for (i = 0; i < ELAN_MAX_LIGHTS; i++)
   {
      elan_light_t *l = &lights[light_count];
      const uint32_t *p;
      uint32_t pcw;

      l->diffuse[0]  = (model[2] >> i) & 1;
      l->specular[0] = (model[2] >> (16 + i)) & 1;
      l->diffuse[1]  = (model[5] >> i) & 1;
      l->specular[1] = (model[5] >> (16 + i)) & 1;
      if (!(l->diffuse[0] | l->specular[0] | l->diffuse[1] | l->specular[1]))
         continue;
      if (st.lights[i] == ELAN_NONE || st.lights[i] > ELAN_RAM_SIZE - 32)
         continue;      /* used by the model, never defined */
      p = (const uint32_t *)(ram + st.lights[i]);
      pcw = p[0];

      l->color[0] = (float)(p[1] >> 24) * (1.0f / 255.0f);
      l->color[1] = (float)((p[1] >> 16) & 0xff) * (1.0f / 255.0f);
      l->color[2] = (float)((p[1] >> 8) & 0xff) * (1.0f / 255.0f);
      l->routing = (p[2] >> 24) & 0xf;
      l->dir[0] = -light_dir(pcw, p[2], 16, 16);
      l->dir[1] = -light_dir(pcw, p[2], 8, 4);
      l->dir[2] = -light_dir(pcw, p[2], 0, 0);
      l->dist_attn = l->angle_attn = 0;
      if (pcw & (1u << 20))
      {
         l->parallel = 1;
         l->dmode = (p[2] >> 28) & 3;
         l->smode = LMODE_SINGLE_SIDED;
      }
      else
      {
         l->dmode = (p[1] >> 5) & 7;
         l->smode = (p[2] >> 28) & 3;
         /* A point light with no position and no falling off is a
          * parallel one. */
         l->parallel = p[3] == 0 && p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0;
         if (!l->parallel)
         {
            l->pos[0] = u32_float(p[3]);
            l->pos[1] = u32_float(p[4]);
            l->pos[2] = u32_float(p[5]);
            l->dist_mode = (p[2] >> 31) & 1;
            /* 16 bits each: the top half of a float */
            l->dist_a  = u32_float(p[6] << 16);
            l->dist_b  = u32_float(p[6] & 0xffff0000u);
            l->angle_a = u32_float(p[7] << 16);
            l->angle_b = u32_float(p[7] & 0xffff0000u);
            l->dist_attn  = l->dist_a != 1.0f || l->dist_b != 0.0f;
            l->angle_attn = l->angle_a != 1.0f || l->angle_b != 0.0f;
         }
      }
      if (l->parallel)
         normalize3(l->dir);
      if ((int)i == bump_id)
         bump_light = light_count;
      any_specular[0] |= l->specular[0];
      any_specular[1] |= l->specular[1];
      light_count++;
   }
}

static void update_all(void)
{
   update_matrix();
   update_gmp();
   lights_dirty = true;
}

/* The end of a list: the next one starts with nothing set. */
static void state_reset(void)
{
   unsigned i;
   st.gmp = ELAN_NONE;
   st.instance = ELAN_NONE;
   st.light_model = ELAN_NONE;
   for (i = 0; i < ELAN_MAX_LIGHTS; i++)
      st.lights[i] = ELAN_NONE;
   update_all();
}

/* ---- lighting ---- */

/* @base and @offset: the vertex's colours for volume @vol, replaced by
 * what the lights make of them. @pos and @n in light space. */
static void light_vertex(float *base, float *offset, int vol, const float *pos, const float *n)
{
   float diffuse[3];
   float specular[3];
   float refl[3];
   float diffuse_alpha = 0.0f;
   float specular_alpha = 0.0f;
   int i, c;

   diffuse[0] = diffuse[1] = diffuse[2] = 0.0f;
   specular[0] = specular[1] = specular[2] = 0.0f;
   if (any_specular[vol])
   {
      /* the view direction mirrored in the surface */
      float view[3];
      float d;
      view[0] = pos[0]; view[1] = pos[1]; view[2] = pos[2];
      normalize3(view);
      d = 2.0f * dot3(n, view);
      refl[0] = view[0] - d * n[0];
      refl[1] = view[1] - d * n[1];
      refl[2] = view[2] - d * n[2];
   }
   else
      refl[0] = refl[1] = refl[2] = 0.0f;

   for (i = 0; i < light_count; i++)
   {
      const elan_light_t *l = &lights[i];
      float dir[3];
      float color[3];

      if (!(l->diffuse[vol] | l->specular[vol]))
         continue;
      color[0] = l->color[0]; color[1] = l->color[1]; color[2] = l->color[2];
      if (l->parallel)
      {
         dir[0] = l->dir[0]; dir[1] = l->dir[1]; dir[2] = l->dir[2];
      }
      else
      {
         float dist2, inv;
         dir[0] = l->pos[0] - pos[0];
         dir[1] = l->pos[1] - pos[1];
         dir[2] = l->pos[2] - pos[2];
         dist2 = dot3(dir, dir);
         inv = dist2 > 0.0f ? 1.0f / sqrt_float(dist2) : 0.0f;
         dir[0] *= inv; dir[1] *= inv; dir[2] *= inv;
         if (l->dist_attn)
         {
            float f = clamp01(l->dist_b * (l->dist_mode ? dist2 * inv : inv) + l->dist_a);
            color[0] *= f; color[1] *= f; color[2] *= f;
         }
         if (l->angle_attn)
         {
            float cosine = dot3(dir, l->dir);
            float f = clamp01((1.0f - (cosine > 0.0f ? cosine : 0.0f)) * l->angle_b + l->angle_a);
            color[0] *= f; color[1] *= f; color[2] *= f;
         }
      }
      if (l->diffuse[vol])
      {
         float factor = (l->routing & ROUTING_SUB) ? -2.0f : 2.0f;
         if (l->dmode == LMODE_SINGLE_SIDED)
         {
            float d = dot3(n, dir);
            factor *= d > 0.0f ? d : 0.0f;
         }
         else if (l->dmode == LMODE_DOUBLE_SIDED)
            factor *= (float)fabs((double)dot3(n, dir));

         if (l->routing & ROUTING_ALPHA)
            diffuse_alpha += color[0] * factor;
         else
         {
            float *to = (l->routing & ROUTING_DIFF_TO_OFFSET) ? specular : diffuse;
            for (c = 0; c < 3; c++)
               to[c] += color[c] * factor * base[c];
         }
      }
      if (l->specular[vol])
      {
         float factor = (l->routing & ROUTING_SUB) ? -2.0f : 2.0f;
         if (l->smode <= LMODE_DOUBLE_SIDED)
         {
            float d = dot3(dir, refl);
            if (l->smode == LMODE_DOUBLE_SIDED)
               d = (float)fabs((double)d);
            if (d > 0.0f)
               factor *= clamp01((float)pow((double)d, (double)gloss[vol]));
            else
               factor *= gloss[vol] == 0.0f ? 1.0f : 0.0f;    /* pow(0, gloss) */
         }
         if (l->routing & ROUTING_ALPHA)
            specular_alpha += color[0] * factor;
         else
         {
            float *to = (l->routing & ROUTING_SPEC_TO_OFFSET) ? specular : diffuse;
            for (c = 0; c < 3; c++)
               to[c] += color[c] * factor * offset[c];
         }
      }
   }

   for (c = 0; c < 3; c++)
   {
      diffuse[c]  += ambient_material[vol][0] ? ambient[vol][0][c] * base[c] : ambient[vol][0][c];
      specular[c] += ambient_material[vol][1] ? ambient[vol][1][c] * offset[c] : ambient[vol][1][c];
      base[c] = diffuse[c];
      offset[c] = specular[c];
   }
   base[3] += diffuse_alpha;
   offset[3] += specular_alpha;
   if (base_over)
   {
      /* light the base colour has no room for goes to the offset colour */
      for (c = 0; c < 4; c++)
         if (base[c] > 1.0f)
            offset[c] += base[c] - 1.0f;
   }
   for (c = 0; c < 4; c++)
   {
      base[c] = clamp01(base[c]);
      offset[c] = clamp01(offset[c]);
   }
}

/* The PowerVR's bump map parameters for a vertex, as an offset colour:
 * how far up from the surface the light is and where around it, from
 * the surface's own axes. */
static void bump_vertex(float *offset, const uint32_t *bump, const float *n_model, const float *pos)
{
   float k1 = 1.0f, k2 = 0.0f, k3 = 0.0f, q = 0.0f;

   if (bump_light >= 0)
   {
      const elan_light_t *l = &lights[bump_light];
      float degree = (float)(bump[0] & 0xff) * (1.0f / 255.0f);
      float tangent[3], bitangent[3], normal[3], d[3], dir[3];
      float sin_t;
      double angle;

      tangent[0] = (float)(signed char)(bump[1] & 0xff);
      tangent[1] = (float)(signed char)((bump[1] >> 8) & 0xff);
      tangent[2] = (float)(signed char)((bump[1] >> 16) & 0xff);
      bitangent[0] = (float)(signed char)(bump[2] & 0xff);
      bitangent[1] = (float)(signed char)((bump[2] >> 8) & 0xff);
      bitangent[2] = (float)(signed char)((bump[2] >> 16) & 0xff);
      normal[0] = n_model[0]; normal[1] = n_model[1]; normal[2] = n_model[2];
      normalize3(tangent);
      normalize3(bitangent);
      normalize3(normal);
      if (l->parallel)
      {
         d[0] = l->dir[0]; d[1] = l->dir[1]; d[2] = l->dir[2];
      }
      else
      {
         d[0] = l->pos[0] - pos[0];
         d[1] = l->pos[1] - pos[1];
         d[2] = l->pos[2] - pos[2];
      }
      /* back from light space to the model's */
      dir[0] = d[0] * nmat[0] + d[1] * nmat[3] + d[2] * nmat[6];
      dir[1] = d[0] * nmat[1] + d[1] * nmat[4] + d[2] * nmat[7];
      dir[2] = d[0] * nmat[2] + d[1] * nmat[5] + d[2] * nmat[8];
      normalize3(dir);

      sin_t = clamp01(dot3(dir, normal));
      k1 = 1.0f - degree;
      k2 = degree * sin_t;
      k3 = degree * sqrt_float(1.0f - sin_t * sin_t);
      angle = atan2((double)dot3(dir, bitangent), (double)dot3(dir, tangent));
      if (angle < 0.0)
         angle += 2.0 * 3.14159265358979323846;
      q = (float)(angle * (1.0 / (2.0 * 3.14159265358979323846)));
   }
   offset[0] = k2;
   offset[1] = k3;
   offset[2] = q;
   offset[3] = k1;
}

/* ---- a polygon list ---- */

/* what the list being drawn needs done to its vertices */
static struct
{
   unsigned flags;         /* VTX_* */
   unsigned uv_at;         /* byte offsets in a vertex */
   unsigned rgb_at;
   unsigned bump_at;
   bool texture;
   bool two_volumes;
   bool bump;              /* the texture is a bump map: the offset colour is its parameters */
   bool env[2];
   bool constant[2];
   bool need_normal;
   bool clip;
   float proj[4];
} poly;

/* The strip being made is written straight to where it is sent from.
 * Its last vertex is the one that has to say so, and a strip that the
 * clipping leaves under three vertices of is not sent at all: so the
 * vertices of the strip that are still in the buffer are known. */
static unsigned strip_total;     /* vertices in the strip */
static unsigned strip_held;      /* of them, still in out[] (they are the last there) */
static unsigned strip_blocks;    /* 32-byte blocks to a vertex */

static void strip_end(void)
{
   if (strip_total >= 3)
   {
      out[(out_count - strip_blocks) * 8] |= TA_END_OF_STRIP;
      count_out += strip_total;
   }
   else
      out_count -= strip_held * strip_blocks;    /* (they were never sent: there is room for three) */
   strip_total = 0;
   strip_held = 0;
}

/* A vertex of the strip, to the screen and into the tile accelerator's
 * form. */
static void strip_vertex(const elan_vtx_t *v)
{
   uint32_t *p;
   float iz = 1.0f / v->z;
   float c[4];

   if (out_count + strip_blocks > OUT_BLOCKS)
   {
      /* Full. A strip under way keeps its last vertex here - it may turn
       * out to be the last of the strip - and its first three until
       * there are three; the rest goes. */
      unsigned keep = strip_total >= 3 ? 1 : strip_held;
      unsigned blocks = keep * strip_blocks;
      if (keep > strip_held)
         keep = strip_held, blocks = keep * strip_blocks;
      out_count -= blocks;
      if (out_count)
         elan_host_ta(out, out_count);
      memmove(out, out + out_count * 8, blocks * 32);
      out_count = blocks;
      strip_held = keep;
   }
   p = out + out_count * 8;
   out_count += strip_blocks;
   strip_total++;
   strip_held++;

   p[0] = TA_PARAM_VERTEX;
   p[1] = float_u32(poly.proj[0] * v->x * iz + poly.proj[1]);
   p[2] = float_u32(poly.proj[2] * v->y * iz + poly.proj[3]);
   p[3] = float_u32(iz);
   if (!poly.texture)
   {
      /* no texture: the PowerVR has no use for an offset colour, the
       * light sent to it goes with the base colour */
      c[0] = v->col[0][0][0] + v->col[0][1][0];
      c[1] = v->col[0][0][1] + v->col[0][1][1];
      c[2] = v->col[0][0][2] + v->col[0][1][2];
      c[3] = v->col[0][0][3] + v->col[0][1][3];
      if (!poly.two_volumes)
      {
         p[4] = 0;
         p[5] = 0;
         p[6] = pack_color(c);
         p[7] = 0;
      }
      else
      {
         p[4] = pack_color(c);
         c[0] = v->col[1][0][0] + v->col[1][1][0];
         c[1] = v->col[1][0][1] + v->col[1][1][1];
         c[2] = v->col[1][0][2] + v->col[1][1][2];
         c[3] = v->col[1][0][3] + v->col[1][1][3];
         p[5] = pack_color(c);
         p[6] = 0;
         p[7] = 0;
      }
   }
   else
   {
      p[4] = float_u32(v->uv[0][0]);
      p[5] = float_u32(v->uv[0][1]);
      p[6] = pack_color(v->col[0][0]);
      p[7] = pack_color(v->col[0][1]);
      if (poly.two_volumes)
      {
         p[8]  = float_u32(v->uv[1][0]);
         p[9]  = float_u32(v->uv[1][1]);
         p[10] = pack_color(v->col[1][0]);
         p[11] = pack_color(v->col[1][1]);
         p[12] = p[13] = p[14] = p[15] = 0;
      }
   }
}

/* Clipping a strip against the near plane as it goes by, a vertex at a
 * time (P.-G. Maillot, "Three-Dimensional Homogeneous Clipping of
 * Triangle Strips", Graphics Gems II): p and q are the two vertices
 * before the one that arrives. */
static struct
{
   elan_vtx_t p, q;
   float p_dist, q_dist;
   int count;
   int code;
   bool dupe_next;
} clipper;

static void clip_out(const elan_vtx_t *v)
{
   if (clipper.dupe_next)
      strip_vertex(v);
   clipper.dupe_next = false;
   strip_vertex(v);
}

/* The point of the edge a-b on the plane. */
static void clip_edge(elan_vtx_t *v, const elan_vtx_t *a, float a_dist, const elan_vtx_t *b, float b_dist)
{
   const float *fa = (const float *)a;
   const float *fb = (const float *)b;
   float *fv = (float *)v;
   float wb;
   float wa;
   unsigned i;

   a_dist = (float)fabs((double)a_dist);
   b_dist = (float)fabs((double)b_dist);
   wb = a_dist / (a_dist + b_dist);
   wa = 1.0f - wb;
   for (i = 0; i < sizeof(elan_vtx_t) / sizeof(float); i++)
      fv[i] = fa[i] * wa + fb[i] * wb;
   v->z = near_plane;
}

static void clip_vertex(const elan_vtx_t *r)
{
   float r_dist;
   elan_vtx_t t;

   if (!poly.clip)
   {
      strip_vertex(r);
      return;
   }
   r_dist = r->z - near_plane;
   clipper.code >>= 1;
   clipper.code |= (r_dist < 0.0f) << 2;
   if (clipper.count == 1)
   {
      switch (clipper.code >> 1)
      {
         case 0:     /* q and r inside */
            clip_out(&clipper.q);
            clip_out(r);
            break;
         case 1:     /* q outside, r inside */
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            clip_out(r);
            break;
         case 2:     /* q inside, r outside */
            clip_out(&clipper.q);
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            break;
         default:    /* both outside */
            break;
      }
   }
   else if (clipper.count >= 2)
   {
      switch (clipper.code)
      {
         case 0:     /* all inside */
            clip_out(r);
            break;
         case 1:     /* p outside, q and r inside */
            clip_edge(&t, r, r_dist, &clipper.p, clipper.p_dist);
            clip_out(&t);
            clip_out(&clipper.q);
            clip_out(r);
            break;
         case 2:     /* p inside, q outside, r inside */
            clip_out(r);
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            clip_out(r);
            break;
         case 3:     /* p and q outside, r inside */
            clip_edge(&t, r, r_dist, &clipper.p, clipper.p_dist);
            clip_out(&t);
            clip_out(&t);
            clip_out(&t);  /* one more, to keep the strip's winding */
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            clip_out(r);
            break;
         case 4:     /* p and q inside, r outside */
            clip_edge(&t, r, r_dist, &clipper.p, clipper.p_dist);
            clip_out(&t);
            clip_out(&clipper.q);
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            break;
         case 5:     /* p outside, q inside, r outside */
            clip_edge(&t, &clipper.q, clipper.q_dist, r, r_dist);
            clip_out(&t);
            break;
         case 6:     /* p inside, q and r outside */
            clip_edge(&t, r, r_dist, &clipper.p, clipper.p_dist);
            clip_out(&t);
            clip_out(&t);
            clip_out(&t);
            break;
         default:    /* all outside */
            clipper.dupe_next = !clipper.dupe_next;
            break;
      }
   }
   clipper.p = clipper.q;
   clipper.p_dist = clipper.q_dist;
   clipper.q = *r;
   clipper.q_dist = r_dist;
   clipper.count++;
}

static void clip_start(void)
{
   strip_end();
   if (out_count + 3 * strip_blocks > OUT_BLOCKS)
      out_flush();
   clipper.count = 0;
   clipper.code = 0;
   clipper.dupe_next = false;
}

/* Transforms and lights the vertex at @src. */
static void make_vertex(elan_vtx_t *v, const uint8_t *src)
{
   const uint32_t *w = (const uint32_t *)src;
   float x = u32_float(w[1]);
   float y = u32_float(w[2]);
   float z = u32_float(w[3]);
   float n_model[3];
   float n[3];
   float pos[3];
   int vol;
   int vols = poly.two_volumes ? 2 : 1;

   v->x = mat[0] * x + mat[1] * y + mat[2] * z + mat[3];
   v->y = mat[4] * x + mat[5] * y + mat[6] * z + mat[7];
   v->z = mat[8] * x + mat[9] * y + mat[10] * z + mat[11];
   pos[0] = -v->x;
   pos[1] = v->y;
   pos[2] = -v->z;

   n[0] = n[1] = n[2] = 0.0f;
   n_model[0] = n_model[1] = n_model[2] = 0.0f;
   if (poly.need_normal)
   {
      if (poly.flags & VTX_NORMAL)
      {
         n_model[0] = u32_float(w[4]);
         n_model[1] = u32_float(w[5]);
         n_model[2] = u32_float(w[6]);
      }
      else
      {
         n_model[0] = (float)(signed char)(w[0] & 0xff) * (1.0f / 127.0f);
         n_model[1] = (float)(signed char)((w[0] >> 8) & 0xff) * (1.0f / 127.0f);
         n_model[2] = (float)(signed char)((w[0] >> 16) & 0xff) * (1.0f / 127.0f);
      }
      n[0] = nmat[0] * n_model[0] + nmat[1] * n_model[1] + nmat[2] * n_model[2];
      n[1] = nmat[3] * n_model[0] + nmat[4] * n_model[1] + nmat[5] * n_model[2];
      n[2] = nmat[6] * n_model[0] + nmat[7] * n_model[1] + nmat[8] * n_model[2];
      normalize3(n);
   }

   for (vol = 0; vol < vols; vol++)
   {
      float *base = v->col[vol][0];
      float *offset = v->col[vol][1];

      /* texture coordinates */
      if (poly.env[0] | poly.env[1])
      {
         v->uv[vol][0] = env_u;
         v->uv[vol][1] = env_v;
         if (poly.env[vol])
         {
            /* the texture is looked up by where the surface faces */
            v->uv[vol][0] = clamp01(env_u + n[0] * 0.5f + 0.5f);
            v->uv[vol][1] = clamp01(env_v + n[1] * 0.5f + 0.5f);
         }
      }
      else if (poly.flags & VTX_UV)
      {
         v->uv[vol][0] = u32_float(*(const uint32_t *)(src + poly.uv_at));
         v->uv[vol][1] = u32_float(*(const uint32_t *)(src + poly.uv_at + 4));
      }
      else
         v->uv[vol][0] = v->uv[vol][1] = 0.0f;

      /* the colours the lights work on: the vertex's own, or the model's */
      if (poly.flags & VTX_RGB)
         unpack_color(*(const uint32_t *)(src + poly.rgb_at + vol * 4), base);
      else
         base[0] = base[1] = base[2] = base[3] = 1.0f;
      offset[0] = offset[1] = offset[2] = offset[3] = 0.0f;
      if (cur_gmp)
      {
         if (cur_gmp[2] & (1u << (vol * 4)))
            memcpy(base, gmp_col[vol][0], 4 * sizeof(float));
         if (cur_gmp[2] & (2u << (vol * 4)))
            memcpy(offset, gmp_col[vol][1], 4 * sizeof(float));
      }

      if (poly.bump)
      {
         /* a bump map takes its light from the offset colour's
          * parameters; the base colour is left as it is */
         if (vol == 0)
            bump_vertex(offset, (const uint32_t *)(src + poly.bump_at), n_model, pos);
      }
      else if (!poly.constant[vol])
         light_vertex(base, offset, vol, pos, n);
   }
}

static unsigned vertex_size(unsigned flags)
{
   switch (flags)
   {
      case 0x002: return 16;     /* position */
      case 0x00a: return 24;     /* + texture coordinates */
      case 0x00e: return 40;     /* + a float normal */
      case 0x042: return 24;     /* position, colours */
      case 0x04a: return 32;     /* + texture coordinates */
      case 0x10a: return 40;     /* texture coordinates, bump map axes */
   }
   return 0;
}

/* A modifier volume: only where it is matters. */
static void modvol_triangle(float (*v)[3])
{
   uint32_t *p = out_alloc(2);
   int i;

   p[0] = TA_PARAM_VERTEX | TA_END_OF_STRIP;
   for (i = 0; i < 3; i++)
   {
      /* what is behind the eye is laid on a plane just in front of it */
      float z = v[i][2] > MODVOL_Z_MIN ? v[i][2] : MODVOL_Z_MIN;
      float iz = 1.0f / z;
      p[1 + i * 3] = float_u32(st.proj[0] * v[i][0] * iz + st.proj[1]);
      p[2 + i * 3] = float_u32(st.proj[2] * v[i][1] * iz + st.proj[3]);
      p[3 + i * 3] = float_u32(iz);
   }
   p[10] = p[11] = p[12] = p[13] = p[14] = p[15] = 0;
}

static void modvol_edge(float *v, const float *a, float a_dist, const float *b, float b_dist)
{
   float s;
   int i;
   a_dist = (float)fabs((double)a_dist);
   b_dist = (float)fabs((double)b_dist);
   s = 1.0f / (a_dist + b_dist);
   for (i = 0; i < 3; i++)
      v[i] = (a[i] * b_dist + b[i] * a_dist) * s;
}

/* A triangle of a volume that crosses the near plane is cut along it
 * into three, so that each piece is wholly on one side (M. McGuire,
 * "Efficient Triangle and Quadrilateral Clipping within Shaders", 2011). */
static void modvol_clip(float (*v)[3])
{
   float d[3];
   float t[3][3];
   float v3[3], v4[3];
   int in;
   int lone;
   int a, b, c;

   d[0] = v[0][2] - near_plane;
   d[1] = v[1][2] - near_plane;
   d[2] = v[2][2] - near_plane;
   in = (d[0] >= 0.0f) + (d[1] >= 0.0f) + (d[2] >= 0.0f);
   if (in == 0 || in == 3)
   {
      modvol_triangle(v);
      return;
   }
   /* the vertex that is alone on its side */
   if ((d[0] >= 0.0f) == (d[2] >= 0.0f))
      lone = 1;
   else if ((d[0] >= 0.0f) == (d[1] >= 0.0f))
      lone = 2;
   else
      lone = 0;
   a = lone;
   b = (lone + 1) % 3;
   c = (lone + 2) % 3;
   modvol_edge(v3, v[a], d[a], v[b], d[b]);
   modvol_edge(v4, v[a], d[a], v[c], d[c]);

   memcpy(t[0], v[a], sizeof(t[0])); memcpy(t[1], v3, sizeof(v3)); memcpy(t[2], v4, sizeof(v4));
   modvol_triangle(t);
   memcpy(t[0], v3, sizeof(v3)); memcpy(t[1], v[b], sizeof(t[1])); memcpy(t[2], v4, sizeof(v4));
   modvol_triangle(t);
   memcpy(t[0], v[c], sizeof(t[0])); memcpy(t[1], v4, sizeof(v4)); memcpy(t[2], v[b], sizeof(t[2]));
   modvol_triangle(t);
}

static void modvol_list(const uint32_t *ich, const uint8_t *vtx, unsigned size, int list)
{
   uint32_t *p = out_alloc(1);
   uint32_t isp = ich[1];
   float v0[3], v1[3];
   unsigned count = ich[7];
   unsigned start = 0;
   unsigned i;

   /* the volume instruction is two bits here; a closed volume is not culled */
   isp &= 0x7fffffffu;
   if (!open_volume)
      isp &= ~0x18000000u;
   p[0] = TA_PARAM_POLYGON | ((uint32_t)list << 24) | (model_user_clip << 16)
        | (ich[0] & TA_OBJ_VOLUME);
   p[1] = isp;
   p[2] = p[3] = p[4] = p[5] = p[6] = p[7] = 0;

   v0[0] = v0[1] = v0[2] = 0.0f;
   v1[0] = v1[1] = v1[2] = 0.0f;
   for (i = 0; i < count; i++, vtx += size)
   {
      const uint32_t *w = (const uint32_t *)vtx;
      float x = u32_float(w[1]);
      float y = u32_float(w[2]);
      float z = u32_float(w[3]);
      float v[3];
      unsigned n = i - start;

      v[0] = mat[0] * x + mat[1] * y + mat[2] * z + mat[3];
      v[1] = mat[4] * x + mat[5] * y + mat[6] * z + mat[7];
      v[2] = mat[8] * x + mat[9] * y + mat[10] * z + mat[11];
      if (n >= 2)
      {
         float tri[3][3];
         memcpy(tri[n & 1], v0, sizeof(v0));
         memcpy(tri[(n & 1) ^ 1], v1, sizeof(v1));
         memcpy(tri[2], v, sizeof(v));
         modvol_clip(tri);
         count_out += 3;
      }
      if (w[0] & 0x80000000u)
         start = i + 1;
      memcpy(v0, v1, sizeof(v0));
      memcpy(v1, v, sizeof(v));
   }
   count_in += count;
}

static void polygon_list(const uint32_t *ich, const uint8_t *vtx, unsigned size)
{
   uint32_t pcw = ich[0];
   uint32_t isp = ich[1];
   uint32_t tsp0 = ich[2];
   uint32_t tcw0 = ich[3];
   uint32_t tsp1 = ich[4];
   uint32_t tcw1 = ich[5];
   unsigned flags = ich[6];
   unsigned count = ich[7];
   unsigned obj;
   unsigned i;
   int list;
   float z_min, z_max;
   const uint8_t *v;
   bool shadow;
   bool proj_flip;
   bool strip_start;
   elan_vtx_t center;
   elan_vtx_t buf[2];
   const elan_vtx_t *last;
   uint32_t *p;

   list = elan_host_ta_list();
   if (list < 0)
      list = (int)PCW_LIST(pcw);
   if (list & 1)
   {
      modvol_list(ich, vtx, size, list);
      return;
   }

   /* Nothing of it between the near and the far plane: gone. Some of it
    * nearer than the near plane: its strips are clipped. */
   z_min = 3.0e38f;
   z_max = -3.0e38f;
   for (i = 0, v = vtx; i < count; i++, v += size)
   {
      const uint32_t *w = (const uint32_t *)v;
      float z = mat[8] * u32_float(w[1]) + mat[9] * u32_float(w[2]) + mat[10] * u32_float(w[3]) + mat[11];
      if (z < z_min)
         z_min = z;
      if (z > z_max)
         z_max = z;
   }
   count_in += count;
   if (!(z_max >= near_plane) || !(z_min <= far_plane))
      return;     /* (or the matrix is not numbers) */

   if (lights_dirty)
      update_lights();

   poly.flags = flags;
   poly.uv_at = (flags & VTX_NORMAL) ? 32 : 16;
   poly.rgb_at = 16 + ((flags & VTX_UV) ? 8 : 0);
   poly.bump_at = 24;
   poly.clip = z_min < near_plane;
   memcpy(poly.proj, st.proj, sizeof(poly.proj));

   obj = pcw & (TA_OBJ_GOURAUD | TA_OBJ_OFFSET);
   if ((flags & VTX_UV) && (pcw & TA_OBJ_TEXTURE))
      obj |= TA_OBJ_TEXTURE;
   if (flags & VTX_BUMP)
      obj |= TA_OBJ_TEXTURE | TA_OBJ_OFFSET;

   poly.env[0] = poly.env[1] = false;
   poly.constant[0] = poly.constant[1] = false;
   if (cur_gmp)
   {
      uint32_t select = cur_gmp[2];
      poly.constant[0] = (select >> 9) & 1;
      poly.constant[1] = (select >> 10) & 1;
      /* Environment mapping: the texture is the surroundings, mirrored */
      if (select & (1u << 11))
      {
         obj = (obj | TA_OBJ_TEXTURE) & ~TA_OBJ_OFFSET;
         tsp0 = (tsp0 | TSP_USE_ALPHA) & ~TSP_IGNORE_TEX_A;
         poly.env[0] = true;
      }
      if (select & (1u << 12))
      {
         obj = (obj | TA_OBJ_TEXTURE) & ~TA_OBJ_OFFSET;
         tsp1 = (tsp1 | TSP_USE_ALPHA) & ~TSP_IGNORE_TEX_A;
         poly.env[1] = true;
      }
   }
   tsp0 ^= model_tsp;
   tsp1 ^= model_tsp;

   /* a mirrored projection turns every polygon over */
   proj_flip = ((-st.proj[0] < 0.0f) == (st.proj[2] < 0.0f));
   isp ^= (uint32_t)(culling_reversed ^ proj_flip) << 27;
   shadow = ((pcw & TA_OBJ_SHADOW) != 0) ^ shadowed_volume;
   poly.two_volumes = shadow && (pcw & TA_OBJ_VOLUME);
   poly.texture = (obj & TA_OBJ_TEXTURE) != 0;
   poly.bump = poly.texture && (flags & VTX_BUMP) && ((tcw0 >> 27) & 7) == PIXEL_BUMP_MAP;
   if (!poly.two_volumes)
      poly.constant[1] = true;
   poly.need_normal = !poly.constant[0] || !poly.constant[1] || poly.env[0] || poly.env[1] || poly.bump;
   if (shadow)
      obj |= TA_OBJ_SHADOW;
   if (poly.two_volumes)
      obj |= TA_OBJ_VOLUME;
   strip_blocks = (poly.two_volumes && poly.texture) ? 2 : 1;

   /* the polygon's parameters, as a game would send them */
   p = out_alloc(1);
   p[0] = TA_PARAM_POLYGON | ((uint32_t)list << 24) | (model_user_clip << 16) | obj;
   p[1] = (isp & 0xfc000000u)
        | ((obj & TA_OBJ_TEXTURE) ? 1u << 25 : 0)
        | ((obj & TA_OBJ_OFFSET) ? 1u << 24 : 0)
        | ((obj & TA_OBJ_GOURAUD) ? 1u << 23 : 0)
        | (isp & 0x00300000u);
   p[2] = tsp0;
   p[3] = tcw0;
   if (poly.two_volumes)
   {
      p[4] = tsp1;
      p[5] = tcw1;
   }
   else
      p[4] = p[5] = 0;
   p[6] = p[7] = 0;

   memset(buf, 0, sizeof(buf));
   memset(&center, 0, sizeof(center));
   strip_start = true;
   last = NULL;
   strip_total = 0;
   strip_held = 0;
   for (i = 0, v = vtx; i < count; i++, v += size)
   {
      uint32_t header = *(const uint32_t *)v;
      elan_vtx_t *cur = &buf[i & 1];

      make_vertex(cur, v);
      if (strip_start)
      {
         clip_start();
         center = *cur;       /* should it turn out to be a fan */
         strip_start = false;
      }
      else if ((header & 0x60000000u) == 0x40000000u && last)
      {
         /* a fan: a triangle of its centre, the last vertex and this one */
         clip_start();
         clip_vertex(&center);
         clip_vertex(last);
      }
      clip_vertex(cur);
      last = cur;
      if (header & 0x80000000u)
      {
         strip_start = true;
         last = NULL;
      }
   }
   strip_end();
}

/* ---- the display list ---- */

/* A display list is lists calling lists: a link runs another list and
 * comes back, a model does the same with its own settings, and games
 * chain a frame's lists one from the end of the other, hundreds deep.
 * Where each one is to be gone on with is kept here, not on the
 * processor's stack. */
typedef struct elan_call
{
   const uint8_t *data;
   const uint8_t *buffer;
   size_t buffer_size;
   int size;
   bool model;
} elan_call_t;

static elan_call_t calls[MAX_DEPTH];

/* Runs the @size bytes of commands at @data, which is in @buffer.
 * False if the chip gave up on them. */
static bool execute(const uint8_t *data, int size, const uint8_t *buffer, size_t buffer_size)
{
   unsigned depth = 0;
   unsigned budget = MAX_COMMANDS;

   if (data < buffer || (size_t)(data - buffer) > buffer_size)
      return false;
   if (size < 0 || (size_t)size > buffer_size - (size_t)(data - buffer))
      size = (int)(buffer_size - (size_t)(data - buffer));

   for (;;)
   {
      const uint32_t *w;
      uint32_t pcw;
      int used = 32;
      size_t room;
      /* a list to run before this one is gone on with */
      const uint8_t *call = NULL;
      int call_size = 0;
      bool call_model = false;

      while (size < 32)
      {
         /* the end of this list: back to the one that called it */
         if (depth == 0)
            return true;
         depth--;
         if (calls[depth].model)
         {
            culling_reversed = false;
            open_volume = false;
            shadowed_volume = false;
            model_user_clip = 0;
            model_tsp = 0;
         }
         data = calls[depth].data;
         size = calls[depth].size;
         buffer = calls[depth].buffer;
         buffer_size = calls[depth].buffer_size;
      }
      if (--budget == 0)
         return false;     /* a list that goes round in circles */

      w = (const uint32_t *)data;
      pcw = w[0];
      /* A command can be longer than what is left of the list it is in
       * - the lengths games give are not that careful - but not than the
       * memory it is in. */
      room = buffer_size - (size_t)(data - buffer);

      if (pcw & PCW_NAOMI2)
      {
         switch (PCW_CMD(pcw))
         {
            case CMD_NULL:
               break;

            case CMD_PROJ_MATRIX:
               st.proj[0] = u32_float(w[2]);
               st.proj[1] = u32_float(w[3]);
               st.proj[2] = u32_float(w[4]);
               st.proj[3] = u32_float(w[5]);
               break;

            case CMD_MATRIX_LIGHT:
               if (w[1] == 0xf && w[2] == 0x7f)
               {
                  /* a model's place in the scene */
                  used = 160;
                  st.instance = room >= (size_t)used ? ram_address(data) : ELAN_NONE;
                  update_matrix();
               }
               else if (w[1] & 0x10)
               {
                  st.light_model = ram_address(data);
                  lights_dirty = true;
               }
               else
               {
                  st.lights[w[1] & 0xf] = ram_address(data);
                  lights_dirty = true;
               }
               break;

            case CMD_MODEL:
               if (!ram)
                  return false;
               culling_reversed = !((w[1] >> 27) & 1);
               open_volume = (w[1] >> 28) & 1;
               shadowed_volume = (pcw >> 7) & 1;
               model_user_clip = (pcw >> 16) & 3;
               model_tsp = w[2];
               call = ram + (w[4] & ELAN_RAM_MASK & ~7u);
               call_size = (int)(w[6] & 0x7fffffffu);
               call_model = true;
               break;

            case CMD_REGISTER_WAIT:
               /* wait for a list to be over: it is, as soon as its
                * end has been sent */
               if (w[1] != 0xffffffffu && w[3] != 0)
               {
                  unsigned bit;
                  switch (w[3])
                  {
                     case 0x80:     bit = 7; break;    /* opaque */
                     case 0x100:    bit = 8; break;    /* opaque modifier volumes */
                     case 0x200:    bit = 9; break;    /* translucent */
                     case 0x400:    bit = 10; break;   /* translucent modifier volumes */
                     case 0x200000: bit = 21; break;   /* punch-through */
                     default:
                        return false;
                  }
                  out_flush();
                  elan_host_list_end(bit);
                  state_reset();
               }
               break;

            case CMD_LINK:
               if (w[1] & 0xa0000000u)
               {
                  /* a texture, to video memory */
                  out_flush();
                  if (!elan_host_texture_dma(w[2], w[1] & ELAN_RAM_MASK, w[3], (w[1] & 0x80000000u) == 0))
                     return false;
                  st.dma_busy = 1;
               }
               else
               {
                  if (!ram)
                     return false;
                  call = ram + (w[1] & ELAN_RAM_MASK & ~3u);
                  call_size = (int)(w[3] & 0x7fffffffu);
               }
               break;

            case CMD_GMP:
               used = 64;
               st.gmp = room >= (size_t)used ? ram_address(data) : ELAN_NONE;
               update_gmp();
               break;

            case CMD_ICH:
            {
               unsigned vsize = vertex_size(w[6]);
               unsigned count = w[7];
               if (vsize)
               {
                  if (count > (room - 32) / vsize)
                     return false;     /* runs past the end of the memory */
                  polygon_list(w, data + 32, vsize);
                  used = 32 + (int)(vsize * count);
               }
               break;
            }

            default:
               return false;
         }
      }
      else
      {
         /* Not for the chip: tile accelerator parameters, passed on. */
         int blocks = 0;
         out_flush();
         do
         {
            elan_host_ta((const uint32_t *)(data + blocks * 32), 1);
            blocks++;
         }
         while (blocks * 32 + 32 <= size
               && (elan_host_ta_half()
                  || !(*(const uint32_t *)(data + blocks * 32) & PCW_NAOMI2)));
         used = blocks * 32;
      }
      data += used;
      size -= used;

      if (call)
      {
         size_t at = (size_t)(call - ram);
         if (depth == MAX_DEPTH)
            return false;
         calls[depth].data = data;
         calls[depth].size = size;
         calls[depth].buffer = buffer;
         calls[depth].buffer_size = buffer_size;
         calls[depth].model = call_model;
         depth++;
         data = call;
         buffer = ram;
         buffer_size = ELAN_RAM_SIZE;
         size = (size_t)call_size > ELAN_RAM_SIZE - at ? (int)(ELAN_RAM_SIZE - at) : call_size;
      }
   }
}

/* ---- registers ---- */

uint32_t elan_reg_read(uint32_t addr)
{
   switch (addr & 0xff)
   {
      case 0x00:  /* what chip this is */
         return 0xe1ad0000u;
      case 0x04:  /* revision */
         return 0x10;
      case 0x0c:  /* commands waiting: games go on when it is below 2 or 3 */
         return 1;
      case 0x10:
         return st.reg10;
      case 0x14:  /* memory refresh */
         return 0x2029;
      case 0x1c:  /* memory configuration */
         return 0x87320961u;
      case 0x30:  /* which tiles go to which PowerVR */
         return st.reg30;
      case 0x74:  /* interrupts: 1 transfer done, 2 command done, 0x10 error */
         return st.reg74;
   }
   return 0;
}

void elan_reg_write(uint32_t addr, uint32_t data)
{
   switch (addr & 0xff)
   {
      case 0x08:  /* reset */
         if (data == 0)
            st.reg74 = 0;
         break;
      case 0x10:
         st.reg10 = data;
         break;
      case 0x30:
         st.reg30 = data;
         break;
      case 0x74:
         st.reg74 &= ~data;
         break;
   }
}

void elan_cmd_write(uint32_t addr, uint32_t data)
{
   unsigned i = (addr & 31) >> 2;

   st.cmd[i] = data;
   if (i != 7)
      return;
   if (execute((const uint8_t *)st.cmd, sizeof(st.cmd), (const uint8_t *)st.cmd, sizeof(st.cmd)))
   {
      if (!st.dma_busy)
         st.reg74 |= 2;
   }
   else
   {
      st.reg74 |= 0x12;
      /* (whatever model it stopped in is not the next command's) */
      culling_reversed = false;
      open_volume = false;
      shadowed_volume = false;
      model_user_clip = 0;
      model_tsp = 0;
   }
   out_flush();
}

void elan_dma_done(void)
{
   st.dma_busy = 0;
   st.reg74 |= 1;
}

void elan_init(uint8_t *memory)
{
   ram = memory;
   out = (uint32_t *)(((size_t)out_space + 31) & ~(size_t)31);
}

void elan_reset(void)
{
   memset(&st, 0, sizeof(st));
   st.reg30 = 0x31;
   st.proj[0] = 579.411194f;
   st.proj[1] = -320.0f;
   st.proj[2] = -579.411194f;
   st.proj[3] = -240.0f;
   near_plane = 0.001f;
   far_plane = 100000.0f;
   culling_reversed = false;
   open_volume = false;
   shadowed_volume = false;
   model_tsp = 0;
   model_user_clip = 0;
   out_count = 0;
   state_reset();
}

void elan_get_state(elan_state_t *state)
{
   *state = st;
}

void elan_set_state(const elan_state_t *state)
{
   unsigned i;
   st = *state;
   /* (a state is not to be trusted: what the addresses are checked
    * against is where they are used) */
   if (st.gmp != ELAN_NONE)
      st.gmp &= ~3u;
   if (st.instance != ELAN_NONE)
      st.instance &= ~3u;
   if (st.light_model != ELAN_NONE)
      st.light_model &= ~3u;
   for (i = 0; i < ELAN_MAX_LIGHTS; i++)
      if (st.lights[i] != ELAN_NONE)
         st.lights[i] &= ~3u;
   update_all();
}

void elan_counters(unsigned *in, unsigned *out_vertices)
{
   *in = count_in;
   *out_vertices = count_out;
   count_in = count_out = 0;
}
