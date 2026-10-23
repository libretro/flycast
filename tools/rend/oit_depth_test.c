/* The per-pixel renderer's depth test of a translucent fragment under
 * SSAA (vulkan/shaders/oit_sample_depth.glsl), on a device: that the
 * depth it tests each sample with is the one the device's depth test took
 * there, and that the depth it keeps the fragment with is in the
 * triangle. See tools/rend/oit_depth_test.sh, which makes the shaders and
 * runs the sample counts.
 *
 *   oit_depth_test N VERT FRAG_A FRAG_B [pdc]
 *        a model of small triangles that are not coplanar (the game's
 *        vertices, z = 1/w, in the renderers' own terms: shaders/
 *        position.glsl's uniform block, a D32 float depth buffer cleared
 *        to 0, larger nearer) drawn into N samples a pixel with FRAG_A,
 *        writing the depth (greater or equal), then the same triangles
 *        again in the next subpass with FRAG_B, once a pixel (no sample
 *        shading), the depth an input attachment (binding 5, as the
 *        renderer has it) and a sampled image too (binding 7: a device
 *        may shade a shader that reads a sample of an input attachment at
 *        each sample, as lavapipe does, and one that samples a texture
 *        once a pixel, as asked); with pdc, VK_EXT_post_depth_coverage
 *        on, for a FRAG_B that takes the samples the depth test passed
 *        (skipped, exit code 3, where the device has none): FRAG_B counts
 *        into a storage buffer what is printed. Each sample is covered by the
 *        same triangle in both passes, so a depth test done right sees
 *        every one of them.
 *
 * Plain C99 and the Vulkan loader; Mesa's software device (lavapipe) is
 * enough to run it, a GPU is what it is for. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 320
#define H 240
/* the model's grid, in the game's 640x480 */
#define GX 53
#define GY 41
/* what FRAG_B counts (the c[] of its storage buffer) */
#define C_WORDS 16

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s: %d\n", __FILE__, __LINE__, #x, (int)r_); exit(2); } } while (0)

static VkDevice device;
static VkPhysicalDeviceMemoryProperties mem_props;

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

static struct image make_image(VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect)
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
   ici.samples = samples;
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

/* subpass 0 (@second 0): depth written, greater or equal; subpass 1: no
 * depth attachment, the fragment shader specialized for @samples */
static VkPipeline make_pipeline(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vert, VkShaderModule frag,
      int second, VkSampleCountFlagBits samples)
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
   VkSpecializationMapEntry entry = { 0, 0, 4 };
   VkSpecializationInfo spec = { 1, &entry, 4, NULL };
   int32_t n = (int32_t)samples;
   VkPipeline p;
   spec.pData = &n;
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vert;
   stages[0].pName = "main";
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = frag;
   stages[1].pName = "main";
   stages[1].pSpecializationInfo = second ? &spec : NULL;
   vi.vertexBindingDescriptionCount = 1;
   vi.pVertexBindingDescriptions = &binding;
   vi.vertexAttributeDescriptionCount = 1;
   vi.pVertexAttributeDescriptions = &attr;
   ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   vp.viewportCount = 1;
   vp.pViewports = &viewport;
   vp.scissorCount = 1;
   vp.pScissors = &scissor;
   rs.polygonMode = VK_POLYGON_MODE_FILL;
   rs.cullMode = VK_CULL_MODE_NONE;
   rs.lineWidth = 1.f;
   ms.rasterizationSamples = samples;
   ms.sampleShadingEnable = VK_FALSE;
   /* (the second as the renderer's lists' pass: tested, greater or
    * equal, not written - before the shader only where it asks) */
   ds.depthTestEnable = VK_TRUE;
   ds.depthWriteEnable = second ? VK_FALSE : VK_TRUE;
   ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
   att.colorWriteMask = second ? 0 : 15;
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
   gci.subpass = second ? 1 : 0;
   CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gci, NULL, &p));
   return p;
}

/* a vertex's 1/w: a surface with folds and bumps, between 0.01 and 0.05,
 * not a power of two apart from anything */
static float model_z(unsigned i, unsigned j)
{
   uint32_t h = (i * 73856093u) ^ (j * 19349663u);
   float bump;
   h ^= h >> 13;
   h *= 0x5bd1e995u;
   h ^= h >> 15;
   bump = (float)(h & 1023u) / 1023.f;
   return 0.0103f + 0.0003f * (float)i + 0.00041f * (float)j + 0.0021f * bump;
}

int main(int argc, char **argv)
{
   VkInstance instance;
   VkPhysicalDevice gpu;
   VkQueue queue;
   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   uint32_t n = 1;
   float prio = 1.f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkPhysicalDeviceFeatures features = { 0 };
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkPhysicalDeviceProperties props;
   VkAttachmentDescription atts[2] = { { 0 }, { 0 } };
   VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
   /* (the renderer's lists' subpass: the depth read-only, both tested against and read) */
   VkAttachmentReference input_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
   VkSubpassDescription subs[2] = { { 0 }, { 0 } };
   VkSubpassDependency dep = { 0 };
   VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
   VkRenderPass rp;
   VkDescriptorSetLayoutBinding bindings[4] = {
      { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
      { 5, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
      { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
      { 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
   VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorSetLayout dsl;
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout layout;
   VkDescriptorPoolSize psizes[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkDescriptorPool pool;
   VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkDescriptorSet set;
   VkDescriptorBufferInfo ubo_info, out_info;
   VkDescriptorImageInfo depth_info, sampled_info;
   VkWriteDescriptorSet writes[4] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
   VkSampler sampler;
   struct image color, depth;
   struct buffer verts, ubo, out;
   VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
   VkImageView views[2];
   VkFramebuffer fb;
   VkPipeline pa, pb;
   VkShaderModule vert;
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkClearValue clears[2];
   VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
   VkDeviceSize zero = 0;
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   VkSampleCountFlagBits samples;
   float *v, *u;
   const uint32_t *c;
   unsigned i, j, nverts, k;
   const float fz_max = 0.05f;
   int bad, has_pdc = 0, pdc_wanted = argc == 6 && !strcmp(argv[5], "pdc");
   if ((argc != 5 && !(argc == 6 && !strcmp(argv[5], "pdc"))) || (atoi(argv[1]) != 1 && atoi(argv[1]) != 2 && atoi(argv[1]) != 4 && atoi(argv[1]) != 8))
   {
      fprintf(stderr, "usage: %s 1|2|4|8 VERT.spv FRAG_A.spv FRAG_B.spv [pdc]\n", argv[0]);
      return 2;
   }
   samples = (VkSampleCountFlagBits)atoi(argv[1]);
   app.apiVersion = VK_API_VERSION_1_0;
   ici.pApplicationInfo = &app;
   CHECK(vkCreateInstance(&ici, NULL, &instance));
   CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu));
   vkGetPhysicalDeviceProperties(gpu, &props);
   if (!(props.limits.framebufferDepthSampleCounts & samples) || !(props.limits.framebufferColorSampleCounts & samples))
   {
      printf("oit_depth_test: %s cannot draw %d samples a pixel: skipped\n", props.deviceName, (int)samples);
      return 3;
   }
   vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
   features.fragmentStoresAndAtomics = VK_TRUE;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.pEnabledFeatures = &features;
   {
      /* (VK_EXT_post_depth_coverage, for FRAG_B's that take the samples the depth test passed) */
      static const char *pdc = "VK_EXT_post_depth_coverage";
      VkExtensionProperties ext[512];
      uint32_t next = 512, e;
      vkEnumerateDeviceExtensionProperties(gpu, NULL, &next, ext);
      for (e = 0; e < next; e++)
         if (!strcmp(ext[e].extensionName, pdc))
         {
            dci.enabledExtensionCount = 1;
            dci.ppEnabledExtensionNames = &pdc;
            has_pdc = 1;
         }
   }
   if (pdc_wanted && !has_pdc)
   {
      printf("oit_depth_test: %s has no VK_EXT_post_depth_coverage: skipped\n", props.deviceName);
      return 3;
   }
   CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
   vkGetDeviceQueue(device, 0, 0, &queue);
   cpi.queueFamilyIndex = 0;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
   cai.commandPool = cmd_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));

   color = make_image(VK_FORMAT_R8G8B8A8_UNORM, samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
   depth = make_image(VK_FORMAT_D32_SFLOAT, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
         | VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_IMAGE_ASPECT_DEPTH_BIT);
   atts[0].format = VK_FORMAT_R8G8B8A8_UNORM;
   atts[0].samples = samples;
   atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   atts[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   atts[1].format = VK_FORMAT_D32_SFLOAT;
   atts[1].samples = samples;
   atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   subs[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subs[0].colorAttachmentCount = 1;
   subs[0].pColorAttachments = &color_ref;
   subs[0].pDepthStencilAttachment = &depth_ref;
   subs[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subs[1].colorAttachmentCount = 1;
   subs[1].pColorAttachments = &color_ref;
   subs[1].inputAttachmentCount = 1;
   subs[1].pInputAttachments = &input_ref;
   subs[1].pDepthStencilAttachment = &input_ref;
   dep.srcSubpass = 0;
   dep.dstSubpass = 1;
   dep.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
   dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
   dep.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
   dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   dep.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
   dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
   rpi.attachmentCount = 2;
   rpi.pAttachments = atts;
   rpi.subpassCount = 2;
   rpi.pSubpasses = subs;
   rpi.dependencyCount = 1;
   rpi.pDependencies = &dep;
   CHECK(vkCreateRenderPass(device, &rpi, NULL, &rp));
   dli.bindingCount = 4;
   dli.pBindings = bindings;
   CHECK(vkCreateDescriptorSetLayout(device, &dli, NULL, &dsl));
   pli.setLayoutCount = 1;
   pli.pSetLayouts = &dsl;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
   dpi.maxSets = 1;
   dpi.poolSizeCount = 4;
   dpi.pPoolSizes = psizes;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
   dai.descriptorPool = pool;
   dai.descriptorSetCount = 1;
   dai.pSetLayouts = &dsl;
   CHECK(vkAllocateDescriptorSets(device, &dai, &set));

   /* the model: a grid of cells a little over 12 game pixels (6 pixels
    * here) wide, two triangles each, every vertex at its own depth */
   nverts = (GX - 1) * (GY - 1) * 6;
   verts = make_buffer(nverts * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   v = (float *)verts.mapped;
   k = 0;
   for (j = 0; j + 1 < GY; j++)
      for (i = 0; i + 1 < GX; i++)
      {
         static const unsigned corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
         unsigned t;
         for (t = 0; t < 6; t++)
         {
            const unsigned ci = i + corner[t][0], cj = j + corner[t][1];
            v[k++] = 3.3f + (float)ci * 12.07f + ((ci * 7u + cj * 3u) % 5u) * 0.37f;
            v[k++] = 2.9f + (float)cj * 11.61f + ((ci * 5u + cj * 11u) % 7u) * 0.29f;
            v[k++] = model_z(ci, cj);
            v[k++] = 1.f;
         }
      }
   ubo = make_buffer(80, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   u = (float *)ubo.mapped;
   memset(u, 0, 80);
   u[0] = 2.f / 640.f; u[5] = 2.f / 480.f; u[10] = 1.f; u[15] = 1.f;
   u[12] = -1.f; u[13] = -1.f;
   u[16] = 1.f / fz_max;	/* depth_scale */
   u[17] = 1.f;		/* depth_mul */
   out = make_buffer(C_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   memset(out.mapped, 0, C_WORDS * 4);
   ((uint32_t *)out.mapped)[6] = 0xFFFFFFFFu;	/* (a minimum) */

   ubo_info.buffer = ubo.buffer;
   ubo_info.offset = 0;
   ubo_info.range = 80;
   depth_info.sampler = VK_NULL_HANDLE;
   depth_info.imageView = depth.view;
   depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
   CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
   sampled_info = depth_info;
   sampled_info.sampler = sampler;
   out_info.buffer = out.buffer;
   out_info.offset = 0;
   out_info.range = C_WORDS * 4;
   for (i = 0; i < 4; i++)
   {
      writes[i].dstSet = set;
      writes[i].dstBinding = bindings[i].binding;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = bindings[i].descriptorType;
   }
   writes[0].pBufferInfo = &ubo_info;
   writes[1].pImageInfo = &depth_info;
   writes[2].pBufferInfo = &out_info;
   writes[3].pImageInfo = &sampled_info;
   vkUpdateDescriptorSets(device, 4, writes, 0, NULL);

   views[0] = color.view;
   views[1] = depth.view;
   fci.renderPass = rp;
   fci.attachmentCount = 2;
   fci.pAttachments = views;
   fci.width = W;
   fci.height = H;
   fci.layers = 1;
   CHECK(vkCreateFramebuffer(device, &fci, NULL, &fb));

   vert = load_module(argv[2]);
   pa = make_pipeline(rp, layout, vert, load_module(argv[3]), 0, samples);
   pb = make_pipeline(rp, layout, vert, load_module(argv[4]), 1, samples);

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
   vkCmdDraw(cmd, nverts, 1, 0, 0);
   vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
   vkCmdDraw(cmd, nverts, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   CHECK(vkEndCommandBuffer(cmd));
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));

   /* FRAG_B's counts (oit_depth_test.sh says what each is) */
   c = (const uint32_t *)out.mapped;
   printf("device: %s, %d sample%s a pixel\n", props.deviceName, (int)samples, samples > 1 ? "s" : "");
   printf("fragments %u (%u of them on an edge, %u shaded at a sample), samples covered %u\n", c[0], c[1], c[12], c[2]);
   printf("the plane at each sample: %u samples missed (the device's depth there is nearer), %u its depth to the bit, "
         "%u ulp at most off\n", c[3], c[4], c[5]);
   printf("the centre's depth for all of them: %u samples missed\n", c[7]);
   printf("the kept depth outside its samples' depths: %u fragments (the centre's: %u)\n", c[8], c[9]);
   printf("gl_FragCoord.z is the depth at a sample it covers, to the bit, in %u of the edge fragments; "
         "the plane's at the centre within 2 ulp in %u\n", c[10], c[11]);
   bad = c[0] == 0 || c[3] != 0 || c[8] != 0;
   printf("oit_depth_test: %s\n", bad ? "FAIL" : "PASS");
   return bad;
}
