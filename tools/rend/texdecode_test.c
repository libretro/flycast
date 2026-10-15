/* Textures made on the GPU against the texture cache's CPU converters, byte
 * for byte.
 *
 * Every pixel format (the reserved one too), twiddled, compressed, planar
 * and planar compressed, with and without mipmaps, at sizes from 8x8 to
 * 1024x1024 square and not, the planar ones read with strides narrower
 * than, as wide as and wider than the texture and running into the end of
 * video memory, paletted ones with each palette format and selection and
 * as the palette indices the GPU looks up itself, 16-bit formats to 16
 * bits and to 32, Force Texture LOD0 on and off: each one is made by the
 * texture cache itself (texdecode_cpu.cpp, against core/rend/TexCache.o),
 * which converts it on the CPU and says what it would have handed a
 * renderer that converts on the GPU - the bytes to copy and the levels to
 * make. Then
 *   1. the C text of the kernel (rend/texdecode.h) makes the levels from
 *      those bytes, and they have to be the CPU's;
 *   2. the kernel itself makes them on a Vulkan device - Mesa's software
 *      one does - into images made as the renderer makes them (a format
 *      of the CPU's, written through views of integers), they are copied
 *      back, and they have to be the CPU's;
 *   3. the planar 16-bit textures the renderer copies to an image as they
 *      are, in the format that has their texels in the PowerVR's order,
 *      are copied so and read by a shader, and so are the CPU's texels in
 *      the CPU's format: every texel has to read as the same colour.
 * With the validation layer there, as it is with TEXD_VALIDATE=1 (the
 * default of texdecode_test.sh when it is installed), anything it says is
 * a failure too.
 *
 * Built by texdecode_test.sh, which compiles the kernels with
 * glslangValidator from the same header the core does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "texdecode.h"
#include "texdecode_case.h"

#define VRAM_BYTES (8u * 1024 * 1024)
#define MAX_TEXELS (1024u * 1024 * 4 / 3 + 16)

static unsigned char *vram_mem;
static unsigned char *cpu_out;
static unsigned char *ref_out;
static unsigned rnd_state = 0x2545F491u;
static unsigned rnd(void)
{
   rnd_state ^= rnd_state << 13;
   rnd_state ^= rnd_state >> 17;
   rnd_state ^= rnd_state << 5;
   return rnd_state;
}

static int failures, validation_errors;

static void fail(const struct texd_case *c, const char *what)
{
   if (failures++ < 20)
      printf("FAIL %s: fmt %u scan %u vq %u mip %u stride %u (ctl %u) %ux%u filter %u force32 %u lod0 %u pal %u/%u at %06x\n",
            what, c->fmt, c->scan_order, c->vq, c->mip, c->stride_sel, c->text_control & 31,
            8u << c->tex_u, 8u << c->tex_v, c->filter, c->force32, c->lod0, c->pal_ctrl, c->pal_select, c->addr);
}

/* ---- Vulkan ---- */

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDescriptorSetLayout ring_layout, image_layout, cmp_layout;
static VkPipelineLayout kernel_layout, cmp_pipeline_layout;
static VkPipeline kernels[3], cmp_pipeline;
static VkDescriptorPool desc_pool;
static VkSampler sampler;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s: %d\n", #x, (int)r_); exit(2); } } while (0)

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
   p = (unsigned char *)malloc((size_t)n);
   if (fread(p, 1, (size_t)n, f) != (size_t)n) exit(2);
   fclose(f);
   *size = (size_t)n;
   return p;
}

static VkPipeline make_pipeline(const char *path, VkPipelineLayout layout)
{
   size_t size;
   unsigned char *code = read_file(path, &size);
   VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
   VkShaderModule module;
   VkPipeline pipeline;
   mi.codeSize = size;
   mi.pCode = (const uint32_t *)code;
   CHECK(vkCreateShaderModule(device, &mi, NULL, &module));
   pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   pi.stage.module = module;
   pi.stage.pName = "main";
   pi.layout = layout;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
   vkDestroyShaderModule(device, module, NULL);
   free(code);
   return pipeline;
}

static VkDescriptorSetLayout make_layout(const VkDescriptorType *types, unsigned n)
{
   VkDescriptorSetLayoutBinding b[4];
   VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorSetLayout layout;
   unsigned i;
   memset(b, 0, sizeof(b));
   for (i = 0; i < n; i++)
   {
      b[i].binding = i;
      b[i].descriptorType = types[i];
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
   }
   ci.bindingCount = n;
   ci.pBindings = b;
   CHECK(vkCreateDescriptorSetLayout(device, &ci, NULL, &layout));
   return layout;
}

static void vk_init(const char *spv_dir, int validate)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
   const char *exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   uint32_t n = 1;
   float prio = 1.f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkPhysicalDeviceFeatures features;
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkPhysicalDeviceProperties props;
   VkDescriptorPoolSize sizes[3];
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
   VkDescriptorSetLayout set_layouts[2];
   VkDescriptorType t_ring[] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
   VkDescriptorType t_image[] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
   VkDescriptorType t_cmp[] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
   char path[1024];
   static const char *names[3] = { "r8ui", "r16ui", "r32ui" };
   unsigned i;

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
   printf("device: %s, Vulkan %u.%u%s\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
         validate ? ", validation layer on" : "");
   vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
   vkGetPhysicalDeviceFeatures(gpu, &features);
   if (!features.shaderStorageImageExtendedFormats)
   {
      printf("the device has no 16- and 8-bit storage images\n");
      exit(2);
   }
   memset(&features, 0, sizeof(features));
   features.shaderStorageImageExtendedFormats = VK_TRUE;
   queue_family = 0;
   qci.queueFamilyIndex = queue_family;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.pEnabledFeatures = &features;
   CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
   vkGetDeviceQueue(device, queue_family, 0, &queue);
   cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cpi.queueFamilyIndex = queue_family;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
   cai.commandPool = cmd_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   CHECK(vkCreateFence(device, &fci, NULL, &fence));

   ring_layout = make_layout(t_ring, 1);
   image_layout = make_layout(t_image, 1);
   cmp_layout = make_layout(t_cmp, 2);
   set_layouts[0] = ring_layout;
   set_layouts[1] = image_layout;
   pli.setLayoutCount = 2;
   pli.pSetLayouts = set_layouts;
   pli.pushConstantRangeCount = 1;
   pli.pPushConstantRanges = &push;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &kernel_layout));
   for (i = 0; i < 3; i++)
   {
      snprintf(path, sizeof(path), "%s/k_%s.spv", spv_dir, names[i]);
      kernels[i] = make_pipeline(path, kernel_layout);
   }
   pli.setLayoutCount = 1;
   pli.pSetLayouts = &cmp_layout;
   push.size = 8;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &cmp_pipeline_layout));
   snprintf(path, sizeof(path), "%s/sample.spv", spv_dir);
   cmp_pipeline = make_pipeline(path, cmp_pipeline_layout);

   sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   sizes[0].descriptorCount = 64;
   sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   sizes[1].descriptorCount = 64;
   sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   sizes[2].descriptorCount = 64;
   dpi.flags = 0;
   dpi.maxSets = 64;
   dpi.poolSizeCount = 3;
   dpi.pPoolSizes = sizes;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &desc_pool));

   sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
   sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
   sci.maxLod = 16.f;
   CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
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

struct buffer { VkBuffer buffer; VkDeviceMemory memory; void *mapped; };

static struct buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
   struct buffer b;
   VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   bi.size = size;
   bi.usage = usage;
   CHECK(vkCreateBuffer(device, &bi, NULL, &b.buffer));
   vkGetBufferMemoryRequirements(device, b.buffer, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkAllocateMemory(device, &ai, NULL, &b.memory));
   CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
   CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
   return b;
}

static void free_buffer(struct buffer *b)
{
   vkDestroyBuffer(device, b->buffer, NULL);
   vkFreeMemory(device, b->memory, NULL);
}

static unsigned integer_images;

/* An image of @format as the renderer makes them, for the kernel or not */
static int supported(VkFormat format, int storage)
{
   VkImageFormatProperties ifp;
   VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImageCreateFlags flags = 0;
   if (storage)
   {
      usage |= VK_IMAGE_USAGE_STORAGE_BIT;
      flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (format != VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0);
   }
   return vkGetPhysicalDeviceImageFormatProperties(gpu, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ifp) == VK_SUCCESS;
}

struct image
{
   VkImage image;
   VkDeviceMemory memory;
   VkImageView sampled;
   VkImageView levels[TEXD_MAX_LEVELS];
   unsigned nlevels;
};

/* As the renderer makes an image for the kernel (vulkan/texture.cpp): or,
 * @storage_view 0, one to be copied into */
static struct image make_image(VkFormat format, VkFormat storage_view, unsigned w, unsigned h, unsigned nlevels, VkComponentMapping swizzle)
{
   struct image im;
   VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   VkImageViewUsageCreateInfo usage = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
   unsigned i;

   memset(&im, 0, sizeof(im));
   if (!supported(format, storage_view != VK_FORMAT_UNDEFINED))
   {
      /* (this device has no such format: the kernel's bits are checked
       * through an image of the integers the renderer would view it as) */
      if (storage_view == VK_FORMAT_UNDEFINED)
      {
         printf("no format %d for a copy\n", (int)format);
         exit(2);
      }
      format = storage_view;
      integer_images++;
   }
   ci.imageType = VK_IMAGE_TYPE_2D;
   ci.format = format;
   ci.extent.width = w;
   ci.extent.height = h;
   ci.extent.depth = 1;
   ci.mipLevels = nlevels;
   ci.arrayLayers = 1;
   ci.samples = VK_SAMPLE_COUNT_1_BIT;
   ci.tiling = VK_IMAGE_TILING_OPTIMAL;
   ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   if (storage_view != VK_FORMAT_UNDEFINED)
   {
      ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
      if (format != storage_view)
         ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      if (format != VK_FORMAT_R8G8B8A8_UNORM && format != storage_view)
         ci.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
   }
   ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   CHECK(vkCreateImage(device, &ci, NULL, &im.image));
   vkGetImageMemoryRequirements(device, im.image, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   CHECK(vkAllocateMemory(device, &ai, NULL, &im.memory));
   CHECK(vkBindImageMemory(device, im.image, im.memory, 0));

   vi.image = im.image;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = format;
   vi.components = swizzle;
   vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vi.subresourceRange.levelCount = nlevels;
   vi.subresourceRange.layerCount = 1;
   usage.usage = ci.usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT;
   if (ci.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
      vi.pNext = &usage;
   CHECK(vkCreateImageView(device, &vi, NULL, &im.sampled));
   im.nlevels = nlevels;
   if (storage_view != VK_FORMAT_UNDEFINED)
   {
      VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
      usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
      vi.pNext = &usage;
      vi.format = storage_view;
      vi.components = identity;
      vi.subresourceRange.levelCount = 1;
      for (i = 0; i < nlevels; i++)
      {
         vi.subresourceRange.baseMipLevel = i;
         CHECK(vkCreateImageView(device, &vi, NULL, &im.levels[i]));
      }
   }
   return im;
}

static void free_image(struct image *im)
{
   unsigned i;
   for (i = 0; i < im->nlevels; i++)
      if (im->levels[i])
         vkDestroyImageView(device, im->levels[i], NULL);
   vkDestroyImageView(device, im->sampled, NULL);
   vkDestroyImage(device, im->image, NULL);
   vkFreeMemory(device, im->memory, NULL);
}

static void barrier(VkImage image, unsigned nlevels, VkImageLayout from, VkImageLayout to,
      VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
   b.srcAccessMask = src;
   b.dstAccessMask = dst;
   b.oldLayout = from;
   b.newLayout = to;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = image;
   b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   b.subresourceRange.levelCount = nlevels;
   b.subresourceRange.layerCount = 1;
   vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void begin(void)
{
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   CHECK(vkResetCommandBuffer(cmd, 0));
   CHECK(vkBeginCommandBuffer(cmd, &bi));
}

static void submit(void)
{
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   CHECK(vkEndCommandBuffer(cmd));
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   CHECK(vkResetFences(device, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull));
   CHECK(vkResetDescriptorPool(device, desc_pool, 0));
}

static VkDescriptorSet alloc_set(VkDescriptorSetLayout layout)
{
   VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkDescriptorSet set;
   ai.descriptorPool = desc_pool;
   ai.descriptorSetCount = 1;
   ai.pSetLayouts = &layout;
   CHECK(vkAllocateDescriptorSets(device, &ai, &set));
   return set;
}

/* the format of the image the renderer has for a texture of TextureType @type */
static VkFormat cpu_format(int type)
{
   switch (type)
   {
   case 0: return VK_FORMAT_R5G6B5_UNORM_PACK16;     /* _565 */
   case 1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;   /* _5551 */
   case 2: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;   /* _4444 */
   case 3: return VK_FORMAT_R8G8B8A8_UNORM;          /* _8888 */
   default: return VK_FORMAT_R8_UNORM;               /* _8 */
   }
}

static unsigned texel_bytes(unsigned out)
{
   return out == TEXD_OUT32 ? 4 : out == TEXD_OUT16 ? 2 : 1;
}

/* where level @i of the plan's (0 the smallest) is in what the CPU made */
static size_t level_at(const struct texd_plan *plan, unsigned i)
{
   size_t at = 0;
   unsigned j;
   if (!plan->mipmapped)
      return 0;
   for (j = 0; j < i; j++)
      at += (size_t)1 << (2 * j);
   return at * texel_bytes(plan->out);
}

/* 2. the kernel on the device */
static int run_kernel(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *bytes, unsigned nbytes)
{
   const unsigned px = texel_bytes(plan->out);
   const unsigned nlevels = plan->mipmapped ? plan->levels : 1;
   const VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
   const VkFormat view = px == 4 ? VK_FORMAT_R32_UINT : px == 2 ? VK_FORMAT_R16_UINT : VK_FORMAT_R8_UINT;
   const unsigned base = 64;   /* somewhere in the ring that is not its start */
   struct buffer ring = make_buffer(base * 4 + nbytes + 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   size_t total = (size_t)plan->width * plan->height * px * 2 + 64;
   struct buffer back = make_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   struct image im = make_image(cpu_format(type), view, plan->width, plan->height, nlevels, identity);
   VkDescriptorSet ring_set = alloc_set(ring_layout);
   VkDescriptorBufferInfo rbi;
   VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
   size_t at;
   unsigned i;
   int bad = 0;

   memcpy((unsigned char *)ring.mapped + base * 4, bytes, nbytes);
   rbi.buffer = ring.buffer;
   rbi.offset = 0;
   rbi.range = VK_WHOLE_SIZE;
   w.dstSet = ring_set;
   w.descriptorCount = 1;
   w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   w.pBufferInfo = &rbi;
   vkUpdateDescriptorSets(device, 1, &w, 0, NULL);

   begin();
   barrier(im.image, nlevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernels[plan->out]);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel_layout, 0, 1, &ring_set, 0, NULL);
   for (i = 0; i < plan->levels; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      VkDescriptorSet set = alloc_set(image_layout);
      VkDescriptorImageInfo ii;
      uint32_t pc[6];
      ii.sampler = VK_NULL_HANDLE;
      ii.imageView = im.levels[lv->mip];
      ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      w.dstSet = set;
      w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w.pBufferInfo = NULL;
      w.pImageInfo = &ii;
      vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel_layout, 1, 1, &set, 0, NULL);
      pc[0] = base; pc[1] = lv->data; pc[2] = plan->pal; pc[3] = lv->wh; pc[4] = lv->sr; pc[5] = lv->mode;
      vkCmdPushConstants(cmd, kernel_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
      vkCmdDispatch(cmd, ((lv->wh & 0xFFFF) + 7) / 8, ((lv->wh >> 16) + 7) / 8, 1);
   }
   barrier(im.image, nlevels, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   at = 0;
   for (i = 0; i < plan->levels; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      VkBufferImageCopy r;
      memset(&r, 0, sizeof(r));
      r.bufferOffset = at;
      r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      r.imageSubresource.mipLevel = lv->mip;
      r.imageSubresource.layerCount = 1;
      r.imageExtent.width = lv->wh & 0xFFFF;
      r.imageExtent.height = lv->wh >> 16;
      r.imageExtent.depth = 1;
      vkCmdCopyImageToBuffer(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back.buffer, 1, &r);
      at += ((size_t)(lv->wh & 0xFFFF) * (lv->wh >> 16) * px + 3) & ~(size_t)3;
   }
   submit();

   at = 0;
   for (i = 0; i < plan->levels && !bad; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      size_t n = (size_t)(lv->wh & 0xFFFF) * (lv->wh >> 16) * px;
      if (memcmp((unsigned char *)back.mapped + at, cpu_out + level_at(plan, i), n) != 0)
         bad = 1;
      at += (n + 3) & ~(size_t)3;
   }
   if (bad)
      fail(c, "kernel");
   free_image(&im);
   free_buffer(&back);
   free_buffer(&ring);
   return !bad;
}

/* 3. a planar 16-bit texture copied as it is, read against the CPU's */
static VkFormat native_format(int type, VkComponentMapping *swizzle)
{
   const VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
   const VkComponentMapping argb = { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B };
   *swizzle = identity;
   switch (type)
   {
   case 0: return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case 1: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
   case 2: *swizzle = argb; return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
   default: return VK_FORMAT_UNDEFINED;
   }
}

static void read_image(struct image *im, unsigned w, unsigned h, struct buffer *out)
{
   VkDescriptorSet set = alloc_set(cmp_layout);
   VkDescriptorImageInfo ii;
   VkDescriptorBufferInfo bi;
   VkWriteDescriptorSet ws[2];
   uint32_t pc[2];
   memset(ws, 0, sizeof(ws));
   ii.sampler = sampler;
   ii.imageView = im->sampled;
   ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   bi.buffer = out->buffer;
   bi.offset = 0;
   bi.range = VK_WHOLE_SIZE;
   ws[0].sType = ws[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   ws[0].dstSet = ws[1].dstSet = set;
   ws[0].descriptorCount = ws[1].descriptorCount = 1;
   ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   ws[0].pImageInfo = &ii;
   ws[1].dstBinding = 1;
   ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   ws[1].pBufferInfo = &bi;
   vkUpdateDescriptorSets(device, 2, ws, 0, NULL);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cmp_pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cmp_pipeline_layout, 0, 1, &set, 0, NULL);
   pc[0] = w;
   pc[1] = h;
   vkCmdPushConstants(cmd, cmp_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
   vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

static void upload(struct image *im, const struct buffer *from, unsigned row, unsigned w, unsigned h)
{
   VkBufferImageCopy r;
   memset(&r, 0, sizeof(r));
   r.bufferRowLength = row;
   r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   r.imageSubresource.layerCount = 1;
   r.imageExtent.width = w;
   r.imageExtent.height = h;
   r.imageExtent.depth = 1;
   barrier(im->image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyBufferToImage(cmd, from->buffer, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
   barrier(im->image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

static int run_native(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *bytes)
{
   const unsigned w = plan->width, h = plan->height, stride = plan->level[0].sr & 0xFFFF;
   const VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
   VkComponentMapping swizzle;
   const VkFormat nf = native_format(type, &swizzle);
   struct buffer raw = make_buffer(plan->bytes + 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   struct buffer conv = make_buffer((size_t)w * h * 2 + 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   struct buffer a = make_buffer((size_t)w * h * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   struct buffer b = make_buffer((size_t)w * h * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   struct image ni, ci;
   int ok;

   if (nf == VK_FORMAT_UNDEFINED)
   {
      fail(c, "native format");
      return 0;
   }
   ni = make_image(nf, VK_FORMAT_UNDEFINED, w, h, 1, swizzle);
   if (type == 2 && !supported(VK_FORMAT_R4G4B4A4_UNORM_PACK16, 0))
   {
      /* no R4G4B4A4 here: the CPU's texels read as what they are, red
       * at the top, through a format that has blue there */
      const VkComponentMapping bgra = { VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A };
      ci = make_image(VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_UNDEFINED, w, h, 1, bgra);
   }
   else
      ci = make_image(cpu_format(type), VK_FORMAT_UNDEFINED, w, h, 1, identity);
   memcpy(raw.mapped, bytes, plan->bytes);
   memcpy(conv.mapped, cpu_out, (size_t)w * h * 2);
   begin();
   upload(&ni, &raw, stride, w, h);
   upload(&ci, &conv, w, w, h);
   read_image(&ni, w, h, &a);
   read_image(&ci, w, h, &b);
   submit();
   ok = memcmp(a.mapped, b.mapped, (size_t)w * h * 16) == 0;
   if (!ok)
      fail(c, "copied as it is");
   free_image(&ni);
   free_image(&ci);
   free_buffer(&a);
   free_buffer(&b);
   free_buffer(&conv);
   free_buffer(&raw);
   return ok;
}

/* ---- the corpus ---- */

static unsigned cases, planned, unplanned, natives, gpu_cases;
static unsigned by_out[3], by_scan[4], mipmapped, levels_made;
static unsigned long long raw_bytes, converted_bytes;
static int use_gpu = 1;

static void run_case(struct texd_case *c)
{
   struct texd_plan plan;
   size_t bytes = 0;
   int type = 0;
   unsigned i, nbytes;
   unsigned char *raw;

   cases++;
   memset(&plan, 0, sizeof(plan));
   if (!texd_cpu_run(c, &plan, cpu_out, MAX_TEXELS * 4, &bytes, &type))
   {
      unplanned++;
      return;
   }
   planned++;
   by_out[plan.out]++;
   by_scan[(plan.level[0].mode >> 4) & 3]++;
   mipmapped += plan.mipmapped != 0;
   levels_made += plan.levels;
   if (bytes == 0)
   {
      fail(c, "no texels from the CPU");
      return;
   }
   /* the bytes as the renderer copies them: the texture's, the palette after */
   nbytes = plan.pal + plan.pal_entries * 4;
   raw = (unsigned char *)calloc(1, nbytes + 4);
   memcpy(raw, vram_mem + plan.src, plan.bytes);
   if (plan.pal_entries)
   {
      /* the palette tables are TexCache's; the shim's palette_update() made them */
      extern unsigned palette16_ram[1024], palette32_ram[1024];
      memcpy(raw + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);
   }
   raw_bytes += nbytes;
   converted_bytes += bytes;

   /* 1. the C text */
   for (i = 0; i < plan.levels; i++)
   {
      const struct texd_level *lv = &plan.level[i];
      size_t n = (size_t)(lv->wh & 0xFFFF) * (lv->wh >> 16) * texel_bytes(plan.out);
      texd_c_level(raw, &plan, lv, ref_out, lv->wh & 0xFFFF);
      if (memcmp(ref_out, cpu_out + level_at(&plan, i), n) != 0)
      {
         fail(c, "C text");
         break;
      }
   }
   if (use_gpu)
   {
      gpu_cases++;
      run_kernel(c, &plan, type, raw, nbytes);
      if (plan.native)
      {
         natives++;
         run_native(c, &plan, type, raw);
      }
   }
   free(raw);
}

static void random_vram(void)
{
   unsigned i;
   for (i = 0; i < VRAM_BYTES; i++)
      vram_mem[i] = (unsigned char)rnd();
}

int main(int argc, char **argv)
{
   static const unsigned sizes[][2] = {
      {0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 3}, {3, 2}, {0, 4}, {4, 0}, {5, 5}, {7, 0}, {0, 7}, {6, 3}, {7, 7}
   };
   const char *spv_dir = argc > 1 ? argv[1] : ".";
   unsigned fmt, layout, s, var, mip, lu;
   struct texd_case c;
   unsigned char *pal;
   unsigned i;

   if (argc > 2 && !strcmp(argv[2], "cpu"))
      use_gpu = 0;
   vram_mem = (unsigned char *)malloc(VRAM_BYTES);
   cpu_out = (unsigned char *)malloc(MAX_TEXELS * 4);
   ref_out = (unsigned char *)malloc(MAX_TEXELS * 4);
   random_vram();
   texd_cpu_init(vram_mem);
   pal = (unsigned char *)texd_cpu_palette_ram();
   for (i = 0; i < 4096; i++)
      pal[i] = (unsigned char)rnd();
   if (use_gpu)
      vk_init(spv_dir, getenv("TEXD_VALIDATE") && atoi(getenv("TEXD_VALIDATE")));

   for (fmt = 0; fmt < 8; fmt++)
      for (layout = 0; layout < 4; layout++)       /* twiddled, compressed, planar, planar compressed */
         for (mip = 0; mip < 2; mip++)
            for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]) + (mip ? 8 : 0); s++)
               for (var = 0; var < 6; var++)
               {
                  memset(&c, 0, sizeof(c));
                  c.fmt = fmt;
                  c.scan_order = layout >= 2;
                  c.vq = layout & 1;
                  c.mip = mip;
                  if (s < sizeof(sizes) / sizeof(sizes[0]))
                  {
                     c.tex_u = sizes[s][0];
                     c.tex_v = sizes[s][1];
                  }
                  else
                     c.tex_u = c.tex_v = s - sizeof(sizes) / sizeof(sizes[0]);   /* every mipmapped size */
                  /* the big ones once */
                  if ((c.tex_u + c.tex_v >= 12) && var > 0)
                     continue;
                  c.pal_ctrl = var & 3;
                  c.pal_select = rnd() & 63;
                  c.filter = (var >> 1) & 1;
                  c.force32 = var == 1 || var == 4;
                  c.lod0 = var == 5 && mip;
                  /* planar: no stride, a narrower one, a wider one, one of nothing */
                  if (c.scan_order)
                  {
                     static const unsigned strides[6] = { 0, 1, 2, 31, 4, 0 };
                     c.stride_sel = var != 0;
                     c.text_control = strides[var];
                  }
                  c.addr = (rnd() % (VRAM_BYTES - 0x300000)) & ~7u;
                  /* and one planar texture that runs into the end of video memory */
                  if (c.scan_order && var == 3)
                     c.addr = (VRAM_BYTES - (8u << c.tex_u) * (8u << c.tex_v)) & ~7u;
                  run_case(&c);
               }

   /* random ones */
   for (lu = 0; lu < 2000; lu++)
   {
      memset(&c, 0, sizeof(c));
      c.fmt = rnd() & 7;
      c.scan_order = rnd() & 1;
      c.vq = rnd() & 1;
      c.mip = rnd() & 1;
      c.stride_sel = rnd() & 1;
      c.pal_select = rnd() & 63;
      c.tex_u = rnd() % 6;
      c.tex_v = rnd() % 6;
      c.filter = rnd() & 1;
      c.force32 = (rnd() & 3) == 0;
      c.lod0 = (rnd() & 7) == 0;
      c.text_control = rnd() & 31;
      c.pal_ctrl = rnd() & 3;
      c.addr = (rnd() % (VRAM_BYTES - 0x100000)) & ~7u;
      run_case(&c);
   }

   printf("texdecode: %u textures, %u planned for the GPU (%u not: what the GPU is not given), %u through the kernel%s, %u of them copied as they are too\n",
         cases, planned, unplanned, gpu_cases, use_gpu ? "" : " (none: no GPU)", natives);
   printf("texdecode: to 8 bits %u, 16 bits %u, 32 bits %u; twiddled %u, compressed %u, planar %u, planar compressed %u; mipmapped %u; %u levels\n",
         by_out[0], by_out[1], by_out[2], by_scan[0], by_scan[1], by_scan[2], by_scan[3], mipmapped, levels_made);
   if (integer_images)
      printf("texdecode: %u images made as integers, the device having no image of the texture's format for the kernel\n", integer_images);
   printf("texdecode: bytes copied for the GPU %llu against %llu the CPU converted (%.1f%%)\n",
         raw_bytes, converted_bytes, converted_bytes ? 100.0 * raw_bytes / converted_bytes : 0.0);
   if (validation_errors)
      printf("FAIL: %d messages from the validation layer\n", validation_errors);
   if (failures || validation_errors)
   {
      printf("FAIL: %d textures differ\n", failures);
      return 1;
   }
   printf("texdecode: all the same, byte for byte\n");
   return 0;
}
