/* What tile_test.c and tile_test_d3d12.c have in common: the frames, the
 * reference, the comparison (main()). Each has the device's side:
 *   init(DIR, validate)       the device, and the kernels from DIR
 *   gpu_draw(...)             a frame drawn by the kernels (see there)
 * and sets shared_bytes (what the device gives a workgroup: the walk's K)
 * and counts validation_errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pvr_compute.h"
#include "tile_ref.h"

static int validation_errors;
static unsigned shared_bytes;

/* a clock, in milliseconds */
#if defined(_WIN32)
#include <windows.h>
static double now_ms(void)
{
   LARGE_INTEGER c, f;
   QueryPerformanceCounter(&c);
   QueryPerformanceFrequency(&f);
   return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
}
#else
static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
#endif

static void init(const char *dir, int validate);
/* The frame drawn by the kernels at @samples, keeping @keep: into @out;
 * the stats words into @stats. With @gpu, as a frame of the decode's
 * (gpu_frame()); 0 if it did not come to the triangles it should. The
 * milliseconds it took. */
static double gpu_draw(const unsigned *frame0, unsigned words, unsigned samples, unsigned keep, unsigned *out, unsigned *stats, int gpu);

static unsigned char *read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   unsigned char *p;
   long n;
   if (!f) { printf("no %s\n", path); exit(2); }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   p = (unsigned char *)malloc((size_t)n + 4);
   if (fread(p, 1, (size_t)n, f) != (size_t)n) exit(2);
   fclose(f);
   *size = (size_t)n;
   return p;
}

/* A frame of the parse's as a frame of the decode's would be (see the top):
 * the words into @g (as many as the frame's), the decode's commands,
 * translucent draw records and frame words into @cmd, @trd and @dfrm
 * (room for as many as the frame has entries, words of the frame and
 * PVRC_TAG_O_ZSCALE + 1); the number of triangles it comes to */
static unsigned gpu_frame(const unsigned *frame, unsigned words, unsigned *g, unsigned *cmd, unsigned *trd, unsigned *dfrm)
{
   const unsigned n = frame[PVRC_F_SOURCES], osrc = frame[PVRC_F_O_SRC], total = frame[PVRC_F_PRIMS];
   const unsigned passes = frame[PVRC_F_PASSES], omvp = frame[PVRC_F_O_MVP], opoly = frame[PVRC_F_O_POLY];
   unsigned *map = (unsigned *)malloc((n + 1) * sizeof(unsigned));
   unsigned s, k, m = 0, ncmd = 0, ntrd = 0, prims = 0, group = PVRC_NONE, prev_end = 0, prev_poly = 0;
   memcpy(g, frame, (size_t)words * 4);
   memset(dfrm, 0, (PVRC_TAG_O_ZSCALE + 1) * 4);
   dfrm[PVRC_TAG_O_ZSCALE] = 0x3f800000u;	/* (1: the frame's PVRC_F_ZCLIP stays as it is) */
   for (s = 0; s < n; s++)
   {
      const unsigned *e = frame + osrc + s * PVRC_SRC_WORDS;
      const unsigned start = e[0], end = s + 1 < n ? e[PVRC_SRC_WORDS] : total, cnt = end - start;
      unsigned *o = g + osrc + m * PVRC_SRC_WORDS;
      int boundary = 0;
      for (k = 0; k < passes * PVRC_PASS_WORDS; k++)
         if (k % PVRC_PASS_WORDS != PVRC_P_FLAGS && frame[PVRC_F_PASS + k] == start)
            boundary = 1;
      if (e[1] & PVRC_SRC_MODVOL)
      {
         o[0] = 0; o[1] = e[1]; o[2] = cnt;
         group = PVRC_NONE;
         prims += cnt;
         map[s] = m++;
         continue;
      }
      {
         const unsigned poly = opoly + (e[1] & PVRC_SRC_INDEX) * PVRC_POLY_WORDS;
         if (frame[poly + PVRC_Q_LIST] != 2)
         {
            unsigned *c = cmd + ncmd * PVRC_TAG_C_WORDS;
            c[PVRC_TAG_C_COUNT] = cnt + 2; c[1] = 1; c[PVRC_TAG_C_FIRST] = e[2]; c[3] = 0; c[4] = 0;
            o[0] = 0; o[1] = e[1]; o[2] = ncmd++;
            group = PVRC_NONE;
            prims += cnt;
            map[s] = m++;
            continue;
         }
         /* a translucent strip: in the group before if it follows on from
          * its last strip with the same parameters */
         if (group != PVRC_NONE && !boundary && prev_end == e[2]
               && memcmp(frame + prev_poly, frame + poly, PVRC_Q_FIRST * 4) == 0
               && frame[prev_poly + PVRC_Q_LIST] == frame[poly + PVRC_Q_LIST])
         {
            trd[group * PVRC_TAG_C_WORDS]++;
            prims += cnt + 2;	/* (the two that reach from the strip before into it draw nothing) */
            map[s] = m - 1;
         }
         else
         {
            group = ntrd++;
            trd[group * PVRC_TAG_C_WORDS] = 1;
            o[0] = 0; o[1] = e[1] | PVRC_SRC_TRD; o[2] = group;
            prims += cnt;
            map[s] = m++;
         }
         {
            unsigned *c = trd + ntrd * PVRC_TAG_C_WORDS;
            c[PVRC_TAG_C_COUNT] = cnt + 2; c[1] = 1; c[PVRC_TAG_C_FIRST] = e[2]; c[3] = 0; c[4] = 0;
            ntrd++;
         }
         prev_end = e[2] + cnt + 2;
         prev_poly = poly;
      }
   }
   map[n] = m;
   /* the passes' and the volumes' numbers as entries */
   for (k = 0; k < passes * PVRC_PASS_WORDS; k++)
   {
      const unsigned v = frame[PVRC_F_PASS + k];
      if (k % PVRC_PASS_WORDS == PVRC_P_FLAGS)
         continue;
      for (s = 0; s < n && frame[osrc + s * PVRC_SRC_WORDS] != v; s++)
         ;
      g[PVRC_F_PASS + k] = map[s];
   }
   for (k = 0; omvp + k * PVRC_MVP_WORDS < osrc; k++)
   {
      const unsigned v = frame[omvp + k * PVRC_MVP_WORDS + PVRC_M_CLOSE_END];
      if (v == PVRC_NONE)
         continue;
      for (s = 0; s + 1 < n && frame[osrc + (s + 1) * PVRC_SRC_WORDS] <= v; s++)
         ;
      g[omvp + k * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = map[s];
   }
   g[PVRC_F_SOURCES] = m;
   g[PVRC_F_PRIMS] = 0;
   g[PVRC_F_CHUNKS] = 0;
   g[PVRC_F_GPU] = 0x7fffffffu;
   g[PVRC_F_PRIM_ROOM] = prims;
   free(map);
   return prims;
}

/* The reference's picture of @frame (@words) at @n samples, and its
 * counts: drawn, or read from TILE_REF_DIR when it has them from a run
 * before, and kept there when it has not (tile_test.sh names the directory
 * by the reference's preprocessed source, so that the pictures in it are
 * this reference's). Its key is the frame's words' hash and @n. The
 * milliseconds drawing it took into @ms (0 when read). 0 if there was not
 * the memory. */
static int ref_picture(const unsigned *frame, unsigned words, unsigned n, unsigned *ref, struct tile_ref_stats *rst, double *ms)
{
   const char *dir = getenv("TILE_REF_DIR");
   const unsigned w = frame[PVRC_F_WIDTH], h = frame[PVRC_F_HEIGHT];
   unsigned long long key = 1469598103934665603ull;
   char path[1024];
   FILE *f;
   unsigned i;
   for (i = 0; i < words; i++)
      key = (key ^ frame[i]) * 1099511628211ull;
   if (dir)
   {
      snprintf(path, sizeof(path), "%s/%08x%08x_%u.ref", dir, (unsigned)(key >> 32), (unsigned)key, n);
      f = fopen(path, "rb");
      if (f)
      {
         unsigned head[3];
         const int ok = fread(head, sizeof(head), 1, f) == 1 && head[0] == w && head[1] == h && head[2] == (unsigned)sizeof(*rst)
               && fread(rst, sizeof(*rst), 1, f) == 1 && fread(ref, (size_t)w * h * 4, 1, f) == 1;
         fclose(f);
         if (ok)
         {
            *ms = 0.0;
            return 1;
         }
      }
   }
   *ms = now_ms();
   if (!tile_ref(frame, n, NULL, ref, rst))
      return 0;
   *ms = now_ms() - *ms;
   if (dir)
   {
      /* (written whole under another name, then renamed: a run that stops
       * leaves no half of one) */
      char tmp[1100];
      snprintf(tmp, sizeof(tmp), "%s.part", path);
      f = fopen(tmp, "wb");
      if (f)
      {
         const unsigned head[3] = { w, h, (unsigned)sizeof(*rst) };
         const int ok = fwrite(head, sizeof(head), 1, f) == 1 && fwrite(rst, sizeof(*rst), 1, f) == 1 && fwrite(ref, (size_t)w * h * 4, 1, f) == 1;
         if (fclose(f) == 0 && ok)
         {
            remove(path);
            rename(tmp, path);
         }
         else
            remove(tmp);
      }
   }
   return 1;
}

static void write_ppm(const char *dir, const char *name, const unsigned *px, unsigned w, unsigned h)
{
   char path[1024];
   FILE *f;
   unsigned i;
   snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
   f = fopen(path, "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   for (i = 0; i < w * h; i++)
   {
      unsigned char c[3] = { (unsigned char)px[i], (unsigned char)(px[i] >> 8), (unsigned char)(px[i] >> 16) };
      fwrite(c, 1, 3, f);
   }
   fclose(f);
}

#if !defined(TILE_BENCH)	/* (tile_bench.c has its own) */
int main(int argc, char **argv)
{
   const char *samples_s = getenv("TILE_SAMPLES") ? getenv("TILE_SAMPLES") : "1 2 4 8";
   const char *keep_s = getenv("TILE_KEEP") ? getenv("TILE_KEEP") : "0 1";
   const char *ppm = getenv("TILE_PPM");
   int validate = getenv("TILE_VALIDATE") && atoi(getenv("TILE_VALIDATE"));
   const int use_gpu = !getenv("TILE_GPU") || atoi(getenv("TILE_GPU")) != 0;
   int a, runs = 0, same = 0, differ = 0;
   if (argc < 3)
   {
      printf("usage: %s KERNEL_DIR FRAME...\n", argv[0]);
      return 2;
   }
   init(argv[1], validate);
   {
      unsigned sl[8], kl[8], ns = 0, nk = 0;
      char *e;
      const char *p;
      for (p = samples_s; ns < 8; p = e)
      {
         unsigned long v = strtoul(p, &e, 10);
         if (e == p)
            break;
         sl[ns++] = (unsigned)v;
      }
      for (p = keep_s; nk < 8; p = e)
      {
         unsigned long v = strtoul(p, &e, 10);
         if (e == p)
            break;
         kl[nk++] = v == 0 ? pvrc_fragments_kept(shared_bytes) : (unsigned)v;
      }
      for (a = 2; a < argc; a++)
      {
         size_t bytes;
         unsigned *frame = (unsigned *)read_file(argv[a], &bytes);
         const unsigned words = (unsigned)(bytes / 4), w = frame[PVRC_F_WIDTH], h = frame[PVRC_F_HEIGHT];
         unsigned *ref = (unsigned *)malloc((size_t)w * h * 4), *got = (unsigned *)malloc((size_t)w * h * 4);
         unsigned si, ki;
         for (si = 0; si < ns; si++)
         {
            const unsigned n = sl[si];
            struct tile_ref_stats rst;
            double ref_ms;
            if (!ref_picture(frame, words, n, ref, &rst, &ref_ms))
            {
               printf("tile_test: no memory\n");
               return 2;
            }
            for (ki = 0; ki < nk * (use_gpu ? 2 : 1); ki++)
            {
               const unsigned keep = kl[ki % nk];
               const int as_gpu = ki >= nk;
               unsigned stats[PVRC_STATS], i, bad = 0, first = 0;
               double ms = gpu_draw(frame, words, n, keep, got, stats, as_gpu);
               for (i = 0; i < w * h; i++)
                  if (got[i] != ref[i])
                  {
                     if (bad == 0)
                        first = i;
                     bad++;
                  }
               runs++;
               if (bad == 0)
                  same++;
               else
                  differ++;
               printf("tile_test: %s at %u sample%s, K %u%s: %s", argv[a], n, n > 1 ? "s" : "", keep, as_gpu ? ", as the decode's" : "", bad ? "DIFFERENT" : "the same");
               if (bad)
                  printf(" (%u pixels; the first at %u,%u: %08x, the reference %08x)", bad, first % w, first / w, got[first], ref[first]);
               printf("; %u primitives, %u passes, %u layers at most, %u rounds, %u runs walked whole, %u left out, %u clipped at the eye (the device %u), %u passed over by the tiles' depths; device %.1f ms, reference %.1f ms\n",
                     frame[PVRC_F_PRIMS], frame[PVRC_F_PASSES], rst.layers_most, stats[PVRC_ST_PEELS] ? stats[PVRC_ST_PEELS] : 1,
                     stats[PVRC_ST_SKIPPED], stats[PVRC_ST_DROPPED], rst.clipped, stats[PVRC_ST_CLIPPED], stats[PVRC_ST_HIZ], ms, ref_ms);
               if (bad && ppm)
               {
                  char name[256];
                  const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
                  snprintf(name, sizeof(name), "%s_%u_%u%s_gpu", base, n, keep, as_gpu ? "_dec" : "");
                  write_ppm(ppm, name, got, w, h);
                  snprintf(name, sizeof(name), "%s_%u_%u_ref", base, n, keep);
                  write_ppm(ppm, name, ref, w, h);
               }
               fflush(stdout);
            }
         }
         free(ref);
         free(got);
         free(frame);
      }
   }
   printf("tile_test: %d runs, %d the same, %d different; validation errors: %d\n", runs, same, differ, validation_errors);
   printf("tile_test: %s\n", differ == 0 && validation_errors == 0 && runs > 0 ? "PASS" : "FAIL");
   return differ == 0 && validation_errors == 0 && runs > 0 ? 0 : 1;
}
#endif
