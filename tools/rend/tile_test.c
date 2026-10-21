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
 * TILE_PPM a directory to write the pictures that differ to.
 *
 * Each frame is also drawn the way a frame of the GPU's TA decode is
 * (TILE_GPU=0 not): its table of starts made what the CPU writes for one
 * (PVRC_SRC_WORDS) - the polygons' index ranges as indirect commands, the
 * translucent strips that follow one another with the same parameters as
 * one draw group of translucent draw records, the render passes' and the
 * volumes' numbers as entries of the table - and numbered on the device by
 * PVRC_K_GPUSRC, the setup, scatter and sort dispatched from its counts:
 * the picture has to be the reference's still, and the count of triangles
 * what the groups give. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "tile_test_main.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s: %d\n", #x, (int)r_); exit(2); } } while (0)

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
   VkDescriptorSetLayoutBinding b[13];
   VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
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
   for (k = 0; k < 13; k++)
   {
      b[k].binding = k;
      b[k].descriptorCount = 1;
      b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      b[k].descriptorType = k < 3 || k >= 7 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : k == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   }
   b[6].descriptorCount = PVRC_TEX_SLOTS;
   lci.bindingCount = 13;
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

static double gpu_draw(const unsigned *frame0, unsigned words, unsigned samples, unsigned keep, unsigned *out, unsigned *stats, int gpu)
{
   const unsigned w = frame0[PVRC_F_WIDTH], h = frame0[PVRC_F_HEIGHT], runs = frame0[PVRC_F_RUNS];
   const unsigned tiles = frame0[PVRC_F_TILES_X] * frame0[PVRC_F_TILES_Y];
   const unsigned binWords = pvrc_bin_words(runs, frame0[PVRC_F_POOL]);
   const unsigned nsrc = frame0[PVRC_F_SOURCES] + 1;
   struct buffer cb = make_buffer((VkDeviceSize)nsrc * PVRC_TAG_C_WORDS * 4, 0);
   struct buffer tb = make_buffer((VkDeviceSize)nsrc * 2 * PVRC_TAG_C_WORDS * 4, 0);
   struct buffer db = make_buffer((PVRC_TAG_O_ZSCALE + 1) * 4, 0);
   unsigned *gf = gpu ? (unsigned *)malloc((size_t)words * 4) : NULL;
   const unsigned prims = gpu ? gpu_frame(frame0, words, gf, (unsigned *)cb.mapped, (unsigned *)tb.mapped, (unsigned *)db.mapped) : frame0[PVRC_F_PRIMS];
   const unsigned *frame = gpu ? gf : frame0;
   struct buffer fb = make_buffer((VkDeviceSize)words * 4, 0);
   struct buffer rb = make_buffer((VkDeviceSize)(prims ? prims : 1) * PVRC_REC_WORDS * 4, 0);
   struct buffer bb = make_buffer((VkDeviceSize)binWords * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   struct buffer ob = make_buffer((VkDeviceSize)w * h * 4, 0);
   struct image outImage = make_image(w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   struct image dummy = make_image(1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
   VkPipeline pipelines[PVRC_KERNELS + 1];
   VkDescriptorBufferInfo bi[9];
   VkDescriptorImageInfo ii[3 + PVRC_TEX_SLOTS];
   VkWriteDescriptorSet wr[13];
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkBufferImageCopy copy;
   VkClearColorValue zero;
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   unsigned k, fw = PVRC_FINE_W(samples), fh = PVRC_FINE_H(samples);
   double t0, t1;

   memcpy(fb.mapped, frame, (size_t)words * 4);
   memset(bb.mapped, 0, (size_t)binWords * 4);
   for (k = 1; k <= PVRC_KERNELS; k++)
      pipelines[k] = make_pipeline(k, samples, keep);
   memset(wr, 0, sizeof(wr));
   bi[0].buffer = fb.buffer; bi[0].offset = 0; bi[0].range = VK_WHOLE_SIZE;
   bi[1].buffer = rb.buffer; bi[1].offset = 0; bi[1].range = VK_WHOLE_SIZE;
   bi[2].buffer = bb.buffer; bi[2].offset = 0; bi[2].range = VK_WHOLE_SIZE;
   /* (the vertices, indices and volume triangles are the frame's own words, either way) */
   for (k = 3; k < 6; k++)
      bi[k] = bi[0];
   bi[6].buffer = cb.buffer; bi[6].offset = 0; bi[6].range = VK_WHOLE_SIZE;
   bi[7].buffer = tb.buffer; bi[7].offset = 0; bi[7].range = VK_WHOLE_SIZE;
   bi[8].buffer = db.buffer; bi[8].offset = 0; bi[8].range = VK_WHOLE_SIZE;
   for (k = 0; k < 9; k++)
   {
      VkWriteDescriptorSet *x = &wr[k < 3 ? k : k + 4];
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
   vkUpdateDescriptorSets(device, 13, wr, 0, NULL);

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
#define INDIRECT(kernel, at) do { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[kernel]); \
      vkCmdDispatchIndirect(cmd, bb.buffer, (VkDeviceSize)(PVRC_B_STATS(runs) + (at)) * 4); mem_barrier(cmd); \
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, kernel); } while (0)
   if (gpu)
   {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[PVRC_K_GPUSRC]);
      vkCmdDispatch(cmd, 1, 1, 1);
      mem_barrier(cmd);
      INDIRECT(PVRC_K_SETUP, PVRC_ST_SETUP_X);
      STEP(PVRC_K_SCAN, 1, 1);
      INDIRECT(PVRC_K_SCATTER, PVRC_ST_SETUP_X);
      INDIRECT(PVRC_K_SORT, PVRC_ST_SORT_X);
   }
   else
   {
      STEP(PVRC_K_SETUP, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
      STEP(PVRC_K_SCAN, 1, 1);
      STEP(PVRC_K_SCATTER, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
      STEP(PVRC_K_SORT, tiles, frame[PVRC_F_CHUNKS]);
   }
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
   t0 = now_ms();
   submit();
   t1 = now_ms();
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
   if (gpu && ((const unsigned *)fb.mapped)[PVRC_F_PRIMS] != prims)
   {
      printf("tile_test: numbered on the device to %u triangles, not %u\n", ((const unsigned *)fb.mapped)[PVRC_F_PRIMS], prims);
      validation_errors++;
   }
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
   free_buffer(&cb);
   free_buffer(&tb);
   free_buffer(&db);
   free(gf);
   free_buffer(&rb);
   free_buffer(&bb);
   free_buffer(&ob);
   return t1 - t0;
}

