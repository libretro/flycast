#pragma once
#include "ta.h"
#include "pvr_regs.h"
#include "helper_classes.h"

// helper for 32 byte aligned memory allocation
void* OS_aligned_malloc(size_t align, size_t size);

// helper for 32 byte aligned memory de-allocation
void OS_aligned_free(void *ptr);

//Vertex storage types
struct Vertex
{
	float x,y,z;

	u8 col[4];
	u8 vtx_spc[4];

	float u,v;

   // Two volumes format
	u8 col1[4];
	u8 spc1[4];

	float u1,v1;
};

struct PolyParam
{
	u32 first;		//entry index , holds vertex/pos data
	u32 count;

	u64 texid;

	TSP tsp;
	TCW tcw;
	PCW pcw;
	ISP_TSP isp;
	float zvZ;
	u32 tileclip;
	//float zMin,zMax;
   TSP tsp1;
	TCW tcw1;
	u64 texid1;
};

struct ModifierVolumeParam
{
	u32 first;
	u32 count;
   ISP_Modvol isp;
	u32 tileclip;	// the clipping rectangle and mode it was sent under, as a polygon's
};

struct ModTriangle
{
	f32 x0,y0,z0,x1,y1,z1,x2,y2,z2;
};

// (tad_context::ol_base: not known)
#define TAD_OL_UNKNOWN 0xFFFFFFFFu

struct  tad_context
{
	u8* thd_data;
	u8* thd_root;
	u8* thd_old_data;
   u8 *render_passes[10];
   u32 render_pass_count;
   /* Where each render pass's object lists were written (TA_OL_BASE when
    * its lists were started, TAD_OL_UNKNOWN when that is not known: a frame
    * from a save state). A render draws the passes whose lists its region
    * array points to (ta_select_passes()). */
   u32 ol_base[10];

   void ForgetOlBases()
   {
      for (u32 i = 0; i < sizeof(ol_base) / sizeof(ol_base[0]); i++)
         ol_base[i] = TAD_OL_UNKNOWN;
   }

   void Clear()
   {
      thd_old_data = thd_data = thd_root;
      render_pass_count = 0;
      ForgetOlBases();
   }

   void ClearPartial()
	{
		thd_old_data = thd_data;
		thd_data = thd_root;
	}

   void Continue()
	{
      render_passes[render_pass_count] = End();
		if (render_pass_count < sizeof(render_passes) / sizeof(u8*) - 1)
			render_pass_count++;
	}

   u8* End() const
   {
      return thd_data == thd_root ? thd_old_data : thd_data;
   }

   void Reset(u8* ptr)
	{
		thd_data = thd_root = thd_old_data = ptr;
      render_pass_count = 0;
      ForgetOlBases();
	}
};

struct RenderPass {
   bool autosort;
	bool z_clear;
	bool mv_op_tr_shared;	// the translucent modifier volumes are the opaque ones
	u32 op_count;
	u32 mvo_count;
	u32 pt_count;
	u32 tr_count;
   u32 mvo_tr_count;
};

struct rend_context
{
	u8* proc_start;
	u8* proc_end;

	f32 fZ_min;
	f32 fZ_max;

	bool Overrun;
	bool isRTT;
	bool clearFramebuffer;	// drawn to a framebuffer the game has not drawn to lately: see rend_start_render()

   bool isRenderFramebuffer;

	FB_X_CLIP_type    fb_X_CLIP;
	FB_Y_CLIP_type    fb_Y_CLIP;

   u32 fog_clamp_min;
	u32 fog_clamp_max;

	List<Vertex>      verts;
	List<u32>         idx;
	List<ModTriangle> modtrig;
	List<ModifierVolumeParam>  global_param_mvo;
   List<ModifierVolumeParam>  global_param_mvo_tr;

	List<PolyParam>   global_param_op;
	List<PolyParam>   global_param_pt;
	List<PolyParam>   global_param_tr;
   List<RenderPass>  render_passes;

	/* The region array's table of tiles (rend/tile/pvr_tile.h,
	 * pvr_region_entry()): a word per entry of the array, in its order,
	 * for the tiled renderers. Built only while ta_set_region_table(true)
	 * asks for it; region_tile_count is then how many entries the array
	 * had, of which the first REGION_TILES_MAX are kept here, and 0 when it
	 * was not built. */
	u32 *region_tiles;
	u32 region_tile_count;

	/* GPU-side TA processing drew it (ta_gpu_prepare(), TA_GPU_DRAW): the
	 * polygon parameter lists hold draw groups, and the vertices, indices
	 * and gpu_modtrig modifier volume triangles are on the GPU only */
	bool gpu;
	u32 gpu_modtrig;
	/* The render passes of the frame's data this render draws, the first
	 * and the last (ta_select_passes()); and whether there are others, which
	 * the game may start another render of the same frame for (a NAOMI 2
	 * game drawing one list continuation to the screen and the next to a
	 * texture: Virtua Fighter 4's reflections). Not cleared with the lists:
	 * set for each render as it is started. */
	u8 pass_first;
	u8 pass_last;
	bool keep;
	/* ...and with TA_GPU_SHADOWS, what each render pass's opaque modifier
	 * volumes cover (ModVolBounds()): min x, min y, max x, max y */
	f32 gpu_shadow_area[10][4];

	void Clear()
	{
		verts.Clear();
		idx.Clear();
		global_param_op.Clear();
		global_param_pt.Clear();
		global_param_tr.Clear();
		modtrig.Clear();
		global_param_mvo.Clear();
      global_param_mvo_tr.Clear();
      render_passes.Clear();
		region_tile_count = 0;
		gpu = false;
		gpu_modtrig = 0;

		Overrun=false;
		fZ_min= 1000000.0f;
		fZ_max= 1.0f;
      isRenderFramebuffer = false;
		clearFramebuffer = false;
	}
};

#define TA_DATA_SIZE (8 * 1024 * 1024)

/* Entries of the region array a frame's table keeps: 40 x 15 tiles, the
 * most a frame has, each in up to 10 passes, and some more (32 KB) */
#define REGION_TILES_MAX 8192

/* A NAOMI 2's geometry processor sends several times what a game can send
 * by itself: everything a frame is kept in is that much bigger there. */
#define TA_NAOMI2_SCALE 4

/* The control-word walk's table of a frame (ta_set_walk()): an entry for
 * each block of the frame's data that is not a vertex, as the blocks come
 * in, for the header pass (ta_walk_parse()). Room for this many, times
 * TA_NAOMI2_SCALE on a NAOMI 2 (64 KB, 256 KB there): games send a few
 * thousand parameters a frame. A frame with more is parsed the way it
 * always is. */
#define TA_WALK_MAX 16384
/* (walk_count: the table is not the frame's, or not all of it) */
#define TA_WALK_INVALID 0xFFFFFFFFu

static inline u32 ta_data_size(void)
{
	return settings.System == DC_PLATFORM_NAOMI2 ? TA_DATA_SIZE * TA_NAOMI2_SCALE : TA_DATA_SIZE;
}

/* The most tile accelerator data a save state carries, of all the frames
 * being written together: a state is always counted as having this much
 * (see pvr_serialize_v21()). */
#define TA_STATE_MAX (ta_data_size() / 4)

//vertex lists
struct TA_context
{
	u32 Address;

	/* The registers as they were when the game started this render, for
	 * the renderer to read while the emulation moves on (threaded
	 * rendering). */
	u8 *regs;

	u32 data_size;		// of the tile accelerator data: ta_data_size() when this was allocated
	u32 lastFrameUsed;	// tactx_frames when the game last selected it (tactx_Find())

	tad_context tad;
	rend_context rend;

	/* The control-word walk's table (TA_WALK_MAX): the block number of each
	 * block of the data that is not a vertex, in order. walk_restart: the
	 * lists were started again, and the next entry starts the table of a
	 * new frame's data. Written by the emulation thread as the blocks come
	 * in, read by whoever draws the frame once it is handed over. */
	u32 *walk;
	u32 walk_size;		// entries it has room for
	u32 walk_count;		// entries it has; TA_WALK_INVALID: it is not to be trusted
	bool walk_restart;

	/* GPU-side TA processing: which of the renderer's mapped slices the
	 * data is (tactx_gpu_pool_attach()), -1 for memory of its own; and
	 * the renderer's serial of the last frame whose GPU work reads it,
	 * 0 for none. A slice is written again only once that work is done. */
	int pool;
	u32 gpu_serial;
	/* A render drew some of its render passes and left it in the list for
	 * another (rend.keep), and the game has not begun a frame here since:
	 * it is not a frame being written, and a save state does not have it
	 * (nor anything it changes: tactx_Restart()) */
	bool drawn;

	
	/*
		Dreamcast games use up to 20k vtx, 30k idx, 1k (in total) parameters.
		at 30 fps, thats 600kvtx (900 stripped)
		at 20 fps thats 1.2M vtx (~ 1.8M stripped)

		allocations allow much more than that !

		some stats:
			recv:   idx: 33528, vtx: 23451, op: 128, pt: 4, tr: 133, mvo: 14, modt: 342
			sc:     idx: 26150, vtx: 17417, op: 162, pt: 12, tr: 244, mvo: 6, modt: 2044
			doa2le: idx: 47178, vtx: 34046, op: 868, pt: 0, tr: 354, mvo: 92, modt: 976 (overruns)
			ika:    idx: 46748, vtx: 33818, op: 984, pt: 9, tr: 234, mvo: 10, modt: 16, ov: 0  
			ct:     idx: 30920, vtx: 21468, op: 752, pt: 0, tr: 360, mvo: 101, modt: 732, ov: 0
			sa2:    idx: 36094, vtx: 24520, op: 1330, pt: 10, tr: 177, mvo: 39, modt: 360, ov: 0
	*/

	// the render draws every render pass of the data
	bool AllPasses() const
	{
		return rend.pass_first == 0 && rend.pass_last >= tad.render_pass_count;
	}

	void MarkRend(u32 render_pass)
	{
      //verify(render_pass <= tad.render_pass_count);

		rend.proc_start = render_pass == 0 ? tad.thd_root :
         tad.render_passes[render_pass - 1];
      rend.proc_end = render_pass == tad.render_pass_count ? tad.End() : 
         tad.render_passes[render_pass];
	}
	/* false: there was not the memory for it, and it holds nothing (a
	 * NAOMI 2's frame takes about 60 megabytes). @data: the data goes
	 * there, a slice of ta_data_size() bytes of the GPU pool's (@slot),
	 * and is not the context's to free. */
	bool Alloc(u8 *data = NULL, int slot = -1)
	{
      data_size = ta_data_size();
      const unsigned scale = data_size / TA_DATA_SIZE;
      unsigned modtrig_size = 16384 * scale;
      unsigned    vert_size = 4*1024*1024 * scale; //up to 4 mb of vtx data/frame = ~ 96k vtx/frame
      pool = data != NULL ? slot : -1;
      gpu_serial = 0;
      tad.Reset(data != NULL ? data : (u8*)OS_aligned_malloc(32, data_size));
      regs = (u8*)calloc(1, pvr_RegSize);

		rend.verts.InitBytes(vert_size,&rend.Overrun, "verts"); 
		rend.idx.Init(120*1024 * scale,&rend.Overrun, "idx"); // up to 120K indices (idx have stripification overhead)
		rend.global_param_op.Init(16384 * scale,&rend.Overrun, "global_param_op");
		rend.global_param_pt.Init(5120 * scale,&rend.Overrun, "global_param_pt");
		rend.global_param_mvo.Init(4096 * scale,&rend.Overrun, "global_param_mvo");
      rend.global_param_mvo_tr.Init(4096 * scale,&rend.Overrun, "global_param_mvo_tr");
#if STRIPS_AS_PPARAMS
      // That makes a lot of polyparams but this is required for proper sorting...
		// Rez uses more than 8192 translucent polygons sometimes
      rend.global_param_tr.Init(10240 * scale, &rend.Overrun, "global_param_tr");
#else
		rend.global_param_tr.Init(8192 * scale,&rend.Overrun, "global_param_tr");
#endif

		rend.modtrig.Init(modtrig_size,&rend.Overrun, "modtrig");

      rend.render_passes.Init(sizeof(RenderPass) * 10, &rend.Overrun, "render_passes");	// 10 render passes
		rend.region_tiles = (u32 *)malloc(REGION_TILES_MAX * sizeof(u32));
		rend.region_tile_count = 0;
		walk_size = TA_WALK_MAX * scale;
		walk = (u32 *)malloc(walk_size * sizeof(u32));

		if (!tad.thd_root || !regs || !rend.verts.daty || !rend.idx.daty
				|| !rend.global_param_op.daty || !rend.global_param_pt.daty
				|| !rend.global_param_mvo.daty || !rend.global_param_mvo_tr.daty
				|| !rend.global_param_tr.daty || !rend.modtrig.daty
				|| !rend.render_passes.daty || !rend.region_tiles || !walk)
		{
			Free();
			tad.Reset(NULL);
			regs = NULL;
			data_size = 0;
			return false;
		}
		
		Reset();
		return true;
	}

	void Reset()
	{
      tad.Clear();
		rend.Clear();
		rend.pass_first = 0;
		rend.pass_last = 0xFF;
		rend.keep = false;
		drawn = false;
		rend.proc_end = rend.proc_start = tad.thd_root;
		walk_count = TA_WALK_INVALID;
		walk_restart = false;
	}

	void Free()
	{
      if (pool < 0)
         OS_aligned_free(tad.thd_root);
      pool = -1;
      free(regs);
		rend.verts.Free();
		rend.idx.Free();
		rend.global_param_op.Free();
		rend.global_param_pt.Free();
		rend.global_param_tr.Free();
		rend.modtrig.Free();
		rend.global_param_mvo.Free();
      rend.global_param_mvo_tr.Free();
      rend.render_passes.Free();
		free(rend.region_tiles);
		rend.region_tiles = NULL;
		free(walk);
		walk = NULL;
	}
};

extern TA_context* ta_ctx;
extern tad_context ta_tad;

extern TA_context*  vd_ctx;
extern rend_context vd_rc;

TA_context* tactx_Find(u32 addr, bool allocnew);
void tactx_Reset(void);

TA_context* tactx_Alloc();
void tactx_Recycle(TA_context* poped_ctx);
/* The frame at @addr, for a render the game starts (NULL if there is
 * none), left where it is - as the frame being written, if it is - for
 * a render of some of its render passes (rend.keep: ta_select_passes()),
 * which the game may start another render of; otherwise tactx_Remove() it
 * and it is the render's. (Emulation thread.) */
TA_context* tactx_Get(u32 addr);
void tactx_Remove(TA_context* ctx);
/* The game begins a frame where one was drawn and kept (drawn): it is begun
 * as in a frame made anew there, the last in the list. (Emulation thread,
 * @ctx the current one.) */
void tactx_Restart(TA_context* ctx);
/* A render is done with @ctx: recycled, unless it was left where it was */
void tactx_Drawn(TA_context* ctx);
/* The frames being written: their tables of the control-word walk are not
 * to be trusted (from the emulation thread) */
void tactx_walk_forget(void);

/*
	Ta Context

	Rend Context
*/

#define TACTX_NONE (0xFFFFFFFF)

void SetCurrentTARC(u32 addr);
bool QueueRender(TA_context* ctx);
TA_context* DequeueRender();
void FinishRender(TA_context* ctx);

//must be moved to proper header
void FillBGP(TA_context* ctx);
void SerializeTAContext(void **data, unsigned int *total_size);
void UnserializeTAContext(void **data, unsigned int *total_size, serialize_version_enum version);

/* ---- GPU-side TA processing, its decode (rend/ta_gpu.h) ---- */

/* The most a frame of the machine's size has of what the decode makes and
 * reads, for the renderer to size its buffers by */
struct TaGpuSizes
{
	u32 verts, idx, list[3], modtrig;
	u32 headers, runs;	// in the tables
	u32 table_words;
};
void ta_gpu_sizes(TaGpuSizes *sizes);

/* Where the renderer has things: the tables' words TAG_T_TA_BASE to
 * TAG_T_REC_TR, in that order */
struct TaGpuLayout
{
	u32 words[13];
};

/* What the header pass found, for the renderer */
struct TaGpuFrame
{
	u32 table_words;	// of the tables written
	u32 verts, modtrig, headers, slots, segments, passes;
	RenderPass pass[10];	// the region settings (the counts are the GPU's to give)
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rv;		// there is something to draw
	u32 trd;		// translucent draw record slots the frame takes (TAG_H_TRB)
	u32 shadows;		// (TA_GPU_SHADOWS) the draw groups whose second drawing for shadows the GPU decides
	u32 why;		// TA_GPU_WHY_*: why it was not decoded, if it was not
};

/* ta_gpu_prepare()'s @flags */
/* The renderer draws the frame from the decode, and the full parse is not
 * run: the lists of polygon parameters are made of the draw groups, in the
 * lists' order - for the opaque and punch-through lists, first the slot of
 * the group's draw command, count its vertices; for the translucent list,
 * first the group's place in the translucent draw records, count its
 * vertices, which is the most draw records it can have - and the render
 * passes count those. Frames that the full parse would drop for want of
 * room, or might, are left to it, and frames it passes over. */
#define TA_GPU_DRAW     1u
/* ...by a renderer that sorts translucent polygons on the CPU, from the
 * CPU's vertices: a frame that may have a sorted pass is left to the
 * full parse */
#define TA_GPU_NO_SORT  2u
/* The decode gives each triangle of a flat-shaded polygon the colours of
 * its last vertex in its first, as the Vulkan renderers draw them
 * (BaseDrawer::SetProvokingVertices()) */
#define TA_GPU_PROVOKE  4u
/* The translucent draw records number each strip as its polygon parameter
 * (firstInstance: the per-pixel renderer's polygon number) */
#define TA_GPU_INSTANCE 8u
/* ...by a renderer that sorts translucent triangles as GenSorted() does
 * (rend/sorter.h; its firstProvoking with TA_GPU_PROVOKE, for Vulkan, and
 * without it for OpenGL): each sorted render pass's
 * translucent triangles are sorted here from the depths of the blocks the
 * game sent - nothing else of a vertex is read - in GenSorted()'s order,
 * ties and all, for ta_gpu_sorted() and ta_gpu_sorted_write(). Needs
 * ta_gpu_sort_reserve(); without it, a frame that may have a sorted pass is
 * left to the full parse as with TA_GPU_NO_SORT. */
#define TA_GPU_SORT     16u
/* ...by a renderer that draws the polygons that take shadows a second
 * time where the opaque modifier volumes are (rend/shadows.h): each render
 * pass's volumes' bounds are worked out here from their blocks
 * (rend_context::gpu_shadow_area), and the GPU decides which of its draw
 * groups reach into them (TaGpuFrame::shadows, TAG_K_SHADOW) */
#define TA_GPU_SHADOWS  32u

enum { TA_GPU_WHY_NONE, TA_GPU_WHY_TABLE, TA_GPU_WHY_PARSE, TA_GPU_WHY_SORT, TA_GPU_WHY_WALK, TA_GPU_WHY_ROOM, TA_GPU_WHY_COUNT };

/* Whether frames are decoded on the GPU: the control-word walk on, and the
 * header pass's memory made for frames of the machine's size. (From the
 * thread that renders, between frames.) */
void ta_set_gpu_processing(bool on);
bool ta_gpu_processing(void);
/* What the core option asks for (TA Processing: GPU), for the renderers
 * that can to take up (from any thread) */
void ta_set_gpu_wanted(bool on);
bool ta_gpu_wanted(void);
/* The header pass over @ctx (after FillBGP(), before the full parse) and
 * the decode's tables written to @table, up to @table_room words, with
 * @layout's places. false: this frame is not one the GPU decode does - no
 * table to trust, a list left open at the end of a render pass, the data
 * ending in the middle of something, a parameter where a strip has not
 * ended, more than there is room for - and only the full parse can do it
 * (@out->why says which); with TA_GPU_DRAW, it is also made ready to
 * draw in the full parse's place (@flags: TA_GPU_*). (From the thread that renders.) */
bool ta_gpu_prepare(TA_context *ctx, const TaGpuLayout *layout, u32 *table, u32 table_room, TaGpuFrame *out, u32 flags);

/* TA_GPU_SORT's memory, made (or freed) now, for frames of the machine's
 * size: about 50 bytes for each triangle a frame can have. (From the
 * thread that renders, between frames.) */
bool ta_gpu_sort_reserve(bool on);
struct SortTrigDrawParam;
/* What the last ta_gpu_prepare() with TA_GPU_SORT sorted, until the next:
 * render pass @pass's draws (GenSorted()'s @pidx_sort, first counting from
 * the first pass's first index, as Drawer::SortTriangles() has them), and
 * how many there are; NULL and 0 for a pass that is not sorted. The
 * polygon parameters they point to are the header pass's. */
const SortTrigDrawParam *ta_gpu_sorted(u32 pass, u32 *count);
/* ...and its depth prepass's draws (GenPrepass()'s; rend/sorter.h), first
 * counting from the first pass's first sorted index as well */
const SortTrigDrawParam *ta_gpu_prepass(u32 pass, u32 *count);
// ...the indices of all of them (GenSorted()'s @vidx_sort, pass after pass, then the prepasses')
u32 ta_gpu_sorted_indices(void);
/* ...and the prepass's checks after them (TAG_K_PREPASS, at TAG_T_PREP):
 * how many, for a dispatch of a workgroup each, which the renderer makes
 * after the decode's TAG_K_UNPACK where there are any; all of it written
 * to @dst, ta_gpu_sorted_words() words, before that dispatch is made */
u32 ta_gpu_prepass_checks(void);
u32 ta_gpu_sorted_words(void);
void ta_gpu_sorted_write(u32 *dst);
// ...and what the last sort took, in nanoseconds (0 for none)
u64 ta_gpu_sorted_ns(void);
// (TA_GPU_DRAW, with flat strips looked for) how long the last frame's took to find, in ns
u64 ta_gpu_flat_ns(void);

/* The GPU pool: @count contexts made now whose data are the slices of
 * @slice bytes from @base (the renderer's persistently mapped, host-cached
 * buffer), given to new frames before any other while it is there. One is
 * written again only once the renderer says the GPU work that last read it
 * is done (tactx_gpu_done()); if none is free, a frame gets a context of
 * its own memory: the spare made with the pool, and past that one made then
 * as without the pool. Detached, the contexts still in use
 * have their data copied to memory of their own and go on. Both with the
 * machine standing still and the GPU idle. */
bool tactx_gpu_pool_attach(u8 *base, u32 slice, u32 count);
void tactx_gpu_pool_detach(void);
/* Since the pool was attached: frames that found it all taken (and went to
 * a spare context of their own memory, drawn from the CPU's parse), and of
 * them those that found no spare either and had one allocated */
void tactx_gpu_pool_stats(u32 *short_frames, u32 *allocated);
/* The GPU work of every frame up to renderer serial @serial is done (from
 * the thread that renders) */
void tactx_gpu_done(u32 serial);
