/* The shared rules of tiled rendering and SSAA (core/rend/ssaa_pattern.h,
 * core/rend/tile/pvr_*.h), checked against their definitions:
 * - the sample pattern against the positions the Vulkan and Direct3D
 *   specifications list;
 * - single rate: a sprite's snapped edges against the pixel-centre rule,
 *   every sample of a covered pixel covered and none of another; the
 *   top-left rule, by tilings of a square in which every point has to be
 *   covered exactly once; the flat rule against a test of every vertex;
 * - the resolve against per-channel arithmetic, for every sample count,
 *   in C and by the lanes' butterfly; the uniform test;
 * - the order of translucent fragment keys, ties and odd depths included;
 * - modifier volume parity, on cases worked by hand;
 * - the arithmetic of tiles, chunks and the bin pool, overflow included;
 * - the clamp of the sample count to the budget;
 * - the region array's table of tiles.
 *
 * To run: tools/rend/ssaa_rules_test.sh (which also compiles the headers
 * as GLSL and HLSL when glslangValidator is there). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssaa_pattern.h"
#include "tile/pvr_tile.h"
#include "tile/pvr_resolve.h"
#include "tile/pvr_modvol.h"

static int failures;
static unsigned checks;

static void check(int ok, const char *what, long a, long b)
{
   checks++;
   if (ok)
      return;
   if (failures < 40)
      printf("FAIL: %s (%ld, %ld)\n", what, a, b);
   failures++;
}

static pvr_u32 rnd_state = 12345u;
static pvr_u32 rnd(void)
{
   rnd_state ^= rnd_state << 13;
   rnd_state ^= rnd_state >> 17;
   rnd_state ^= rnd_state << 5;
   return rnd_state;
}

static pvr_u32 float_bits(float f)
{
   pvr_u32 u;
   memcpy(&u, &f, sizeof(u));
   return u;
}

/* ---- the sample pattern ---- */

static void test_pattern(void)
{
   /* as the specifications list them */
   static const float p2[2][2] = { { 0.75f, 0.75f }, { 0.25f, 0.25f } };
   static const float p4[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
   static const float p8[8][2] = { { 0.5625f, 0.3125f }, { 0.4375f, 0.6875f }, { 0.8125f, 0.5625f }, { 0.3125f, 0.1875f },
                                   { 0.1875f, 0.8125f }, { 0.0625f, 0.4375f }, { 0.6875f, 0.9375f }, { 0.9375f, 0.0625f } };
   pvr_u32 n, i, j;

   check(SSAA_SAMPLE_X16(1u, 0u) == 8u && SSAA_SAMPLE_Y16(1u, 0u) == 8u, "1 sample at the centre", 0, 0);
   for (i = 0; i < 2; i++)
      check(SSAA_SAMPLE_X16(2u, i) * 0.0625f == p2[i][0] && SSAA_SAMPLE_Y16(2u, i) * 0.0625f == p2[i][1], "2x pattern", (long)i, 0);
   for (i = 0; i < 4; i++)
      check(SSAA_SAMPLE_X16(4u, i) * 0.0625f == p4[i][0] && SSAA_SAMPLE_Y16(4u, i) * 0.0625f == p4[i][1], "4x pattern", (long)i, 0);
   for (i = 0; i < 8; i++)
      check(SSAA_SAMPLE_X16(8u, i) * 0.0625f == p8[i][0] && SSAA_SAMPLE_Y16(8u, i) * 0.0625f == p8[i][1], "8x pattern", (long)i, 0);

   /* every column and row once, centred on the pixel */
   for (n = 2; n <= 8; n <<= 1)
   {
      pvr_u32 sx = 0, sy = 0;
      for (i = 0; i < n; i++)
      {
         sx += SSAA_SAMPLE_X16(n, i);
         sy += SSAA_SAMPLE_Y16(n, i);
         for (j = 0; j < i; j++)
            check(SSAA_SAMPLE_X16(n, i) != SSAA_SAMPLE_X16(n, j) && SSAA_SAMPLE_Y16(n, i) != SSAA_SAMPLE_Y16(n, j),
                  "samples share no column or row", (long)n, (long)i);
      }
      check(sx == 8u * n && sy == 8u * n, "pattern centred", (long)n, (long)sx);
      check(SSAA_LOG2(n) == (n == 2 ? 1u : n == 4 ? 2u : 3u), "log2 of the count", (long)n, 0);
   }
}

/* ---- single rate ---- */

static void test_sprite_snap(void)
{
   static const struct { float x; int e; } cases[] = {
      { 10.5f, 10 }, { 10.4999f, 10 }, { 10.5001f, 11 }, { 10.0f, 10 }, { 9.51f, 10 }, { 0.5f, 0 },
      { 0.0f, 0 }, { -0.5f, -1 }, { -0.4f, 0 }, { 639.5f, 639 }, { 640.0f, 640 }, { 640.5f, 640 }
   };
   unsigned c, t;

   for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
      check(pvr_snap_edge(cases[c].x) == cases[c].e, "snapped edge", (long)(cases[c].x * 10000), pvr_snap_edge(cases[c].x));
   check(pvr_snap_edge(1e30f) == 1048576, "far right kept to range", pvr_snap_edge(1e30f), 0);
   check(pvr_snap_edge(-1e30f) == -1048576, "far left kept to range", pvr_snap_edge(-1e30f), 0);

   /* pixel i covered iff x0 <= i + 0.5 < x1; and every sample of a covered
    * pixel is in the snapped rectangle, no sample of another */
   for (t = 0; t < 20000; t++)
   {
      float x0 = (float)((int)(rnd() % 2048) - 512) / 16.0f;
      float x1 = x0 + (float)(rnd() % 1024) / 16.0f;
      int e0 = pvr_snap_edge(x0), e1 = pvr_snap_edge(x1), i;
      pvr_u32 n, s;
      for (i = (int)x0 - 2; i < (int)x1 + 3; i++)
      {
         int covered = x0 <= i + 0.5f && i + 0.5f < x1;
         int snapped = e0 <= i && i < e1;
         check(covered == snapped, "sprite pixel coverage", (long)i, (long)(x0 * 16));
         for (n = 1; n <= 8; n <<= 1)
            for (s = 0; s < n; s++)
            {
               float sx = (float)i + SSAA_SAMPLE_X16(n, s) / 16.0f;
               int in = (float)e0 <= sx && sx < (float)e1;
               check(in == covered, "sprite samples follow their pixel", (long)i, (long)s);
            }
      }
   }
}

/* covered count of point (px, py) by triangle a b c, made clockwise */
static int tri_covers(int ax, int ay, int bx, int by, int cx, int cy, int px, int py)
{
   if (pvr_edge_eval(ax, ay, bx, by, cx, cy) < 0)
   {
      int tx = bx, ty = by;
      bx = cx; by = cy;
      cx = tx; cy = ty;
   }
   if (pvr_edge_eval(ax, ay, bx, by, cx, cy) == 0)
      return 0;	/* no area: setup drops it */
   return pvr_edge_inside(pvr_edge_eval(ax, ay, bx, by, px, py), bx - ax, by - ay)
       && pvr_edge_inside(pvr_edge_eval(bx, by, cx, cy, px, py), cx - bx, cy - by)
       && pvr_edge_inside(pvr_edge_eval(cx, cy, ax, ay, px, py), ax - cx, ay - cy);
}

static void test_top_left(void)
{
   const int S = 32;
   unsigned t;

   /* the square [0, S) split by either diagonal, and as a fan around a
    * point inside it: every point covered once inside, never outside */
   for (t = 0; t < 40; t++)
   {
      int cx = t < 2 ? 0 : 1 + (int)(rnd() % (S - 1));
      int cy = t < 2 ? 0 : 1 + (int)(rnd() % (S - 1));
      int px, py;
      for (py = -2; py <= S + 2; py++)
         for (px = -2; px <= S + 2; px++)
         {
            int expect = px >= 0 && px < S && py >= 0 && py < S;
            int n;
            if (t == 0)
               n = tri_covers(0, 0, S, 0, S, S, px, py) + tri_covers(0, 0, S, S, 0, S, px, py);
            else if (t == 1)
               n = tri_covers(S, 0, S, S, 0, S, px, py) + tri_covers(0, 0, S, 0, 0, S, px, py);
            else
               n = tri_covers(0, 0, S, 0, cx, cy, px, py) + tri_covers(S, 0, S, S, cx, cy, px, py)
                 + tri_covers(S, S, 0, S, cx, cy, px, py) + tri_covers(0, S, 0, 0, cx, cy, px, py);
            check(n == expect, "top-left rule: covered once", (long)px, (long)py);
         }
   }
   check(pvr_edge_top_left(5, 0) == 1 && pvr_edge_top_left(-5, 0) == 0, "top and bottom edges", 0, 0);
   check(pvr_edge_top_left(0, -5) == 1 && pvr_edge_top_left(0, 5) == 0, "left and right edges", 0, 0);
}

static pvr_u32 flat_bit(const float *z, unsigned n)
{
   pvr_u32 z_and = PVR_FLAT_AND_INIT, z_or = PVR_FLAT_OR_INIT;
   unsigned i;
   for (i = 0; i < n; i++)
   {
      z_and &= float_bits(z[i]);
      z_or |= float_bits(z[i]);
   }
   return PVR_FLAT_RATE_BIT(z_and, z_or);
}

static void test_flat(void)
{
   static const float flat[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
   static const float one_off[4] = { 0.25f, 0.25f, 0.2500001f, 0.25f };
   static const float signs[2] = { 0.0f, -0.0f };
   static const float choice[3] = { 1.0f, 0.5f, 1.0000001f };
   unsigned t;

   check(flat_bit(flat, 4) == PVR_PCW_SINGLE_RATE, "equal depths are flat", 0, 0);
   check(flat_bit(one_off, 4) == 0, "one depth off is not flat", 0, 0);
   check(flat_bit(signs, 2) == 0, "zero and minus zero are not the same bits", 0, 0);
   check(flat_bit(flat, 1) == PVR_PCW_SINGLE_RATE, "one vertex is flat", 0, 0);
   check(flat_bit(flat, 0) == 0, "no vertex is not flat", 0, 0);
   /* the bit is one of the control word's reserved bits (8 to 15), and
    * none of those the renderers compare polygons by (PCW_DRAW_MASK) */
   check((PVR_PCW_SINGLE_RATE & 0xff00u) == PVR_PCW_SINGLE_RATE && (PVR_PCW_SINGLE_RATE & 0xceu) == 0,
         "single-rate bit is a reserved bit", (long)PVR_PCW_SINGLE_RATE, 0);

   for (t = 0; t < 20000; t++)
   {
      float z[12];
      unsigned n = 1 + rnd() % 12, i, all_same = 1;
      for (i = 0; i < n; i++)
      {
         z[i] = choice[rnd() % (t & 1 ? 3 : 1 + (rnd() & 1))];
         if (float_bits(z[i]) != float_bits(z[0]))
            all_same = 0;
      }
      check(flat_bit(z, n) == (all_same ? PVR_PCW_SINGLE_RATE : 0), "flat iff every depth is the same", (long)n, 0);
   }
}

/* ---- the resolve ---- */

static pvr_u32 resolve_ref(const pvr_u32 *s, pvr_u32 n)
{
   pvr_u32 out = 0, ch, i;
   for (ch = 0; ch < 4; ch++)
   {
      pvr_u32 sum = 0;
      for (i = 0; i < n; i++)
         sum += (s[i] >> (8 * ch)) & 255u;
      out |= ((sum + n / 2) / n) << (8 * ch);
   }
   return out;
}

static void test_resolve(void)
{
   pvr_u32 n, t, i;
   pvr_u32 s[8];

   for (n = 1; n <= 8; n <<= 1)
   {
      const pvr_u32 log2n = SSAA_LOG2(n);
      for (t = 0; t < 100000; t++)
      {
         pvr_u32 mode = t % 4, r;
         for (i = 0; i < n; i++)
         {
            if (mode == 0)
               s[i] = rnd();
            else if (mode == 1)	/* near the rounding points */
               s[i] = (rnd() & 0x03030303u) * (t & 1 ? 1u : 0x3fu);
            else if (mode == 2)
               s[i] = (rnd() & 1) ? 0xffffffffu : 0u;
            else
               s[i] = i == 0 ? rnd() : s[0] ^ ((rnd() % 7 == 0) ? (1u << (rnd() % 32)) : 0u);
         }
         r = pvr_resolve_samples(s, log2n);
         check(r == resolve_ref(s, n), "resolve: (sum + N/2) / N per channel", (long)n, (long)t);
         check(pvr_resolve_samples_uniform(s, log2n) == (memcmp(s, s + 1, (n - 1) * sizeof(s[0])) == 0 ? 1u : 0u),
               "uniform test", (long)n, (long)t);
         if (pvr_resolve_samples_uniform(s, log2n))
            check(r == s[0], "a uniform pixel resolves to its value", (long)n, (long)t);
      }
   }
   /* rounding by hand: half up */
   s[0] = 0; s[1] = 1;
   check(pvr_resolve_samples(s, 1) == 1, "2x: 0 and 1 give 1", 0, 0);
   s[0] = 0; s[1] = 1; s[2] = 0; s[3] = 0;
   check(pvr_resolve_samples(s, 2) == 0, "4x: sum 1 gives 0", 0, 0);
   s[2] = 1;
   check(pvr_resolve_samples(s, 2) == 1, "4x: sum 2 gives 1", 0, 0);
   for (i = 0; i < 8; i++)
      s[i] = 0xffffffffu;
   check(pvr_resolve_samples(s, 3) == 0xffffffffu, "8x: white stays white", 0, 0);

   /* the lanes' butterfly (pvr_resolve_lanes() and its uniform test), on
    * 32 lanes in lock step, against the resolve in C */
   for (n = 1; n <= 8; n <<= 1)
   {
      const pvr_u32 log2n = SSAA_LOG2(n);
      for (t = 0; t < 2000; t++)
      {
         pvr_u32 c[32], rb[32], ga[32], nrb[32], nga[32], diff[32], ndiff[32], m, l;
         for (l = 0; l < 32; l++)
         {
            c[l] = (t & 1) ? rnd() : ((l / n) * 0x01020304u + ((rnd() % 5 == 0) ? 1u : 0u));
            rb[l] = pvr_resolve_rb(c[l]);
            ga[l] = pvr_resolve_ga(c[l]);
            diff[l] = 0;
         }
         for (m = 1; m < n; m <<= 1)
         {
            for (l = 0; l < 32; l++)
            {
               nrb[l] = rb[l] + pvr_lane_c_shuffle_xor(rb, l, m);
               nga[l] = ga[l] + pvr_lane_c_shuffle_xor(ga, l, m);
               ndiff[l] = diff[l] | (c[l] != pvr_lane_c_shuffle_xor(c, l, m) ? 1u : 0u);
            }
            for (l = 0; l < 32; l++)
               diff[l] = ndiff[l] | pvr_lane_c_shuffle_xor(ndiff, l, m);
            memcpy(rb, nrb, sizeof(rb));
            memcpy(ga, nga, sizeof(ga));
         }
         for (l = 0; l < 32; l++)
         {
            const pvr_u32 *px = c + (l & ~(n - 1));
            check(pvr_resolve_join(rb[l], ga[l], log2n) == pvr_resolve_samples(px, log2n), "butterfly resolve", (long)n, (long)l);
            check((diff[l] ^ 1u) == pvr_resolve_samples_uniform(px, log2n), "butterfly uniform test", (long)n, (long)l);
         }
      }
   }
   /* the C lane helpers */
   {
      pvr_u32 cond[32], l, ref = 0;
      for (l = 0; l < 32; l++)
      {
         cond[l] = rnd() & 1;
         if (cond[l])
            ref |= 1u << l;
      }
      check(pvr_lane_c_ballot(cond, 32) == ref, "ballot", 0, 0);
      for (l = 0; l < 32; l++)
      {
         pvr_u32 k, cnt = 0;
         for (k = 0; k < l; k++)
            cnt += cond[k];
         check(pvr_lane_c_prefix_count(cond, 32, l) == cnt, "prefix count", (long)l, 0);
      }
   }
}

/* ---- fragment key order ---- */

struct frag { float z; pvr_u32 prim, hi, lo; };

static float clamp_neg(float z)
{
   return (float_bits(z) & 0x80000000u) ? 0.0f : z;
}

static void test_keys(void)
{
   static const float depths[] = { 0.5f, 0.25f, 0.5f, 1.0f, 0.0f, -0.0f, -2.0f, 1e-30f, 3.0e38f, 0.25f, 0.5f };
   struct frag f[64];
   unsigned nf, t, i, j, nearest;

   for (nearest = 0; nearest < 2; nearest++)
      for (t = 0; t < 2000; t++)
      {
         nf = 2 + rnd() % 60;
         for (i = 0; i < nf; i++)
         {
            f[i].z = depths[rnd() % (sizeof(depths) / sizeof(depths[0]))];
            f[i].prim = i;	/* sent in this order */
            f[i].hi = pvr_key_hi(f[i].z, nearest);
            f[i].lo = pvr_key_lo(f[i].prim, rnd(), rnd());
         }
         /* shuffled, then sorted by the keys */
         for (i = nf - 1; i > 0; i--)
         {
            struct frag tmp;
            j = rnd() % (i + 1);
            tmp = f[i]; f[i] = f[j]; f[j] = tmp;
         }
         for (i = 1; i < nf; i++)
         {
            struct frag v = f[i];
            for (j = i; j > 0 && pvr_key_before(v.hi, v.lo, f[j - 1].hi, f[j - 1].lo); j--)
               f[j] = f[j - 1];
            f[j] = v;
         }
         for (i = 1; i < nf; i++)
         {
            float a = clamp_neg(f[i - 1].z), b = clamp_neg(f[i].z);
            int ordered = nearest ? a > b || (a == b && f[i - 1].prim < f[i].prim)
                                  : a < b || (a == b && f[i - 1].prim < f[i].prim);
            check(ordered, nearest ? "keys: nearest first, ties in sending order" : "keys: farthest first, ties in sending order",
                  (long)(a * 1000), (long)(b * 1000));
         }
      }
   check(PVR_KEY_PRIM(pvr_key_lo(0x1abcd, 1, 0x1234)) == 0xabcd && PVR_KEY_PARITY(pvr_key_lo(0x1abcd, 1, 0x1234)) == 1
         && PVR_KEY_FLAGS(pvr_key_lo(0x1abcd, 1, 0x1234)) == 0x1234, "key fields", 0, 0);
   check(pvr_key_hi(-0.0f, 0) == 0 && pvr_key_hi(-1.0f, 0) == 0, "negative depths count as zero", 0, 0);
}

/* ---- modifier volumes ---- */

/* a sample's state after a volume of parameters, each of @count
 * triangles at depths @z (all over the sample on the screen unless
 * @inside says), in mode @mode, the last parameter the volume's last */
static pvr_u32 run_param(pvr_u32 state, const float *z, const pvr_u32 *inside, unsigned count,
                         pvr_u32 mode, pvr_u32 volume_last, float depth)
{
   const pvr_u32 or_mode = pvr_modvol_or_mode(volume_last, mode);
   unsigned i;
   for (i = 0; i < count; i++)
      state = pvr_modvol_triangle(state, pvr_modvol_hit(inside ? inside[i] : 1u, z[i], depth), or_mode);
   return pvr_modvol_close(state, mode);
}

static void test_modvol(void)
{
   /* a closed box between 1/w 0.4 (back) and 0.6 (front): a front and a
    * back face over the sample */
   static const float box[2] = { 0.6f, 0.4f };
   static const float box2[2] = { 0.3f, 0.2f };
   static const float quad[1] = { 0.5f };
   pvr_u32 s;

   /* inclusion: inside only for a surface between the faces */
   check(pvr_modvol_shadowed(run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.7f)) == 0, "surface in front of the box", 0, 0);
   check(pvr_modvol_shadowed(run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f)) == 1, "surface inside the box", 0, 0);
   check(pvr_modvol_shadowed(run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.3f)) == 0, "surface behind the box", 0, 0);
   check(pvr_modvol_shadowed(run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.6f)) == 0, "a face at the surface's depth does not hit", 0, 0);
   check((run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f) & PVR_MV_CURRENT) == 0, "closing clears the volume bit", 0, 0);

   /* a second inclusion volume keeps the first's inside */
   s = run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f);
   s = run_param(s, box2, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f);
   check(pvr_modvol_shadowed(s) == 1, "inclusion: inside one of two volumes", 0, 0);
   s = run_param(0, box2, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f);
   check(pvr_modvol_shadowed(s) == 0, "inclusion: inside none", 0, 0);

   /* exclusion: inside before and not in this one */
   s = run_param(0, box2, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.25f);	/* inside box2 */
   check(pvr_modvol_shadowed(run_param(s, box, NULL, 2, PVR_MV_MODE_EXCLUSION, 1, 0.25f)) == 1, "exclusion: outside the excluding volume", 0, 0);
   s = run_param(0, box, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.5f);
   check(pvr_modvol_shadowed(run_param(s, box, NULL, 2, PVR_MV_MODE_EXCLUSION, 1, 0.5f)) == 0, "exclusion: inside the excluding volume", 0, 0);

   /* a volume sent as several parameters: normal ones XOR, the last
    * closes */
   s = run_param(0, box, NULL, 1, PVR_MV_MODE_NORMAL, 0, 0.5f);
   check(s == PVR_MV_CURRENT, "a normal parameter flips the volume bit and closes nothing", (long)s, 0);
   s = run_param(s, box + 1, NULL, 1, PVR_MV_MODE_INCLUSION, 1, 0.5f);
   check(s == PVR_MV_INSIDE, "the volume's last parameter closes it", (long)s, 0);

   /* open volume / quad: OR, so two hits stay inside */
   check(pvr_modvol_or_mode(0, PVR_MV_MODE_INCLUSION) == 1 && pvr_modvol_or_mode(1, PVR_MV_MODE_INCLUSION) == 0
         && pvr_modvol_or_mode(0, PVR_MV_MODE_NORMAL) == 0, "OR mode", 0, 0);
   {
      static const float twice[2] = { 0.5f, 0.5f };
      check(pvr_modvol_shadowed(run_param(0, twice, NULL, 2, PVR_MV_MODE_INCLUSION, 0, 0.4f)) == 1, "open volume: two hits stay in", 0, 0);
      check(pvr_modvol_shadowed(run_param(0, twice, NULL, 2, PVR_MV_MODE_INCLUSION, 1, 0.4f)) == 0, "closed volume: two hits cancel", 0, 0);
      check(pvr_modvol_shadowed(run_param(0, quad, NULL, 1, PVR_MV_MODE_INCLUSION, 0, 0.4f)) == 1, "quad over the surface", 0, 0);
   }
   /* the parameter of a mode that is not inclusion or exclusion leaves
    * the summary as it is */
   check(pvr_modvol_close(PVR_MV_INSIDE | PVR_MV_CURRENT, PVR_MV_MODE_NORMAL) == (PVR_MV_INSIDE | PVR_MV_CURRENT), "normal mode closes nothing", 0, 0);

   /* a box's front face made of two triangles: a sample on their shared
    * edge is hit by exactly one, so it is still inside the box */
   {
      int px, py;
      for (py = 0; py <= 16; py++)
         for (px = 0; px <= 16; px++)
         {
            pvr_u32 in[4];
            float z[4] = { 0.6f, 0.6f, 0.4f, 0.4f };
            in[0] = (pvr_u32)tri_covers(0, 0, 16, 0, 16, 16, px, py);	/* front, two halves */
            in[1] = (pvr_u32)tri_covers(0, 0, 16, 16, 0, 16, px, py);
            in[2] = (pvr_u32)tri_covers(0, 0, 16, 16, 16, 0, px, py);	/* back, split the same way */
            in[3] = (pvr_u32)tri_covers(0, 0, 0, 16, 16, 16, px, py);
            s = run_param(0, z, in, 4, PVR_MV_MODE_INCLUSION, 1, 0.5f);
            check(pvr_modvol_shadowed(s) == (px < 16 && py < 16 ? 1u : 0u), "shared edges counted once", (long)px, (long)py);
         }
   }
}

/* ---- tiles, chunks, bin pool ---- */

static void test_bins(void)
{
   pvr_u32 counts[24], offset[24], placed[24], pool = 100, passes = 0, i, done = 0;

   check(pvr_tiles_across(640, 5) == 20 && pvr_tiles_across(480, 5) == 15 && pvr_tiles_across(641, 5) == 21, "tiles across", 0, 0);
   check(pvr_tiles_across(5120, 6) == 80 && pvr_tiles_across(1, 4) == 1 && pvr_tiles_across(0, 5) == 0, "tiles across, other sizes", 0, 0);
   check(pvr_chunk_count(0) == 0 && pvr_chunk_count(1) == 1 && pvr_chunk_count(65536) == 1 && pvr_chunk_count(65537) == 2,
         "chunks", 0, 0);
   check(pvr_chunk_count(0xffffffffu) == 65536u, "chunks of the most primitives", (long)pvr_chunk_count(0xffffffffu), 0);
   check(PVR_CHUNK_OF(65537u) == 1 && PVR_CHUNK_INDEX(65537u) == 1 && PVR_CHUNK_INDEX(65535u) == 65535u, "chunk and index", 0, 0);
   /* the design's figure: 20x15 tiles, all 64K primitives in each */
   check(pvr_bin_worst_bytes(300, 65536) == 39321600u, "worst-case bins at 1x", (long)pvr_bin_worst_bytes(300, 65536), 0);
   check(pvr_bin_worst_bytes(1u << 20, 65536) == 0xffffffffu, "worst case saturates", 0, 0);
   check(pvr_bin_fits(0, 100, 100) == 1 && pvr_bin_fits(1, 100, 100) == 0 && pvr_bin_fits(100, 0, 100) == 1, "fits at the end", 0, 0);
   check(pvr_bin_fits(0xfffffff0u, 0x20u, 0xffffffffu) == 0, "a run that wraps around does not fit", 0, 0);
   check(pvr_bin_fits(0, 101, 100) == 0, "a run larger than the pool", 0, 0);

   /* a frame whose bins overflow the pool: each pass places the tiles
    * whose runs fit after a prefix sum over those not placed yet, marks
    * the others, and goes again over the same pool */
   for (i = 0; i < 24; i++)
   {
      counts[i] = 3 + (i * 7) % 29;
      placed[i] = 0;
   }
   while (done < 24 && passes < 24)
   {
      pvr_u32 at = 0, used = 0;
      passes++;
      for (i = 0; i < 24; i++)
      {
         if (placed[i])
            continue;
         offset[i] = at;
         at += counts[i];	/* (the prefix sum) */
         if (pvr_bin_fits(offset[i], counts[i], pool))
         {
            placed[i] = passes;
            used += counts[i];
            done++;
         }
      }
      check(used <= pool, "a pass stays inside the pool", (long)used, (long)pool);
   }
   check(done == 24, "every tile placed", (long)done, 0);
   {
      pvr_u32 total = 0;
      for (i = 0; i < 24; i++)
         total += counts[i];
      /* never more passes than tiles, and never fewer than the pool allows */
      check(passes >= (total + pool - 1) / pool && passes <= 24, "overflow passes", (long)passes, (long)total);
   }
}

/* ---- budget ---- */

static void test_budget(void)
{
   check(ssaa_per_sample_kib(640, 480, 4) == 1200, "640x480 RGBA8", (long)ssaa_per_sample_kib(640, 480, 4), 0);
   check(ssaa_per_sample_kib(5120, 3840, 8) == 153600, "4x internal, colour and depth", (long)ssaa_per_sample_kib(5120, 3840, 8), 0);
   check(ssaa_per_sample_kib(1, 1, 1) == 1, "rounded up", 0, 0);
   check(ssaa_per_sample_kib(0, 480, 4) == 0, "nothing", 0, 0);
   check(ssaa_per_sample_kib(32768, 65536, 4) == 8388608u, "8 GB, in KiB first", (long)ssaa_per_sample_kib(32768, 65536, 4), 0);
   check(ssaa_per_sample_kib(65536, 65536, 1) == 0xffffffffu, "more pixels than 32 bits count", 0, 0);
   check(ssaa_per_sample_kib(0xffffffffu, 0xffffffffu, 4) == 0xffffffffu, "saturates", 0, 0);

   check(ssaa_clamp_samples(8, 1000000, 100, 1200) == 8, "fits: 8x", 0, 0);
   check(ssaa_clamp_samples(8, 3000, 500, 1200) == 2, "clamped to 2x", (long)ssaa_clamp_samples(8, 3000, 500, 1200), 0);
   check(ssaa_clamp_samples(4, 5300, 500, 1200) == 4, "exactly 4x", (long)ssaa_clamp_samples(4, 5300, 500, 1200), 0);
   check(ssaa_clamp_samples(4, 5299, 500, 1200) == 2, "a KiB short of 4x", (long)ssaa_clamp_samples(4, 5299, 500, 1200), 0);
   check(ssaa_clamp_samples(8, 400, 500, 1200) == 1, "fixed part alone too much", 0, 0);
   check(ssaa_clamp_samples(8, 1000, 0, 0) == 8, "nothing per sample", 0, 0);
   check(ssaa_clamp_samples(3, 1000000, 0, 1) == 2, "3 counts as 2", 0, 0);
   check(ssaa_clamp_samples(0, 1000000, 0, 1) == 1, "0 counts as 1", 0, 0);
   check(ssaa_clamp_samples(16, 1000000, 0, 1) == 8, "at most 8", 0, 0);
   /* what the device can draw with: every request against every set of counts */
   {
      pvr_u32 req, counts;
      for (req = 0; req <= 16; req++)
         for (counts = 0; counts < 512; counts++)
         {
            pvr_u32 n = ssaa_device_samples(req, counts), m;
            pvr_u32 want = req >= 8 ? 8 : req >= 4 ? 4 : req >= 2 ? 2 : 1;
            /* the largest of 1, 2, 4, 8 that is no more than asked and that the device has; 1 if none */
            pvr_u32 ref = 1;
            for (m = 2; m <= want; m <<= 1)
               if (counts & m)
                  ref = m;
            check(n == ref, "the device's most samples, no more than asked", (long)req, (long)counts);
         }
   }
   check(ssaa_device_samples(8, 0x05) == 4, "lavapipe's 1 and 4: 8x is 4x", 0, 0);
   check(ssaa_device_samples(2, 0x05) == 1, "lavapipe's 1 and 4: 2x is off", 0, 0);
}

/* ---- the region array's table ---- */

static pvr_u32 control(pvr_u32 x, pvr_u32 y, pvr_u32 flags)
{
   return (x << 2) | (y << 8) | (flags << 28);
}

static void test_region(void)
{
   /* tile (0,0) three passes, (1,0) two, (0,1) one, the last */
   static const pvr_u32 xy[6][3] = { { 0, 0, 0 }, { 0, 0, 4 }, { 0, 0, 1 }, { 1, 0, 2 }, { 1, 0, 6 }, { 0, 1, 8 } };
   static const pvr_u32 pass[6] = { 0, 1, 2, 0, 1, 0 };
   pvr_u32 prev = 0, e, i;

   for (i = 0; i < 6; i++)
   {
      e = pvr_region_entry(control(xy[i][0], xy[i][1], xy[i][2]), prev, i == 0);
      check(PVR_RGN_X(e) == xy[i][0] && PVR_RGN_Y(e) == xy[i][1], "region entry tile", (long)i, (long)e);
      check(PVR_RGN_PASS(e) == pass[i], "region entry pass", (long)i, (long)PVR_RGN_PASS(e));
      check(((e & PVR_RGN_FLUSH_ACCUM) != 0) == ((xy[i][2] & 1) != 0) && ((e & PVR_RGN_PRESORT) != 0) == ((xy[i][2] & 2) != 0)
            && ((e & PVR_RGN_NO_Z_CLEAR) != 0) == ((xy[i][2] & 4) != 0) && ((e & PVR_RGN_LAST) != 0) == ((xy[i][2] & 8) != 0),
            "region entry flags", (long)i, (long)e);
      prev = e;
   }
   /* the reserved bits of the control word do not leak in */
   e = pvr_region_entry(control(63, 63, 15) | 0x0fff0003u, 0, 1);
   check(PVR_RGN_X(e) == 63 && PVR_RGN_Y(e) == 63 && (e & 0xfff00000u) == 0 && PVR_RGN_PASS(e) == 0, "reserved bits ignored", (long)e, 0);
   /* a tile's passes saturate, and the first entry starts again */
   prev = 0;
   for (i = 0; i < 20; i++)
      prev = pvr_region_entry(control(5, 7, 0), prev, i == 0);
   check(PVR_RGN_PASS(prev) == PVR_RGN_PASS_MAX, "passes saturate", (long)PVR_RGN_PASS(prev), 0);
   check(PVR_RGN_PASS(pvr_region_entry(control(5, 7, 0), prev, 1)) == 0, "first entry is pass 0", 0, 0);
}

int main(void)
{
   test_pattern();
   test_sprite_snap();
   test_top_left();
   test_flat();
   test_resolve();
   test_keys();
   test_modvol();
   test_bins();
   test_budget();
   test_region();
   printf("ssaa_rules_test: %u checks, %d failed\n", checks, failures);
   return failures ? 1 : 0;
}
