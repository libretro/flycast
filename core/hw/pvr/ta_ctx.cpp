#include "ta_ctx.h"
#include "lockfree.h"
#include "spg.h"
#include "oslib/oslib.h"

#include "hw/sh4/sh4_sched.h"

#if defined(HAVE_LIBNX)
#include <malloc.h>
#endif

extern u32 fskip;
extern u32 FrameCount;

int frameskip=0;
bool FrameSkipping=false;		// global switch to enable/disable frameskip

TA_context* ta_ctx;
tad_context ta_tad;

TA_context*  vd_ctx;
rend_context vd_rc;

// helper for 32 byte aligned memory allocation
void* OS_aligned_malloc(size_t align, size_t size)
{
#ifdef __MINGW32__
   return __mingw_aligned_malloc(size, align);
#elif defined(_WIN32)
   return _aligned_malloc(size, align);
#elif defined(HAVE_LIBNX)
   return memalign(align, size);
#else
   void *p = NULL;
   int ret = posix_memalign(&p, align, size);
   return (ret == 0) ? p : 0;
#endif
}

// helper for 32 byte aligned memory de-allocation
void OS_aligned_free(void *ptr)
{
#ifdef __MINGW32__
   __mingw_aligned_free(ptr);
#elif defined(_WIN32)
   _aligned_free(ptr);
#else
   free(ptr);
#endif
}

void SetCurrentTARC(u32 addr)
{
	if (addr != TACTX_NONE)
	{
		if (ta_ctx)
			SetCurrentTARC(TACTX_NONE);

      verify(ta_ctx == 0);
		//set new context
		ta_ctx = tactx_Find(addr,true);

		/* No memory for it: there is no frame being written, and what the
		 * game sends is dropped as it is before any list is opened. */
		if (ta_ctx == NULL)
			return;

		//copy cached params
		ta_tad = ta_ctx->tad;
	}
	else
	{
		//Flush cache to context
      verify(ta_ctx != 0);
		ta_ctx->tad=ta_tad;
		
		//clear context
		ta_ctx=0;
      ta_tad.Reset(0);
	}
}

/* The one frame waiting to be drawn or being drawn. In threaded rendering
 * the emulation thread fills the slot and the libretro thread empties it
 * once the frame is drawn; a context in the slot belongs to whoever draws
 * it. */
static retro_atomic_ptr_t rqueue;

bool QueueRender(TA_context* ctx)
{
   verify(ctx != 0);

   if (FrameSkipping && frameskip) {
 		frameskip=1-frameskip;
		tactx_Drawn(ctx);
		fskip++;
		return false;
 	}

	/* Never taken in practice: a render is drawn where it is started
	 * (non-threaded), or the emulation thread has waited for the one before
	 * it to be finished (threaded). */
	if (retro_atomic_load_acquire_ptr(&rqueue))
   {
		tactx_Drawn(ctx);
		return false;
	}

   retro_atomic_store_release_ptr(&rqueue, ctx);

	return true;
}

TA_context* DequeueRender(void)
{
	TA_context* rv = (TA_context *)retro_atomic_load_acquire_ptr(&rqueue);

	if (rv)
		FrameCount++;

	return rv;
}

void FinishRender(TA_context* ctx)
{
	if (ctx != NULL)
	{
		verify(retro_atomic_load_relaxed_ptr(&rqueue) == ctx);
		retro_atomic_store_release_ptr(&rqueue, NULL);

		tactx_Drawn(ctx);
	}
}

/* Spare contexts. Both threads give them back; a slot hands each one to
 * a single taker. */
static cSlotCache<TA_context, 3> ctx_pool;
/* The contexts being written, by the address the game gives each: a
 * handful, looked up a few times a frame. An address is a megabyte
 * (TA_CURRENT_CTX), so there are never more than sixteen. */
#define TA_CONTEXT_ADDRESSES 16
static TA_context *ctx_list[TA_CONTEXT_ADDRESSES];
static unsigned ctx_count;
/* Renders started (tactx_Get()), for the age of a frame: one the game has
 * not selected for more than TA_CONTEXT_MAX_AGE of them is given to the
 * next new address instead of a new one being made. A game that sends
 * frames to ever different addresses and never draws some of them would
 * otherwise keep one buffer (8 MB, 60 MB on a NAOMI 2) for each address it
 * ever used. Counted on the emulation side, whatever is skipped or drawn. */
static u32 tactx_frames;
#define TA_CONTEXT_MAX_AGE 60

/* GPU-side TA processing's pool (tactx_gpu_pool_attach()). Each context
 * of it is free, in use (a frame being written or drawn), or retired: given
 * back, and free once the GPU work of its gpu_serial is done. Taken by the
 * emulation thread, given back by either thread, the GPU's progress said
 * by the thread that renders; one atomic word each, and no lock. */
#define TA_GPU_POOL_MAX 16
enum { POOL_FREE, POOL_USED, POOL_RETIRED };
static TA_context *gpu_pool[TA_GPU_POOL_MAX];
static retro_atomic_int_t gpu_pool_state[TA_GPU_POOL_MAX];
static retro_atomic_int_t gpu_pool_count = RETRO_ATOMIC_INT_INITIALIZER(0);
static retro_atomic_int_t gpu_done_serial = RETRO_ATOMIC_INT_INITIALIZER(0);
/* Frames that found the pool all taken, and of them those that found no
 * spare context either and had one allocated (tactx_gpu_pool_stats()) */
static retro_atomic_int_t gpu_pool_short = RETRO_ATOMIC_INT_INITIALIZER(0);
static retro_atomic_int_t gpu_pool_allocated = RETRO_ATOMIC_INT_INITIALIZER(0);

static TA_context *gpu_pool_take(void)
{
	const int n = retro_atomic_load_acquire_int(&gpu_pool_count);
	const u32 done = (u32)retro_atomic_load_acquire_int(&gpu_done_serial);
	for (int i = 0; i < n; i++)
	{
		const int st = retro_atomic_load_acquire_int(&gpu_pool_state[i]);
		TA_context *ctx = gpu_pool[i];
		if (st == POOL_FREE || (st == POOL_RETIRED && (s32)(done - ctx->gpu_serial) >= 0))
			if (retro_atomic_cas_int(&gpu_pool_state[i], st, POOL_USED))
			{
				ctx->gpu_serial = 0;
				return ctx;
			}
	}
	return NULL;
}

void tactx_gpu_done(u32 serial)
{
	retro_atomic_store_release_int(&gpu_done_serial, (int)serial);
}

bool tactx_gpu_pool_attach(u8 *base, u32 slice, u32 count)
{
	tactx_gpu_pool_detach();
	if (count > TA_GPU_POOL_MAX)
		count = TA_GPU_POOL_MAX;
	if (slice < ta_data_size())
		return false;
	for (u32 i = 0; i < count; i++)
	{
		TA_context *ctx = new TA_context();
		if (!ctx->Alloc(base + (size_t)i * slice, (int)i))
		{
			delete ctx;
			count = i;
			break;
		}
		gpu_pool[i] = ctx;
		retro_atomic_store_relaxed_int(&gpu_pool_state[i], POOL_FREE);
	}
	/* A frame that finds every slice taken is given a context of its own
	 * memory, and drawn from the CPU's parse: one is made now, for the
	 * spares, so that it is not made while a game runs */
	if (count != 0)
	{
		TA_context *spare = ctx_pool.Take();
		if (spare != NULL && spare->data_size != ta_data_size())
		{
			spare->Free();
			delete spare;
			spare = NULL;
		}
		if (spare == NULL)
		{
			spare = new TA_context();
			if (!spare->Alloc())
			{
				delete spare;
				spare = NULL;
			}
		}
		if (spare != NULL && !ctx_pool.Put(spare))
		{
			spare->Free();
			delete spare;
		}
	}
	retro_atomic_store_relaxed_int(&gpu_pool_short, 0);
	retro_atomic_store_relaxed_int(&gpu_pool_allocated, 0);
	retro_atomic_store_release_int(&gpu_pool_count, (int)count);
	return count != 0;
}

void tactx_gpu_pool_stats(u32 *short_frames, u32 *allocated)
{
	*short_frames = (u32)retro_atomic_load_acquire_int(&gpu_pool_short);
	*allocated = (u32)retro_atomic_load_acquire_int(&gpu_pool_allocated);
}

/* @ctx's data moved to memory of its own, @from its old place */
static void tactx_rebase(TA_context *ctx, u8 *to)
{
	tad_context *tads[2] = { &ctx->tad, ctx == ta_ctx ? &ta_tad : NULL };
	const u8 *from = ctx->tad.thd_root;
	// (the one being written has its pointers in ta_tad)
	const tad_context &live = ctx == ta_ctx ? ta_tad : ctx->tad;
	memcpy(to, from, live.End() - from);
	for (int t = 0; t < 2; t++)
	{
		tad_context *tad = tads[t];
		if (tad == NULL)
			continue;
		tad->thd_data = to + (tad->thd_data - from);
		tad->thd_old_data = to + (tad->thd_old_data - from);
		for (u32 p = 0; p < sizeof(tad->render_passes) / sizeof(tad->render_passes[0]); p++)
			if (tad->render_passes[p] >= from && tad->render_passes[p] <= from + ctx->data_size)
				tad->render_passes[p] = to + (tad->render_passes[p] - from);
		tad->thd_root = to;
	}
}

void tactx_gpu_pool_detach(void)
{
	const int n = retro_atomic_load_acquire_int(&gpu_pool_count);
	retro_atomic_store_release_int(&gpu_pool_count, 0);
	for (int i = 0; i < n; i++)
	{
		TA_context *ctx = gpu_pool[i];
		gpu_pool[i] = NULL;
		if (retro_atomic_load_acquire_int(&gpu_pool_state[i]) == POOL_USED)
		{
			/* a frame being written or drawn: it goes on in memory of its
			 * own (the renderer is changing; this is not while a game runs) */
			u8 *data = (u8 *)OS_aligned_malloc(32, ctx->data_size);
			if (data != NULL)
			{
				tactx_rebase(ctx, data);
				ctx->pool = -1;
				ctx->gpu_serial = 0;
				continue;
			}
			// no memory: what it had is lost, and it is empty
			WARN_LOG(PVR, "Out of memory for a tile accelerator frame: its lists are dropped");
			ctx->pool = -1;
			ctx->tad.Reset((u8 *)OS_aligned_malloc(32, ctx->data_size));
			if (ctx == ta_ctx)
				ta_tad = ctx->tad;
			continue;
		}
		ctx->Free();
		delete ctx;
	}
}

TA_context* tactx_Alloc(void)
{
	TA_context* rv = gpu_pool_take();
	if (rv != NULL)
		return rv;
	const bool pooled = retro_atomic_load_acquire_int(&gpu_pool_count) != 0;
	if (pooled)
		retro_atomic_inc_int(&gpu_pool_short);
	rv = ctx_pool.Take();

	if (rv && rv->data_size != ta_data_size())
	{
		// kept from a machine whose frames are another size
		rv->Free();
		delete rv;
		rv = NULL;
	}
	if (!rv)
   {
      /* (with the pool: more frames at once than its slices and the spare
       * made with it, which only a game writing to many addresses at once
       * comes to; counted, and said once) */
      if (pooled && retro_atomic_fetch_add_int(&gpu_pool_allocated, 1) == 0)
         WARN_LOG(PVR, "TA processing: every frame of the GPU pool and the spare are in use: one is allocated");
      rv = new TA_context();
      if (!rv->Alloc())
      {
         WARN_LOG(PVR, "Out of memory for a tile accelerator frame: its lists are dropped");
         delete rv;
         rv = NULL;
      }
   }

   return rv;
}

void tactx_Recycle(TA_context* poped_ctx)
{
   poped_ctx->Reset();
   if (poped_ctx->pool >= 0)
   {
      retro_atomic_store_release_int(&gpu_pool_state[poped_ctx->pool],
            poped_ctx->gpu_serial != 0 ? POOL_RETIRED : POOL_FREE);
      return;
   }
   if (!ctx_pool.Put(poped_ctx))
   {
      poped_ctx->Free();
      delete poped_ctx;
   }
}

TA_context* tactx_Find(u32 addr, bool allocnew)
{
   TA_context *old_ctx = 0;

   for (unsigned i=0; i<ctx_count; i++)
   {
      if (ctx_list[i]->Address==addr)
      {
         ctx_list[i]->lastFrameUsed = tactx_frames;
         if (ctx_list[i]->data_size != ta_data_size())
         {
            // left by a machine whose frames are another size
            TA_context *ctx = ctx_list[i];
            ctx->Free();
            if (!ctx->Alloc())
            {
               delete ctx;
               ctx_count--;
               memmove(&ctx_list[i], &ctx_list[i + 1], (ctx_count - i) * sizeof(*ctx_list));
               return 0;
            }
         }
         return ctx_list[i];
      }
      if (tactx_frames - ctx_list[i]->lastFrameUsed > TA_CONTEXT_MAX_AGE
            && ctx_list[i]->data_size == ta_data_size())
         old_ctx = ctx_list[i];
   }

   if (allocnew)
   {
      TA_context *rv;
      if (old_ctx)
      {
         // what the game sent to it long ago and never drew is dropped
         old_ctx->Reset();
         old_ctx->Address = addr;
         old_ctx->lastFrameUsed = tactx_frames;
         return old_ctx;
      }
      if (ctx_count == TA_CONTEXT_ADDRESSES)
         return 0;
      rv = tactx_Alloc();
      if (!rv)
         return 0;
      rv->Address=addr;
      rv->lastFrameUsed = tactx_frames;
      ctx_list[ctx_count++] = rv;

      return rv;
   }

   return 0;
}

void tactx_walk_forget(void)
{
	for (unsigned i = 0; i < ctx_count; i++)
	{
		ctx_list[i]->walk_count = TA_WALK_INVALID;
		ctx_list[i]->walk_restart = false;
	}
}

void tactx_Drawn(TA_context* ctx)
{
	// (one left in the list is still the list's)
	if (!ctx->rend.keep)
		tactx_Recycle(ctx);
}

TA_context* tactx_Get(u32 addr)
{
	// (called once for each render the game starts)
	tactx_frames++;
	for (unsigned i = 0; i < ctx_count; i++)
		if (ctx_list[i]->Address == addr)
		{
			TA_context *ctx = ctx_list[i];
			// (the one being written has its pointers out, in ta_tad)
			if (ctx == ta_ctx)
				ctx->tad = ta_tad;
			return ctx;
		}
	return 0;
}

void tactx_Remove(TA_context* ctx)
{
	for (unsigned i = 0; i < ctx_count; i++)
		if (ctx_list[i] == ctx)
		{
			if (ta_ctx == ctx)
				SetCurrentTARC(TACTX_NONE);
			ctx_count--;
			memmove(&ctx_list[i], &ctx_list[i + 1], (ctx_count - i) * sizeof(*ctx_list));
			return;
		}
}

void tactx_Restart(TA_context* ctx)
{
	for (unsigned i = 0; i < ctx_count; i++)
		if (ctx_list[i] == ctx)
		{
			memmove(&ctx_list[i], &ctx_list[i + 1], (ctx_count - 1 - i) * sizeof(*ctx_list));
			ctx_list[ctx_count - 1] = ctx;
			break;
		}
	ta_tad.Clear();
	ctx->walk_count = TA_WALK_INVALID;
	ctx->walk_restart = false;
	ctx->drawn = false;
}

/* The machine is reset: the frames it was writing are the last run's, and
 * none is kept. (A frame handed over to be drawn is not one of these: it
 * is whoever draws it's.) */
void tactx_Reset(void)
{
	if (ta_ctx)
		SetCurrentTARC(TACTX_NONE);
	while (ctx_count)
		tactx_Recycle(ctx_list[--ctx_count]);
}

const u32 NULL_CONTEXT = ~0u;

/* Past @size bytes of a state that there is nowhere to put. */
static void skip_state_bytes(void **data, unsigned int *total_size, u32 size)
{
	if (*data != NULL)
	{
		if (!ra_unserialize_has(*data, size))
		{
			ra_unserialize_fail();
			return;
		}
		*data = (u8 *)*data + size;
	}
	*total_size += size;
}

/* The frame a state from before version 21 says was being written to, for
 * pvr_state_before_v21() */
static u32 old_state_addr = NULL_CONTEXT;

/* The place a state has had for one frame's data since before V21. The
 * frames are in pvr_serialize_v21()'s part now, all of them and whole, and
 * here there is only which frame was being written to, with no data and no
 * continued lists: what a reader of the older layout takes for a frame that
 * nothing has been sent to yet. (The data used to be written here as far as
 * a stale mark, so the size of a state changed from one to the next, and
 * one could come out larger than the frontend had been told a state is.)
 * Twelve bytes at most, and counted as that. */
void SerializeTAContext(void **data, unsigned int *total_size)
{
	const u32 none = 0;

	if (*data == NULL)
	{
		*total_size += 12;
		return;
	}
	if (ta_ctx == NULL)
	{
		LIBRETRO_S(NULL_CONTEXT);
		return;
	}
	LIBRETRO_S(ta_ctx->Address);
	LIBRETRO_S(none);		// bytes of data
	LIBRETRO_S(none);		// lists continued
}

void UnserializeTAContext(void **data, unsigned int *total_size, serialize_version_enum version)
{
	u32 address;
	LIBRETRO_US(address);
	old_state_addr = address;
	if (address == NULL_CONTEXT)
		return;
	/* A frame is named by the megabyte its lists go to (TA_CURRENT_CTX).
	 * With any other name it would never be found again, and stay. */
	address &= 0xF00000;
	old_state_addr = address;
	SetCurrentTARC(address);
	u32 size;
	LIBRETRO_US(size);
	if (ta_ctx == NULL)
	{
		// (no memory for the frame: what the state has of it is passed over)
		skip_state_bytes(data, total_size, size);
		if (version >= V12)
		{
			u32 count;
			LIBRETRO_US(count);
			if (count > 10)
				count = 10;
			skip_state_bytes(data, total_size, count * 4);
		}
		return;
	}
	/* size is attacker-controlled (comes straight from the state). The TA
	 * buffer is a fixed TA_DATA_SIZE allocation, so a corrupt/malicious state
	 * with a larger size would memcpy past the end of the heap block. Clamp. */
	if (size > ta_ctx->data_size)
		size = ta_ctx->data_size;
	LIBRETRO_USA(ta_ctx->tad.thd_root, size);
	// (data is in 32-byte blocks, and is copied to where this points as aligned ones)
	size &= ~31u;
	ta_ctx->tad.thd_data = ta_ctx->tad.thd_root + size;
	ta_ctx->walk_count = TA_WALK_INVALID;	// (the data is not what was walked)
   if (version >= V12)
	{
		/* render_passes is a fixed [10] array of which the last is never
		 * counted (tad_context::Continue() writes the one after the count):
		 * no more than nine from a state either. (As many offsets are read
		 * as were before, when a count of ten was let through.) */
		const u32 max_passes = (u32)(sizeof(ta_ctx->tad.render_passes)
			/ sizeof(ta_ctx->tad.render_passes[0])) - 1;
		u32 count;
		LIBRETRO_US(count);
		if (count > max_passes + 1)
			count = max_passes + 1;
		ta_ctx->tad.render_pass_count = count > max_passes ? max_passes : count;
		for (u32 i = 0; i < count; i++)
		{
			u32 offset;
			LIBRETRO_US(offset);
			if (i >= max_passes)
				continue;
			offset &= ~31u;
			if (offset > size)
				offset = size;
			ta_ctx->tad.render_passes[i] = ta_ctx->tad.thd_root + offset;
		}
	}
	else
	{
		ta_ctx->tad.render_pass_count = 0;
	}
}

/* ---- What a save state has of the PowerVR side from version 21 on ----
 *
 * Up to version 20 a state had one frame's tile accelerator data, cut at
 * the place it had reached when the game last opened a list in it - none
 * of it, most of the time - and loading put it beside whatever the frames
 * of the moment held. A frame half sent when a state was saved came back
 * as something else. Now every frame being written is in the state, as
 * far as it has got, and loading replaces them all.
 *
 * In order:
 *   the light gun               5 bytes, spg_gun_serialize()
 *   u16  how much of a macroblock the YUV converter has received (the
 *        bytes themselves, YUV_tempdata, were in the state already)
 *   u8   how many frames are being written, 0 to 16
 *   u8   which of them the tile accelerator is writing to, 0xFF for none
 *   and for each frame:
 *     u8   which it is: its address, in megabytes
 *     u32  how many bytes of data it has (a multiple of 32); bit 0 says
 *          that the game has opened a new list in it since and sent
 *          nothing yet, so that they are still what a render draws but
 *          the next block starts over; bit 1 that it is not being written
 *          but was drawn and kept (drawn: a render drew some of its render
 *          passes, and the game may start another render of it - Virtua
 *          Fighter 4 draws each frame again, into a texture, in the next)
 *     u8   how many times a list was continued, 0 to 9
 *     u32  for each of those, where in the data
 *     the data
 * The state of the list being sent (ta_fsm[2048], ta_fsm_cl) was in the
 * state already. With nothing sent and one frame open - the end of most
 * frames - that is 15 bytes, and nothing is copied.
 *
 * TA_STATE_MAX (ta_ctx.h) is the most data, of all frames together, that
 * a state carries. The frontend asks once how big a state can be and is
 * told this much, every time; what a state does not use of it is not
 * written. A frame whose data no longer fits is saved without it and
 * comes back empty, as every frame did before. (Its buffer holds 8
 * megabytes, and there can be sixteen of them.) The frames drawn and kept
 * come after the ones being written, and one that does not fit is not in
 * the state. They were in none before: a render the game started of one
 * after a state was loaded found nothing, and drew nothing - run ahead or
 * rolled back, Virtua Fighter 4 drew none of its frames into the texture
 * it fades with, and video memory and the picture were not those of the
 * frames run straight through.
 *
 * Threads. The frames here are the ones the game is writing to: ctx_list,
 * the emulation side's alone. A frame handed over to be drawn has been
 * taken out of the list first (tactx_Remove()) and is never looked at,
 * but for one a render draws some render passes of (rend.keep), which
 * stays: it is read, not written, while it is drawn. And
 * a state is saved or loaded only by whoever holds the machine
 * (EmuBaton::Hold()), which is to be had only between two frames: the
 * emulation thread is asleep, and every render it handed over has been
 * drawn and given back, since the frame is not over before that. So
 * nothing is being drawn either while the frames are replaced, and the
 * spare ones change hands as they do at any other time. */
#define TA_STATE_CONTEXTS 16
#define TA_STATE_PASSES   9

extern u32 YUV_index;
void spg_gun_serialize(void **data, unsigned int *total_size);
void spg_gun_unserialize(void **data, unsigned int *total_size);
void spg_gun_before_v21(void);

void pvr_serialize_v21(void **data, unsigned int *total_size)
{
	u16 yuv_index = (u16)YUV_index;
	u8 count = 0;
	u8 current = 0xFF;
	u32 room = TA_STATE_MAX;
	unsigned i;

	spg_gun_serialize(data, total_size);
	LIBRETRO_S(yuv_index);

	if (*data == NULL)
	{
		// how much room a state needs: as much as one ever does
		*total_size += 2 + TA_STATE_CONTEXTS * (1 + 4 + 1 + 4 * TA_STATE_PASSES) + TA_STATE_MAX;
		return;
	}

	/* The frames being written, then the ones drawn and kept, which come
	 * back as they were if they fit */
	const TA_context *saved[TA_STATE_CONTEXTS];
	for (i = 0; i < ctx_count && count < TA_STATE_CONTEXTS; i++)
	{
		if (ctx_list[i]->drawn)
			continue;
		if (ctx_list[i] == ta_ctx)
			current = count;
		saved[count++] = ctx_list[i];
	}
	const u8 writing = count;
	for (i = 0; i < ctx_count && count < TA_STATE_CONTEXTS; i++)
	{
		const TA_context *ctx = ctx_list[i];
		if (!ctx->drawn)
			continue;
		const u32 size = (u32)(ctx->tad.thd_data - ctx->tad.thd_root) & ~31u;
		if (size > ctx->data_size)
			continue;
		saved[count++] = ctx;
	}
	/* (as many of the drawn ones as there is room for, after the others) */
	{
		u32 left = room;
		u8 n;
		for (n = 0; n < writing; n++)
		{
			const TA_context *ctx = saved[n];
			const tad_context& tad = ctx == ta_ctx ? ta_tad : ctx->tad;
			const bool restarted = tad.thd_data == tad.thd_root;
			const u32 size = (u32)((restarted ? tad.thd_old_data : tad.thd_data) - tad.thd_root) & ~31u;
			if (size <= left && size <= ctx->data_size)
				left -= size;
		}
		for (; n < count; n++)
		{
			const u32 size = (u32)(saved[n]->tad.thd_data - saved[n]->tad.thd_root) & ~31u;
			if (size > left)
				break;
			left -= size;
		}
		count = n;
	}
	LIBRETRO_S(count);
	LIBRETRO_S(current);

	for (i = 0; i < count; i++)
	{
		const TA_context *ctx = saved[i];
		const bool drawn = i >= writing;
		// (the one being written to has its pointers out, in ta_tad)
		const tad_context& tad = ctx == ta_ctx ? ta_tad : ctx->tad;
		const bool restarted = !drawn && tad.thd_data == tad.thd_root;
		u8 key = (u8)(ctx->Address >> 20);
		u32 size = (u32)((restarted ? tad.thd_old_data : tad.thd_data) - tad.thd_root) & ~31u;
		u8 passes = tad.render_pass_count > TA_STATE_PASSES ? TA_STATE_PASSES : (u8)tad.render_pass_count;
		u32 word;

		if (size > room || size > ctx->data_size)
		{
			size = 0;
			passes = 0;
		}
		room -= size;
		word = size | (restarted && size != 0 ? 1 : 0) | (drawn ? 2 : 0);

		LIBRETRO_S(key);
		LIBRETRO_S(word);
		LIBRETRO_S(passes);
		for (u32 pass = 0; pass < passes; pass++)
		{
			u32 offset = (u32)(tad.render_passes[pass] - tad.thd_root);
			LIBRETRO_S(offset);
		}
		LIBRETRO_SA(tad.thd_root, size);
	}
}

/* false: a length that cannot be, and so no telling where the rest of the
 * state is. */
bool pvr_unserialize_v21(void **data, unsigned int *total_size)
{
	u16 yuv_index;
	u8 count;
	u8 current;
	u32 current_addr = TACTX_NONE;

	spg_gun_unserialize(data, total_size);
	LIBRETRO_US(yuv_index);
	// a macroblock is 384 bytes, received 32 at a time
	YUV_index = yuv_index < 384 ? yuv_index & ~31u : 0;

	LIBRETRO_US(count);
	LIBRETRO_US(current);

	// the frames of before the load are not the state's
	tactx_Reset();
	old_state_addr = NULL_CONTEXT;

	if (count > TA_STATE_CONTEXTS)
		return false;

	for (u32 i = 0; i < count; i++)
	{
		u8 key;
		u32 word;
		u8 passes;
		u32 size;
		TA_context *ctx;

		LIBRETRO_US(key);
		LIBRETRO_US(word);
		LIBRETRO_US(passes);

		ctx = tactx_Find((u32)(key & 15) << 20, true);
		size = word & ~31u;
		if ((word & 28) != 0 || size > ta_data_size())
			return false;
		if (ctx == NULL)
		{
			// (no memory for the frame: it is passed over, and not written to)
			skip_state_bytes(data, total_size, passes * 4 + size);
			continue;
		}
		if (size > ctx->data_size)
			return false;

		tad_context& tad = ctx->tad;
		tad.render_pass_count = passes > TA_STATE_PASSES ? TA_STATE_PASSES : passes;
		for (u32 pass = 0; pass < passes; pass++)
		{
			u32 offset;
			LIBRETRO_US(offset);
			if (pass >= TA_STATE_PASSES)
				continue;
			offset &= ~31u;
			if (offset > size)
				offset = size;
			tad.render_passes[pass] = tad.thd_root + offset;
		}
		LIBRETRO_USA(tad.thd_root, size);
		if (word & 1)
		{
			tad.thd_data     = tad.thd_root;
			tad.thd_old_data = tad.thd_root + size;
		}
		else
		{
			tad.thd_data     = tad.thd_root + size;
			tad.thd_old_data = tad.thd_root;
		}

		// the control-word walk's table of the data, which the state does not have
		ta_walk_rebuild(ctx);

		if (word & 2)
		{
			// drawn and kept: there for another render, not written to
			ctx->drawn = true;
			ctx->rend.keep = true;
			continue;
		}

		if (i == current)
			current_addr = ctx->Address;
	}

	if (current_addr != TACTX_NONE)
	{
		/* The frame being written: its last render pass's lists go where
		 * the register says (where the others' went is not in a state:
		 * see ta_select_passes()) */
		TA_context *ctx = tactx_Find(current_addr, false);
		if (ctx != NULL)
			ctx->tad.ol_base[ctx->tad.render_pass_count] = TA_OL_BASE & 0xFFFFE0u;
		SetCurrentTARC(current_addr);
	}
	return true;
}

/* A state from before version 21 has been loaded. What it has of a frame
 * is not where the frame had got to, and of the other frames it has
 * nothing: every frame of before the load is dropped, and the one the
 * state says was being written to (UnserializeTAContext() read which)
 * starts empty. The YUV converter starts on a whole macroblock. */
void pvr_state_before_v21(void)
{
	const u32 addr = old_state_addr;

	spg_gun_before_v21();
	YUV_index = 0;

	tactx_Reset();
	old_state_addr = NULL_CONTEXT;
	if (addr != NULL_CONTEXT)
		SetCurrentTARC(addr);
}

/* ---- From version 28 on ----
 *
 *   the light gun        1 byte, spg_gun_serialize_v28()
 *   u8 age[16]           for each frame being written, by its address in
 *                        megabytes (the key of pvr_serialize_v21()), how
 *                        many renders have been started since the game last
 *                        selected it, up to TA_CONTEXT_MAX_AGE + 1; 0 where
 *                        there is no frame
 * 17 bytes, always. Read after pvr_unserialize_v21(), which makes the
 * frames. */
void spg_gun_serialize_v28(void **data, unsigned int *total_size);
void spg_gun_unserialize_v28(void **data, unsigned int *total_size);
void spg_gun_before_v28(void);

void pvr_serialize_v28(void **data, unsigned int *total_size)
{
	u8 ages[TA_STATE_CONTEXTS];

	spg_gun_serialize_v28(data, total_size);
	memset(ages, 0, sizeof(ages));
	for (unsigned i = 0; i < ctx_count; i++)
	{
		const u32 age = tactx_frames - ctx_list[i]->lastFrameUsed;
		ages[(ctx_list[i]->Address >> 20) & 15] = age > TA_CONTEXT_MAX_AGE ? TA_CONTEXT_MAX_AGE + 1 : (u8)age;
	}
	LIBRETRO_SA(ages, TA_STATE_CONTEXTS);
}

bool pvr_unserialize_v28(void **data, unsigned int *total_size)
{
	u8 ages[TA_STATE_CONTEXTS];

	spg_gun_unserialize_v28(data, total_size);
	LIBRETRO_USA(ages, TA_STATE_CONTEXTS);
	for (unsigned i = 0; i < ctx_count; i++)
	{
		u32 age = ages[(ctx_list[i]->Address >> 20) & 15];
		if (age > TA_CONTEXT_MAX_AGE)
			age = TA_CONTEXT_MAX_AGE + 1;
		ctx_list[i]->lastFrameUsed = tactx_frames - age;
	}
	return true;
}

/* An older state: every frame it has was selected just now, and a gun
 * transfer under way is reported at the next vertical blank out. */
void pvr_state_before_v28(void)
{
	spg_gun_before_v28();
	for (unsigned i = 0; i < ctx_count; i++)
		ctx_list[i]->lastFrameUsed = tactx_frames;
}
