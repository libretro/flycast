#include "Renderer_if.h"
#include "rend/last_picture.h"
#include "ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"
#include "hw/mem/_vmem.h"
#include "cheats.h"
#include "spg.h"

/*

	rendv3 ideas
	- multiple backends
	  - ESish
	    - OpenGL ES2.0
	    - OpenGL ES3.0
	    - OpenGL 3.1
	  - OpenGL 4.x
	  - Direct3D 10+ ?
	- correct memory ordering model
	- resource pools
	- threaded TA
	- threaded rendering
	- RTTs
	- framebuffers
	- overlays


	PHASES
	- TA submission (memops, dma)

	- TA parsing (defered, rend thread)

	- CORE render (in-order, defered, rend thread)


	submission is done in-order
	- Partial handling of TA values
	- Gotchas with TA contexts

	parsing is done on demand and out-of-order, and might be skipped
	- output is only consumed by renderer

	render is queued on RENDER_START, and won't stall the emulation or might be skipped
	- VRAM integrity is an issue with out-of-order or delayed rendering.
	- selective vram snapshots require TA parsing to complete in order with REND_START / REND_END


	Complications
	- For some apis (gles2, maybe gl31) texture allocation needs to happen on the gpu thread
	- multiple versions of different time snapshots of the same texture are required
	- TA parsing vs frameskip logic


	Texture versioning and staging
	 A memory copy of the texture can be used to temporary store the texture before upload to vram
	 This can be moved to another thread
	 If the api supports async resource creation, we don't need the extra copy
	 Texcache lookups need to be versioned


	rendv2x hacks
	- Only a single pending render. Any renders while still pending are dropped (before parsing)
	- wait and block for parse/texcache. Render is async
*/

extern int screen_width;
extern int screen_height;

u32 VertexCount=0;
u32 FrameCount=1;

Renderer* renderer;
static Renderer* fallback_renderer;
bool renderer_changed = false;	// Signals the renderer interface to switch renderer

#if !defined(TARGET_NO_THREADS)
#include "libretro/emu_baton.h"
extern EmuBaton emu_baton;
#endif
u32 fb_w_cur = 1;

int max_idx,max_mvo,max_op,max_pt,max_tr,max_vtx,max_modt, ovrn;
bool pend_rend = false;

static bool render_called = false;
static bool frame_produced = false;	/* new frame produced this vblank (for is_dupe) */
u32 fb_watch_addr_start;
u32 fb_watch_addr_end;
bool fb_dirty;

TA_context* _pvrrc;

void rend_create_renderer()
{
	switch (settings.pvr.rend)
	{
	default:
	case 0:
		NOTICE_LOG(PVR, "Creating Open GL per-triangle/strip renderer");
		renderer = rend_GLES2();
		break;
#if defined(HAVE_GL4)
	case 3:
		NOTICE_LOG(PVR, "Creating Open GL per-pixel renderer");
		renderer = rend_GL4();
		fallback_renderer = rend_GLES2();
		break;
#endif
#ifdef HAVE_VULKAN
	case 4:
		NOTICE_LOG(PVR, "Creating Vulkan per-triangle/strip renderer");
		renderer = rend_Vulkan();
		break;
	case 5:
		NOTICE_LOG(PVR, "Creating Vulkan per-pixel renderer");
		renderer = rend_OITVulkan();
		break;
#endif
	}
}

/* The last two framebuffers the game rendered to. A game flips between two
 * and draws over what it drew there two frames before. When it renders to
 * a third - after the BIOS has handed over, or on a change of video mode -
 * what the console has there is not what was on screen: the renderers draw
 * into one picture, though, and whatever the game does not draw over would
 * be left from the frame before. The picture is cleared then, to the
 * border colour. (Upstream: the BIOS's last frame showing above and below
 * the picture in San Francisco Rush 2049.) */
static u32 fb_addr_history[2] = { 1, 1 };

void rend_init_renderer()
{
	if (!renderer->Init())
    {
		delete renderer;
    	if (fallback_renderer == NULL || !fallback_renderer->Init())
    	{
         delete fallback_renderer;
    		die("Renderer initialization failed\n");
    	}
    	INFO_LOG(PVR, "Selected renderer initialization failed. Falling back to default renderer.");
    	renderer  = fallback_renderer;
    	fallback_renderer = NULL;	// avoid double-free
    }
}

/* The context is going (libretro thread, the machine standing still). */
void rend_keep_picture()
{
	if (renderer != NULL)
		renderer->KeepPicture();
	int w, h;
	if (last_picture(&w, &h) != NULL)
		NOTICE_LOG(RENDERER, "%d x %d picture kept for the next context", w, h);
}

void rend_resize(int width, int height)
{
	if (renderer != NULL)
		renderer->Resize(width, height);
}

void rend_term_renderer()
{
	/* A new renderer has no picture, and the first render to each
	 * framebuffer starts from an empty one - unless the last picture was
	 * kept for it: then the game goes on where it was. */
	int w, h;
	if (last_picture(&w, &h) == NULL)
		fb_addr_history[0] = fb_addr_history[1] = 1;
	if (renderer != NULL)
	{
		renderer->Term();
		delete renderer;
		renderer = NULL;
	}
	if (fallback_renderer != NULL)
	{
		delete fallback_renderer;
		fallback_renderer = NULL;
	}
	texcache_scratch_free();
}

bool rend_frame(TA_context* ctx, bool draw_osd)
{
   if (renderer_changed || renderer == NULL)
   {
	  renderer_changed = false;
	  if (renderer != NULL)
		 rend_term_renderer();
	  rend_create_renderer();
	  rend_init_renderer();
   }
#if !defined(TARGET_NO_THREADS)
   /* The renderer reads the registers as they were when the game started
    * this render. */
   if (settings.rend.ThreadedRendering)
      rend_pvr_regs = ctx->regs;
#endif
   /* The palette this frame is drawn with, converted here, on the thread
    * that draws, from the registers the frame is drawn with. */
   palette_update();

   bool proc = renderer->Process(ctx);
#if !defined(TARGET_NO_THREADS)
   /* Process() has read everything the frame takes from the game's memory:
    * the display lists, the textures, the framebuffer. The emulation thread
    * has been asleep since it handed the render over, so what was read is
    * what non-threaded rendering would have read; from here it can run
    * while the frame is drawn. A render to a texture ends up back in the
    * game's memory, so that one keeps the emulation asleep until it is
    * finished. */
   const bool hold = proc && ctx->rend.isRTT;
   if (settings.rend.ThreadedRendering && !hold)
      emu_baton.AckWork();
#endif

   bool do_swp = proc && renderer->Render();
   /* a picture kept across a context is for the first render to the screen
    * after it, and that has now been made */
   if (!ctx->rend.isRTT)
      last_picture_drop();

#if !defined(TARGET_NO_THREADS)
   if (settings.rend.ThreadedRendering && hold)
      emu_baton.AckWork();
#endif

   return do_swp;
}

/* Draw the render that is queued. Non-threaded rendering calls this where
 * the game starts the render; threaded rendering calls it on the libretro
 * thread for the render the emulation thread has handed over. Either way
 * it is the same code doing the same thing to the same data. */
bool rend_single_frame(void)
{
	while (true)
	{
		if (_pvrrc == NULL)
		{
			_pvrrc = DequeueRender();
			if (_pvrrc == NULL)
				return false;
		}

		bool do_swp = rend_frame(_pvrrc, true);

		//clear up & free data ..
		FinishRender(_pvrrc);
		_pvrrc=0;
		rend_pvr_regs = pvr_regs;

		if (do_swp)
			return true;
	}
}

void rend_start_render(void)
{
#if !defined(TARGET_NO_THREADS)
   /* One render at a time: the libretro thread may still be drawing the
    * last one. Nothing is ever dropped to make room. */
   if (settings.rend.ThreadedRendering)
      emu_baton.WaitWorkDone();
#endif
   render_called = true;
   pend_rend = false;
   TA_context* ctx = tactx_Pop(CORE_CURRENT_CTX);

   // No end of render interrupt when rendering the framebuffer
	if (!ctx || !ctx->rend.isRenderFramebuffer)
		SetREP(ctx);

   if (ctx)
   {
      bool is_rtt=(FB_W_SOF1& 0x1000000)!=0 && !ctx->rend.isRenderFramebuffer;

      if (!ctx->rend.Overrun)
      {
         //printf("REP: %.2f ms\n",render_end_pending_cycles/200000.0);
         if (!ctx->rend.isRenderFramebuffer)
            FillBGP(ctx);

         ctx->rend.isRTT      = is_rtt;
         ctx->rend.clearFramebuffer = false;
         if (!is_rtt && !ctx->rend.isRenderFramebuffer
               && FB_W_SOF1 != fb_addr_history[0] && FB_W_SOF1 != fb_addr_history[1])
         {
            ctx->rend.clearFramebuffer = true;
            fb_addr_history[0] = fb_addr_history[1];
            fb_addr_history[1] = FB_W_SOF1;
         }

         ctx->rend.fb_X_CLIP  = FB_X_CLIP;
         ctx->rend.fb_Y_CLIP  = FB_Y_CLIP;

         ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
			ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;

         max_idx              = std::max(max_idx,  ctx->rend.idx.used());
         max_vtx              = std::max(max_vtx,  ctx->rend.verts.used());
         max_op               = std::max(max_op,   ctx->rend.global_param_op.used());
         max_pt               = std::max(max_pt,   ctx->rend.global_param_pt.used());
         max_tr               = std::max(max_tr,   ctx->rend.global_param_tr.used());

         max_mvo              = std::max(max_mvo,  ctx->rend.global_param_mvo.used());
         max_modt             = std::max(max_modt, ctx->rend.modtrig.used());

         if (QueueRender(ctx))
         {
#if !defined(TARGET_NO_THREADS)
            if (settings.rend.ThreadedRendering)
            {
               /* Hand the render to the libretro thread, with the registers
                * as they are now, and sleep until it has read what it needs
                * of the game's memory. */
               memcpy(ctx->regs, pvr_regs, pvr_RendRegSize);
               emu_baton.HandOver(ctx);
            }
            else
#endif
            	rend_single_frame();
            pend_rend = true;
         }
      }
      else
      {
         ovrn++;
         INFO_LOG(PVR, "WARNING: Rendering context is overrun (%d), aborting frame", ovrn);
         tactx_Recycle(ctx);
      }
   }
}

void rend_end_render(void)
{
   /* Non-threaded only: in threaded rendering this is the emulation thread,
    * which leaves the renderer alone. */
   if (pend_rend && !settings.rend.ThreadedRendering && renderer != NULL)
      renderer->Present();
}

void rend_term(void)
{
}

void rend_vblank()
{
   /* Did the guest produce a new frame during this vblank interval? A 3D frame
    * sets render_called via STARTRENDER; a direct framebuffer write is turned
    * into a render just below. If neither happened the displayed image is
    * unchanged, so the frontend must be told this frame is a duplicate --
    * otherwise stale content (a 480i off-field, a 30fps title's repeat field,
    * a static loading screen) is delivered as a fresh frame and pacing breaks. */
   bool produced = render_called;
   if (!render_called && fb_dirty && FB_R_CTRL.fb_enable)
	{
		DEBUG_LOG(PVR, "Direct framebuffer write detected");
		u32 saved_ctx_addr = PARAM_BASE;
		bool restore_ctx = ta_ctx != NULL;
		PARAM_BASE = 0xF00000;
		SetCurrentTARC(CORE_CURRENT_CTX);
		ta_ctx->Reset();
		ta_ctx->rend.isRenderFramebuffer = true;
		ta_ctx->rend.isRTT = false;
		rend_start_render();
		PARAM_BASE = saved_ctx_addr;
		if (restore_ctx)
			SetCurrentTARC(CORE_CURRENT_CTX);
		fb_dirty = false;
		produced = true;
	}
	frame_produced = produced;
	render_called = false;
	check_framebuffer_write();
	cheatManager.Apply();

   os_DoEvents();
}

/* Consume the "new frame produced this vblank" flag. Used by os_DoEvents to set
 * is_dupe for the frontend. One-shot: cleared on read. */
bool rend_frame_produced(void)
{
	bool r = frame_produced;
	frame_produced = false;
	return r;
}

void check_framebuffer_write()
{
   u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * (FB_R_SIZE.fb_x_size + FB_R_SIZE.fb_modulus) * 4;
	fb_watch_addr_start = (SPG_CONTROL.interlace ? FB_R_SOF2 : FB_R_SOF1) & VRAM_MASK;
	fb_watch_addr_end = fb_watch_addr_start + fb_size;
}
