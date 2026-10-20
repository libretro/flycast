/* The compute renderer's reference on the CPU (core/rend/tile/pvr_compute.h):
 * a frame's words drawn one sample at a time, every primitive of each list
 * looked at in order - no tiles, no bins, no lanes - and every translucent
 * fragment of a sample sorted at once, with no limit, where the kernels
 * keep K at a time and peel the rest. The rules are the kernels', in the
 * same operations in the same order, for the kernels' test mode (depth is
 * 1/w, textures pvrc_test_texel()) and built without fused operations
 * (-ffp-contract=off), as the kernels are with PVRC_EXACT; so a frame
 * comes out the same to the bit. tools/rend/tile_test compares the two.
 *
 * C89. */
#include <stdlib.h>
#include <string.h>
#include "pvr_compute.h"
#include "tile_ref.h"

static const unsigned *F;

static float ff(unsigned i)
{
   float f;
   memcpy(&f, &F[i], 4);
   return f;
}

static unsigned fu(float f)
{
   unsigned u;
   memcpy(&u, &f, 4);
   return u;
}

/* The kernels' 64-bit integers, for the edges (C89 has no long long: this
 * is for the machines the test runs on, whose long is 64 bits) */
typedef long ref_s64;
typedef char ref_s64_is_64_bits[sizeof(ref_s64) >= 8 ? 1 : -1];

/* ---- Setup, as the setup kernel does it ---- */

typedef struct
{
   int x[3], y[3];
   float q[3];
   unsigned vi[3];
   unsigned src, extra;
   unsigned shift;	/* x and y divided by 2^shift (pvrc_xy_shift()) */
   unsigned neg;	/* a bit for each vertex behind the eye (PVRC_RX_NEG_AT) */
   int bx0, by0, bx1, by1;
   int live;
} Rec;

static Rec *recs;
static unsigned nrecs;
static unsigned ref_dropped;
static unsigned ref_clipped;

/* (the kernel's clamp of a pixel far out to what an int has) */
static int ref_int(ref_s64 v)
{
   return v < -0x7fffffffL ? -0x7fffffff : v > 0x7fffffffL ? 0x7fffffff : (int)v;
}

static void setup(unsigned prim, Rec *r)
{
   unsigned osrc = F[PVRC_F_O_SRC], lo = 0, hi = F[PVRC_F_SOURCES], i, src, index, k, isp, rect0, rect1, cull;
   int modvol, culledOwn, front, empty, bad = 0, behind;
   float vx[3], vy[3], vz[3];
   ref_s64 cross;
   float px[3], py[3], most = 0.0f, down;
   unsigned shift;
   while (hi - lo > 1)
   {
      unsigned mid = (lo + hi) >> 1;
      if (F[osrc + mid * PVRC_SRC_WORDS] <= prim)
         lo = mid;
      else
         hi = mid;
   }
   i = prim - F[osrc + lo * PVRC_SRC_WORDS];
   src = F[osrc + lo * PVRC_SRC_WORDS + 1];
   modvol = (src & PVRC_SRC_MODVOL) != 0;
   index = src & ~PVRC_SRC_MODVOL;
   memset(r, 0, sizeof(*r));
   if (modvol)
   {
      unsigned mp = F[PVRC_F_O_MVP] + index * PVRC_MVP_WORDS;
      unsigned t = F[PVRC_F_O_MOD] + (F[mp + PVRC_M_FIRST] + i) * 9;
      for (k = 0; k < 3; k++)
      {
         vx[k] = ff(t + k * 3);
         vy[k] = ff(t + k * 3 + 1);
         vz[k] = ff(t + k * 3 + 2);
      }
      isp = F[mp + PVRC_M_ISP];
      rect0 = F[PVRC_F_SCISSOR] | (F[PVRC_F_SCISSOR + 1] << 16);
      rect1 = F[PVRC_F_SCISSOR + 2] | (F[PVRC_F_SCISSOR + 3] << 16);
   }
   else
   {
      unsigned pp = F[PVRC_F_O_POLY] + index * PVRC_POLY_WORDS;
      unsigned at = F[PVRC_F_O_IDX] + F[osrc + lo * PVRC_SRC_WORDS + 2] + i;
      unsigned odd = i & 1, ov = F[PVRC_F_O_VTX];
      r->vi[0] = F[at];
      r->vi[1] = F[at + 1 + odd];
      r->vi[2] = F[at + 2 - odd];
      r->extra = r->vi[0];
      for (k = 0; k < 3; k++)
      {
         vx[k] = ff(ov + r->vi[k] * PVRC_VTX_WORDS);
         vy[k] = ff(ov + r->vi[k] * PVRC_VTX_WORDS + 1);
         vz[k] = ff(ov + r->vi[k] * PVRC_VTX_WORDS + 2);
      }
      isp = F[pp + PVRC_Q_ISP];
      rect0 = F[pp + PVRC_Q_RECT];
      rect1 = F[pp + PVRC_Q_RECT + 1];
   }
   for (k = 0; k < 3; k++)
   {
      px[k] = ff(PVRC_F_XFORM) * vx[k] + ff(PVRC_F_XFORM + 2);
      py[k] = ff(PVRC_F_XFORM + 1) * vy[k] + ff(PVRC_F_XFORM + 3);
      if (!(fabs(px[k]) <= most))
         most = (float)fabs(px[k]);
      if (!(fabs(py[k]) <= most))
         most = (float)fabs(py[k]);
   }
   shift = pvrc_xy_shift(most);
   down = 1.0f / (float)(1u << shift);
   for (k = 0; k < 3; k++)
   {
      r->x[k] = pvrc_fixed(px[k] * down);
      r->y[k] = pvrc_fixed(py[k] * down);
      r->q[k] = ff(PVRC_F_XFORM + 4) * vz[k];
      if (!(fabs(r->q[k]) > 0.0f) || fabs(r->q[k]) > 3.4e37f || px[k] != px[k] || py[k] != py[k])
         bad = 1;
   }
   r->shift = shift;
   cross = (ref_s64)(r->x[1] - r->x[0]) * (ref_s64)(r->y[2] - r->y[0]) - (ref_s64)(r->x[2] - r->x[0]) * (ref_s64)(r->y[1] - r->y[0]);
   behind = (r->q[0] < 0.0f) + (r->q[1] < 0.0f) + (r->q[2] < 0.0f);
   front = (cross < 0) != ((behind & 1) != 0);
   cull = (isp >> 27) & 3;
   culledOwn = (cull == 3 && !front) || (cull == 2 && front);
   empty = bad || cross == 0 || behind == 3;
   if (modvol)
   {
      unsigned mode = isp >> 29;
      r->extra = (front ? PVRC_RX_FRONT : PVRC_RX_BACK) | (culledOwn ? PVRC_RX_CULLED : 0)
            | (pvr_modvol_or_mode((isp >> 26) & 1, mode) ? PVRC_RX_OR : 0) | (mode << PVRC_RX_MODE_SHIFT);
   }
   else if (culledOwn)
      empty = 1;
   if (bad)
      ref_dropped++;
   if (cross < 0)
   {
      int t;
      float f;
      unsigned u;
      t = r->x[1]; r->x[1] = r->x[2]; r->x[2] = t;
      t = r->y[1]; r->y[1] = r->y[2]; r->y[2] = t;
      f = r->q[1]; r->q[1] = r->q[2]; r->q[2] = f;
      u = r->vi[1]; r->vi[1] = r->vi[2]; r->vi[2] = u;
   }
   r->neg = (r->q[0] < 0.0f ? 1u : 0u) | (r->q[1] < 0.0f ? 2u : 0u) | (r->q[2] < 0.0f ? 4u : 0u);
   if (r->neg != 0 && !empty)
      ref_clipped++;
   r->src = src;
   {
      int mnx = r->x[0] < r->x[1] ? r->x[0] : r->x[1], mny = r->y[0] < r->y[1] ? r->y[0] : r->y[1];
      int mxx = r->x[0] > r->x[1] ? r->x[0] : r->x[1], mxy = r->y[0] > r->y[1] ? r->y[0] : r->y[1];
      int bx0, by0, bx1, by1;
      if (r->x[2] < mnx) mnx = r->x[2];
      if (r->y[2] < mny) mny = r->y[2];
      if (r->x[2] > mxx) mxx = r->x[2];
      if (r->y[2] > mxy) mxy = r->y[2];
      /* (an arithmetic shift, as the kernel's, of x and y at full size) */
      {
         const ref_s64 m = (ref_s64)1 << shift;
         const ref_s64 ax0 = (ref_s64)mnx * m - 255, ay0 = (ref_s64)mny * m - 255, ax1 = (ref_s64)mxx * m, ay1 = (ref_s64)mxy * m;
         bx0 = ref_int(ax0 >= 0 ? ax0 / 256 : -((-ax0 + 255) / 256));
         by0 = ref_int(ay0 >= 0 ? ay0 / 256 : -((-ay0 + 255) / 256));
         bx1 = ref_int(ax1 >= 0 ? ax1 / 256 : -((-ax1 + 255) / 256));
         by1 = ref_int(ay1 >= 0 ? ay1 / 256 : -((-ay1 + 255) / 256));
      }
      if (r->neg != 0)
      {
         /* (the kernel's: all of its clip) */
         bx0 = by0 = 0;
         bx1 = by1 = 0x7fffffff;
      }
      if (bx0 < (int)(rect0 & 0xffff)) bx0 = (int)(rect0 & 0xffff);
      if (by0 < (int)(rect0 >> 16)) by0 = (int)(rect0 >> 16);
      if (bx1 > (int)(rect1 & 0xffff) - 1) bx1 = (int)(rect1 & 0xffff) - 1;
      if (by1 > (int)(rect1 >> 16) - 1) by1 = (int)(rect1 >> 16) - 1;
      if (bx1 > (int)F[PVRC_F_WIDTH] - 1) bx1 = (int)F[PVRC_F_WIDTH] - 1;
      if (by1 > (int)F[PVRC_F_HEIGHT] - 1) by1 = (int)F[PVRC_F_HEIGHT] - 1;
      r->live = !(empty || bx0 > bx1 || by0 > by1);
      r->bx0 = bx0; r->by0 = by0; r->bx1 = bx1; r->by1 = by1;
   }
}

/* ---- A primitive at a sample ---- */

static int g_px, g_py, g_sx, g_sy, g_cx, g_cy, g_s0x, g_s0y, g_in;
static unsigned g_samples;

static int rec_near(const Rec *r)
{
   return r->live && g_px >= r->bx0 && g_px <= r->bx1 && g_py >= r->by0 && g_py <= r->by1;
}

static int in_rect(unsigned r0, unsigned r1)
{
   return g_in && g_px >= (int)(r0 & 0xffff) && g_py >= (int)(r0 >> 16) && g_px < (int)(r1 & 0xffff) && g_py < (int)(r1 >> 16);
}

static int edge_in(ref_s64 e, int dx, int dy)
{
   return e > 0 || (e == 0 && (dy < 0 || (dy == 0 && dx > 0)));
}

static void rec_edges(const Rec *r, int px, int py, ref_s64 *e)
{
   /* (at its shift, the place divided to the nearest as the kernels' is: a
    * place is not negative) */
   const int half = (1 << r->shift) >> 1;
   px = (px + half) >> r->shift;
   py = (py + half) >> r->shift;
   e[0] = (ref_s64)(r->x[2] - r->x[1]) * (ref_s64)(py - r->y[1]) - (ref_s64)(r->y[2] - r->y[1]) * (ref_s64)(px - r->x[1]);
   e[1] = (ref_s64)(r->x[0] - r->x[2]) * (ref_s64)(py - r->y[2]) - (ref_s64)(r->y[0] - r->y[2]) * (ref_s64)(px - r->x[2]);
   e[2] = (ref_s64)(r->x[1] - r->x[0]) * (ref_s64)(py - r->y[0]) - (ref_s64)(r->y[1] - r->y[0]) * (ref_s64)(px - r->x[0]);
}

/* The clip of a triangle with a vertex behind the eye (the kernels'
 * rec_clip_plane(), rec_clip_at()) */
static float rec_clip_at(const Rec *r, const ref_s64 *e)
{
   const float zs = ff(PVRC_F_ZCLIP);
   float g[3], a, b, c;
   int k;
   for (k = 0; k < 3; k++)
   {
      const float q = r->q[k];
      const float d = q * zs;
      const float gk = q * d;
      g[k] = q > 0.0f && d < 1.0f ? gk : q;
   }
   a = (float)e[0] * g[0];
   b = (float)e[1] * g[1];
   c = (float)e[2] * g[2];
   return (a + b) + c;
}

static int rec_inside(const Rec *r, const ref_s64 *e)
{
   int s0, s1, s2;
   if (r->neg == 0)
      return edge_in(e[0], r->x[2] - r->x[1], r->y[2] - r->y[1]) && edge_in(e[1], r->x[0] - r->x[2], r->y[0] - r->y[2])
            && edge_in(e[2], r->x[1] - r->x[0], r->y[1] - r->y[0]);
   s0 = (r->neg & 1) ? -1 : 1;
   s1 = (r->neg & 2) ? -1 : 1;
   s2 = (r->neg & 4) ? -1 : 1;
   return edge_in(s0 * e[0], s0 * (r->x[2] - r->x[1]), s0 * (r->y[2] - r->y[1]))
         && edge_in(s1 * e[1], s1 * (r->x[0] - r->x[2]), s1 * (r->y[0] - r->y[2]))
         && edge_in(s2 * e[2], s2 * (r->x[1] - r->x[0]), s2 * (r->y[1] - r->y[0]))
         && rec_clip_at(r, e) >= 0.0f;
}

static int rec_covers(const Rec *r, int px, int py)
{
   ref_s64 e[3];
   rec_edges(r, px, py, e);
   return rec_inside(r, e);
}

typedef struct { float f0, f1, f2, s, w; } Bary;

static Bary rec_bary(const Rec *r, const ref_s64 *e)
{
   Bary b;
   float b0 = (float)e[0], b1 = (float)e[1], b2 = (float)e[2];
   float area = (float)(e[0] + e[1] + e[2]);
   b.f0 = b0 * r->q[0];
   b.f1 = b1 * r->q[1];
   b.f2 = b2 * r->q[2];
   b.s = (b.f0 + b.f1) + b.f2;
   b.w = b.s / area;
   return b;
}

static float bary_mix(const Bary *b, float a0, float a1, float a2)
{
   return ((b->f0 * a0 + b->f1 * a1) + b->f2 * a2) / b->s;
}

static unsigned vtxw(unsigned v, unsigned k)
{
   return F[F[PVRC_F_O_VTX] + v * PVRC_VTX_WORDS + k];
}

static void vtx_color(unsigned v, unsigned k, float *c)
{
   unsigned w = vtxw(v, k);
   c[0] = (float)(w & 255) / 255.0f;
   c[1] = (float)((w >> 8) & 255) / 255.0f;
   c[2] = (float)((w >> 16) & 255) / 255.0f;
   c[3] = (float)(w >> 24) / 255.0f;
}

typedef struct
{
   float base[4], offs[4], base1[4], offs1[4];
   float uv[2], uv1[2];
   float w;
   float pos[2];
} Frag;

static void frag_mix4(const Rec *r, const Bary *b, unsigned k, float *out)
{
   float c0[4], c1[4], c2[4];
   int i;
   vtx_color(r->vi[0], k, c0);
   vtx_color(r->vi[1], k, c1);
   vtx_color(r->vi[2], k, c2);
   for (i = 0; i < 4; i++)
      out[i] = bary_mix(b, c0[i], c1[i], c2[i]);
}

static float frag_mix1(const Rec *r, const Bary *b, unsigned k)
{
   unsigned w0 = vtxw(r->vi[0], k), w1 = vtxw(r->vi[1], k), w2 = vtxw(r->vi[2], k);
   float a0, a1, a2;
   memcpy(&a0, &w0, 4);
   memcpy(&a1, &w1, 4);
   memcpy(&a2, &w2, 4);
   return bary_mix(b, a0, a1, a2);
}

static void rec_frag(const Rec *r, unsigned pp, const ref_s64 *e, int px, int py, Frag *f)
{
   Bary b = rec_bary(r, e);
   unsigned flags = F[pp + PVRC_Q_FLAGS];
   int two = (flags & PVRC_QF_TWO_VOLUMES) != 0;
   if (flags & PVRC_QF_GOURAUD)
   {
      frag_mix4(r, &b, 3, f->base);
      frag_mix4(r, &b, 4, f->offs);
      if (two)
      {
         frag_mix4(r, &b, 7, f->base1);
         frag_mix4(r, &b, 8, f->offs1);
      }
   }
   else
   {
      vtx_color(r->extra, 3, f->base);
      vtx_color(r->extra, 4, f->offs);
      if (two)
      {
         vtx_color(r->extra, 7, f->base1);
         vtx_color(r->extra, 8, f->offs1);
      }
   }
   f->uv[0] = frag_mix1(r, &b, 5);
   f->uv[1] = frag_mix1(r, &b, 6);
   if (two)
   {
      f->uv1[0] = frag_mix1(r, &b, 9);
      f->uv1[1] = frag_mix1(r, &b, 10);
   }
   f->w = b.w;
   f->pos[0] = (float)px / 256.0f;
   f->pos[1] = (float)py / 256.0f;
}

/* ---- Shading ---- */

#define SHADE_DEPTH 0
#define SHADE_COLOR 1
#define SHADE_OIT 2

static float clampf(float x, float lo, float hi)
{
   return x < lo ? lo : (x > hi ? hi : x);
}

static float minf_(float a, float b) { return b < a ? b : a; }
static float maxf_(float a, float b) { return a < b ? b : a; }

/* GLSL's clamp(): min(max(x, lo), hi) */
static float glsl_clamp(float x, float lo, float hi)
{
   return minf_(maxf_(x, lo), hi);
}

static void tex_fetch(unsigned slot, const float *uv, float *c)
{
   unsigned h = pvrc_test_texel(slot, uv[0], uv[1]);
   c[0] = pvrc_from8(h);
   c[1] = pvrc_from8(h >> 8);
   c[2] = pvrc_from8(h >> 16);
   c[3] = pvrc_from8(h >> 24);
}

static int shade(unsigned pp, const Frag *f, int pass, int shadowed, float *color)
{
   unsigned flags = F[pp + PVRC_Q_FLAGS];
   int two = (flags & PVRC_QF_TWO_VOLUMES) != 0;
   float offset[4], uv[2];
   int area1 = 0, useAlpha, ignoreTexA, i;
   unsigned shadInstr, fogCtrl;
   (void)clampf;
   if (flags & PVRC_QF_CLIP_INSIDE)
   {
      unsigned c0 = F[pp + PVRC_Q_CLIP], c1 = F[pp + PVRC_Q_CLIP + 1];
      if (f->pos[0] >= (float)(c0 & 0xffff) && f->pos[0] <= (float)(c1 & 0xffff)
            && f->pos[1] >= (float)(c0 >> 16) && f->pos[1] <= (float)(c1 >> 16))
         return 1;
   }
   memcpy(color, f->base, 16);
   memcpy(offset, f->offs, 16);
   uv[0] = f->uv[0];
   uv[1] = f->uv[1];
   useAlpha = (flags & (two ? PVRC_QF_USE_ALPHA0 : PVRC_QF_USE_ALPHA)) != 0;
   ignoreTexA = (flags & (two ? PVRC_QF_IGNORE_TEXA0 : PVRC_QF_IGNORE_TEXA)) != 0;
   shadInstr = (flags >> PVRC_QF_SHAD_SHIFT) & 3;
   fogCtrl = (flags >> PVRC_QF_FOG_SHIFT) & 3;
   if (pass == SHADE_COLOR && shadowed)
   {
      if (two)
      {
         memcpy(color, f->base1, 16);
         memcpy(offset, f->offs1, 16);
         uv[0] = f->uv1[0];
         uv[1] = f->uv1[1];
         area1 = 1;
         useAlpha = (flags & PVRC_QF_USE_ALPHA1) != 0;
         ignoreTexA = (flags & PVRC_QF_IGNORE_TEXA1) != 0;
         shadInstr = (flags >> PVRC_QF_SHAD1_SHIFT) & 3;
         fogCtrl = (flags >> PVRC_QF_FOG1_SHIFT) & 3;
      }
      else
      {
         float scale = ff(PVRC_F_SHADE);
         for (i = 0; i < 3; i++)
         {
            color[i] *= scale;
            offset[i] *= scale;
         }
      }
   }
   if (!useAlpha)
      color[3] = 1.0f;
   if (flags & PVRC_QF_TEXTURE)
   {
      unsigned slots = F[pp + PVRC_Q_TEX];
      float texcol[4];
      if (two && area1)
         tex_fetch(slots >> 16, uv, texcol);
      else
         tex_fetch(slots & 0xffff, uv, texcol);
      if (ignoreTexA)
         texcol[3] = 1.0f;
      if (shadInstr == 0)
         memcpy(color, texcol, 16);
      else if (shadInstr == 1)
      {
         for (i = 0; i < 3; i++)
            color[i] *= texcol[i];
         color[3] = texcol[3];
      }
      else if (shadInstr == 2)
      {
         for (i = 0; i < 3; i++)
            color[i] = color[i] * (1.0f - texcol[3]) + texcol[i] * texcol[3];
      }
      else
         for (i = 0; i < 4; i++)
            color[i] *= texcol[i];
      if (flags & PVRC_QF_OFFSET)
         for (i = 0; i < 3; i++)
            color[i] += offset[i];
   }
   if (flags & PVRC_QF_CLAMP)
      for (i = 0; i < 4; i++)
         color[i] = glsl_clamp(color[i], ff(PVRC_F_CLAMP_MIN + i), ff(PVRC_F_CLAMP_MAX + i));
   if (fogCtrl == 3)
   {
      for (i = 0; i < 3; i++)
         color[i] = ff(PVRC_F_FOG_RAM + i);
      color[3] = pvrc_fog_lut(f->w, ff(PVRC_F_FOG_DEN), (const unsigned char *)(F + PVRC_F_FOG)) * (256.0f / 255.0f);
   }
   if (fogCtrl == 0)
   {
      float fog = pvrc_fog_lut(f->w, ff(PVRC_F_FOG_DEN), (const unsigned char *)(F + PVRC_F_FOG));
      for (i = 0; i < 3; i++)
         color[i] = color[i] * (255.0f / 256.0f - fog) + ff(PVRC_F_FOG_RAM + i) * fog;
   }
   if ((flags & PVRC_QF_OFFSET) && fogCtrl == 1)
   {
      float fog = offset[3] * (255.0f / 256.0f);
      for (i = 0; i < 3; i++)
         color[i] = color[i] * (255.0f / 256.0f - fog) + ff(PVRC_F_FOG_VERT + i) * fog;
   }
   for (i = 0; i < 4; i++)
      color[i] *= ff(pp + PVRC_Q_TRILIN);
   if (flags & PVRC_QF_ALPHA_TEST)
   {
      if (ff(PVRC_F_ALPHA_REF) > color[3])
         return 1;
      color[3] = 1.0f;
   }
   return 0;
}

static unsigned col_pack(const float *c)
{
   return pvrc_unorm8(c[0]) | (pvrc_unorm8(c[1]) << 8) | (pvrc_unorm8(c[2]) << 16) | (pvrc_unorm8(c[3]) << 24);
}

static void col_unpack(unsigned c, float *o)
{
   o[0] = pvrc_from8(c);
   o[1] = pvrc_from8(c >> 8);
   o[2] = pvrc_from8(c >> 16);
   o[3] = pvrc_from8(c >> 24);
}

static unsigned blend8(unsigned tsp, const float *src, unsigned dst)
{
   float s[4], d[4], res[4];
   unsigned si = tsp >> 29, di = (tsp >> 26) & 7, c;
   for (c = 0; c < 4; c++)
      s[c] = glsl_clamp(src[c], 0.0f, 1.0f);
   col_unpack(dst, d);
   for (c = 0; c < 4; c++)
   {
      float sf = pvrc_blend_factor(si, 1, s[c], s[3], d[c], d[3]);
      float df = pvrc_blend_factor(di, 0, s[c], s[3], d[c], d[3]);
      res[c] = s[c] * sf + d[c] * df;
   }
   return col_pack(res);
}

/* ---- The walk of one sample ---- */

static int single_rate(unsigned pp)
{
   return g_samples == 1 || (F[pp + PVRC_Q_FLAGS] & PVRC_QF_SINGLE_RATE) != 0;
}

static unsigned rec_poly(const Rec *r)
{
   return F[PVRC_F_O_POLY] + r->src * PVRC_POLY_WORDS;
}

/* Where a fragment shaded once a pixel takes its inputs: the centre where
 * the triangle covers all the pixel's samples, else the first sample it
 * covers (the device's centroid, Mesa's choice) */
static void rate_pos(const Rec *r, int *x, int *y)
{
   ref_s64 e[3];
   unsigned s, first = g_samples;
   int all = 1;
   *x = g_cx;
   *y = g_cy;
   for (s = 0; s < g_samples && g_samples > 1; s++)
   {
      int sx = g_px * 256 + (int)SSAA_SAMPLE_X16(g_samples, s) * 16;
      int sy = g_py * 256 + (int)SSAA_SAMPLE_Y16(g_samples, s) * 16;
      int c;
      rec_edges(r, sx, sy, e);
      c = rec_inside(r, e);
      if (c && first == g_samples)
      {
         first = s;
         *x = sx;
         *y = sy;
      }
      all = all && c;
   }
   if (all || first == g_samples)
   {
      *x = g_cx;
      *y = g_cy;
   }
}

/* ...its inputs there, with gl_FragCoord (its 1/w for the depth and the
 * fog) at the centre */
static void rate_frag(const Rec *r, unsigned pp, Frag *f)
{
   ref_s64 e[3];
   int x, y;
   rate_pos(r, &x, &y);
   rec_edges(r, x, y, e);
   rec_frag(r, pp, e, x, y, f);
   if (x != g_cx || y != g_cy)
   {
      Bary b;
      rec_edges(r, g_cx, g_cy, e);
      b = rec_bary(r, e);
      f->w = b.w;
      f->pos[0] = (float)g_cx / 256.0f;
      f->pos[1] = (float)g_cy / 256.0f;
   }
}

static int lane_frag(const Rec *r, unsigned pp, int centre, Frag *f)
{
   ref_s64 e[3];
   if (!in_rect(F[pp + PVRC_Q_RECT], F[pp + PVRC_Q_RECT + 1]))
      return 0;
   rec_edges(r, g_sx, g_sy, e);
   if (!rec_inside(r, e))
      return 0;
   if (centre || single_rate(pp))
      rate_frag(r, pp, f);
   else
      rec_frag(r, pp, e, g_sx, g_sy, f);
   return 1;
}

static float vol_w(const Rec *r)
{
   ref_s64 e[3];
   Bary b;
   rec_edges(r, g_cx, g_cy, e);
   b = rec_bary(r, e);
   return b.w;
}

static unsigned mvp_word(const Rec *r, unsigned k)
{
   return F[F[PVRC_F_O_MVP] + (r->src & ~PVRC_SRC_MODVOL) * PVRC_MVP_WORDS + k];
}

static int vol_close_counts(unsigned cov, unsigned cull)
{
   if (cull == 3)
      cov &= PVRC_RX_FRONT;
   else if (cull == 2)
      cov &= PVRC_RX_BACK;
   return cov != 0;
}

/* (test mode) */
static float depth_of(float w)
{
   return w;
}

typedef struct { unsigned kh, kl; } Key;
static Key *keys;

static int key_cmp(const void *a, const void *b)
{
   const Key *x = (const Key *)a, *y = (const Key *)b;
   if (pvr_key_before(x->kh, x->kl & ~3u, y->kh, y->kl & ~3u))
      return -1;
   if (pvr_key_before(y->kh, y->kl & ~3u, x->kh, x->kl & ~3u))
      return 1;
   return 0;
}

static unsigned ref_layers_most;

/* the volumes of the range [lo, hi) applied, at this sample: the opaque
 * ones to @stencil, or the translucent ones to the @n fragments in @k */
static void volumes(unsigned lo, unsigned hi, int opaque, unsigned *stencil, float depth, Key *k, unsigned n)
{
   unsigned cov = 0, pending = PVRC_NONE, prim, j;
   const Rec *closer = NULL;
   for (prim = lo; ; prim++)
   {
      int end = prim >= hi;
      const Rec *r;
      if (pending != PVRC_NONE && (end || prim > pending))
      {
         unsigned mp = F[PVRC_F_O_MVP] + mvp_word(closer, PVRC_M_CLOSE_BY) * PVRC_MVP_WORDS;
         unsigned cisp = F[mp + PVRC_M_ISP];
         if (vol_close_counts(cov, (cisp >> 27) & 3) && in_rect(F[mp + PVRC_M_RECT], F[mp + PVRC_M_RECT + 1]))
         {
            if (opaque)
               *stencil = (*stencil & 0x80) | pvr_modvol_close(*stencil & 3, cisp >> 29);
            else
               for (j = 0; j < n; j++)
               {
                  unsigned fpp = F[PVRC_F_O_POLY] + recs[k[j].kl >> 2].src * PVRC_POLY_WORDS;
                  if (F[fpp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW)
                     k[j].kl = (k[j].kl & ~3u) | pvr_modvol_close(k[j].kl & 3, cisp >> 29);
               }
         }
         cov = 0;
         pending = PVRC_NONE;
      }
      if (end)
         break;
      r = &recs[prim];
      {
         unsigned e = mvp_word(r, PVRC_M_CLOSE_END);
         if (e != PVRC_NONE)
         {
            pending = e;
            closer = r;
         }
      }
      if (!rec_near(r) || !g_in)
         continue;
      if (!rec_covers(r, opaque ? g_sx : g_s0x, opaque ? g_sy : g_s0y))
         continue;
      cov |= r->extra & (PVRC_RX_FRONT | PVRC_RX_BACK);
      if ((r->extra & PVRC_RX_CULLED) || !in_rect(mvp_word(r, PVRC_M_RECT), mvp_word(r, PVRC_M_RECT + 1)))
         continue;
      if (opaque)
      {
         if (depth_of(vol_w(r)) > depth)
            *stencil = (*stencil & 0x80) | pvr_modvol_triangle(*stencil & 3, 1, (r->extra & PVRC_RX_OR) ? 1 : 0);
      }
      else
      {
         float vz = depth_of(vol_w(r));
         for (j = 0; j < n; j++)
         {
            unsigned fpp = F[PVRC_F_O_POLY] + recs[k[j].kl >> 2].src * PVRC_POLY_WORDS;
            float fz;
            memcpy(&fz, &k[j].kh, 4);
            if ((F[fpp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) && vz >= fz)
               k[j].kl = (k[j].kl & ~3u) | pvr_modvol_triangle(k[j].kl & 3, 1, (r->extra & PVRC_RX_OR) ? 1 : 0);
         }
      }
   }
}

static unsigned walk_sample(unsigned start)
{
   unsigned col = start, pass, passes = F[PVRC_F_PASSES], prim;
   float depth = 0.0f;
   for (pass = 0; pass < passes; pass++)
   {
      unsigned pw = PVRC_F_PASS + pass * PVRC_PASS_WORDS;
      unsigned opLo = F[pw + PVRC_P_OP], mvoLo = F[pw + PVRC_P_MVO], ptLo = F[pw + PVRC_P_PT];
      unsigned trLo = F[pw + PVRC_P_TR], trHi = F[pw + PVRC_P_TR_END];
      unsigned mvtLo = F[pw + PVRC_P_MVT], mvtHi = F[pw + PVRC_P_MVT_END];
      int autosort = (F[pw + PVRC_P_FLAGS] & PVRC_PF_AUTOSORT) != 0;
      int lastPass = pass + 1 == passes;
      unsigned stencil = 0;
      int shadowed;
      Frag f;
      float c[4];

      for (prim = opLo; prim < mvoLo; prim++)
      {
         const Rec *r = &recs[prim];
         unsigned pp, isp;
         float z;
         if (!rec_near(r))
            continue;
         pp = rec_poly(r);
         if (!lane_frag(r, pp, 0, &f))
            continue;
         isp = F[pp + PVRC_Q_ISP];
         z = depth_of(f.w);
         if (!pvrc_depth_pass(isp >> 29, z, depth))
            continue;
         if ((F[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) && shade(pp, &f, SHADE_DEPTH, 0, c))
            continue;
         if (!(isp & (1u << 26)))
            depth = z;
         stencil = (stencil & 0x7f) | ((F[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) ? 0x80 : 0);
      }
      for (prim = ptLo; prim < trLo; prim++)
      {
         const Rec *r = &recs[prim];
         unsigned pp;
         float z;
         if (!rec_near(r))
            continue;
         pp = rec_poly(r);
         if (!lane_frag(r, pp, 0, &f))
            continue;
         z = depth_of(f.w);
         if (!pvrc_depth_pass(6, z, depth))
            continue;
         if (shade(pp, &f, SHADE_DEPTH, 0, c))
            continue;
         depth = z;
         stencil = (stencil & 0x7f) | ((F[pp + PVRC_Q_FLAGS] & PVRC_QF_SHADOW) ? 0x80 : 0);
      }
      volumes(mvoLo, ptLo, 1, &stencil, depth, NULL, 0);
      shadowed = stencil == 0x81;

      for (prim = opLo; prim < mvoLo; prim++)
      {
         const Rec *r = &recs[prim];
         unsigned pp, isp;
         if (!rec_near(r))
            continue;
         pp = rec_poly(r);
         if (!lane_frag(r, pp, 0, &f))
            continue;
         isp = F[pp + PVRC_Q_ISP];
         if (!pvrc_depth_pass(!(isp & (1u << 26)) ? 2 : isp >> 29, depth_of(f.w), depth))
            continue;
         if (!shade(pp, &f, SHADE_COLOR, shadowed, c))
            col = blend8(F[pp + PVRC_Q_TSP], c, col);
      }
      for (prim = ptLo; prim < trLo; prim++)
      {
         const Rec *r = &recs[prim];
         unsigned pp, isp;
         if (!rec_near(r))
            continue;
         pp = rec_poly(r);
         if (!lane_frag(r, pp, 0, &f))
            continue;
         isp = F[pp + PVRC_Q_ISP];
         if (!pvrc_depth_pass(!(isp & (1u << 26)) ? 2 : 6, depth_of(f.w), depth))
            continue;
         if (!shade(pp, &f, SHADE_COLOR, shadowed, c))
            col = blend8(F[pp + PVRC_Q_TSP], c, col);
      }

      if (!autosort)
      {
         for (prim = trLo; prim < trHi; prim++)
         {
            const Rec *r = &recs[prim];
            unsigned pp, isp;
            float z;
            if (!rec_near(r))
               continue;
            pp = rec_poly(r);
            if (!lane_frag(r, pp, 0, &f))
               continue;
            isp = F[pp + PVRC_Q_ISP];
            z = depth_of(f.w);
            if (!pvrc_depth_pass(isp >> 29, z, depth))
               continue;
            if (shade(pp, &f, SHADE_COLOR, shadowed, c))
               continue;
            if (!(isp & (1u << 26)))
               depth = z;
            col = blend8(F[pp + PVRC_Q_TSP], c, col);
         }
      }
      else if (trHi > trLo)
      {
         unsigned n = 0, j, ch;
         float fc[4], sec[4];
         col_unpack(col, fc);
         sec[0] = sec[1] = sec[2] = sec[3] = 0.0f;
         for (prim = trLo; prim < trHi; prim++)
         {
            const Rec *r = &recs[prim];
            unsigned pp;
            float w, z;
            if (!rec_near(r))
               continue;
            pp = rec_poly(r);
            if (!in_rect(F[pp + PVRC_Q_RECT], F[pp + PVRC_Q_RECT + 1]))
               continue;
            if (!rec_covers(r, g_sx, g_sy))
               continue;
            w = vol_w(r);
            z = depth_of(w);
            if (!(z >= depth))
               continue;
            if (F[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE)
            {
               unsigned c0 = F[pp + PVRC_Q_CLIP], c1 = F[pp + PVRC_Q_CLIP + 1];
               float x = (float)g_px + 0.5f, y = (float)g_py + 0.5f;
               if (x >= (float)(c0 & 0xffff) && x <= (float)(c1 & 0xffff) && y >= (float)(c0 >> 16) && y <= (float)(c1 >> 16))
                  continue;
            }
            keys[n].kh = pvr_key_hi(z, 0);
            keys[n].kl = prim << 2;
            n++;
         }
         qsort(keys, n, sizeof(Key), key_cmp);
         if (n > ref_layers_most)
            ref_layers_most = n;
         volumes(mvtLo, mvtHi, 0, NULL, depth, keys, n);
         for (j = 0; j < n; j++)
         {
            const Rec *r = &recs[keys[j].kl >> 2];
            unsigned pp = rec_poly(r), tsp;
            int inVol = (keys[j].kl & PVR_MV_INSIDE) != 0, two, toSec;
            float src[4], dst[4], res[4], cl[4];
            lane_frag(r, pp, 1, &f);
            shade(pp, &f, SHADE_OIT, 0, c);
            two = F[pp + PVRC_Q_TSP1] != 0xffffffffu;
            tsp = (inVol && two) ? F[pp + PVRC_Q_TSP1] : F[pp + PVRC_Q_TSP];
            if ((tsp >> 25) & 1)
               memcpy(src, sec, 16);
            else
            {
               for (ch = 0; ch < 4; ch++)
                  cl[ch] = glsl_clamp(c[ch], 0.0f, 1.0f);
               col_unpack(col_pack(cl), src);
               if (inVol && !two)
                  for (ch = 0; ch < 3; ch++)
                     src[ch] = src[ch] * ff(PVRC_F_SHADE);
            }
            toSec = (tsp >> 24) & 1;
            memcpy(dst, toSec ? sec : fc, 16);
            for (ch = 0; ch < 4; ch++)
            {
               float sf = pvrc_blend_factor(tsp >> 29, 1, src[ch], src[3], dst[ch], dst[3]);
               float df = pvrc_blend_factor((tsp >> 26) & 7, 0, src[ch], src[3], dst[ch], dst[3]);
               res[ch] = glsl_clamp(dst[ch] * df + src[ch] * sf, 0.0f, 1.0f);
            }
            memcpy(toSec ? sec : fc, res, 16);
         }
         col = col_pack(fc);
      }

      if (!lastPass)
         for (prim = trLo; prim < trHi; prim++)
         {
            const Rec *r = &recs[prim];
            unsigned pp, isp;
            float z;
            if (!rec_near(r))
               continue;
            pp = rec_poly(r);
            if (!lane_frag(r, pp, 0, &f))
               continue;
            isp = F[pp + PVRC_Q_ISP];
            z = depth_of(f.w);
            if (!pvrc_depth_pass(autosort ? 6 : isp >> 29, z, depth))
               continue;
            if ((F[pp + PVRC_Q_FLAGS] & PVRC_QF_CLIP_INSIDE) && shade(pp, &f, SHADE_DEPTH, 0, c))
               continue;
            if (!(isp & (1u << 26)))
               depth = z;
         }
   }
   return col;
}

int tile_ref(const unsigned *frame, unsigned samples, const unsigned *last, unsigned *out, struct tile_ref_stats *st)
{
   unsigned prims, i, w, h, x, y, s, log2n;
   unsigned sc[8];
   F = frame;
   g_samples = samples;
   log2n = SSAA_LOG2(samples);
   prims = F[PVRC_F_PRIMS];
   w = F[PVRC_F_WIDTH];
   h = F[PVRC_F_HEIGHT];
   if (prims > nrecs)
   {
      free(recs);
      free(keys);
      recs = (Rec *)malloc(sizeof(Rec) * prims);
      keys = (Key *)malloc(sizeof(Key) * prims);
      if (!recs || !keys)
         return 0;
      nrecs = prims;
   }
   ref_dropped = 0;
   ref_clipped = 0;
   ref_layers_most = 0;
   for (i = 0; i < prims; i++)
      setup(i, &recs[i]);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         unsigned start = (F[PVRC_F_FLAGS] & PVRC_FF_LAST_PICTURE) && last ? last[y * w + x] : F[PVRC_F_CLEAR];
         g_px = (int)x;
         g_py = (int)y;
         g_in = 1;
         g_cx = g_px * 256 + 128;
         g_cy = g_py * 256 + 128;
         g_s0x = g_px * 256 + (int)SSAA_SAMPLE_X16(samples, 0u) * 16;
         g_s0y = g_py * 256 + (int)SSAA_SAMPLE_Y16(samples, 0u) * 16;
         for (s = 0; s < samples; s++)
         {
            g_sx = g_px * 256 + (int)SSAA_SAMPLE_X16(samples, s) * 16;
            g_sy = g_py * 256 + (int)SSAA_SAMPLE_Y16(samples, s) * 16;
            sc[s] = walk_sample(start);
         }
         out[y * w + x] = pvr_resolve_samples(sc, log2n);
      }
   if (st)
   {
      st->dropped = ref_dropped;
      st->clipped = ref_clipped;
      st->layers_most = ref_layers_most;
   }
   (void)fu;
   return 1;
}
