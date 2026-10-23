/* How many frames does the core add between a button and the picture? A
 * libretro frontend with no screen that runs a game from a state twice -
 * once with a button pressed from frame N on, once without - and writes
 * down, after every retro_run, what the frontend would show (the picture
 * handed to video_cb, read back from OpenGL or Vulkan and hashed; a frame
 * the core calls a duplicate shows the one before), a hash of main memory,
 * and what the video chip's registers say at the frame's end: where it
 * shows the picture from (FB_R_SOF1), where it last drew one (FB_W_SOF1),
 * and how many renders the core has started. Two such traces are then held
 * against each other (lagmeter --compare), which says:
 *
 *   read    the first frame whose memory differs: when the game took the
 *           button in. This is the game, not the core.
 *   drawn   the first frame in which the game had a picture drawn that
 *           differs - known from the frames' pictures and the renders
 *           started, below.
 *   shown   the frame at whose end the console would show it: the first
 *           vblank at or after "drawn" at which the chip shows the
 *           framebuffer that picture was drawn to. On the console it is
 *           then on the screen through the frame after; a frontend shows
 *           the picture of retro_run k through the frame after k as well,
 *           so the two are counted the same way.
 *   present the first frame the frontend is handed a picture that differs.
 *
 * present - N is the lag a player sees in frames; shown - N is what the
 * game takes on the console; present - shown is what the core adds (0 is
 * the console's own timing, less than 0 a picture shown before the game
 * has asked for it).
 *
 *   lagmeter CORE CONTENT FRAMES [key=value ...]   a run; key=value are core options
 *   lagmeter --compare A.trace B.trace N           A without the press, B with it
 *   lagmeter --align A_ B_ FIRST LAST              two renderers' pictures (LAG_SHOT's
 *                                                  A_<F>.ppm, B_<F>.ppm) of frames FIRST
 *                                                  to LAST: each B picture has to be
 *                                                  nearest the A picture of its frame
 *
 * The environment, for a run:
 *   LAG_TRACE   the trace file to write
 *   LAG_STATE   a state to load after LAG_WARM frames (default 1)
 *   LAG_PRESS   "N:ID[:FRAMES]": the first pad's button ID (a
 *               RETRO_DEVICE_ID_JOYPAD_) held from frame N, for FRAMES
 *               frames or to the end
 *   LAG_PORT    the port the press goes to (default 0), and LAG_SCRIPT's
 *   LAG_SCRIPT  "F:ID[:FRAMES],...": more presses, in both runs (default 10
 *               frames each), to bring a game to where the press is to be
 *               measured from a state, or from the start
 *   LAG_SAVE    "F:FILE": the state after frame F written to FILE
 *   LAG_POLL    early, normal or late (default): when the frontend reads
 *               the pad, as RetroArch's "Polling Behavior" (below)
 *   LAG_VIDEO   "gl": OpenGL through EGL with no display; "vulkan": a
 *               Vulkan device of its own (the first the loader has - Mesa's
 *               lavapipe, here). The core option for the renderer has to
 *               ask for the same.
 *   LAG_DIR     the system and save directory (default /tmp)
 *   LAG_SHOT    "F[,F...]:PREFIX": the picture shown after frame F written
 *               to PREFIX<F>.ppm
 *   LAG_SYMS    "base=ADDR regs=ADDR count=ADDR" from nm: retro_init's
 *               address, pvr_regs' and FrameCount's (lagmeter.sh does it).
 *               Without it the registers are not traced, and "shown" is
 *               not worked out.
 *
 * Each line of a trace: frame, V (a new picture) or D (a duplicate), the
 * hash of the picture shown, of main memory, FB_R_SOF1, FB_W_SOF1, the
 * render count, how many times the core called input_poll and input_state
 * in the frame, and the sound frames it made (how long it ran). The first
 * line has the sound's rate and the frame rate.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../core/libretro-common/include/libretro.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "../../core/libretro-common/include/libretro_vulkan.h"

static uint64_t hash_bytes(const void *data, size_t size, uint64_t h)
{
   const unsigned char *p = (const unsigned char *)data;
   size_t i;
   for (i = 0; i < size; i++)
      h = (h ^ p[i]) * 0x100000001b3ull;
   return h;
}

/* ---- a trace, and two held against each other ---- */

struct line
{
   int frame;
   char kind;
   uint64_t picture, ram;
   unsigned fb_r, fb_w, renders, polls, reads, samples;
};

static double trace_rate, trace_fps;	// the sound's sample rate, the frame rate the core says

static int trace_read(const char *path, struct line **out)
{
   FILE *f = fopen(path, "r");
   struct line *l = NULL;
   int n = 0, room = 0;
   char buf[256];
   if (!f)
   {
      fprintf(stderr, "lagmeter: no %s\n", path);
      return -1;
   }
   while (fgets(buf, sizeof(buf), f))
   {
      struct line x;
      unsigned long long p, r;
      if (sscanf(buf, "# rate %lf fps %lf", &trace_rate, &trace_fps) == 2)
         continue;
      x.polls = x.reads = x.samples = 0;
      if (sscanf(buf, "%d %c %llx %llx %x %x %u %u %u %u", &x.frame, &x.kind, &p, &r, &x.fb_r, &x.fb_w, &x.renders,
            &x.polls, &x.reads, &x.samples) < 7)
         continue;
      x.picture = p;
      x.ram = r;
      if (n == room)
      {
         room = room ? room * 2 : 1024;
         l = (struct line *)realloc(l, sizeof(*l) * room);
      }
      l[n++] = x;
   }
   fclose(f);
   *out = l;
   return n;
}

static int compare_main(int argc, char **argv)
{
   struct line *a, *b;
   int na, nb, n, i, press;
   int read = -1, drawn = -1, shown = -1, present = -1, diverge_before = -1;
   unsigned target = 0;

   if (argc < 5)
      return 2;
   na = trace_read(argv[2], &a);
   nb = trace_read(argv[3], &b);
   press = atoi(argv[4]);
   if (na <= 0 || nb <= 0)
      return 1;
   n = na < nb ? na : nb;
   for (i = 0; i < n; i++)
   {
      if (a[i].frame < press)
      {
         if (a[i].ram != b[i].ram || a[i].picture != b[i].picture)
            diverge_before = a[i].frame;
         continue;
      }
      if (read < 0 && a[i].ram != b[i].ram)
         read = a[i].frame;
      if (present < 0 && a[i].picture != b[i].picture)
         present = a[i].frame;
   }
   if (diverge_before >= 0)
   {
      printf("the runs differ at frame %d, before the press: not a measurement\n", diverge_before);
      return 3;
   }
   /* drawn: the frame of the last render started at or before present
    * (the render count goes up in the frame a render is started in) - the
    * render whose picture present shows, for a core that shows a render
    * no sooner than the frame that started it and no later than the next
    * render. */
   if (present >= 0)
   {
      int pi = -1, k;
      for (i = 0; i < n; i++)
         if (b[i].frame == present)
            pi = i;
      k = pi;
      while (k > 0 && b[k].renders == b[k - 1].renders && b[k - 1].frame >= press)
         k--;
      drawn = b[k].frame;
      target = b[k].fb_w & 0xffffff;
      for (i = k; i < n; i++)
         if ((b[i].fb_r & 0xffffff) == target)
         {
            shown = b[i].frame;
            break;
         }
   }
   printf("press %d  read %d  drawn %d  shown %d  present %d", press, read, drawn, shown, present);
   if (present >= 0)
   {
      printf("  |  lag %d", present - press);
      if (shown >= 0)
         printf("  game %d  core %+d", shown - press, present - shown);
      /* The lag in the machine's own frames: the time the retro_runs from
       * the press to the picture ran, from the sound they made. (A core
       * whose retro_run is not one frame of the machine's - one that
       * returns when a picture is made, every other vblank for a game at
       * 30 frames a second - has its lag in retro_runs mean another
       * thing.) */
      if (trace_rate > 0 && trace_fps > 0)
      {
         double sum = 0;
         for (i = 0; i < n; i++)
            if (b[i].frame >= press && b[i].frame <= present)
               sum += b[i].samples;
         if (sum > 0)
            printf("  |  %.2f frames of the machine's", sum * trace_fps / trace_rate - 1.0);
      }
   }
   printf("\n");
   return 0;
}

/* Two renderers' pictures of the same frames, as LAG_SHOT writes them:
 * is each picture of the one run nearest the other's picture of the same
 * frame, rather than one of the frames before or after? A renderer that
 * shows a frame early or late has its pictures nearest the other's of
 * another frame (the two do not draw alike to the pixel, so equal hashes
 * are not asked for). Frames whose pictures are all alike (a still
 * screen) say nothing and are passed over. */
static unsigned char *ppm_read(const char *path, unsigned *w, unsigned *h)
{
   FILE *f = fopen(path, "rb");
   unsigned char *p;
   if (!f)
      return NULL;
   if (fscanf(f, "P6 %u %u 255", w, h) != 2)
   {
      fclose(f);
      return NULL;
   }
   fgetc(f);
   p = (unsigned char *)malloc((size_t)*w * *h * 3);
   if (fread(p, 3, (size_t)*w * *h, f) != (size_t)*w * *h)
   {
      free(p);
      p = NULL;
   }
   fclose(f);
   return p;
}

static double ppm_diff(const char *a, const char *b)
{
   unsigned wa, ha, wb, hb;
   unsigned char *pa = ppm_read(a, &wa, &ha), *pb = ppm_read(b, &wb, &hb);
   double d = -1;
   if (pa && pb && wa == wb && ha == hb)
   {
      size_t i, n = (size_t)wa * ha * 3;
      unsigned long long sum = 0;
      for (i = 0; i < n; i++)
         sum += pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
      d = (double)sum / n;
   }
   free(pa);
   free(pb);
   return d;
}

static int align_main(int argc, char **argv)
{
   int first, last, f, bad = 0;
   char a[1024], b[1024];
   if (argc < 6)
      return 2;
   first = atoi(argv[4]);
   last = atoi(argv[5]);
   for (f = first + 1; f < last; f++)
   {
      double d[3];
      int k;
      for (k = 0; k < 3; k++)
      {
         snprintf(a, sizeof(a), "%s%d.ppm", argv[2], f);
         snprintf(b, sizeof(b), "%s%d.ppm", argv[3], f - 1 + k);
         d[k] = ppm_diff(a, b);
         if (d[k] < 0)
         {
            printf("frame %d: no picture to compare (%s, %s)\n", f, a, b);
            return 1;
         }
      }
      if (d[1] <= d[0] + 0.05 && d[1] <= d[2] + 0.05)
         continue;
      /* a still picture, or one that hardly moves: nothing to tell */
      if ((d[0] > d[2] ? d[0] : d[2]) - d[1] < 0.5 && d[1] - (d[0] < d[2] ? d[0] : d[2]) < 0.5)
         continue;
      printf("frame %d: nearest the other's frame %d (%.2f %.2f %.2f)\n", f, d[0] < d[2] ? f - 1 : f + 1, d[0], d[1], d[2]);
      bad++;
   }
   printf("%s: %d frame(s) out of step\n", bad ? "FAIL" : "ok", bad);
   return bad ? 3 : 0;
}

/* ---- the core's registers, found through nm ---- */

static uintptr_t sym_base, sym_regs, sym_count;
static unsigned char *core_regs;
static uint32_t *core_count;

static void syms_read(void *core)
{
   const char *s = getenv("LAG_SYMS");
   uintptr_t init;
   if (!s || sscanf(s, "base=%lx regs=%lx count=%lx", &sym_base, &sym_regs, &sym_count) != 3)
      return;
   init = (uintptr_t)dlsym(core, "retro_init");
   core_regs = (unsigned char *)(init - sym_base + sym_regs);
   core_count = (uint32_t *)(init - sym_base + sym_count);
}

static unsigned reg(unsigned offset)
{
   uint32_t v;
   if (!core_regs)
      return 0;
   memcpy(&v, core_regs + offset, 4);
   return v;
}

/* ---- OpenGL ---- */

static int video_gl, video_vk;
static EGLDisplay egl_display;
static EGLContext egl_context;
static GLuint fbo, fbo_color, fbo_depth;
#define FBO_W 2560
#define FBO_H 1920
static unsigned char *pixels;
static unsigned pixels_w, pixels_h;

static int egl_start(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   static const EGLint config_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
   static const EGLint context_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
   EGLConfig config = NULL;
   EGLint major, minor, n = 0;
   PFNGLGENFRAMEBUFFERSPROC gen_fb;
   PFNGLBINDFRAMEBUFFERPROC bind_fb;
   PFNGLGENRENDERBUFFERSPROC gen_rb;
   PFNGLBINDRENDERBUFFERPROC bind_rb;
   PFNGLRENDERBUFFERSTORAGEPROC storage;
   PFNGLFRAMEBUFFERRENDERBUFFERPROC attach;

   if (!get_display)
      return 0;
   egl_display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   if (!egl_display || !eglInitialize(egl_display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
      return 0;
   eglChooseConfig(egl_display, config_attribs, &config, 1, &n);
   egl_context = eglCreateContext(egl_display, n ? config : NULL, EGL_NO_CONTEXT, context_attribs);
   if (!egl_context || !eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context))
      return 0;
   gen_fb = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
   bind_fb = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
   gen_rb = (PFNGLGENRENDERBUFFERSPROC)eglGetProcAddress("glGenRenderbuffers");
   bind_rb = (PFNGLBINDRENDERBUFFERPROC)eglGetProcAddress("glBindRenderbuffer");
   storage = (PFNGLRENDERBUFFERSTORAGEPROC)eglGetProcAddress("glRenderbufferStorage");
   attach = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)eglGetProcAddress("glFramebufferRenderbuffer");
   gen_fb(1, &fbo);
   bind_fb(GL_FRAMEBUFFER, fbo);
   gen_rb(1, &fbo_color);
   bind_rb(GL_RENDERBUFFER, fbo_color);
   storage(GL_RENDERBUFFER, GL_RGBA8, FBO_W, FBO_H);
   attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fbo_color);
   gen_rb(1, &fbo_depth);
   bind_rb(GL_RENDERBUFFER, fbo_depth);
   storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, FBO_W, FBO_H);
   attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo_depth);
   pixels = (unsigned char *)malloc((size_t)FBO_W * FBO_H * 4);
   fprintf(stderr, "lagmeter: OpenGL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
   return 1;
}

static void gl_picture(unsigned w, unsigned h)
{
   PFNGLBINDFRAMEBUFFERPROC bind_fb = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
   if (w > FBO_W || h > FBO_H)
      w = h = 0;
   bind_fb(GL_READ_FRAMEBUFFER, fbo);
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   pixels_w = w;
   pixels_h = h;
}

static retro_proc_address_t gl_proc(const char *name)
{
   return (retro_proc_address_t)eglGetProcAddress(name);
}

static uintptr_t gl_framebuffer(void)
{
   return fbo;
}

/* ---- Vulkan: a device of the core's making, on the first GPU ---- */

static PFN_vkGetInstanceProcAddr vk_gipa;
static VkInstance vk_instance;
static struct retro_vulkan_context vk_ctx;
static struct retro_hw_render_interface_vulkan vk_if;
static const struct retro_hw_render_context_negotiation_interface_vulkan *vk_neg;
static struct retro_vulkan_image vk_image;
static int vk_have_image;
static unsigned vk_sync;
static VkCommandBuffer vk_pending[16];
static unsigned vk_pending_count;
static VkCommandPool vk_pool;
static VkCommandBuffer vk_copy_cmd;
static VkBuffer vk_buf;
static VkDeviceMemory vk_mem;
static VkFence vk_fence;
static void *vk_map;
#define VK_BUF_SIZE ((VkDeviceSize)FBO_W * FBO_H * 4)

#define VKF(name) static PFN_##name p_##name
VKF(vkCreateInstance); VKF(vkGetDeviceProcAddr); VKF(vkQueueSubmit); VKF(vkQueueWaitIdle);
VKF(vkCreateCommandPool); VKF(vkAllocateCommandBuffers); VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer);
VKF(vkCmdPipelineBarrier); VKF(vkCmdCopyImageToBuffer); VKF(vkCreateBuffer); VKF(vkGetBufferMemoryRequirements);
VKF(vkAllocateMemory); VKF(vkBindBufferMemory); VKF(vkMapMemory); VKF(vkCreateFence); VKF(vkWaitForFences);
VKF(vkResetFences); VKF(vkResetCommandBuffer); VKF(vkGetPhysicalDeviceMemoryProperties); VKF(vkDeviceWaitIdle);

static void vk_set_image(void *handle, const struct retro_vulkan_image *image, uint32_t num_semaphores,
      const VkSemaphore *semaphores, uint32_t src_queue_family)
{
   (void)handle; (void)num_semaphores; (void)semaphores; (void)src_queue_family;
   if (image)
   {
      vk_image = *image;
      vk_have_image = 1;
   }
   else
      vk_have_image = 0;
}
static uint32_t vk_get_sync_index(void *handle) { (void)handle; return vk_sync; }
static uint32_t vk_get_sync_index_mask(void *handle) { (void)handle; return 3; }
static void vk_set_command_buffers(void *handle, uint32_t num, const VkCommandBuffer *cmd)
{
   unsigned i;
   (void)handle;
   for (i = 0; i < num && vk_pending_count < 16; i++)
      vk_pending[vk_pending_count++] = cmd[i];
}
static void vk_wait_sync_index(void *handle) { (void)handle; p_vkQueueWaitIdle(vk_ctx.queue); }
static void vk_lock_queue(void *handle) { (void)handle; }
static void vk_unlock_queue(void *handle) { (void)handle; }
static void vk_set_signal_semaphore(void *handle, VkSemaphore semaphore) { (void)handle; (void)semaphore; }

static int vk_start(void)
{
   static void *loader;
   VkApplicationInfo app;
   const VkApplicationInfo *core_app = NULL;
   VkInstanceCreateInfo ci;
   PFN_vkGetDeviceProcAddr gdpa;

   loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!loader)
      return 0;
   vk_gipa = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");
   p_vkCreateInstance = (PFN_vkCreateInstance)vk_gipa(NULL, "vkCreateInstance");
   if (vk_neg && vk_neg->get_application_info)
      core_app = vk_neg->get_application_info();
   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "lagmeter";
   app.apiVersion = VK_API_VERSION_1_1;
   memset(&ci, 0, sizeof(ci));
   ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ci.pApplicationInfo = core_app ? core_app : &app;
   if (p_vkCreateInstance(&ci, NULL, &vk_instance) != VK_SUCCESS)
      return 0;
#define VKI(name) p_##name = (PFN_##name)vk_gipa(vk_instance, #name)
   VKI(vkGetDeviceProcAddr); VKI(vkGetPhysicalDeviceMemoryProperties);
   if (!vk_neg || !vk_neg->create_device
         || !vk_neg->create_device(&vk_ctx, vk_instance, VK_NULL_HANDLE, VK_NULL_HANDLE, vk_gipa, NULL, 0, NULL, 0, NULL))
   {
      fprintf(stderr, "lagmeter: the core made no Vulkan device\n");
      return 0;
   }
   gdpa = p_vkGetDeviceProcAddr;
#define VKD(name) p_##name = (PFN_##name)gdpa(vk_ctx.device, #name)
   VKD(vkQueueSubmit); VKD(vkQueueWaitIdle); VKD(vkCreateCommandPool); VKD(vkAllocateCommandBuffers);
   VKD(vkBeginCommandBuffer); VKD(vkEndCommandBuffer); VKD(vkCmdPipelineBarrier); VKD(vkCmdCopyImageToBuffer);
   VKD(vkCreateBuffer); VKD(vkGetBufferMemoryRequirements); VKD(vkAllocateMemory); VKD(vkBindBufferMemory);
   VKD(vkMapMemory); VKD(vkCreateFence); VKD(vkWaitForFences); VKD(vkResetFences); VKD(vkResetCommandBuffer);
   VKD(vkDeviceWaitIdle);
   {
      VkCommandPoolCreateInfo pci;
      VkCommandBufferAllocateInfo ai;
      VkBufferCreateInfo bi;
      VkMemoryRequirements req;
      VkPhysicalDeviceMemoryProperties props;
      VkMemoryAllocateInfo mi;
      VkFenceCreateInfo fi;
      unsigned t;

      memset(&pci, 0, sizeof(pci));
      pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pci.queueFamilyIndex = vk_ctx.queue_family_index;
      p_vkCreateCommandPool(vk_ctx.device, &pci, NULL, &vk_pool);
      memset(&ai, 0, sizeof(ai));
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = vk_pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      p_vkAllocateCommandBuffers(vk_ctx.device, &ai, &vk_copy_cmd);
      memset(&bi, 0, sizeof(bi));
      bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bi.size = VK_BUF_SIZE;
      bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      p_vkCreateBuffer(vk_ctx.device, &bi, NULL, &vk_buf);
      p_vkGetBufferMemoryRequirements(vk_ctx.device, vk_buf, &req);
      p_vkGetPhysicalDeviceMemoryProperties(vk_ctx.gpu, &props);
      memset(&mi, 0, sizeof(mi));
      mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mi.allocationSize = req.size;
      for (t = 0; t < props.memoryTypeCount; t++)
         if ((req.memoryTypeBits >> t & 1) && (props.memoryTypes[t].propertyFlags
               & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
               == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
      mi.memoryTypeIndex = t;
      p_vkAllocateMemory(vk_ctx.device, &mi, NULL, &vk_mem);
      p_vkBindBufferMemory(vk_ctx.device, vk_buf, vk_mem, 0);
      p_vkMapMemory(vk_ctx.device, vk_mem, 0, VK_BUF_SIZE, 0, &vk_map);
      memset(&fi, 0, sizeof(fi));
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      p_vkCreateFence(vk_ctx.device, &fi, NULL, &vk_fence);
   }
   memset(&vk_if, 0, sizeof(vk_if));
   vk_if.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
   vk_if.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
   vk_if.handle = &vk_if;
   vk_if.instance = vk_instance;
   vk_if.gpu = vk_ctx.gpu;
   vk_if.device = vk_ctx.device;
   vk_if.get_device_proc_addr = gdpa;
   vk_if.get_instance_proc_addr = vk_gipa;
   vk_if.queue = vk_ctx.queue;
   vk_if.queue_index = vk_ctx.queue_family_index;
   vk_if.set_image = vk_set_image;
   vk_if.get_sync_index = vk_get_sync_index;
   vk_if.get_sync_index_mask = vk_get_sync_index_mask;
   vk_if.set_command_buffers = vk_set_command_buffers;
   vk_if.wait_sync_index = vk_wait_sync_index;
   vk_if.lock_queue = vk_lock_queue;
   vk_if.unlock_queue = vk_unlock_queue;
   vk_if.set_signal_semaphore = vk_set_signal_semaphore;
   return 1;
}

/* The picture the core set, read back: what the frontend would draw on its screen. */
static void vk_picture(unsigned w, unsigned h)
{
   VkCommandBufferBeginInfo bi;
   VkImageMemoryBarrier b;
   VkBufferImageCopy region;
   VkSubmitInfo si;
   unsigned x, y;
   int bgr;

   if (vk_pending_count)
   {
      memset(&si, 0, sizeof(si));
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = vk_pending_count;
      si.pCommandBuffers = vk_pending;
      p_vkQueueSubmit(vk_ctx.queue, 1, &si, VK_NULL_HANDLE);
      vk_pending_count = 0;
   }
   pixels_w = pixels_h = 0;
   if (!vk_have_image || w > FBO_W || h > FBO_H)
      return;
   p_vkResetCommandBuffer(vk_copy_cmd, 0);
   memset(&bi, 0, sizeof(bi));
   bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   p_vkBeginCommandBuffer(vk_copy_cmd, &bi);
   memset(&b, 0, sizeof(b));
   b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = vk_image.image_layout;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = vk_image.create_info.image;
   b.subresourceRange = vk_image.create_info.subresourceRange;
   p_vkCmdPipelineBarrier(vk_copy_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
         0, NULL, 0, NULL, 1, &b);
   memset(&region, 0, sizeof(region));
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = w;
   region.imageExtent.height = h;
   region.imageExtent.depth = 1;
   p_vkCmdCopyImageToBuffer(vk_copy_cmd, vk_image.create_info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf, 1, &region);
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout = vk_image.image_layout;
   p_vkCmdPipelineBarrier(vk_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
         0, NULL, 0, NULL, 1, &b);
   p_vkEndCommandBuffer(vk_copy_cmd);
   memset(&si, 0, sizeof(si));
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &vk_copy_cmd;
   p_vkResetFences(vk_ctx.device, 1, &vk_fence);
   p_vkQueueSubmit(vk_ctx.queue, 1, &si, vk_fence);
   p_vkWaitForFences(vk_ctx.device, 1, &vk_fence, VK_TRUE, ~0ull);
   /* to RGBA, bottom row first, as OpenGL reads it */
   bgr = vk_image.create_info.format == VK_FORMAT_B8G8R8A8_UNORM || vk_image.create_info.format == VK_FORMAT_B8G8R8A8_SRGB;
   for (y = 0; y < h; y++)
   {
      const unsigned char *s = (const unsigned char *)vk_map + (size_t)y * w * 4;
      unsigned char *d = pixels + (size_t)(h - 1 - y) * w * 4;
      for (x = 0; x < w; x++, s += 4, d += 4)
      {
         d[0] = bgr ? s[2] : s[0];
         d[1] = s[1];
         d[2] = bgr ? s[0] : s[2];
         d[3] = s[3];
      }
   }
   pixels_w = w;
   pixels_h = h;
   vk_sync = (vk_sync + 1) & 1;
}

/* ---- the frontend ---- */

static char **options;
static int option_count;
static struct retro_hw_render_callback hw;
static int have_hw;
static uint64_t shown_hash;
static int frame_now, press_frame = -1, press_id, press_frames = 1 << 30, press_port;
static int video_valid;
static unsigned polls, reads;

static void log_line(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("LAG_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static bool environment(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_line;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = getenv("LAG_DIR") ? getenv("LAG_DIR") : "/tmp";
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned *)data = video_vk ? RETRO_HW_CONTEXT_VULKAN : RETRO_HW_CONTEXT_OPENGL_CORE;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         struct retro_hw_render_callback *cb = (struct retro_hw_render_callback *)data;
         if (video_vk ? cb->context_type != RETRO_HW_CONTEXT_VULKAN
               : (cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL))
            return false;
         if (!video_vk)
         {
            cb->get_current_framebuffer = gl_framebuffer;
            cb->get_proc_address = gl_proc;
         }
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      {
         struct retro_hw_render_context_negotiation_interface *n = (struct retro_hw_render_context_negotiation_interface *)data;
         if (n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
         n->interface_version = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
         if (((const struct retro_hw_render_context_negotiation_interface *)data)->interface_type
               != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
         vk_neg = (const struct retro_hw_render_context_negotiation_interface_vulkan *)data;
         return true;
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         if (!video_vk)
            return false;
         *(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)&vk_if;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         size_t len = strlen(var->key);
         int i;
         for (i = 0; i < option_count; i++)
            if (!strncmp(options[i], var->key, len) && options[i][len] == '=')
            {
               var->value = options[i] + len + 1;
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
         *(int *)data = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
         return true;
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
         return true;
      default:
         return false;
   }
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
   unsigned y;
   if (!data)
      return;   /* the picture before it again */
   video_valid = 1;
   if (data == RETRO_HW_FRAME_BUFFER_VALID)
   {
      if (video_vk)
         vk_picture(width, height);
      else if (video_gl)
         gl_picture(width, height);
      shown_hash = hash_bytes(pixels, (size_t)pixels_w * pixels_h * 4, 0x5bd1e995);
      return;
   }
   shown_hash = 0x2545f4914f6cdd1dull;
   for (y = 0; y < height; y++)
      shown_hash = hash_bytes((const unsigned char *)data + y * pitch, width * 2, shown_hash);
}

static unsigned samples;	// sound frames this retro_run made: how much time it ran
static void audio_sample(int16_t left, int16_t right) { (void)left; (void)right; samples++; }
static size_t audio_sample_batch(const int16_t *data, size_t frames) { (void)data; samples += (unsigned)frames; return frames; }
/* When the frontend reads its pad, by LAG_POLL (RetroArch's "Polling
 * Behavior"): the press happens between the frames before N and N.
 *   early   before each retro_run (what the core asks then is the pad as
 *           it was then)
 *   normal  when the core calls input_poll: a poll at the end of frame
 *           N - 1 is before the press, and the frame after has the pad as
 *           it was then
 *   late    at the first input_state after the core called input_poll
 *           (RetroArch's default) */
static int poll_mode = 2;   /* 0 early, 1 normal, 2 late */
static int pad_latched, poll_owed;

/* LAG_SCRIPT's presses, the same in both runs: to bring the game to where
 * the press is to be measured */
#define SCRIPT_MAX 256
static int script_frame[SCRIPT_MAX], script_id[SCRIPT_MAX], script_len[SCRIPT_MAX], script_count;

/* the buttons held now, a bit each */
static int pad_real(void)
{
   int held = 0, i;
   if (press_frame >= 0 && frame_now >= press_frame && frame_now < press_frame + press_frames)
      held |= 1 << press_id;
   for (i = 0; i < script_count; i++)
      if (frame_now >= script_frame[i] && frame_now < script_frame[i] + script_len[i])
         held |= 1 << script_id[i];
   return held;
}

static void input_poll(void)
{
   polls++;
   if (poll_mode == 1)
      pad_latched = pad_real();
   else if (poll_mode == 2)
      poll_owed = 1;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   int held;
   reads++;
   if (poll_mode == 2 && poll_owed)
   {
      pad_latched = pad_real();
      poll_owed = 0;
   }
   held = port == (unsigned)press_port ? pad_latched : 0;
   (void)index;
   if ((device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD || !held)
      return 0;
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
      return (int16_t)held;
   return id < 16 && (held >> id & 1);
}

static long file_read(const char *path, unsigned char **out)
{
   FILE *f = fopen(path, "rb");
   long size;
   if (!f)
      return -1;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   *out = (unsigned char *)malloc(size > 0 ? size : 1);
   if (fread(*out, 1, size, f) != (size_t)size)
      size = -1;
   fclose(f);
   return size;
}

/* A state as RetroArch writes it: compressed (RZIP: zstd chunks; libzstd
 * is loaded for it), and/or with RetroArch's own header (RASTATE, the
 * core's part in a MEM block). What comes back is the core's part. */
static long state_read(const char *path, unsigned char **out)
{
   unsigned char *d;
   long size = file_read(path, &d);
   if (size >= 20 && !memcmp(d, "#RZIPv", 6))
   {
      size_t (*decompress)(void *, size_t, const void *, size_t);
      void *zstd = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
      uint32_t chunk;
      uint64_t total;
      long p = 20, got = 0;
      unsigned char *o;
      if (!zstd || !(*(void **)&decompress = dlsym(zstd, "ZSTD_decompress")))
      {
         fprintf(stderr, "lagmeter: no libzstd for %s\n", path);
         return -1;
      }
      memcpy(&chunk, d + 8, 4);
      memcpy(&total, d + 12, 8);
      o = (unsigned char *)malloc(total + chunk);
      while (p + 4 <= size && (uint64_t)got < total)
      {
         uint32_t n;
         size_t r;
         memcpy(&n, d + p, 4);
         p += 4;
         r = decompress(o + got, chunk, d + p, n);
         if (r > chunk)
            return -1;
         got += (long)r;
         p += n;
      }
      free(d);
      d = o;
      size = got;
   }
   if (size >= 16 && !memcmp(d, "RASTATE", 7))
   {
      long p = 8;
      while (p + 8 <= size)
      {
         uint32_t n;
         memcpy(&n, d + p + 4, 4);
         if (!memcmp(d + p, "MEM ", 4) && p + 8 + (long)n <= size)
         {
            memmove(d, d + p + 8, n);
            size = n;
            break;
         }
         p += 8 + ((n + 7) & ~7u);
      }
   }
   *out = d;
   return size;
}

static void shot(const char *prefix, int frame)
{
   char path[1024];
   FILE *f;
   unsigned y;
   snprintf(path, sizeof(path), "%s%d.ppm", prefix, frame);
   f = fopen(path, "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%u %u\n255\n", pixels_w, pixels_h);
   for (y = 0; y < pixels_h; y++)
   {
      const unsigned char *row = pixels + (size_t)(pixels_h - 1 - y) * pixels_w * 4;
      unsigned x;
      for (x = 0; x < pixels_w; x++)
         fwrite(row + x * 4, 1, 3, f);
   }
   fclose(f);
}

int main(int argc, char **argv)
{
   struct retro_game_info game;
   int frames, i, warm;
   void *core;
   FILE *trace = NULL;
   void (*core_run)(void);
   bool (*core_load)(const void *, size_t);
   void *(*mem_data)(unsigned);
   size_t (*mem_size)(unsigned);
   const char *shots = getenv("LAG_SHOT");
   const char *shot_prefix = shots ? strchr(shots, ':') : NULL;
   size_t (*core_size)(void);
   bool (*core_save)(void *, size_t);
   int save_frame = -1;
   double av_rate = 0, av_fps = 0;
   const char *save_path = NULL;

   if (argc >= 2 && !strcmp(argv[1], "--compare"))
      return compare_main(argc, argv);
   if (argc >= 2 && !strcmp(argv[1], "--align"))
      return align_main(argc, argv);
   if (argc < 4)
   {
      fprintf(stderr, "usage: %s CORE CONTENT FRAMES [key=value ...]\n"
            "       %s --compare A.trace B.trace N\n", argv[0], argv[0]);
      return 2;
   }
   options = argv + 4;
   option_count = argc - 4;
   frames = atoi(argv[3]);
   warm = getenv("LAG_WARM") ? atoi(getenv("LAG_WARM")) : 1;
   if (getenv("LAG_PRESS"))
   {
      const char *p = getenv("LAG_PRESS");
      if (sscanf(p, "%d:%d:%d", &press_frame, &press_id, &press_frames) < 2)
         return 2;
   }
   if (getenv("LAG_SAVE"))
   {
      save_frame = atoi(getenv("LAG_SAVE"));
      save_path = strchr(getenv("LAG_SAVE"), ':');
      if (!save_path)
         return 2;
      save_path++;
   }
   if (getenv("LAG_SCRIPT"))
   {
      const char *p = getenv("LAG_SCRIPT");
      while (*p && script_count < SCRIPT_MAX)
      {
         int f, id, len = 10, n = 0;
         if (sscanf(p, "%d:%d%n", &f, &id, &n) < 2)
            break;
         p += n;
         if (*p == ':')
         {
            p++;
            len = (int)strtol(p, (char **)&p, 10);
         }
         script_frame[script_count] = f;
         script_id[script_count] = id;
         script_len[script_count++] = len;
         if (*p == ',')
            p++;
      }
   }
   if (getenv("LAG_POLL"))
      poll_mode = !strcmp(getenv("LAG_POLL"), "early") ? 0 : !strcmp(getenv("LAG_POLL"), "normal") ? 1 : 2;
   if (getenv("LAG_PORT"))
      press_port = atoi(getenv("LAG_PORT"));
   if (getenv("LAG_VIDEO") && !strcmp(getenv("LAG_VIDEO"), "gl"))
   {
      if (!egl_start())
      {
         fprintf(stderr, "lagmeter: no OpenGL through EGL\n");
         return 1;
      }
      video_gl = 1;
   }
   else if (getenv("LAG_VIDEO") && !strcmp(getenv("LAG_VIDEO"), "vulkan"))
   {
      video_vk = 1;
      pixels = (unsigned char *)malloc((size_t)FBO_W * FBO_H * 4);
   }

   core = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
   if (!core)
   {
      fprintf(stderr, "lagmeter: %s\n", dlerror());
      return 1;
   }
#define SYM(var, name) *(void **)&var = dlsym(core, name); \
   if (!var) { fprintf(stderr, "lagmeter: no %s in the core\n", name); return 1; }
   {
      void (*set_environment)(retro_environment_t);
      void (*set_video)(retro_video_refresh_t);
      void (*set_audio)(retro_audio_sample_t);
      void (*set_audio_batch)(retro_audio_sample_batch_t);
      void (*set_poll)(retro_input_poll_t);
      void (*set_input)(retro_input_state_t);
      void (*init)(void);
      bool (*load_game)(const struct retro_game_info *);
      void (*get_av_info)(struct retro_system_av_info *);
      void (*set_device)(unsigned, unsigned);
      struct retro_system_av_info av;

      SYM(set_environment, "retro_set_environment")
      SYM(set_video, "retro_set_video_refresh")
      SYM(set_audio, "retro_set_audio_sample")
      SYM(set_audio_batch, "retro_set_audio_sample_batch")
      SYM(set_poll, "retro_set_input_poll")
      SYM(set_input, "retro_set_input_state")
      SYM(init, "retro_init")
      SYM(load_game, "retro_load_game")
      SYM(get_av_info, "retro_get_system_av_info")
      SYM(set_device, "retro_set_controller_port_device")
      SYM(core_run, "retro_run")
      SYM(core_load, "retro_unserialize")
      SYM(core_size, "retro_serialize_size")
      SYM(core_save, "retro_serialize")
      SYM(mem_data, "retro_get_memory_data")
      SYM(mem_size, "retro_get_memory_size")
      syms_read(core);

      set_environment(environment);
      set_video(video_refresh);
      set_audio(audio_sample);
      set_audio_batch(audio_sample_batch);
      set_poll(input_poll);
      set_input(input_state);
      init();
      memset(&game, 0, sizeof(game));
      game.path = argv[2];
      if (!load_game(&game))
      {
         fprintf(stderr, "lagmeter: the core would not load %s\n", argv[2]);
         return 1;
      }
      memset(&av, 0, sizeof(av));
      get_av_info(&av);
      av_rate = av.timing.sample_rate;
      av_fps = av.timing.fps;
      set_device(0, RETRO_DEVICE_JOYPAD);
      set_device(1, RETRO_DEVICE_JOYPAD);
      if (video_vk && !vk_start())
         return 1;
      if (have_hw && hw.context_reset)
         hw.context_reset();
   }

   for (i = 0; i < warm; i++)
   {
      frame_now = -warm + i;
      core_run();
   }
   if (getenv("LAG_STATE") && *getenv("LAG_STATE"))
   {
      unsigned char *in;
      long size = state_read(getenv("LAG_STATE"), &in);
      if (size <= 0 || !core_load(in, (size_t)size))
      {
         fprintf(stderr, "lagmeter: the state %s could not be loaded\n", getenv("LAG_STATE"));
         return 1;
      }
      free(in);
   }
   if (getenv("LAG_TRACE"))
      trace = fopen(getenv("LAG_TRACE"), "w");
   if (trace)
      fprintf(trace, "# rate %f fps %f\n", av_rate, av_fps);
   for (i = 0; i < frames; i++)
   {
      unsigned char *ram = (unsigned char *)mem_data(RETRO_MEMORY_SYSTEM_RAM);
      size_t ram_size = mem_size(RETRO_MEMORY_SYSTEM_RAM);
      uint64_t ram_hash;

      frame_now = i;
      samples = 0;
      if (poll_mode == 0)
         pad_latched = pad_real();
      video_valid = 0;
      core_run();
      ram_hash = ram ? hash_bytes(ram, ram_size, 0xcbf29ce484222325ull) : 0;
      if (trace)
         fprintf(trace, "%d %c %016llx %016llx %08x %08x %u %u %u %u\n", i, video_valid ? 'V' : 'D',
               (unsigned long long)shown_hash, (unsigned long long)ram_hash,
               reg(0x50), reg(0x60), core_count ? *core_count : 0, polls, reads, samples);
      polls = reads = samples = 0;
      if (save_frame == i)
      {
         size_t size = core_size();
         void *buf = malloc(size);
         FILE *f = fopen(save_path, "wb");
         if (buf && f && core_save(buf, size))
            fwrite(buf, 1, size, f);
         if (f)
            fclose(f);
         free(buf);
      }
      if (shot_prefix)
      {
         char want[16];
         const char *s = shots;
         snprintf(want, sizeof(want), "%d", i);
         while (s < shot_prefix)
         {
            size_t len = strcspn(s, ",:");
            if (len == strlen(want) && !strncmp(s, want, len))
               shot(shot_prefix + 1, i);
            s += len + 1;
         }
      }
   }
   if (trace)
      fclose(trace);
   if (video_vk)
      p_vkDeviceWaitIdle(vk_ctx.device);
   fflush(stdout);
   /* (no retro_deinit: a core's threads and the GPU are left to the exit) */
   _exit(0);
   return 0;
}
