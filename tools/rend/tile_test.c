/* The compute renderer's kernels (core/rend/tile/pvr_compute.h) against
 * its reference on the CPU (tile_ref.c), on frames tile_dump wrote: each
 * frame drawn on a Vulkan device by the kernels in their test mode and by
 * the reference, at 1, 2, 4 and 8 samples a pixel, and with the
 * translucent fragments kept K at a time for each K asked for (a K of 1
 * peels every layer apart), and the pictures compared word for word.
 *
 *   tile_test SPV_DIR FRAME...
 *
 * SPV_DIR has k1.spv to k6.spv, the kernels made with PVRC_EXACT (no
 * fused operations: tile_test.sh). The environment: TILE_SAMPLES (default
 * "1 2 4 8"), TILE_KEEP (default "0 1": 0 is the device's own K),
 * TILE_VALIDATE=1 the validation layer, any error it reports a failure;
 * TILE_PPM a directory to write the pictures that differ to. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "pvr_compute.h"
#include "tile_ref.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s: %d\n", #x, (int)r_); exit(2); } } while (0)

static int validation_errors;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipeline_layout;
static VkDescriptorPool desc_pool;
static VkDescriptorSet set;
static VkSampler sampler;
static unsigned char *spv[PVRC_KERNELS + 1];
static size_t spv_size[PVRC_KERNELS + 1];
static unsigned shared_bytes;
static VkQueryPool queries;
static float tick_ns;

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
   (void)type; (void)user;
   if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   {
      if (validation_errors++ < 10)
         printf("validation: %s\n", data->pMessage);
   }
   return VK_FALSE;
}

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

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   uint32_t i;
   for (i = 0; i < mem_props.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   printf("no memory type\n");
   exit(2);
}

struct buffer { VkBuffer buffer; VkDeviceMemory memory; void *mapped; VkDeviceSize size; };

static struct buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
   struct buffer b;
   VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   bi.size = size;
   bi.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
   CHECK(vkCreateBuffer(device, &bi, NULL, &b.buffer));
   vkGetBufferMemoryRequirements(device, b.buffer, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkAllocateMemory(device, &ai, NULL, &b.memory));
   CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
   CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
   b.size = size;
   return b;
}

static void free_buffer(struct buffer *b)
{
   vkDestroyBuffer(device, b->buffer, NULL);
   vkFreeMemory(device, b->memory, NULL);
}

struct image { VkImage image; VkDeviceMemory memory; VkImageView view; };

static struct image make_image(unsigned w, unsigned h, VkImageUsageFlags usage)
{
   struct image im;
   VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = VK_FORMAT_R8G8B8A8_UNORM;
   ci.extent.width = w;
   ci.extent.height = h;
   ci.extent.depth = 1;
   ci.mipLevels = 1;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = usage;
   CHECK(vkCreateImage(device, &ci, NULL, &im.image));
   vkGetImageMemoryRequirements(device, im.image, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
   CHECK(vkAllocateMemory(device, &ai, NULL, &im.memory));
   CHECK(vkBindImageMemory(device, im.image, im.memory, 0));
   vi.image = im.image;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = VK_FORMAT_R8G8B8A8_UNORM;
   vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vi.subresourceRange.levelCount = 1;
   vi.subresourceRange.layerCount = 1;
   CHECK(vkCreateImageView(device, &vi, NULL, &im.view));
   return im;
}

static void free_image(struct image *im)
{
   vkDestroyImageView(device, im->view, NULL);
   vkDestroyImage(device, im->image, NULL);
   vkFreeMemory(device, im->memory, NULL);
}

static void barrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = image;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = 1;
   b.subresourceRange.layerCount = 1;
   vkCmdPipelineBarrier(c, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static void mem_barrier(VkCommandBuffer c)
{
   VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
   b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

static void submit(void)
{
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   CHECK(vkResetFences(device, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull));
}

static void init(const char *dir, int validate)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
   const char *exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   uint32_t n = 1, k;
   float prio = 1.f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkPhysicalDeviceFeatures features;
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkPhysicalDeviceProperties props;
   VkDescriptorSetLayoutBinding b[7];
   VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
         { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PVRC_TEX_SLOTS + 2 } };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
   char path[1024];

   app.apiVersion = VK_API_VERSION_1_1;
   ici.pApplicationInfo = &app;
   if (validate)
   {
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = layers;
      ici.enabledExtensionCount = 1;
      ici.ppEnabledExtensionNames = exts;
   }
   CHECK(vkCreateInstance(&ici, NULL, &instance));
   if (validate)
   {
      VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
      PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
      VkDebugUtilsMessengerEXT messenger;
      mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
      mci.pfnUserCallback = on_message;
      CHECK(create(instance, &mci, NULL, &messenger));
   }
   CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
   vkGetPhysicalDeviceProperties(gpu, &props);
   shared_bytes = props.limits.maxComputeSharedMemorySize;
   printf("device: %s, Vulkan %u.%u, %u bytes of shared memory%s\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), shared_bytes, validate ? ", validation layer on" : "");
   vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
   memset(&features, 0, sizeof(features));
   features.shaderInt64 = VK_TRUE;
   features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
   qci.queueFamilyIndex = 0;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.pEnabledFeatures = &features;
   CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
   vkGetDeviceQueue(device, 0, 0, &queue);
   cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
   cai.commandPool = cmd_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   CHECK(vkCreateFence(device, &fci, NULL, &fence));

   memset(b, 0, sizeof(b));
   for (k = 0; k < 7; k++)
   {
      b[k].binding = k;
      b[k].descriptorCount = 1;
      b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      b[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : k == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   }
   b[6].descriptorCount = PVRC_TEX_SLOTS;
   lci.bindingCount = 7;
   lci.pBindings = b;
   CHECK(vkCreateDescriptorSetLayout(device, &lci, NULL, &set_layout));
   pli.setLayoutCount = 1;
   pli.pSetLayouts = &set_layout;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &pipeline_layout));
   dpi.maxSets = 1;
   dpi.poolSizeCount = 3;
   dpi.pPoolSizes = sizes;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &desc_pool));
   dai.descriptorPool = desc_pool;
   dai.descriptorSetCount = 1;
   dai.pSetLayouts = &set_layout;
   CHECK(vkAllocateDescriptorSets(device, &dai, &set));
   sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
   CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
   {
      VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
      qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qpi.queryCount = 8;
      CHECK(vkCreateQueryPool(device, &qpi, NULL, &queries));
      tick_ns = props.limits.timestampPeriod;
   }
   for (k = 1; k <= PVRC_KERNELS; k++)
   {
      snprintf(path, sizeof(path), "%s/k%u.spv", dir, k);
      spv[k] = read_file(path, &spv_size[k]);
   }
}

static VkPipeline make_pipeline(unsigned k, unsigned samples, unsigned keep)
{
   VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
   VkSpecializationMapEntry entries[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
   unsigned spec[4];
   VkSpecializationInfo si;
   VkShaderModule module;
   VkPipeline pipeline;
   spec[0] = samples;
   spec[1] = SSAA_LOG2(samples);
   spec[2] = keep;
   spec[3] = getenv("TILE_PROD") ? 0 : 1;   /* test mode, but for TILE_PROD (no reference then) */
   si.mapEntryCount = 4;
   si.pMapEntries = entries;
   si.dataSize = sizeof(spec);
   si.pData = spec;
   mi.codeSize = spv_size[k];
   mi.pCode = (const uint32_t *)spv[k];
   CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
   pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   pi.stage.module = module;
   pi.stage.pName = "main";
   pi.stage.pSpecializationInfo = &si;
   pi.layout = pipeline_layout;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
   vkDestroyShaderModule(device, module, NULL);
   return pipeline;
}

/* The frame drawn by the kernels at @samples, keeping @keep: into @out;
 * the stats words into @stats */
static double gpu_draw(const unsigned *frame, unsigned words, unsigned samples, unsigned keep, unsigned *out, unsigned *stats)
{
   const unsigned w = frame[PVRC_F_WIDTH], h = frame[PVRC_F_HEIGHT], runs = frame[PVRC_F_RUNS], prims = frame[PVRC_F_PRIMS];
   const unsigned tiles = frame[PVRC_F_TILES_X] * frame[PVRC_F_TILES_Y];
   const unsigned binWords = pvrc_bin_words(runs, frame[PVRC_F_POOL]);
   struct buffer fb = make_buffer((VkDeviceSize)words * 4, 0);
   struct buffer rb = make_buffer((VkDeviceSize)(prims ? prims : 1) * PVRC_REC_WORDS * 4, 0);
   struct buffer bb = make_buffer((VkDeviceSize)binWords * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   struct buffer ob = make_buffer((VkDeviceSize)w * h * 4, 0);
   struct image outImage = make_image(w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   struct image dummy = make_image(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
   VkPipeline pipelines[PVRC_KERNELS + 1];
   VkDescriptorBufferInfo bi[3];
   VkDescriptorImageInfo ii[3 + PVRC_TEX_SLOTS];
   VkWriteDescriptorSet wr[7];
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkBufferImageCopy copy;
   VkClearColorValue zero;
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   unsigned k, fw = PVRC_FINE_W(samples), fh = PVRC_FINE_H(samples);
   struct timespec t0, t1;

   memcpy(fb.mapped, frame, (size_t)words * 4);
   memset(bb.mapped, 0, (size_t)binWords * 4);
   for (k = 1; k <= PVRC_KERNELS; k++)
      pipelines[k] = make_pipeline(k, samples, keep);
   memset(wr, 0, sizeof(wr));
   bi[0].buffer = fb.buffer; bi[0].offset = 0; bi[0].range = VK_WHOLE_SIZE;
   bi[1].buffer = rb.buffer; bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
   bi[2].buffer = bb.buffer; bi[2].offset = 0; bi[2].range = VK_WHOLE_SIZE;
   for (k = 0; k < 3; k++)
   {
      wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      wr[k].dstSet = set;
      wr[k].dstBinding = k;
      wr[k].descriptorCount = 1;
      wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      wr[k].pBufferInfo = &bi[k];
   }
   ii[0].sampler = VK_NULL_HANDLE; ii[0].imageView = outImage.view; ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
   for (k = 1; k < 3 + PVRC_TEX_SLOTS; k++)
   {
      ii[k].sampler = sampler;
      ii[k].imageView = dummy.view;
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
   vkUpdateDescriptorSets(device, 7, wr, 0, NULL);

   CHECK(vkResetCommandBuffer(cmd, 0));
   CHECK(vkBeginCommandBuffer(cmd, &cbi));
   barrier(cmd, dummy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   memset(&zero, 0, sizeof(zero));
   if (getenv("TILE_PROD"))
      zero.float32[1] = 1.0f;	/* (green, transparent) */
   vkCmdClearColorImage(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
   barrier(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   barrier(cmd, outImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
   vkCmdResetQueryPool(cmd, queries, 0, 8);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 0);
#define STEP(kernel, x, y) do { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kernel]); \
      if ((x) && (y)) { vkCmdDispatch(cmd, (x), (y), 1); } mem_barrier(cmd); \
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, kernel); } while (0)
   STEP(PVRC_K_SETUP, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
   STEP(PVRC_K_SCAN, 1, 1);
   STEP(PVRC_K_SCATTER, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
   STEP(PVRC_K_SORT, tiles, frame[PVRC_F_CHUNKS]);
   STEP(PVRC_K_WALK, (w + fw - 1) / fw, (h + fh - 1) / fh);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[PVRC_K_CLEAR]);
   vkCmdDispatchIndirect(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + PVRC_ST_CLEAR_X) * 4);
   mem_barrier(cmd);
   barrier(cmd, outImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   memset(&copy, 0, sizeof(copy));
   copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent.width = w;
   copy.imageExtent.height = h;
   copy.imageExtent.depth = 1;
   vkCmdCopyImageToBuffer(cmd, outImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob.buffer, 1, &copy);
   CHECK(vkEndCommandBuffer(cmd));
   clock_gettime(CLOCK_MONOTONIC, &t0);
   submit();
   clock_gettime(CLOCK_MONOTONIC, &t1);
   memcpy(out, ob.mapped, (size_t)w * h * 4);
   if (getenv("TILE_TIMES"))
   {
      unsigned long long ts[6];
      if (vkGetQueryPoolResults(device, queries, 0, 6, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
         printf("tile_test: kernels: setup %.2f, scan %.2f, scatter %.2f, sort %.2f, walk %.2f ms\n",
               (ts[1] - ts[0]) * tick_ns / 1e6, (ts[2] - ts[1]) * tick_ns / 1e6, (ts[3] - ts[2]) * tick_ns / 1e6,
               (ts[4] - ts[3]) * tick_ns / 1e6, (ts[5] - ts[4]) * tick_ns / 1e6);
   }
   memcpy(stats, (unsigned *)bb.mapped + PVRC_B_STATS(runs), PVRC_STATS * 4);
   /* the pool is left as the clear leaves it, all 0 */
   {
      const unsigned *pool = (const unsigned *)bb.mapped + PVRC_B_POOL(runs);
      unsigned i, dirty = 0;
      for (i = 0; i < (frame[PVRC_F_POOL] + 1) / 2; i++)
         dirty += pool[i] != 0;
      if (dirty)
      {
         printf("tile_test: the bin pool has %u words left after the clear\n", dirty);
         validation_errors++;
      }
   }
   for (k = 1; k <= PVRC_KERNELS; k++)
      vkDestroyPipeline(device, pipelines[k], NULL);
   free_image(&outImage);
   free_image(&dummy);
   free_buffer(&fb);
   free_buffer(&rb);
   free_buffer(&bb);
   free_buffer(&ob);
   return (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
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

int main(int argc, char **argv)
{
   const char *samples_s = getenv("TILE_SAMPLES") ? getenv("TILE_SAMPLES") : "1 2 4 8";
   const char *keep_s = getenv("TILE_KEEP") ? getenv("TILE_KEEP") : "0 1";
   const char *ppm = getenv("TILE_PPM");
   int validate = getenv("TILE_VALIDATE") && atoi(getenv("TILE_VALIDATE"));
   int a, runs = 0, same = 0, differ = 0;
   if (argc < 3)
   {
      printf("usage: %s SPV_DIR FRAME...\n", argv[0]);
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
            struct timespec t0, t1;
            double ref_ms;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (!tile_ref(frame, n, NULL, ref, &rst))
            {
               printf("tile_test: no memory\n");
               return 2;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ref_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            for (ki = 0; ki < nk; ki++)
            {
               const unsigned keep = kl[ki];
               unsigned stats[PVRC_STATS], i, bad = 0, first = 0;
               double ms = gpu_draw(frame, words, n, keep, got, stats);
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
               printf("tile_test: %s at %u sample%s, K %u: %s", argv[a], n, n > 1 ? "s" : "", keep, bad ? "DIFFERENT" : "the same");
               if (bad)
                  printf(" (%u pixels; the first at %u,%u: %08x, the reference %08x)", bad, first % w, first / w, got[first], ref[first]);
               printf("; %u primitives, %u passes, %u layers at most, %u rounds, %u runs walked whole, %u left out; device %.1f ms, reference %.1f ms\n",
                     frame[PVRC_F_PRIMS], frame[PVRC_F_PASSES], rst.layers_most, stats[PVRC_ST_PEELS] ? stats[PVRC_ST_PEELS] : 1,
                     stats[PVRC_ST_SKIPPED], stats[PVRC_ST_DROPPED], ms, ref_ms);
               if (bad && ppm)
               {
                  char name[256];
                  const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
                  snprintf(name, sizeof(name), "%s_%u_%u_gpu", base, n, keep);
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
