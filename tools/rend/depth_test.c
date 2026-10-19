/* The raster renderers' depth (rend/depth_scale.h): that a pass which
 * tests EQUAL against another pass's depth is exact when the depth is the
 * position's, placed by the same vertex text, and is not when each pass
 * writes a depth of its own from its fragment shader and one of them
 * rounds differently - what a driver does when it compiles each pipeline
 * apart, and what put black specks through the per-pixel renderers' decals
 * and shadows. See tools/rend/depth_test.sh, which makes the shaders and
 * runs the cases.
 *
 *   depth_test VERT_A FRAG_A VERT_B FRAG_B
 *        a floor drawn with pipeline A (depth written, greater or equal),
 *        then the same triangles again with pipeline B (EQUAL, a
 *        different fragment shader and blend state: another pipeline); the
 *        pixels A covered that B did not are the specks: printed, and the
 *        exit code is 0 for none, 1 for some
 *   depth_test -s
 *        the shipped modules (vulkan/raster_spv.inc) looked at: every
 *        vertex module decorates its position invariant, no fragment
 *        module writes the depth
 *
 * The floor is in the renderers' own terms: the game's vertices (x, y in
 * pixels, z = 1/w), the renderers' matrix and depth scale in the uniform
 * block of shaders/position.glsl, a 32-bit float depth buffer cleared to 0.
 * Plain C99 and the Vulkan loader; Mesa's software device (lavapipe) is
 * enough. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 320
#define H 240

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s: %d\n", __FILE__, __LINE__, #x, (int)r_); exit(2); } } while (0)

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags)
{
   uint32_t i;
   for (i = 0; i < mem_props.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(2);
}

struct buffer { VkBuffer buffer; VkDeviceMemory memory; void *mapped; };

static struct buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
   struct buffer b;
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   bci.size = size;
   bci.usage = usage;
   CHECK(vkCreateBuffer(device, &bci, NULL, &b.buffer));
   vkGetBufferMemoryRequirements(device, b.buffer, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   CHECK(vkAllocateMemory(device, &ai, NULL, &b.memory));
   CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
   CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
   return b;
}

struct image { VkImage image; VkDeviceMemory memory; VkImageView view; };

static struct image make_image(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
   struct image im;
   VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
   VkMemoryRequirements req;
   VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
   VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
   ici.imageType = VK_IMAGE_TYPE_2D;
   ici.format = format;
   ici.extent.width = W;
   ici.extent.height = H;
   ici.extent.depth = 1;
   ici.mipLevels = 1;
   ici.arrayLayers = 1;
   ici.samples = VK_SAMPLE_COUNT_1_BIT;
   ici.tiling = VK_IMAGE_TILING_OPTIMAL;
   ici.usage = usage;
   ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   CHECK(vkCreateImage(device, &ici, NULL, &im.image));
   vkGetImageMemoryRequirements(device, im.image, &req);
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   CHECK(vkAllocateMemory(device, &ai, NULL, &im.memory));
   CHECK(vkBindImageMemory(device, im.image, im.memory, 0));
   vci.image = im.image;
   vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vci.format = format;
   vci.subresourceRange.aspectMask = aspect;
   vci.subresourceRange.levelCount = 1;
   vci.subresourceRange.layerCount = 1;
   CHECK(vkCreateImageView(device, &vci, NULL, &im.view));
   return im;
}

static VkShaderModule load_module(const char *path)
{
   FILE *f = fopen(path, "rb");
   long n;
   uint32_t *words;
   VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
   VkShaderModule m;
   if (f == NULL) { perror(path); exit(2); }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   words = (uint32_t *)malloc((size_t)n);
   if (fread(words, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
   fclose(f);
   ci.codeSize = (size_t)n;
   ci.pCode = words;
   CHECK(vkCreateShaderModule(device, &ci, NULL, &m));
   free(words);
   return m;
}

/* A pipeline of the two modules: @equal tests EQUAL and writes no depth
 * (pass B), else greater or equal and writes it (pass A); B blends, A
 * does not (so that the two differ in more than their shaders) */
static VkPipeline make_pipeline(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vert, VkShaderModule frag, int equal)
{
   VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
   VkVertexInputBindingDescription binding = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
   VkViewport viewport = { 0.f, 0.f, W, H, 0.f, 1.f };
   VkRect2D scissor = { { 0, 0 }, { W, H } };
   VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
   VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
   VkPipelineColorBlendAttachmentState att = { 0 };
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
   VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
   VkPipeline p;
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vert;
   stages[0].pName = "main";
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = frag;
   stages[1].pName = "main";
   vi.vertexBindingDescriptionCount = 1;
   vi.pVertexBindingDescriptions = &binding;
   vi.vertexAttributeDescriptionCount = 1;
   vi.pVertexAttributeDescriptions = &attr;
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
   vp.viewportCount = 1;
   vp.pViewports = &viewport;
   vp.scissorCount = 1;
   vp.pScissors = &scissor;
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.lineWidth = 1.f;
   ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   ds.depthTestEnable = VK_TRUE;
   ds.depthWriteEnable = equal ? VK_FALSE : VK_TRUE;
   ds.depthCompareOp = equal ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
   att.blendEnable = equal ? VK_TRUE : VK_FALSE;
   att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
   att.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
   att.colorBlendOp = VK_BLEND_OP_ADD;
   att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
   att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
   att.alphaBlendOp = VK_BLEND_OP_ADD;
   att.colorWriteMask = 15;
   cb.attachmentCount = 1;
   cb.pAttachments = &att;
   gci.stageCount = 2;
   gci.pStages = stages;
   gci.pVertexInputState = &vi;
   gci.pInputAssemblyState = &ia;
   gci.pViewportState = &vp;
   gci.pRasterizationState = &rs;
   gci.pMultisampleState = &ms;
   gci.pDepthStencilState = &ds;
   gci.pColorBlendState = &cb;
   gci.layout = layout;
   gci.renderPass = rp;
   CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, NULL, &p));
   return p;
}

/* The shipped modules (RASTER_SPV_WORDS: the words too) */
#define RASTER_SPV_WORDS
typedef uint32_t u32;
#include "../../core/rend/vulkan/raster_spv.inc"

/* Whether a module decorates something Invariant (the position), and
 * whether it has a FragDepth built-in */
static void module_decorations(const u32 *w, size_t bytes, int *invariant, int *fragdepth)
{
   size_t n = bytes / 4, i = 5;
   *invariant = 0;
   *fragdepth = 0;
   while (i < n)
   {
      u32 op = w[i] & 0xFFFFu, len = w[i] >> 16;
      if (len == 0)
         break;
      /* OpDecorate (71): target, decoration; OpMemberDecorate (72): type, member, decoration */
      if (op == 71 && len >= 3)
      {
         if (w[i + 2] == 18) *invariant = 1;
         if (w[i + 2] == 11 && len >= 4 && w[i + 3] == 22) *fragdepth = 1;
      }
      else if (op == 72 && len >= 4)
      {
         if (w[i + 3] == 18) *invariant = 1;
         if (w[i + 3] == 11 && len >= 5 && w[i + 4] == 22) *fragdepth = 1;
      }
      /* (the decorations come before the functions: OpFunction is 54) */
      if (op == 54)
         break;
      i += len;
   }
}

/* The modules that place the game's vertices (the others draw full-screen quads) */
static const int pvr_vertex_modules[] = { RSPV_TRI_VERT_G0, RSPV_TRI_VERT_G1, RSPV_MODVOL_VERT, RSPV_OIT_VERT_G0, RSPV_OIT_VERT_G1 };

static int shipped(void)
{
   int bad = 0, k;
   unsigned i;
   for (i = 0; i < sizeof(pvr_vertex_modules) / sizeof(pvr_vertex_modules[0]); i++)
   {
      int invariant, fragdepth;
      k = pvr_vertex_modules[i];
      module_decorations(raster_spv[k], raster_spv_size[k], &invariant, &fragdepth);
      if (!invariant)
      {
         printf("FAIL: vertex module %d does not declare its position invariant\n", k);
         bad = 1;
      }
   }
   for (k = 0; k < RSPV_COUNT; k++)
   {
      int invariant, fragdepth;
      module_decorations(raster_spv[k], raster_spv_size[k], &invariant, &fragdepth);
      if (fragdepth)
      {
         printf("FAIL: module %d writes gl_FragDepth\n", k);
         bad = 1;
      }
   }
   printf("depth_test: %d shipped modules: the polygons' vertex modules declare the position invariant, none writes the depth%s\n",
         (int)RSPV_COUNT, bad ? " - FAIL" : "");
   return bad;
}

int main(int argc, char **argv)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   uint32_t n = 1;
   float prio = 1.f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkPhysicalDeviceProperties props;
   VkAttachmentDescription atts[2] = { { 0 }, { 0 } };
   VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { 0 };
   VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
   VkRenderPass rp;
   VkDescriptorSetLayoutBinding ubo_binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorSetLayout dsl;
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout layout;
   VkDescriptorPoolSize psize = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkDescriptorPool pool;
   VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkDescriptorSet set;
   VkDescriptorBufferInfo binfo;
   VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
   struct image color, depth;
   struct buffer verts, ubo, out;
   VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
   VkImageView views[2];
   VkFramebuffer fb;
   VkPipeline pa, pb;
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkClearValue clears[2];
   VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
   VkDeviceSize zero = 0;
   VkBufferImageCopy region = { 0 };
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   float *v, *u;
   const uint8_t *px;
   unsigned covered = 0, specks = 0, x, y;
   /* the uniform block of shaders/position.glsl: the matrix takes the
    * game's 640x480 to clip space (as TransformMatrix does for the screen,
    * z untouched), then 1/fZ_max and the z scale's inverse */
   const float fz_max = 0.05f;
   if (argc == 2 && !strcmp(argv[1], "-s"))
      return shipped();
   if (argc != 5)
   {
      fprintf(stderr, "usage: %s VERT_A.spv FRAG_A.spv VERT_B.spv FRAG_B.spv | -s\n", argv[0]);
      return 2;
   }
   app.apiVersion = VK_API_VERSION_1_0;
   ici.pApplicationInfo = &app;
   CHECK(vkCreateInstance(&ici, NULL, &instance));
   CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
   vkGetPhysicalDeviceProperties(gpu, &props);
   printf("device: %s\n", props.deviceName);
   vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
   vkGetDeviceQueue(device, 0, 0, &queue);
   cpi.queueFamilyIndex = 0;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
   cai.commandPool = cmd_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

   color = make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   depth = make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
   atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
   atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
   atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   atts[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   atts[1].format = VK_FORMAT_D32_SFLOAT;
   atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
   atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
   sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sub.colorAttachmentCount = 1;
   sub.pColorAttachments = &color_ref;
   sub.pDepthStencilAttachment = &depth_ref;
   rpi.attachmentCount = 2;
   rpi.pAttachments = atts;
   rpi.subpassCount = 1;
   rpi.pSubpasses = &sub;
   CHECK(vkCreateRenderPass(device, &rpi, NULL, &rp));
   dli.bindingCount = 1;
   dli.pBindings = &ubo_binding;
   CHECK(vkCreateDescriptorSetLayout(device, &dli, NULL, &dsl));
   pli.setLayoutCount = 1;
   pli.pSetLayouts = &dsl;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
   dpi.maxSets = 1;
   dpi.poolSizeCount = 1;
   dpi.pPoolSizes = &psize;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
   dai.descriptorPool = pool;
   dai.descriptorSetCount = 1;
   dai.pSetLayouts = &dsl;
   CHECK(vkAllocateDescriptorSets(device, &dai, &set));

   /* the floor: a strip of four vertices, z (1/w) from 0.01 at the top to
    * 0.05 at the bottom, not a power of two apart from anything, so that
    * every pixel's depth has bits all the way down */
   verts = make_buffer(4 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   v = (float *)verts.mapped;
   v[0] = 13.f;  v[1] = 17.f;  v[2] = 0.0103f; v[3] = 1.f;
   v[4] = 627.f; v[5] = 23.f;  v[6] = 0.0117f; v[7] = 1.f;
   v[8] = 7.f;   v[9] = 461.f; v[10] = 0.0471f; v[11] = 1.f;
   v[12] = 633.f; v[13] = 473.f; v[14] = 0.0497f; v[15] = 1.f;
   ubo = make_buffer(80, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   u = (float *)ubo.mapped;
   memset(u, 0, 80);
   u[0] = 2.f / 640.f; u[5] = 2.f / 480.f; u[10] = 1.f; u[15] = 1.f;
   u[12] = -1.f; u[13] = -1.f;
   u[16] = 1.f / fz_max;	/* depth_scale */
   u[17] = 1.f;		/* depth_mul */
   binfo.buffer = ubo.buffer;
   binfo.offset = 0;
   binfo.range = 80;
   write.dstSet = set;
   write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   write.pBufferInfo = &binfo;
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
   out = make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

   views[0] = color.view;
   views[1] = depth.view;
   fci.renderPass = rp;
   fci.attachmentCount = 2;
   fci.pAttachments = views;
   fci.width = W;
   fci.height = H;
   fci.layers = 1;
   CHECK(vkCreateFramebuffer(device, &fci, NULL, &fb));

   pa = make_pipeline(rp, layout, load_module(argv[1]), load_module(argv[2]), 0);
   pb = make_pipeline(rp, layout, load_module(argv[3]), load_module(argv[4]), 1);

   CHECK(vkBeginCommandBuffer(cmd, &bi));
   memset(clears, 0, sizeof(clears));
   clears[1].depthStencil.depth = 0.f;
   rbi.renderPass = rp;
   rbi.framebuffer = fb;
   rbi.renderArea.extent.width = W;
   rbi.renderArea.extent.height = H;
   rbi.clearValueCount = 2;
   rbi.pClearValues = clears;
   vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   vkCmdBindVertexBuffers(cmd, 0, 1, &verts.buffer, &zero);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pa);
   vkCmdDraw(cmd, 4, 1, 0, 0);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
   vkCmdDraw(cmd, 4, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = W;
   region.imageExtent.height = H;
   region.imageExtent.depth = 1;
   vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
   CHECK(vkEndCommandBuffer(cmd));
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   /* A writes red, B green over it: a red pixel is one B did not cover */
   px = (const uint8_t *)out.mapped;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         const uint8_t *p = px + (y * W + x) * 4;
         if (p[0] != 0 || p[1] != 0)
            covered++;
         if (p[0] != 0 && p[1] == 0)
            specks++;
      }
   printf("depth_test: %u pixels drawn by pass A, %u of them missed by pass B (EQUAL)%s\n", covered, specks,
         specks != 0 ? ": SPECKS" : "");
   return specks != 0;
}
