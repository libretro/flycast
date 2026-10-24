/* The compute renderer's coverage against the geometry: for each frame
 * tile_dump wrote, every triangle of the polygons and of the modifier
 * volumes is set up and tested by the reference's own code (tile_ref.c,
 * compiled in here: the kernels' arithmetic, which tile_test holds the
 * kernels to bit for bit), and tested again from the game's float
 * vertices in double precision - each vertex taken to the screen with the
 * frame's transform, exact edge functions, the top-left fill rule the
 * kernels keep for a sample on an edge (edge_in()), the facing and the
 * cull of its parameter, and for a triangle with a vertex behind the eye
 * the edges turned for those vertices and the near clip the kernels take
 * (rec_clip_at()). A sample one covers and the other does not is a fault,
 * unless it is within EPS pixels of an edge (the kernels keep a vertex to
 * 1/256 pixel, so a sample that near may go either way), or of the near
 * clip; those are counted.
 *
 * tile_test only compares the kernels with the reference, which does what
 * they do, so a fault of their arithmetic is in both and passes there
 * (Virtua Fighter 3tb's floor bent at 8x, Jet Grind Radio's shadows with a
 * staircase edge): this is the test that sees it.
 *
 * The samples looked at: those within BAND pixels of one of the
 * triangle's exact edges or of its near clip, and on a grid of a pixel
 * every GRID each way. A fault of the arithmetic puts an edge in another
 * place, and what lies between the two places begins at the exact edge,
 * so the band sees every triangle at fault (if not every sample of it),
 * and the grid what does not begin there. That is the triangle's
 * perimeter, not its area: a shadow volume's triangle that covers the
 * screen costs a few thousand samples, not all of them. A triangle one of
 * the two takes and the other drops, every sample of its box; -a every
 * sample of every triangle.
 *
 *   tile_geo [-s "1 2 4 8"] [-b BAND] [-g GRID] [-a] [-t N] [-e EPS] [-v] FRAME...
 *
 * -s the sample counts (default the environment's TILE_SAMPLES, else 1
 * and 4), -b and -g as above (default 1.5 and 16), -t only every Nth
 * triangle looked at (default the environment's TILE_GEO_STRIDE, else 1:
 * all of them) and, whatever N, every triangle with a vertex far out
 * (more than a frame's width or height past its edge), behind the eye, or
 * that one of the two drops and the other not: those are the ones
 * precision fails on. -e the margin in pixels (default 1/256), -v every
 * triangle at fault printed (else the first 8 of a frame). Ends with
 * "tile_geo: PASS" or "tile_geo: FAIL".
 *
 * The reference is included as its text, setup() and rec_covers() as they
 * are, so the same program built with an older tile_ref.c and
 * pvr_compute.h holds that code to the geometry too. C99, built without
 * fused operations (-ffp-contract=off) as the reference is; with -fopenmp
 * the triangles are shared out between threads. */
#include <stdio.h>
#include <math.h>
#include "tile_ref.c"

static double geo_eps = 1.0 / 256.0;
static int geo_verbose;
static unsigned geo_stride = 1;
static double geo_band = 1.5;
static int geo_grid = 16, geo_every;

typedef struct
{
   double x[3], y[3];	/* on the screen, pixels, in the order that has the edges positive inside */
   float q[3];
   int neg[3];		/* behind the eye */
   int live;		/* not dropped, not culled, not all behind the eye, not degenerate */
   int far;		/* a vertex far out, behind the eye, or not a number */
   int all;		/* (with a vertex behind the eye) every pixel of its rect looked at */
   double bx0, by0, bx1, by1;
   unsigned rect0, rect1;
   /* edge k (from vertex k+1 to k+2): from the one of its vertices nearer
    * the screen, so that a vertex billions of pixels out loses nothing of
    * a sample's place; its direction, one over its length, and whether it
    * takes its own samples (the kernels' edge_in(): an edge going up, or
    * along the top to the right), each turned for a vertex behind the eye */
   double ex[3], ey[3], edx[3], edy[3], einv[3];
   int etl[3];
} Geo;

/* A triangle from its words, as the frame gives it: its vertices, its
 * parameter's ISP word and pixel rect; the reference's setup() reads the
 * same words. */
static void geo_setup(unsigned prim, Geo *g)
{
   unsigned osrc = F[PVRC_F_O_SRC], lo = 0, hi = F[PVRC_F_SOURCES], i, src, index, k, isp, cull;
   float vx[3], vy[3], vz[3];
   double cross;
   int behind = 0, front, bad = 0, modvol;
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
   memset(g, 0, sizeof(*g));
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
      g->rect0 = F[PVRC_F_SCISSOR] | (F[PVRC_F_SCISSOR + 1] << 16);
      g->rect1 = F[PVRC_F_SCISSOR + 2] | (F[PVRC_F_SCISSOR + 3] << 16);
   }
   else
   {
      unsigned pp = F[PVRC_F_O_POLY] + index * PVRC_POLY_WORDS;
      unsigned at = F[PVRC_F_O_IDX] + F[osrc + lo * PVRC_SRC_WORDS + 2] + i;
      unsigned odd = i & 1, ov = F[PVRC_F_O_VTX];
      unsigned vi[3];
      vi[0] = F[at];
      vi[1] = F[at + 1 + odd];
      vi[2] = F[at + 2 - odd];
      for (k = 0; k < 3; k++)
      {
         vx[k] = ff(ov + vi[k] * PVRC_VTX_WORDS);
         vy[k] = ff(ov + vi[k] * PVRC_VTX_WORDS + 1);
         vz[k] = ff(ov + vi[k] * PVRC_VTX_WORDS + 2);
      }
      isp = F[pp + PVRC_Q_ISP];
      g->rect0 = F[pp + PVRC_Q_RECT];
      g->rect1 = F[pp + PVRC_Q_RECT + 1];
   }
   for (k = 0; k < 3; k++)
   {
      /* (the transform in double: no rounding of a vertex far out) */
      g->x[k] = (double)ff(PVRC_F_XFORM) * vx[k] + (double)ff(PVRC_F_XFORM + 2);
      g->y[k] = (double)ff(PVRC_F_XFORM + 1) * vy[k] + (double)ff(PVRC_F_XFORM + 3);
      /* (1/w as the kernels have it: whether a vertex is behind the eye,
       * and the near clip, are decisions on it, not geometry) */
      g->q[k] = ff(PVRC_F_XFORM + 4) * vz[k];
      if (!(fabsf(g->q[k]) > 0.0f) || fabsf(g->q[k]) > 3.4e37f || g->x[k] != g->x[k] || g->y[k] != g->y[k]
            || fabs(g->x[k]) > 1e300 || fabs(g->y[k]) > 1e300)
         bad = 1;
      g->neg[k] = g->q[k] < 0.0f;
      behind += g->neg[k];
      if (!(fabs(g->x[k]) <= 2.0 * F[PVRC_F_WIDTH] + 1.0 && fabs(g->y[k]) <= 2.0 * F[PVRC_F_HEIGHT] + 1.0))
         g->far = 1;
   }
   g->far |= behind != 0 || bad;
   if (bad)
      return;
   cross = (g->x[1] - g->x[0]) * (g->y[2] - g->y[0]) - (g->x[2] - g->x[0]) * (g->y[1] - g->y[0]);
   front = (cross < 0) != ((behind & 1) != 0);
   cull = (isp >> 27) & 3;
   if (!modvol && ((cull == 3 && !front) || (cull == 2 && front)))
      return;
   if (cross == 0.0 || behind == 3)
      return;
   if (cross < 0)
   {
      double d;
      float f;
      int n;
      d = g->x[1]; g->x[1] = g->x[2]; g->x[2] = d;
      d = g->y[1]; g->y[1] = g->y[2]; g->y[2] = d;
      f = g->q[1]; g->q[1] = g->q[2]; g->q[2] = f;
      n = g->neg[1]; g->neg[1] = g->neg[2]; g->neg[2] = n;
   }
   for (k = 0; k < 3; k++)
   {
      const unsigned a = (k + 1) % 3, b = (k + 2) % 3;
      const double sg = g->neg[k] ? -1.0 : 1.0;
      const unsigned n = fmax(fabs(g->x[a]), fabs(g->y[a])) <= fmax(fabs(g->x[b]), fabs(g->y[b])) ? a : b;
      g->edx[k] = sg * (g->x[b] - g->x[a]);
      g->edy[k] = sg * (g->y[b] - g->y[a]);
      g->ex[k] = g->x[n];
      g->ey[k] = g->y[n];
      g->einv[k] = 1.0 / hypot(g->edx[k], g->edy[k]);
      g->etl[k] = g->edy[k] < 0 || (g->edy[k] == 0 && g->edx[k] > 0);
   }
   g->live = 1;
   g->all = behind != 0;
   g->bx0 = g->bx1 = g->x[0];
   g->by0 = g->by1 = g->y[0];
   for (k = 1; k < 3; k++)
   {
      if (g->x[k] < g->bx0) g->bx0 = g->x[k];
      if (g->x[k] > g->bx1) g->bx1 = g->x[k];
      if (g->y[k] < g->by0) g->by0 = g->y[k];
      if (g->y[k] > g->by1) g->by1 = g->y[k];
   }
}

/* 1 covered, 0 not, -1 too near an edge or the near clip to say */
static int geo_covers(const Geo *g, double px, double py)
{
   double e[3];
   int k, in = 1, near = 0;
   for (k = 0; k < 3; k++)
   {
      e[k] = g->edx[k] * (py - g->ey[k]) - g->edy[k] * (px - g->ex[k]);
      if (fabs(e[k]) * g->einv[k] < geo_eps)
         near = 1;
      if (!(e[k] > 0 || (e[k] == 0 && g->etl[k])))
         in = 0;
   }
   if (g->all && in)
   {
      /* the near clip: rec_clip_at() in double, with the edges unturned */
      const float zs = ff(PVRC_F_ZCLIP);
      double c = 0, mag = 0, t;
      for (k = 0; k < 3; k++)
      {
         const float q = g->q[k];
         const float d = q * zs;
         const double gk = q > 0.0f && d < 1.0f ? (double)q * (double)q * (double)zs : (double)q;
         t = (g->neg[k] ? -e[k] : e[k]) * gk;
         c += t;
         mag += fabs(t);
      }
      if (fabs(c) <= mag * (1.0 / 1048576.0))
         near = 1;
      if (!(c >= 0))
         in = 0;
   }
   return near ? -1 : in;
}

struct geo_count
{
   unsigned long long tris, far, samples, covered, near, over, under, faulty;
};

/* The pixels either can cover: the union of the two boxes, in the rect */
static void geo_box(const Rec *r, const Geo *g, unsigned w, unsigned h, int *b)
{
   int x0 = 0x7fffffff, y0 = 0x7fffffff, x1 = -1, y1 = -1;
   if (r->live)
   {
      x0 = r->bx0; y0 = r->by0; x1 = r->bx1; y1 = r->by1;
   }
   if (g->live)
   {
      int gx0, gy0, gx1, gy1;
      if (g->all)
      {
         gx0 = 0; gy0 = 0; gx1 = (int)w - 1; gy1 = (int)h - 1;
      }
      else
      {
         /* (to -1..w and -1..h first: a box far off is not wrapped round) */
         gx0 = (int)floor(fmin(fmax(g->bx0, -1.0), (double)w));
         gy0 = (int)floor(fmin(fmax(g->by0, -1.0), (double)h));
         gx1 = (int)floor(fmin(fmax(g->bx1, -1.0), (double)w));
         gy1 = (int)floor(fmin(fmax(g->by1, -1.0), (double)h));
      }
      if (gx0 < x0) x0 = gx0;
      if (gy0 < y0) y0 = gy0;
      if (gx1 > x1) x1 = gx1;
      if (gy1 > y1) y1 = gy1;
   }
   if (x0 < (int)(g->rect0 & 0xffff)) x0 = (int)(g->rect0 & 0xffff);
   if (y0 < (int)(g->rect0 >> 16)) y0 = (int)(g->rect0 >> 16);
   if (x1 > (int)(g->rect1 & 0xffff) - 1) x1 = (int)(g->rect1 & 0xffff) - 1;
   if (y1 > (int)(g->rect1 >> 16) - 1) y1 = (int)(g->rect1 >> 16) - 1;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > (int)w - 1) x1 = (int)w - 1;
   if (y1 > (int)h - 1) y1 = (int)h - 1;
   b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1;
}

struct geo_tri
{
   unsigned long long samples, covered, near, over, under;
};

/* One sample of a triangle. (The reference's rec_near() reads its pixel
 * from globals, so its box test is made here, the same.) */
static void geo_sample(const Rec *r, const Geo *g, int x, int y, unsigned samples, unsigned s, struct geo_tri *t)
{
   const int sx = x * 256 + (int)SSAA_SAMPLE_X16(samples, s) * 16;
   const int sy = y * 256 + (int)SSAA_SAMPLE_Y16(samples, s) * 16;
   const int ref = r->live && x >= r->bx0 && x <= r->bx1 && y >= r->by0 && y <= r->by1 && rec_covers(r, sx, sy);
   const int geo = g->live ? geo_covers(g, x + SSAA_SAMPLE_X16(samples, s) / 16.0, y + SSAA_SAMPLE_Y16(samples, s) / 16.0) : 0;
   t->samples++;
   if (geo < 0)
   {
      t->near++;
      return;
   }
   t->covered += (unsigned)geo;
   if (ref && !geo)
      t->over++;
   else if (geo && !ref)
      t->under++;
}

/* The pixels x0..x1 whose sample (ox past the pixel's corner) lies in
 * lo..hi, in a and b (none: a > b) */
static void geo_span(double lo, double hi, double ox, int x0, int x1, int *a, int *b)
{
   lo -= ox;
   hi -= ox;
   if (!(lo <= hi) || hi < x0 || lo > x1)
   {
      *a = 1;
      *b = 0;
      return;
   }
   *a = lo <= x0 ? x0 : (int)ceil(lo);
   *b = hi >= x1 ? x1 : (int)floor(hi);
}

/* One triangle in its box b: the samples the header says */
static void geo_tri(const Rec *r, const Geo *g, const int *b, unsigned samples, struct geo_tri *t)
{
   double gk[3], cdx = 0, cdy = 0, cband = 0;
   int x, y, k;
   unsigned s;
   memset(t, 0, sizeof(*t));
   if (geo_every || !r->live || !g->live)
   {
      for (y = b[1]; y <= b[3]; y++)
         for (x = b[0]; x <= b[2]; x++)
            for (s = 0; s < samples; s++)
               geo_sample(r, g, x, y, samples, s, t);
      return;
   }
   if (g->all)
   {
      /* the near clip's function, as geo_covers() has it: its slopes */
      const float zs = ff(PVRC_F_ZCLIP);
      for (k = 0; k < 3; k++)
      {
         const float q = g->q[k];
         const float d = q * zs;
         gk[k] = (q > 0.0f && d < 1.0f ? (double)q * (double)q * (double)zs : (double)q) * (g->neg[k] ? -1.0 : 1.0);
         cdx -= gk[k] * g->edy[k];
         cdy += gk[k] * g->edx[k];
      }
      cband = geo_band * hypot(cdx, cdy);
   }
   for (y = b[1]; y <= b[3]; y++)
      for (s = 0; s < samples; s++)
      {
         const double ox = SSAA_SAMPLE_X16(samples, s) / 16.0, py = y + SSAA_SAMPLE_Y16(samples, s) / 16.0;
         int lo[4], hi[4], n = 0, i, j, end;
         for (k = 0; k < 3; k++)
         {
            /* the edge's function is c - edy (px - ex): |that| <= band
             * over its length */
            const double c = g->edx[k] * (py - g->ey[k]), d = geo_band / g->einv[k];
            if (g->edy[k] == 0.0)
            {
               if (fabs(c) <= d)
               {
                  lo[n] = b[0];
                  hi[n++] = b[2];
               }
               continue;
            }
            {
               double p0 = g->ex[k] + (c - d) / g->edy[k], p1 = g->ex[k] + (c + d) / g->edy[k];
               if (p0 > p1)
               {
                  const double m = p0;
                  p0 = p1;
                  p1 = m;
               }
               geo_span(p0, p1, ox, b[0], b[2], &lo[n], &hi[n]);
               if (lo[n] <= hi[n])
                  n++;
            }
         }
         if (g->all)
         {
            /* the clip's function at the box's left, then along the row */
            const double px0 = b[0] + ox;
            double c0 = 0;
            for (k = 0; k < 3; k++)
               c0 += gk[k] * (g->edx[k] * (py - g->ey[k]) - g->edy[k] * (px0 - g->ex[k]));
            if (cdx == 0.0)
            {
               if (fabs(c0) <= cband)
               {
                  lo[n] = b[0];
                  hi[n++] = b[2];
               }
            }
            else
            {
               double p0 = px0 + (-cband - c0) / cdx, p1 = px0 + (cband - c0) / cdx;
               if (p0 > p1)
               {
                  const double m = p0;
                  p0 = p1;
                  p1 = m;
               }
               geo_span(p0, p1, ox, b[0], b[2], &lo[n], &hi[n]);
               if (lo[n] <= hi[n])
                  n++;
            }
         }
         /* the spans in order, each pixel once */
         for (i = 1; i < n; i++)
            for (j = i; j > 0 && lo[j - 1] > lo[j]; j--)
            {
               int m = lo[j]; lo[j] = lo[j - 1]; lo[j - 1] = m;
               m = hi[j]; hi[j] = hi[j - 1]; hi[j - 1] = m;
            }
         end = b[0] - 1;
         for (i = 0; i < n; i++)
         {
            for (x = lo[i] > end + 1 ? lo[i] : end + 1; x <= hi[i]; x++)
               geo_sample(r, g, x, y, samples, s, t);
            if (hi[i] > end)
               end = hi[i];
         }
         /* and the grid, where the spans are not */
         if ((y - b[1]) % geo_grid == 0)
            for (x = b[0]; x <= b[2]; x += geo_grid)
            {
               int in = 0;
               for (i = 0; i < n && !in; i++)
                  in = x >= lo[i] && x <= hi[i];
               if (!in)
                  geo_sample(r, g, x, y, samples, s, t);
            }
      }
}

static Rec *geo_recs;
static Geo *geo_geos;
static struct geo_tri *geo_tris;
static unsigned geo_room;

static int geo_frame(const char *name, unsigned samples, struct geo_count *tc)
{
   const unsigned prims = F[PVRC_F_PRIMS], w = F[PVRC_F_WIDTH], h = F[PVRC_F_HEIGHT];
   unsigned p, shown = 0;
   int ip;
   struct geo_count c;
   if (prims > geo_room)
   {
      free(geo_recs);
      free(geo_geos);
      free(geo_tris);
      geo_recs = (Rec *)malloc(sizeof(Rec) * prims);
      geo_geos = (Geo *)malloc(sizeof(Geo) * prims);
      geo_tris = (struct geo_tri *)malloc(sizeof(struct geo_tri) * prims);
      geo_room = prims;
      if (!geo_recs || !geo_geos || !geo_tris)
      {
         geo_room = 0;
         return 0;
      }
   }
   memset(&c, 0, sizeof(c));
   for (p = 0; p < prims; p++)
   {
      setup(p, &geo_recs[p]);
      geo_setup(p, &geo_geos[p]);
   }
   /* (the ones left out marked as neither live) */
   for (p = 0; p < prims; p++)
      if (geo_recs[p].live != geo_geos[p].live)
         geo_geos[p].far = 1;
      else if (p % geo_stride && !geo_geos[p].far)
         geo_recs[p].live = geo_geos[p].live = 0;
#pragma omp parallel for schedule(dynamic, 16)
   for (ip = 0; ip < (int)prims; ip++)
   {
      int b[4];
      if (!geo_recs[ip].live && !geo_geos[ip].live)
      {
         memset(&geo_tris[ip], 0, sizeof(geo_tris[ip]));
         continue;
      }
      geo_box(&geo_recs[ip], &geo_geos[ip], w, h, b);
      geo_tri(&geo_recs[ip], &geo_geos[ip], b, samples, &geo_tris[ip]);
   }
   for (p = 0; p < prims; p++)
   {
      const Rec *r = &geo_recs[p];
      const Geo *g = &geo_geos[p];
      const struct geo_tri *t = &geo_tris[p];
      if (!r->live && !g->live)
         continue;
      c.tris++;
      c.far += (unsigned)g->far;
      c.samples += t->samples;
      c.covered += t->covered;
      c.near += t->near;
      if (t->over || t->under)
      {
         c.faulty++;
         c.over += t->over;
         c.under += t->under;
         if (geo_verbose || shown++ < 8)
         {
            unsigned k;
            printf("  %s, %u samples: triangle %u (%s %u, shift %u): %llu samples covered that are not, %llu not that are; vertices",
                  name, samples, p, (r->src & PVRC_SRC_MODVOL) ? "volume" : "polygon", r->src & ~PVRC_SRC_MODVOL, r->shift, t->over, t->under);
            for (k = 0; k < 3; k++)
               printf(" (%.6g, %.6g, 1/w %g)", g->x[k], g->y[k], (double)g->q[k]);
            printf("\n");
         }
      }
   }
   printf("tile_geo: %s at %u samples: %ux%u, %llu triangles (%llu far out or behind the eye), %llu samples looked at, %llu covered; "
         "%llu covered that are not, %llu not that are (%llu triangles); %llu within %g px of an edge or the near clip not counted\n",
         name, samples, w, h, c.tris, c.far, c.samples, c.covered, c.over, c.under, c.faulty, c.near, geo_eps);
   tc->tris += c.tris; tc->far += c.far; tc->samples += c.samples; tc->covered += c.covered; tc->near += c.near;
   tc->over += c.over; tc->under += c.under; tc->faulty += c.faulty;
   return 1;
}

static unsigned *geo_load(const char *path)
{
   FILE *f = fopen(path, "rb");
   long n;
   unsigned *w;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   rewind(f);
   w = n >= (long)(PVRC_F_WORDS * 4) ? (unsigned *)malloc((size_t)n) : NULL;
   if (w && fread(w, 1, (size_t)n, f) != (size_t)n)
   {
      free(w);
      w = NULL;
   }
   fclose(f);
   return w;
}

int main(int argc, char **argv)
{
   const char *list = getenv("TILE_SAMPLES");
   const char *stride = getenv("TILE_GEO_STRIDE");
   struct geo_count tc;
   int a, frames = 0, bad = 0;
   if (!list || !*list)
      list = "1 4";
   memset(&tc, 0, sizeof(tc));
   if (stride && atoi(stride) > 1)
      geo_stride = (unsigned)atoi(stride);
   for (a = 1; a < argc; a++)
   {
      if (!strcmp(argv[a], "-s") && a + 1 < argc)
         list = argv[++a];
      else if (!strcmp(argv[a], "-t") && a + 1 < argc)
      {
         a++;
         geo_stride = atoi(argv[a]) > 1 ? (unsigned)atoi(argv[a]) : 1;
      }
      else if (!strcmp(argv[a], "-b") && a + 1 < argc)
         geo_band = atof(argv[++a]);
      else if (!strcmp(argv[a], "-g") && a + 1 < argc)
      {
         a++;
         geo_grid = atoi(argv[a]) > 1 ? atoi(argv[a]) : 1;
      }
      else if (!strcmp(argv[a], "-a"))
         geo_every = 1;
      else if (!strcmp(argv[a], "-e") && a + 1 < argc)
         geo_eps = atof(argv[++a]);
      else if (!strcmp(argv[a], "-v"))
         geo_verbose = 1;
      else
      {
         unsigned *w = geo_load(argv[a]);
         const char *l;
         const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
         if (!w)
         {
            printf("tile_geo: %s: not read\n", argv[a]);
            bad = 1;
            continue;
         }
         F = w;
         if (F[PVRC_F_GPU] != 0)
         {
            /* (the GPU decode's: its starts are not in the frame) */
            printf("tile_geo: %s: a frame of the GPU's decode, left out\n", name);
            free(w);
            continue;
         }
         frames++;
         for (l = list; *l; )
         {
            char *end;
            unsigned long n = strtoul(l, &end, 10);
            if (end == l)
            {
               l++;
               continue;
            }
            l = end;
            if ((n == 1 || n == 2 || n == 4 || n == 8) && !geo_frame(name, (unsigned)n, &tc))
            {
               printf("tile_geo: %s: not the memory\n", name);
               bad = 1;
            }
         }
         free(w);
      }
   }
   (void)tile_ref;
   printf("tile_geo: %d frames, %llu triangles (every %u%s): %llu samples covered that are not, %llu not that are, in %llu triangles; "
         "%llu of %llu samples within %g px of an edge or the near clip not counted\n",
         frames, tc.tris, geo_stride, geo_stride > 1 ? ", and every one far out or behind the eye" : "", tc.over, tc.under, tc.faulty, tc.near, tc.samples, geo_eps);
   if (bad || frames == 0 || tc.over || tc.under)
   {
      printf("tile_geo: FAIL\n");
      return 1;
   }
   printf("tile_geo: PASS\n");
   return 0;
}
