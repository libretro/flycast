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
		tactx_Recycle(ctx);
		fskip++;
		return false;
 	}

	/* Never taken in practice: a render is drawn where it is started
	 * (non-threaded), or the emulation thread has waited for the one before
	 * it to be finished (threaded). */
	if (retro_atomic_load_acquire_ptr(&rqueue))
   {
		tactx_Recycle(ctx);
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

		tactx_Recycle(ctx);
	}
}

/* Spare contexts. Both threads give them back; a slot hands each one to
 * a single taker. */
static cSlotCache<TA_context, 3> ctx_pool;
/* The contexts being written, by the address the game gives each: a
 * handful, looked up a few times a frame. */
static TA_context **ctx_list;
static unsigned ctx_count;
static unsigned ctx_room;

TA_context* tactx_Alloc(void)
{
	TA_context* rv = ctx_pool.Take();

	if (rv && rv->data_size != ta_data_size())
	{
		// kept from a machine whose frames are another size
		rv->Free();
		delete rv;
		rv = NULL;
	}
	if (!rv)
   {
      rv = new TA_context();
      rv->Alloc();
   }

   return rv;
}

void tactx_Recycle(TA_context* poped_ctx)
{
   poped_ctx->Reset();
   if (!ctx_pool.Put(poped_ctx))
   {
      poped_ctx->Free();
      delete poped_ctx;
   }
}

TA_context* tactx_Find(u32 addr, bool allocnew)
{
   for (unsigned i=0; i<ctx_count; i++)
   {
      if (ctx_list[i]->Address==addr)
      {
         if (ctx_list[i]->data_size != ta_data_size())
         {
            // left by a machine whose frames are another size
            ctx_list[i]->Free();
            ctx_list[i]->Alloc();
         }
         return ctx_list[i];
      }
   }

   if (allocnew)
   {
      TA_context *rv;
      if (ctx_count == ctx_room)
      {
         unsigned room = ctx_room ? ctx_room * 2 : 8;
         TA_context **list = (TA_context **)realloc(ctx_list, room * sizeof(*list));
         if (!list)
            return 0;
         ctx_list = list;
         ctx_room = room;
      }
      rv = tactx_Alloc();
      rv->Address=addr;
      ctx_list[ctx_count++] = rv;

      return rv;
   }

   return 0;
}

TA_context* tactx_Pop(u32 addr)
{
	for (unsigned i=0; i<ctx_count; i++)
   {
      if (ctx_list[i]->Address == addr)
      {
         TA_context *rv = ctx_list[i];

         if (ta_ctx == rv)
            SetCurrentTARC(TACTX_NONE);

         ctx_count--;
         memmove(&ctx_list[i], &ctx_list[i + 1], (ctx_count - i) * sizeof(*ctx_list));

         return rv;
      }
   }
	return 0;
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
	/* size is attacker-controlled (comes straight from the state). The TA
	 * buffer is a fixed TA_DATA_SIZE allocation, so a corrupt/malicious state
	 * with a larger size would memcpy past the end of the heap block. Clamp. */
	if (size > ta_ctx->data_size)
		size = ta_ctx->data_size;
	LIBRETRO_USA(ta_ctx->tad.thd_root, size);
	// (data is in 32-byte blocks, and is copied to where this points as aligned ones)
	size &= ~31u;
	ta_ctx->tad.thd_data = ta_ctx->tad.thd_root + size;
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
 *          the next block starts over
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
 * megabytes, and there can be sixteen of them.)
 *
 * Threads. The frames here are the ones the game is writing to: ctx_list,
 * the emulation side's alone. A frame handed over to be drawn has been
 * taken out of the list first (tactx_Pop()) and is never looked at. And
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

	for (i = 0; i < ctx_count && count < TA_STATE_CONTEXTS; i++)
	{
		if (ctx_list[i] == ta_ctx)
			current = count;
		count++;
	}
	LIBRETRO_S(count);
	LIBRETRO_S(current);

	for (i = 0; i < count; i++)
	{
		const TA_context *ctx = ctx_list[i];
		// (the one being written to has its pointers out, in ta_tad)
		const tad_context& tad = ctx == ta_ctx ? ta_tad : ctx->tad;
		const bool restarted = tad.thd_data == tad.thd_root;
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
		word = size | (restarted && size != 0 ? 1 : 0);

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
		if (ctx == NULL || (word & 30) != 0 || size > ctx->data_size)
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

		if (i == current)
			current_addr = ctx->Address;
	}

	if (current_addr != TACTX_NONE)
		SetCurrentTARC(current_addr);
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
