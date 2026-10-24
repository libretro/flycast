/* The compute renderer's kernels timed on frames tile_dump wrote: each
 * frame drawn by one or more sets of kernels, as the renderer draws it
 * (production mode, PVRC_TEST 0), the sets taking turns round after round
 * so that a busy machine slows them alike, and for each set the medians of
 * the GPU's time for each kernel (timestamps), the whole submission and
 * the process's CPU time over it. The pictures each set drew are compared
 * with the first set's, word for word: a change made for speed has to
 * leave them as they are.
 *
 *   tile_bench SET[,SET...] FRAME...
 *
 * A SET is a directory with k1.spv to k7.spv, the kernels as
 * kernels_spv.sh makes them (tile_bench.sh). The environment:
 * BENCH_SAMPLES (default "1"), BENCH_ROUNDS (default 5), BENCH_KEEP (the
 * translucent fragments kept at a time; default the device's), BENCH_PPM a
 * directory to write the first set's pictures to. The textures are one
 * mipmapped texture filtered trilinearly, so that the derivatives the walk
 * gives the sampler show in the pictures. */
#define TILE_BENCH
#include "tile_test.c"

#define BENCH_SETS 8
#define BENCH_STAMPS 8

static unsigned char *set_spv[BENCH_SETS][PVRC_KERNELS + 1];
static int set_variants[BENCH_SETS];	/* (the set's walk has its variants: a file "variants" in its directory) */
static int set_lists[BENCH_SETS];	/* (...dispatched over the sort's lists of tiles: a file "lists") */
static size_t set_spv_size[BENCH_SETS][PVRC_KERNELS + 1];

static double cpu_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
   return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
   const double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

static double median(double *v, unsigned n)
{
   qsort(v, n, sizeof(*v), cmp_double);
   return n == 0 ? 0.0 : (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* a 64 x 64 texture with its 7 levels, each its own pattern, alpha too */
static struct image make_texture(void)
{
   struct image im;
   VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   struct buffer staging = make_buffer(64 * 64 * 4 * 2, 0);
   unsigned *p = (unsigned *)staging.mapped, level, off = 0, x, y;
   VkBufferImageCopy copies[7];
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = VK_FORMAT_R8G8B8A8_UNORM;
   ci.extent.width = ci.extent.height = 64;
   ci.extent.depth = 1;
   ci.mipLevels = 7;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   CHECK(vkCreateImage(device, &ci, NULL, &im.image));
   vkGetImageMemoryRequirements(device, im.image, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
   CHECK(vkAllocateMemory(device, &ai, NULL, &im.memory));
   CHECK(vkBindImageMemory(device, im.image, im.memory, 0));
   memset(copies, 0, sizeof(copies));
   for (level = 0; level < 7; level++)
   {
      const unsigned s = 64u >> level;
      for (y = 0; y < s; y++)
         for (x = 0; x < s; x++)
         {
            const unsigned r = (x * 37u + y * 11u + level * 50u) & 255u, g = level * 36u, bl = ((x ^ y) * 4u) & 255u;
            const unsigned a = ((x + y) & 4u) ? 255u : 96u;
            p[off / 4 + y * s + x] = r | (g << 8) | (bl << 16) | (a << 24);
         }
      copies[level].bufferOffset = off;
      copies[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copies[level].imageSubresource.mipLevel = level;
      copies[level].imageSubresource.layerCount = 1;
      copies[level].imageExtent.width = copies[level].imageExtent.height = s;
      copies[level].imageExtent.depth = 1;
      off += s * s * 4;
   }
   CHECK(vkResetCommandBuffer(cmd, 0));
   CHECK(vkBeginCommandBuffer(cmd, &cbi));
   b.srcAccessMask = 0;
   b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = im.image;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 7;
   b.subresourceRange.layerCount = 1;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   vkCmdCopyBufferToImage(cmd, staging.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 7, copies);
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   CHECK(vkEndCommandBuffer(cmd));
   submit();
   free_buffer(&staging);
   vi.image = im.image;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = VK_FORMAT_R8G8B8A8_UNORM;
   vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vi.subresourceRange.levelCount = 7;
   vi.subresourceRange.layerCount = 1;
   CHECK(vkCreateImageView(device, &vi, NULL, &im.view));
   return im;
}

static const char *stage_names[BENCH_STAMPS - 1] = { "fill", "setup", "scan", "scatter", "sort", "walk", "clear" };

int main(int argc, char **argv)
{
   const char *samples_s = getenv("BENCH_SAMPLES") ? getenv("BENCH_SAMPLES") : "1";
   const unsigned rounds = getenv("BENCH_ROUNDS") ? (unsigned)atoi(getenv("BENCH_ROUNDS")) : 5u;
   const char *ppm = getenv("BENCH_PPM");
   unsigned nsets = 0, s, k, a;
   char path[1024];
   int all_same = 1;
   VkSampler trilinear;
   struct image tex, dummy;
   if (argc < 3)
   {
      printf("usage: %s SET[,SET...] FRAME...\n", argv[0]);
      return 2;
   }
   setenv("TILE_PROD", "1", 1);
   /* (the kernel sets: the first one's directory given to init()) */
   {
      char *list = strdup(argv[1]), *tok, *save = NULL;
      for (tok = strtok_r(list, ",", &save); tok && nsets < BENCH_SETS; tok = strtok_r(NULL, ",", &save))
      {
         if (nsets == 0)
            init(tok, 0);
         for (k = 1; k <= PVRC_KERNELS; k++)
         {
            snprintf(path, sizeof(path), "%s/k%u.spv", tok, k);
            set_spv[nsets][k] = read_file(path, &set_spv_size[nsets][k]);
         }
         snprintf(path, sizeof(path), "%s/variants", tok);
         {
            FILE *v = fopen(path, "rb");
            set_variants[nsets] = v != NULL;
            if (v)
               fclose(v);
         }
         snprintf(path, sizeof(path), "%s/lists", tok);
         {
            FILE *v = fopen(path, "rb");
            set_lists[nsets] = v != NULL;
            if (v)
               fclose(v);
         }
         printf("bench: set %u: %s%s\n", nsets, tok, set_lists[nsets] ? " (the walk's two variants, over the sort's lists)"
               : set_variants[nsets] ? " (the walk's two variants)" : "");
         nsets++;
      }
   }
   {
      VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
      sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
      sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      sci.maxLod = 7.0f;
      CHECK(vkCreateSampler(device, &sci, NULL, &trilinear));
   }
   tex = make_texture();
   dummy = make_image(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
   {
      /* (the dummy, for the last picture and the palette: cleared to 0) */
      VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      VkClearColorValue zero;
      VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      memset(&zero, 0, sizeof(zero));
      CHECK(vkResetCommandBuffer(cmd, 0));
      CHECK(vkBeginCommandBuffer(cmd, &cbi));
      barrier(cmd, dummy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdClearColorImage(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
      barrier(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
      CHECK(vkEndCommandBuffer(cmd));
      submit();
   }
   for (a = 2; a < (unsigned)argc; a++)
   {
      size_t bytes;
      unsigned *frame = (unsigned *)read_file(argv[a], &bytes);
      const unsigned words = (unsigned)(bytes / 4), w = frame[PVRC_F_WIDTH], h = frame[PVRC_F_HEIGHT];
      const unsigned runs = frame[PVRC_F_RUNS], tiles = frame[PVRC_F_TILES_X] * frame[PVRC_F_TILES_Y];
      const unsigned prims = frame[PVRC_F_PRIMS];
      const unsigned binWords = pvrc_bin_words(runs, frame[PVRC_F_POOL]);
      const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
      struct buffer fb = make_buffer((VkDeviceSize)words * 4, 0);
      struct buffer rb = make_buffer((VkDeviceSize)(prims ? prims : 1) * PVRC_REC_WORDS * 4, 0);
      struct buffer bb = make_buffer((VkDeviceSize)binWords * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
      struct buffer ob = make_buffer((VkDeviceSize)w * h * 4, 0);
      struct buffer xb = make_buffer(64, 0);	/* (the decode's buffers, unused for a frame of the parse's) */
      struct image outImage = make_image(w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
      unsigned *first = (unsigned *)malloc((size_t)w * h * 4);
      VkDescriptorBufferInfo bi[9];
      VkDescriptorImageInfo ii[3 + PVRC_TEX_SLOTS];
      VkWriteDescriptorSet wr[13];
      const char *p;
      char *e;
      memcpy(fb.mapped, frame, (size_t)words * 4);
      memset(bb.mapped, 0, (size_t)binWords * 4);
      memset(wr, 0, sizeof(wr));
      bi[0].buffer = fb.buffer; bi[1].buffer = rb.buffer; bi[2].buffer = bb.buffer;
      for (k = 3; k < 6; k++)
         bi[k].buffer = fb.buffer;
      for (k = 6; k < 9; k++)
         bi[k].buffer = xb.buffer;
      for (k = 0; k < 9; k++)
      {
         VkWriteDescriptorSet *x = &wr[k < 3 ? k : k + 4];
         bi[k].offset = 0;
         bi[k].range = VK_WHOLE_SIZE;
         x->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         x->dstSet = set;
         x->dstBinding = k < 3 ? k : k + 4;
         x->descriptorCount = 1;
         x->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         x->pBufferInfo = &bi[k];
      }
      ii[0].sampler = VK_NULL_HANDLE; ii[0].imageView = outImage.view; ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      for (k = 1; k < 3 + PVRC_TEX_SLOTS; k++)
      {
         ii[k].sampler = k < 3 ? sampler : trilinear;
         ii[k].imageView = k < 3 ? dummy.view : tex.view;
         ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      for (k = 3; k < 7; k++)
      {
         wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
         wr[k].dstSet = set;
         wr[k].dstBinding = k;
         wr[k].descriptorCount = k == 6 ? PVRC_TEX_SLOTS : 1;
         wr[k].descriptorType = k == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
         wr[k].pImageInfo = &ii[k - 3];
      }
      vkUpdateDescriptorSets(device, 13, wr, 0, NULL);

      for (p = samples_s; ; p = e)
      {
         const unsigned n = (unsigned)strtoul(p, &e, 10);
         const unsigned keep = getenv("BENCH_KEEP") ? (unsigned)atoi(getenv("BENCH_KEEP")) : pvrc_fragments_kept(shared_bytes);
         const unsigned fw = PVRC_FINE_W(n ? n : 1u), fh = PVRC_FINE_H(n ? n : 1u);
         VkPipeline pipes[BENCH_SETS][PVRC_KERNELS + 2];	/* (the walk's unsorted variant last) */
         double *t[BENCH_SETS][BENCH_STAMPS + 1];
         unsigned r, differ[BENCH_SETS], drawn = 0, far = 0, behind = 0;
         if (e == p)
            break;
         for (s = 0; s < nsets; s++)
         {
            for (k = 1; k <= PVRC_KERNELS; k++)
            {
               spv[k] = set_spv[s][k];
               spv_size[k] = set_spv_size[s][k];
               pipes[s][k] = make_pipeline_variant(k, n, keep, k == PVRC_K_WALK && set_variants[s] && !walk_whole() ? PVRC_V_SORTED : PVRC_V_ALL);
            }
            pipes[s][PVRC_KERNELS + 1] = walk_whole() || !set_variants[s] ? VK_NULL_HANDLE : make_pipeline_variant(PVRC_K_WALK, n, 1, PVRC_V_UNSORTED);
            for (k = 0; k <= BENCH_STAMPS; k++)
               t[s][k] = (double *)calloc(rounds, sizeof(double));
            differ[s] = 0;
         }
         /* (round 0 is not counted: the device makes each pipeline's code
          * the first time it is dispatched) */
         for (r = 0; r <= rounds; r++)
            for (s = 0; s < nsets; s++)
            {
               /* (each round starts with another set, so that no set is
                * always the one after another) */
               const unsigned ss = (s + r) % nsets;
               const unsigned rr = r == 0 ? 0u : r - 1u;
               VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
               VkBufferImageCopy copy;
               unsigned long long ts[BENCH_STAMPS];
               double c0, c1, w0, w1;
               CHECK(vkResetCommandBuffer(cmd, 0));
               CHECK(vkBeginCommandBuffer(cmd, &cbi));
               barrier(cmd, outImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
               vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
               vkCmdResetQueryPool(cmd, queries, 0, BENCH_STAMPS);
               vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 0);
               vkCmdFillBuffer(cmd, bb.buffer, 0, (VkDeviceSize)PVRC_B_POOL(runs) * 4, 0);
               if (set_lists[ss] && !walk_whole())
               {
                  /* (the walk's variants' dispatches across, as the renderers give them) */
                  VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
                  vkCmdFillBuffer(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_WALK_U_X) * 4, 4, pvrc_walk_x(frame[PVRC_F_TILE_LOG2], n ? n : 1u));
                  vkCmdFillBuffer(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_WALK_S_X) * 4, 4, pvrc_walk_x(frame[PVRC_F_TILE_LOG2], n ? n : 1u));
               }
               mem_barrier(cmd);
               vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 1);
#define BSTEP(kernel, x, y, q) do { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[ss][kernel]); \
               if ((x) && (y)) { vkCmdDispatch(cmd, (x), (y), 1); } mem_barrier(cmd); \
               vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, (q)); } while (0)
               BSTEP(PVRC_K_SETUP, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1, 2);
               BSTEP(PVRC_K_SCAN, 1, 1, 3);
               BSTEP(PVRC_K_SCATTER, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1, 4);
               BSTEP(PVRC_K_SORT, tiles, frame[PVRC_F_CHUNKS], 5);
               if (pipes[ss][PVRC_KERNELS + 1] != VK_NULL_HANDLE && set_lists[ss])
               {
                  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[ss][PVRC_KERNELS + 1]);
                  vkCmdDispatchIndirect(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_WALK_U_X) * 4);
                  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[ss][PVRC_K_WALK]);
                  vkCmdDispatchIndirect(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_WALK_S_X) * 4);
                  BSTEP(PVRC_K_WALK, 0, 0, 6);
               }
               else
               {
                  if (pipes[ss][PVRC_KERNELS + 1] != VK_NULL_HANDLE)
                  {
                     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[ss][PVRC_KERNELS + 1]);
                     vkCmdDispatch(cmd, (w + fw - 1) / fw, (h + fh - 1) / fh, 1);
                  }
                  BSTEP(PVRC_K_WALK, (w + fw - 1) / fw, (h + fh - 1) / fh, 6);
               }
               vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[ss][PVRC_K_CLEAR]);
               vkCmdDispatchIndirect(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_CLEAR_X) * 4);
               mem_barrier(cmd);
               vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 7);
               barrier(cmd, outImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
               memset(&copy, 0, sizeof(copy));
               copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
               copy.imageSubresource.layerCount = 1;
               copy.imageExtent.width = w;
               copy.imageExtent.height = h;
               copy.imageExtent.depth = 1;
               vkCmdCopyImageToBuffer(cmd, outImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob.buffer, 1, &copy);
               CHECK(vkEndCommandBuffer(cmd));
               c0 = cpu_ms();
               w0 = now_ms();
               submit();
               w1 = now_ms();
               c1 = cpu_ms();
               CHECK(vkGetQueryPoolResults(device, queries, 0, BENCH_STAMPS, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
               for (k = 0; k + 1 < BENCH_STAMPS; k++)
                  t[ss][k][rr] = (double)(ts[k + 1] - ts[k]) * tick_ns / 1e6;
               t[ss][BENCH_STAMPS - 1][rr] = (double)(ts[BENCH_STAMPS - 1] - ts[1]) * tick_ns / 1e6;	/* (the kernels, without the fill) */
               t[ss][BENCH_STAMPS][rr] = c1 - c0;
               (void)w0; (void)w1;
               if (r == 0)
               {
                  if (ss == 0)
                  {
                     /* (what set 0's setup made of the primitives: how many
                      * are drawn, have a vertex far out, one behind the eye) */
                     const unsigned *rw = (const unsigned *)rb.mapped;
                     unsigned i;
                     memcpy(first, ob.mapped, (size_t)w * h * 4);
                     for (i = 0; i < prims; i++)
                     {
                        const unsigned *rr = rw + i * PVRC_REC_WORDS;
                        if (rr[PVRC_R_BBOX0] == 0xffffffffu)
                           continue;
                        drawn++;
                        far += ((rr[PVRC_R_EXTRA] >> PVRC_RX_SHIFT_AT) & 15u) != 0u;
                        behind += (rr[PVRC_R_EXTRA] & PVRC_RX_NEG) != 0u;
                     }
                  }
                  else
                  {
                     const unsigned *got = (const unsigned *)ob.mapped;
                     unsigned i;
                     for (i = 0; i < w * h; i++)
                        differ[ss] += got[i] != first[i];
                  }
                  if (ss == 0 && ppm)
                  {
                     char name[256];
                     snprintf(name, sizeof(name), "%s_%u", base, n);
                     write_ppm(ppm, name, first, w, h);
                  }
               }
            }
         /* (the pictures compared are round 0's, which set 0 begins) */
         printf("bench: %s, %u x %u, %u primitives (%u drawn, %u with a vertex far out, %u behind the eye), %u sample%s, K %u, %u rounds\n",
               base, w, h, prims, drawn, far, behind, n, n > 1 ? "s" : "", keep, rounds);
         for (s = 0; s < nsets; s++)
         {
            unsigned long long hsh = 1469598103934665603ull;
            printf("bench:   set %u:", s);
            for (k = 0; k + 1 < BENCH_STAMPS; k++)
               printf(" %s %.2f", stage_names[k], median(t[s][k], rounds));
            printf(" | kernels %.2f ms, cpu %.1f ms", median(t[s][BENCH_STAMPS - 1], rounds), median(t[s][BENCH_STAMPS], rounds));
            if (s == 0)
            {
               unsigned i;
               for (i = 0; i < w * h; i++)
                  hsh = (hsh ^ first[i]) * 1099511628211ull;
               printf(", picture %016llx\n", hsh);
            }
            else
            {
               printf(", picture %s", differ[s] ? "DIFFERENT" : "the same");
               if (differ[s])
                  printf(" (%u pixels)", differ[s]);
               printf("\n");
               if (differ[s])
                  all_same = 0;
            }
            for (k = 1; k <= PVRC_KERNELS; k++)
               vkDestroyPipeline(device, pipes[s][k], NULL);
            if (pipes[s][PVRC_KERNELS + 1] != VK_NULL_HANDLE)
               vkDestroyPipeline(device, pipes[s][PVRC_KERNELS + 1], NULL);
            for (k = 0; k <= BENCH_STAMPS; k++)
               free(t[s][k]);
         }
         fflush(stdout);
      }
      free_image(&outImage);
      free_buffer(&fb);
      free_buffer(&rb);
      free_buffer(&bb);
      free_buffer(&ob);
      free_buffer(&xb);
      free(first);
      free(frame);
   }
   printf("bench: %s\n", all_same ? "every set's pictures the same" : "PICTURES DIFFER");
   return all_same ? 0 : 1;
}
