/* GPU-side TA processing, its decode (core/rend/ta_gpu.h): the kernels run
 * on a Vulkan device - or, built with TGT_GL, in OpenGL (ta_gpu_test_gl.c) -
 * and the same steps run one after the other on the CPU (the model), for
 * ta_gpu_test_cpu.cpp to compare with the core's own parse of each frame.
 * See ta_gpu_test.sh.
 *
 * The device is Mesa's software one where it is installed (lavapipe); the
 * validation layer is on with TAG_VALIDATE=1, and any error it reports
 * fails the test. Before any frame, the integer single precision of the
 * header (tag_fadd() and the others) is checked against this CPU's own
 * floating point on special values and a few million random ones. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef TGT_GL
#include <vulkan/vulkan.h>
#endif

/* the model's indices (before the header, which takes them as given) */
static unsigned *m_idx;
static unsigned m_room_idx;
#define TAG_IDX_PUT(at, v) do { if ((at) < m_room_idx) m_idx[at] = (v); } while (0)
#include "ta_gpu.h"
#include "ta_gpu_test.h"

/* ---- single precision on the bits, against the CPU ---- */

static unsigned rnd_state = 12345u;
static unsigned rnd(void)
{
   rnd_state = rnd_state * 1103515245u + 12345u;
   return (rnd_state >> 16) | ((rnd_state & 0xFFFFu) << 16);
}

static unsigned fbits(float f) { unsigned b; memcpy(&b, &f, 4); return b; }
static float bfloat(unsigned b) { float f; memcpy(&f, &b, 4); return f; }

/* A value worth trying: anything, or something near a boundary */
static unsigned pick(void)
{
   static const unsigned special[] = { 0u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u,
      0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FA00000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u,
      0x7F7FFFFFu, 0xFF7FFFFFu, 0x00400000u, 0x3F7FFFFFu, 0x4B000000u, 0x33800000u };
   unsigned r = rnd();
   switch (r % 8u)
   {
   case 0: return special[rnd() % (sizeof(special) / sizeof(special[0]))];
   case 1: return rnd() & 0x80FFFFFFu;            /* denormals and tiny ones */
   case 2: return (rnd() & 0x807FFFFFu) | 0x3F000000u;
   case 3: return (rnd() & 0x8000FFFFu) | ((rnd() % 4u + 125u) << 23);
   default: return rnd();
   }
}

/* x86 SSE gives a NaN operand's own bits, quietened, the first operand's
 * first; what the compiler made the first operand of a commutative one is
 * its business, so where both are NaNs any NaN will do */
static int same_result(unsigned want, unsigned got, unsigned a, unsigned b, int commutative)
{
   if (want == got)
      return 1;
   if (commutative && tag_isnan(a) && tag_isnan(b) && tag_isnan(want) && tag_isnan(got))
      return 1;
   return 0;
}

int tgt_softfloat_test(void)
{
   unsigned i, bad = 0;
   for (i = 0; i < 4000000u; i++)
   {
      unsigned a = pick(), b = pick();
      volatile float fa = bfloat(a), fb = bfloat(b);
      unsigned r;
      int op = (int)(i & 3u);
      if (op == 0) r = fbits(fa + fb);
      else if (op == 1) r = fbits(fa - fb);
      else if (op == 2) r = fbits(fa * fb);
      else r = fbits(fa / fb);
      {
         unsigned g = op == 0 ? tag_fadd(a, b) : op == 1 ? tag_fsub(a, b) : op == 2 ? tag_fmul(a, b) : tag_fdiv(a, b);
         if (!same_result(r, g, a, b, op == 0 || op == 2))
         {
            if (bad++ < 10)
               printf("softfloat: %08x %c %08x is %08x, made %08x\n", a, "+-*/"[op], b, r, g);
         }
      }
   }
   /* the comparisons and the integer conversion */
   for (i = 0; i < 1000000u; i++)
   {
      unsigned a = pick(), b = pick();
      volatile float fa = bfloat(a), fb = bfloat(b);
      if ((fa == fb) != (int)tag_feq(a, b) || (fa < fb) != (int)tag_flt(a, b))
      {
         if (bad++ < 10)
            printf("softfloat: comparing %08x and %08x\n", a, b);
      }
   }
   for (i = 0; i <= (1u << 24); i += 1u + (i >> 12))
      if (tag_u2f(i) != fbits((float)i) && bad++ < 10)
         printf("softfloat: %u as a float\n", i);
   printf("softfloat: add, subtract, multiply, divide and compare against this CPU: %s\n", bad ? "FAIL" : "the same");
   return bad == 0;
}

/* ---- What both devices and the model share ---- */

static struct tgt_sizes sizes;
static unsigned layout_words[13];
static unsigned work_words;

/* the model's own */
static unsigned *m_vtx, *m_mod, *m_work, *m_rec, *m_cmd, *m_frm, *m_trd, *m_trp;

/* The layout, as the renderers make it (the data from word 0), and the
 * model's buffers */
static void model_init(const struct tgt_sizes *sz, unsigned *layout)
{
   unsigned i, strips, w[9];
   sizes = *sz;
   strips = sz->list[0] + sz->list[1] + sz->list[2];
   work_words = tag_work_layout(sz->verts, strips, sz->headers, w);
   layout[0] = 0;
   for (i = 0; i < 9; i++)
      layout[1 + i] = w[i];
   layout[10] = 0;
   layout[11] = sz->list[0] * TAG_C_WORDS;
   layout[12] = (sz->list[0] + sz->list[1]) * TAG_C_WORDS;
   memcpy(layout_words, layout, sizeof(layout_words));

   m_vtx = (unsigned *)calloc((size_t)sz->verts * TAG_V_WORDS, 4);
   m_mod = (unsigned *)calloc((size_t)sz->modtrig * 9 + 1, 4);
   m_idx = (unsigned *)calloc(sz->idx, 4);
   m_room_idx = sz->idx;
   m_work = (unsigned *)calloc(work_words, 4);
   m_rec = (unsigned *)calloc((size_t)strips * TAG_C_WORDS, 4);
   m_cmd = (unsigned *)calloc((size_t)sz->headers * 2 * TAG_C_WORDS, 4);
   m_frm = (unsigned *)calloc(TAG_O_WORDS, 4);
   m_trd = (unsigned *)calloc((size_t)(sz->verts + sz->headers) * TAG_C_WORDS, 4);
   m_trp = (unsigned *)calloc((size_t)sz->list[2] * TAG_P_WORDS, 4);
}

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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

/* (tgt_sorted(): for the device's next decode, and the model's) */
static unsigned *srt_gpu, *srt_model;
static unsigned srt_gpu_words, srt_gpu_checks, srt_model_checks;

void tgt_sorted(unsigned *srt, unsigned words, unsigned checks)
{
   srt_gpu = srt_model = srt;
   srt_gpu_words = words;
   srt_gpu_checks = srt_model_checks = checks;
}

#ifdef TGT_GL
#include "ta_gpu_test_gl.c"
#else
/* ---- Vulkan ---- */

static int validation_errors;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDescriptorSetLayout set_layouts[2];
static VkPipelineLayout pipeline_layout;
static VkPipeline pipelines[TAG_KERNELS + 1];
static VkDescriptorPool desc_pool;
static VkDescriptorSet sets[2];

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

static VkPipeline make_pipeline(const char *path)
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
   pi.layout = pipeline_layout;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
   vkDestroyShaderModule(device, module, NULL);
   free(code);
   return pipeline;
}

static VkDescriptorSetLayout make_layout(unsigned n)
{
   VkDescriptorSetLayoutBinding b[16];   /* (set 1: B_COUNT - 1 of them) */
   VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
   VkDescriptorSetLayout layout;
   unsigned i;
   memset(b, 0, sizeof(b));
   for (i = 0; i < n; i++)
   {
      b[i].binding = i;
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
   }
   ci.bindingCount = n;
   ci.pBindings = b;
   CHECK(vkCreateDescriptorSetLayout(device, &ci, NULL, &layout));
   return layout;
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
   bi.usage = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

/* the buffers, in the kernels' bindings' order: the data, then set 1's */
enum { B_TA, B_TAB, B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_SRT, B_COUNT };
static struct buffer bufs[B_COUNT];

int tgt_validation_errors(void)
{
   return validation_errors;
}

void tgt_init(const char *spv_dir, int validate, const struct tgt_sizes *sz, unsigned *layout)
{
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
   const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
   const char *exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
   uint32_t n = 1;
   float prio = 1.f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
   VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
   VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkPhysicalDeviceProperties props;
   VkDescriptorPoolSize psize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, B_COUNT + 4 };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
   VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
   VkDescriptorBufferInfo infos[B_COUNT];
   VkWriteDescriptorSet writes[2];
   char path[1024];
   unsigned i, strips;

   sizes = *sz;
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
   qci.queueFamilyIndex = 0;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   CHECK(vkCreateDevice(gpu, &dci, NULL, &device));
   vkGetDeviceQueue(device, 0, 0, &queue);
   cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   cpi.queueFamilyIndex = 0;
   CHECK(vkCreateCommandPool(device, &cpi, NULL, &cmd_pool));
   cai.commandPool = cmd_pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = 1;
   CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   CHECK(vkCreateFence(device, &fci, NULL, &fence));

   set_layouts[0] = make_layout(1);
   set_layouts[1] = make_layout(B_COUNT - 1);
   pli.setLayoutCount = 2;
   pli.pSetLayouts = set_layouts;
   CHECK(vkCreatePipelineLayout(device, &pli, NULL, &pipeline_layout));
   for (i = 1; i <= TAG_KERNELS; i++)
   {
      snprintf(path, sizeof(path), "%s/k%u.spv", spv_dir, i);
      pipelines[i] = make_pipeline(path);
   }

   model_init(sz, layout);
   strips = sz->list[0] + sz->list[1] + sz->list[2];

   bufs[B_TA] = make_buffer((VkDeviceSize)sz->ta_words * 4, 0);
   bufs[B_TAB] = make_buffer((VkDeviceSize)sz->table_words * 4, 0);
   bufs[B_VTX] = make_buffer((VkDeviceSize)sz->verts * TAG_V_WORDS * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   bufs[B_MOD] = make_buffer((VkDeviceSize)sz->modtrig * 9 * 4 + 4, 0);
   bufs[B_IDX] = make_buffer((VkDeviceSize)sz->idx * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   bufs[B_WORK] = make_buffer((VkDeviceSize)work_words * 4, 0);
   bufs[B_REC] = make_buffer((VkDeviceSize)strips * TAG_C_WORDS * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   /* (and the second commands for shadows, TAG_T_SHD_CMD) */
   bufs[B_CMD] = make_buffer((VkDeviceSize)sz->headers * 2 * TAG_C_WORDS * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   bufs[B_FRM] = make_buffer(TAG_O_WORDS * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   bufs[B_TRD] = make_buffer((VkDeviceSize)(sz->verts + sz->headers) * TAG_C_WORDS * 4, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   bufs[B_TRP] = make_buffer((VkDeviceSize)sz->list[2] * TAG_P_WORDS * 4, 0);
   /* (the sorted indices' room, as the renderers have it: six words a triangle a frame can have) */
   bufs[B_SRT] = make_buffer((VkDeviceSize)sz->idx * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

   dpi.maxSets = 2;
   dpi.poolSizeCount = 1;
   dpi.pPoolSizes = &psize;
   CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &desc_pool));
   dai.descriptorPool = desc_pool;
   dai.descriptorSetCount = 2;
   dai.pSetLayouts = set_layouts;
   CHECK(vkAllocateDescriptorSets(device, &dai, sets));
   for (i = 0; i < B_COUNT; i++)
   {
      infos[i].buffer = bufs[i].buffer;
      infos[i].offset = 0;
      infos[i].range = VK_WHOLE_SIZE;
   }
   memset(writes, 0, sizeof(writes));
   writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   writes[0].dstSet = sets[0];
   writes[0].descriptorCount = 1;
   writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   writes[0].pBufferInfo = &infos[B_TA];
   writes[1].dstSet = sets[1];
   writes[1].descriptorCount = B_COUNT - 1;
   writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   writes[1].pBufferInfo = &infos[B_TAB];
   vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
}

static void barrier(VkAccessFlags src, VkPipelineStageFlags src_stage, VkAccessFlags dst, VkPipelineStageFlags dst_stage)
{
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
   mb.srcAccessMask = src;
   mb.dstAccessMask = dst;
   vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void compute_barrier(void)
{
   barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
}

static void dispatch(unsigned k, unsigned groups)
{
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[k]);
   if (groups != 0)
      vkCmdDispatch(cmd, groups, 1, 1);
   compute_barrier();
}

static void dispatch_indirect(unsigned k, unsigned at)
{
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[k]);
   vkCmdDispatchIndirect(cmd, bufs[B_FRM].buffer, (VkDeviceSize)at * 4);
   compute_barrier();
}

void tgt_gpu(const void *ta, unsigned ta_bytes, const unsigned *table, unsigned table_words, unsigned trd, struct tgt_out *o, double *ms)
{
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
   const unsigned nv = table[TAG_T_NVERTS], nmod = table[TAG_T_NMOD], nslot = table[TAG_T_NSLOT];
   const unsigned scan_groups = (nv + TAG_SCAN_N - 1u) / TAG_SCAN_N;
   double t0;

   const unsigned checks = srt_gpu != NULL ? srt_gpu_checks : 0;

   memcpy(bufs[B_TA].mapped, ta, ta_bytes);
   memcpy(bufs[B_TAB].mapped, table, (size_t)table_words * 4);
   if (checks != 0)
      memcpy(bufs[B_SRT].mapped, srt_gpu, (size_t)srt_gpu_words * 4);
   /* what is not written this frame is left from the one before, as it
    * would be in the renderer */
   CHECK(vkResetCommandBuffer(cmd, 0));
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   vkCmdFillBuffer(cmd, bufs[B_FRM].buffer, 0, TAG_O_WORDS * 4, 0);
   vkCmdFillBuffer(cmd, bufs[B_FRM].buffer, TAG_O_ZMAX * 4, 4, 0x3F800000u);
   if (nslot != 0)
      vkCmdFillBuffer(cmd, bufs[B_WORK].buffer, (VkDeviceSize)table[TAG_T_W_SLOTH] * 4, (VkDeviceSize)nslot * 4, 0);
   if (trd != 0)
      vkCmdFillBuffer(cmd, bufs[B_TRD].buffer, 0, (VkDeviceSize)trd * TAG_C_WORDS * 4, 0);
   barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 2, sets, 0, NULL);
   dispatch(TAG_K_UNPACK, (nv + nmod + 63u) / 64u);
   if (checks != 0)
      dispatch(TAG_K_PREPASS, checks < 65535u ? checks : 65535u);
   dispatch(TAG_K_COUNT, scan_groups);
   dispatch(TAG_K_COUNTSUM, 1);
   dispatch(TAG_K_ENDS, scan_groups);
   dispatch_indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
   dispatch_indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
   dispatch(TAG_K_GROUPSUM, 1);
   dispatch_indirect(TAG_K_STATES, TAG_O_ARGSCH);
   dispatch_indirect(TAG_K_EMIT, TAG_O_ARGS64);
   dispatch(TAG_K_FINISH, (nslot + 63u) / 64u);
   if (table[TAG_T_NSHD] != 0)
      dispatch(TAG_K_SHADOW, table[TAG_T_NSHD] < 65535u ? table[TAG_T_NSHD] : 65535u);
   if (trd != 0)
      dispatch_indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
   if (table[TAG_T_FLAGS] & TAG_FLAG_BLEED)
      dispatch_indirect(TAG_K_BLEED, TAG_O_ARGS1);
   if (table[TAG_T_FLAGS] & TAG_FLAG_PROVOKE)
      dispatch_indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
   barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
   CHECK(vkEndCommandBuffer(cmd));
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cmd;
   t0 = now_ms();
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
   *ms = now_ms() - t0;
   CHECK(vkResetFences(device, 1, &fence));
   if (checks != 0)
      memcpy(srt_gpu, bufs[B_SRT].mapped, (size_t)srt_gpu_words * 4);
   srt_gpu = NULL;

   o->vtx = (const unsigned *)bufs[B_VTX].mapped;
   o->mod = (const unsigned *)bufs[B_MOD].mapped;
   o->idx = (const unsigned *)bufs[B_IDX].mapped;
   o->rec = (const unsigned *)bufs[B_REC].mapped;
   o->cmd = (const unsigned *)bufs[B_CMD].mapped;
   o->frm = (const unsigned *)bufs[B_FRM].mapped;
   o->work = (const unsigned *)bufs[B_WORK].mapped;
   o->trd = (const unsigned *)bufs[B_TRD].mapped;
   o->trp = (const unsigned *)bufs[B_TRP].mapped;
}

#endif

/* ---- The model: the kernels' steps, one after the other ---- */

static unsigned m_record(unsigned k, unsigned hdr)
{
   const unsigned *tab = tag_c_tab;
   unsigned info = tab[hdr + TAG_H_INFO];
   unsigned seg = TAG_O_SEG + TAG_INFO_SEG(info) * TAG_OS_WORDS;
   unsigned li = m_frm[seg + TAG_OS_LBASE] + (k - m_frm[seg + TAG_OS_SFIRST]);
   unsigned list = TAG_INFO_LIST(info);
   unsigned base = list == 0 ? tab[TAG_T_REC_OP] : list == 1 ? tab[TAG_T_REC_PT] : tab[TAG_T_REC_TR];
   unsigned room = list == 0 ? tab[TAG_T_ROOM_OP] : list == 1 ? tab[TAG_T_ROOM_PT] : tab[TAG_T_ROOM_TR];
   return li < room ? base + li * TAG_C_WORDS : TAG_NONE;
}

/* The state after strips 0 to k-1, from the scan's element of them: what
 * the GPU's workgroups make, against the one after the other */
static int m_check_scan(const tag_elem *pre, unsigned at, unsigned bits, unsigned l, unsigned hd, unsigned last)
{
   if (pre->first == TAG_NONE)
      return last == TAG_NONE;
   return pre->n[0] == at && (pre->bits[0] & (TAG_E_ODD | TAG_E_HAS)) == bits
         && ((bits & TAG_E_HAS) == 0 || (pre->l[0] == l && pre->hd[0] == hd)) && pre->last == last;
}

int tgt_model(const void *ta, const unsigned *table, unsigned trd, struct tgt_out *o)
{
   const unsigned nv = table[TAG_T_NVERTS], nmod = table[TAG_T_NMOD];
   const unsigned room = table[TAG_T_ROOM_OP] + table[TAG_T_ROOM_PT] + table[TAG_T_ROOM_TR];
   unsigned *vflag = m_work + table[TAG_T_W_VFLAG];
   unsigned *send = m_work + table[TAG_T_W_SEND];
   unsigned *ssum = m_work + table[TAG_T_W_SSUM];
   unsigned *sst = m_work + table[TAG_T_W_SSTATE];
   unsigned *sloth = m_work + table[TAG_T_W_SLOTH];
   unsigned v, t, k, i, p, ns = 0, nseg = table[TAG_T_NSEG];
   unsigned at = 0, bits = 0, l = 0, hd = 0, last = TAG_NONE;
   int scan_ok = 1;
   tag_elem pre = tag_elem_none();

   tag_c_set((const tag_u32 *)ta, table, vflag);
   memset(m_frm, 0, TAG_O_WORDS * 4);
   m_frm[TAG_O_ZMAX] = 0x3F800000u;
   memset(sloth, 0, table[TAG_T_NSLOT] * 4);

   for (v = 0; v < nv; v++)
   {
      tag_unpacked u = tag_unpack(v);
      memcpy(&m_vtx[v * TAG_V_WORDS], u.v.w, sizeof(u.v.w));
      vflag[u.place] = u.flags;
      if (u.zcount && u.v.w[TAG_V_Z] > m_frm[TAG_O_ZMAX])
         m_frm[TAG_O_ZMAX] = u.v.w[TAG_V_Z];
   }
   {
      float z, s;
      memcpy(&z, &m_frm[TAG_O_ZMAX], 4);
      s = 1.0f / z;
      memcpy(&m_frm[TAG_O_ZSCALE], &s, 4);
   }
   for (t = 0; t < nmod; t++)
      for (i = 0; i < 9; i++)
         m_mod[t * 9 + i] = tag_modtrig_word(t, i);

   /* the prepass's checks (TAG_K_PREPASS), of the vertices as unpacked */
   if (srt_model != NULL && srt_model_checks != 0 && table[TAG_T_PREP] != TAG_NONE)
   {
      const unsigned base = table[TAG_T_PREP];
      for (k = 0; k < srt_model[base]; k++)
      {
         const unsigned *c = &srt_model[base + 1 + k * TAG_PC_WORDS];
         unsigned any = 0;
         for (v = c[TAG_PC_V]; v < c[TAG_PC_V] + c[TAG_PC_NV] && v < nv; v++)
         {
            const unsigned *w = &m_vtx[v * TAG_V_WORDS];
            any |= tag_prepass_vertex(w[TAG_V_X], w[TAG_V_Y], w[TAG_V_Z], w[TAG_V_COL]);
         }
         if (any == 0)
            for (t = c[TAG_PC_T]; t < c[TAG_PC_T] + c[TAG_PC_NT] && t * 3 + 2 < base; t++)
               srt_model[t * 3 + 1] = srt_model[t * 3 + 2] = srt_model[t * 3];
      }
   }
   srt_model = NULL;

   for (p = 0; p < nv; p++)
      if (vflag[p] & TAG_VF_EOS)
      {
         if (ns < room)
            send[ns] = p;
         ns++;
      }
   if (ns > room)
   {
      ns = room;
      m_frm[TAG_O_OVERRUN] |= 16;
   }
   m_frm[TAG_O_NSTRIPS] = ns;

   for (k = 0; k < ns; k++)
   {
      unsigned hdr = tag_find_header(send[k]);
      unsigned p0 = table[hdr + TAG_H_VSTART];
      tag_strip s;
      if (k > 0 && send[k - 1] + 1 > p0)
         p0 = send[k - 1] + 1;
      s = tag_strip_summary(p0, send[k], hdr);
      ssum[k * TAG_SS_WORDS] = s.n[0];
      ssum[k * TAG_SS_WORDS + 1] = s.n[1];
      ssum[k * TAG_SS_WORDS + 2] = s.n[2];
      ssum[k * TAG_SS_WORDS + 3] = s.last;
      ssum[k * TAG_SS_WORDS + 4] = s.hdr;
      ssum[k * TAG_SS_WORDS + 5] = s.first;
   }

   /* the segments */
   for (i = 0; i < nseg; i++)
   {
      unsigned vs = table[table[TAG_T_OFF_SEG] + i * TAG_G_WORDS + TAG_G_VSTART], lo = 0, hi = ns;
      while (lo < hi)
      {
         unsigned mid = (lo + hi) / 2;
         if (send[mid] < vs)
            lo = mid + 1;
         else
            hi = mid;
      }
      m_frm[TAG_O_SEG + i * TAG_OS_WORDS + TAG_OS_SFIRST] = lo;
   }
   {
      unsigned lbase[3] = { 0, 0, 0 };
      unsigned rooms[3];
      rooms[0] = table[TAG_T_ROOM_OP];
      rooms[1] = table[TAG_T_ROOM_PT];
      rooms[2] = table[TAG_T_ROOM_TR];
      for (i = 0; i < nseg; i++)
      {
         unsigned s = TAG_O_SEG + i * TAG_OS_WORDS;
         unsigned end = i + 1 < nseg ? m_frm[s + TAG_OS_WORDS + TAG_OS_SFIRST] : ns;
         unsigned n = end - m_frm[s + TAG_OS_SFIRST];
         m_frm[s + TAG_OS_STRIPS] = n;
         m_frm[s + TAG_OS_LBASE] = lbase[i % 3];
         lbase[i % 3] += n;
         if (i % 3 == 2)
         {
            m_frm[TAG_O_PASS + (i / 3) * 3] = lbase[0];
            m_frm[TAG_O_PASS + (i / 3) * 3 + 1] = lbase[1];
            m_frm[TAG_O_PASS + (i / 3) * 3 + 2] = lbase[2];
         }
      }
      for (i = 0; i < 3; i++)
         if (lbase[i] + 1 > rooms[i])
            m_frm[TAG_O_OVERRUN] |= 2u << i;
   }

   /* the states, one strip after the other, and the scan's elements
    * folded beside them */
   for (k = 0; k < ns; k++)
   {
      tag_strip st;
      unsigned y, has, r;
      st.n[0] = ssum[k * TAG_SS_WORDS];
      st.n[1] = ssum[k * TAG_SS_WORDS + 1];
      st.n[2] = ssum[k * TAG_SS_WORDS + 2];
      st.last = ssum[k * TAG_SS_WORDS + 3];
      st.hdr = ssum[k * TAG_SS_WORDS + 4];
      st.first = ssum[k * TAG_SS_WORDS + 5];
      if (!m_check_scan(&pre, at, bits, l, hd, last))
         scan_ok = 0;
      y = last == TAG_NONE ? 0 : tag_case(bits, last, st.hdr);
      has = (st.last & 0x80000000u) != 0;
      r = m_record(k, st.hdr);
      sst[k * TAG_ST_WORDS] = at;
      sst[k * TAG_ST_WORDS + 1] = y;
      sst[k * TAG_ST_WORDS + 2] = l;
      if (y == 0)
      {
         hd = k;
         bits = ((st.n[0] & 1) ? TAG_E_ODD : 0) | (has ? TAG_E_HAS : 0);
         if (st.n[0] != 0 && table[st.hdr + TAG_H_SLOT] != TAG_NONE)
            sloth[table[st.hdr + TAG_H_SLOT]] = k + 1;
      }
      else
         bits = (((st.n[y] + (y == 2)) & 1) ? TAG_E_ODD : 0) | TAG_E_HAS;
      sst[k * TAG_ST_WORDS + 3] = hd;
      if (has)
         l = st.last & 0x7FFFFFFFu;
      at += st.n[y];
      last = st.hdr;
      if (r != TAG_NONE)
      {
         m_rec[r + TAG_C_COUNT] = 0;
         m_rec[r + TAG_C_INST] = 1;
         m_rec[r + TAG_C_FIRST] = y == 0 ? at - st.n[y] : st.first;
         m_rec[r + 3] = 0;
         m_rec[r + 4] = 0;
      }
      pre = tag_combine(pre, tag_elem_strip(st, k));
   }
   m_frm[TAG_O_NIDX] = at;
   if (at > table[TAG_T_ROOM_IDX])
      m_frm[TAG_O_OVERRUN] |= 1;

   for (k = 0; k < ns; k++)
   {
      unsigned hdr = ssum[k * TAG_SS_WORDS + 4], p0 = table[hdr + TAG_H_VSTART], n, s0 = sst[k * TAG_ST_WORDS];
      unsigned head = sst[k * TAG_ST_WORDS + 3];
      if (k > 0 && send[k - 1] + 1 > p0)
         p0 = send[k - 1] + 1;
      n = tag_strip_emit(p0, send[k], sst[k * TAG_ST_WORDS + 1], sst[k * TAG_ST_WORDS + 2], s0) - s0;
      if (n != 0)
      {
         unsigned r = m_record(head, ssum[head * TAG_SS_WORDS + 4]);
         if (r != TAG_NONE)
            m_rec[r + TAG_C_COUNT] += n;
      }
   }

   for (i = 0; i < table[TAG_T_NSLOT]; i++)
   {
      unsigned h = sloth[i], count = 0, first = 0;
      if (h != 0 && m_frm[TAG_O_OVERRUN] == 0)
      {
         unsigned r = m_record(h - 1, ssum[(h - 1) * TAG_SS_WORDS + 4]);
         if (r != TAG_NONE)
         {
            count = m_rec[r + TAG_C_COUNT];
            first = m_rec[r + TAG_C_FIRST];
         }
      }
      m_cmd[i * TAG_C_WORDS] = count;
      m_cmd[i * TAG_C_WORDS + 1] = 1;
      m_cmd[i * TAG_C_WORDS + 2] = first;
      m_cmd[i * TAG_C_WORDS + 3] = 0;
      m_cmd[i * TAG_C_WORDS + 4] = 0;
   }

   /* the draw groups for shadows: each one's rectangle, one index after
    * the other, against its render pass's */
   for (i = 0; i < table[TAG_T_NSHD]; i++)
   {
      unsigned base = table[TAG_T_OFF_SHD], w = table[base + TAG_SH_ENTRY + i];
      unsigned at = TAG_SH_SLOT(w) * TAG_C_WORDS, a = base + TAG_SH_AREA + TAG_SH_PASS(w) * 4u;
      unsigned count = m_cmd[at + TAG_C_COUNT], first = m_cmd[at + TAG_C_FIRST], o;
      tag_bounds b = tag_bounds_none();
      for (k = 0; k < count; k++)
      {
         const unsigned *x = &m_vtx[m_idx[first + k] * TAG_V_WORDS];
         b = tag_bounds_add(b, x[TAG_V_X], x[TAG_V_Y]);
      }
      o = (table[TAG_T_SHD_CMD] + TAG_SH_SLOT(w)) * TAG_C_WORDS;
      m_cmd[o + TAG_C_COUNT] = count != 0 && tag_bounds_meet(b, table[a], table[a + 1], table[a + 2], table[a + 3]) ? count : 0;
      m_cmd[o + TAG_C_INST] = 1;
      m_cmd[o + TAG_C_FIRST] = first;
      m_cmd[o + 3] = 0;
      m_cmd[o + 4] = 0;
   }

   if ((table[TAG_T_FLAGS] & TAG_FLAG_BLEED) && m_frm[TAG_O_OVERRUN] == 0)
      for (k = 0; k < ns; k++)
      {
         unsigned hdr = ssum[k * TAG_SS_WORDS + 4], r, f, c, a, b, z, ok = 1, tsp, w, h;
         if (sst[k * TAG_ST_WORDS + 1] != 0)
            continue;
         r = m_record(k, hdr);
         if (r == TAG_NONE || !TAG_INFO_TEX(table[hdr + TAG_H_INFO]) || m_rec[r + TAG_C_COUNT] < 3)
            continue;
         f = m_rec[r + TAG_C_FIRST];
         c = m_rec[r + TAG_C_COUNT];
         a = m_idx[f];
         b = m_idx[f + c - 1];
         z = m_vtx[a * TAG_V_WORDS + TAG_V_Z];
         for (v = a; v <= b; v++)
         {
            const unsigned *x = &m_vtx[v * TAG_V_WORDS];
            if (!tag_bleed_ok(x[TAG_V_U]) || !tag_bleed_ok(x[TAG_V_V]) || (v != a && !tag_feq(z, x[TAG_V_Z])))
               ok = 0;
         }
         if (!ok)
            continue;
         tsp = table[hdr + TAG_H_STATE + TAG_S_TSP];
         w = 8u << ((tsp >> 3) & 7);
         h = 8u << (tsp & 7);
         for (v = a; v <= b; v++)
         {
            m_vtx[v * TAG_V_WORDS + TAG_V_U] = tag_bleed_fix(m_vtx[v * TAG_V_WORDS + TAG_V_U], w);
            m_vtx[v * TAG_V_WORDS + TAG_V_V] = tag_bleed_fix(m_vtx[v * TAG_V_WORDS + TAG_V_V], h);
         }
      }

   /* the translucent draw records: each translucent strip's record in its
    * group's place, the group's count by its last strip */
   memset(m_trd, 0, (size_t)trd * TAG_C_WORDS * 4);
   for (k = 0; trd != 0 && k < ns; k++)
   {
      unsigned hdr = ssum[k * TAG_SS_WORDS + 4], r, li, b, s0, lo, hi, j, a, end;
      if (TAG_INFO_LIST(table[hdr + TAG_H_INFO]) != 2)
         continue;
      r = m_record(k, hdr);
      if (r == TAG_NONE)
         continue;
      li = (r - table[TAG_T_REC_TR]) / TAG_C_WORDS;
      m_trp[li * TAG_P_WORDS] = (table[hdr + TAG_H_STATE + TAG_S_TSP] & 0xFFFF00C0u)
            | ((table[hdr + TAG_H_STATE + TAG_S_ISP] >> 16) & 0xE400u) | ((table[hdr + TAG_H_STATE + TAG_S_PCW] >> 7) & 1u)
            | (table[hdr + TAG_H_STATE + TAG_S_TSP1] != 0xFFFFFFFFu || table[hdr + TAG_H_STATE + TAG_S_TCW1] != 0xFFFFFFFFu ? TAG_P_TWOVOL : 0u);
      m_trp[li * TAG_P_WORDS + 1] = table[hdr + TAG_H_STATE + TAG_S_TSP1];
      if (m_frm[TAG_O_OVERRUN] != 0)
         continue;
      b = table[hdr + TAG_H_TRB];
      lo = 0;
      hi = ns;
      while (lo < hi)
      {
         unsigned mid = (lo + hi) / 2;
         if (send[mid] < table[hdr + TAG_H_TRV])
            lo = mid + 1;
         else
            hi = mid;
      }
      s0 = lo;
      j = k - s0;
      a = (b + 1 + j) * TAG_C_WORDS;
      m_trd[a + TAG_C_COUNT] = m_rec[r + TAG_C_COUNT];
      m_trd[a + TAG_C_INST] = 1;
      m_trd[a + TAG_C_FIRST] = m_rec[r + TAG_C_FIRST];
      m_trd[a + 3] = 0;
      m_trd[a + TAG_C_INSTANCE] = (table[TAG_T_FLAGS] & TAG_FLAG_INSTANCE) ? li : 0;
      end = k + 1 == ns;
      if (!end)
      {
         unsigned next = ssum[(k + 1) * TAG_SS_WORDS + 4];
         end = TAG_INFO_LIST(table[next + TAG_H_INFO]) != 2 || table[next + TAG_H_TRB] != b;
      }
      if (end)
         m_trd[b * TAG_C_WORDS] = j + 1;
   }

   /* the provoking vertex, one index after the other, as the renderer does it */
   if ((table[TAG_T_FLAGS] & TAG_FLAG_PROVOKE) && m_frm[TAG_O_OVERRUN] == 0)
      for (k = 0; k < ns; k++)
      {
         unsigned hdr = ssum[k * TAG_SS_WORDS + 4], r, f, c, n;
         if (table[hdr + TAG_H_STATE + TAG_S_PCW] & 2u)
            continue;
         r = m_record(k, hdr);
         if (r == TAG_NONE || m_rec[r + TAG_C_COUNT] <= 2)
            continue;
         f = m_rec[r + TAG_C_FIRST];
         c = m_rec[r + TAG_C_COUNT];
         for (n = 0; n < c - 2; n++)
         {
            unsigned *d = &m_vtx[m_idx[f + n] * TAG_V_WORDS];
            const unsigned *s = &m_vtx[m_idx[f + n + 2] * TAG_V_WORDS];
            d[TAG_V_COL] = s[TAG_V_COL];
            d[TAG_V_SPC] = s[TAG_V_SPC];
            d[TAG_V_COL1] = s[TAG_V_COL1];
            d[TAG_V_SPC1] = s[TAG_V_SPC1];
         }
      }

   o->vtx = m_vtx;
   o->mod = m_mod;
   o->idx = m_idx;
   o->rec = m_rec;
   o->cmd = m_cmd;
   o->frm = m_frm;
   o->work = m_work;
   o->trd = m_trd;
   o->trp = m_trp;
   return scan_ok;
}
