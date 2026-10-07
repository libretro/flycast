#pragma once
#include "ta_ctx.h"

extern u32 VertexCount;
extern u32 FrameCount;

void rend_term();

void rend_vblank();
void rend_start_render();
void rend_end_render();
void rend_end_wait();

void rend_set_fb_scale(float x,float y);

/* forward declaration */
void dc_stop();

#ifdef GLuint
GLuint
#else
u32
#endif
GetTexture(TSP tsp,TCW tcw);


///////
extern TA_context* _pvrrc;

#define pvrrc (_pvrrc->rend)

struct Renderer
{
	virtual ~Renderer() = default;

	virtual bool Init()=0;
	
	virtual void Resize(int w, int h)=0;

	virtual void Term()=0;

	virtual bool Process(TA_context* ctx)=0;
	virtual bool Render()=0;

	virtual void Present()
   {
      /* Presentation only. Control returns to retro_run at the vblank boundary
       * in os_DoEvents(), never here, so that a frame not being rendered (a
       * static screen such as disc loading) can never stall control flow.
       * Hardware renderers present via video_cb in retro_run; this base does
       * nothing. */
   }

	virtual void DrawOSD() { }

	/* The context is about to be destroyed: put the screen's last picture
	 * in rend/last_picture.h. The renderer's first render to the screen
	 * after that starts from what is there. */
	virtual void KeepPicture() { }

	// @area: 1 for the texture of a polygon's second volume
	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) { return 0; }
};

extern Renderer* renderer;
extern bool renderer_changed;


Renderer* rend_D3D11();
Renderer* rend_GLES2();
Renderer* rend_GL4();
Renderer* rend_Vulkan();
Renderer* rend_OITVulkan();

extern u32 fb_watch_addr_start;
extern u32 fb_watch_addr_end;
extern bool fb_dirty;

void check_framebuffer_write();
bool rend_frame_produced(void);

void rend_create_renderer();
void rend_init_renderer();
void rend_term_renderer();
void rend_keep_picture();
