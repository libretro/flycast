#include "ta.h"
#include "ta_ctx.h"
#include "hw/holly/holly_intc.h"
#include "lockfree.h"

/*
	Threaded TA Implementation

	Main thread -> ta data -> stored	(tactx)

	Render/TA thread -> ta data -> draw lists -> draw
*/

#if HOST_CPU == CPU_X86
#include <xmmintrin.h>
struct simd256_t
{
   DECL_ALIGN(32) __m128 data[2];
};
#elif HOST_CPU == CPU_ARM && defined(__ARM_NEON__)
#include <arm_neon.h>
struct simd256_t
{
	DECL_ALIGN(32) uint64x2_t data[2];
};
#else
struct simd256_t
{
   DECL_ALIGN(32) u64 data[4];
};
#endif


/*
	Partial TA parsing for in emu-side handling. Properly tracks 32/64 byte state, and
	Calls a helper function every time something important (SOL/EOL, etc) happens

	Uses a state machine, with 3 bits state and 8 bits (PT:OBJ[6:2]) input
*/

enum ta_state
{
	               // -> TAS_NS, TAS_PLV32, TAS_PLHV32, TAS_PLV64, TAS_PLHV64, TAS_MLV64
	TAS_NS,        //

	               // -> TAS_NS, TAS_PLV32, TAS_PLHV32, TAS_PLV64, TAS_PLHV64
	TAS_PLV32,     //polygon list PMV<?>, V32
	
	               // -> TAS_NS, TAS_PLV32, TAS_PLHV32, TAS_PLV64, TAS_PLHV64
	TAS_PLV64,     //polygon list PMV<?>, V64

	               // -> TAS_NS, TAS_MLV64, TAS_MLV64_H
	TAS_MLV64,     //mv list

	               // -> TAS_PLV32
	TAS_PLHV32,    //polygon list PMV<64> 2nd half -> V32

	               // -> TAS_PLV64
	TAS_PLHV64,    //polygon list PMV<64> 2nd half -> V64

	               // -> TAS_PLV64_H
	TAS_PLV64_H,   //polygon list V64 2nd half


	               // -> TAS_MLV64
	TAS_MLV64_H    //mv list, 64 bit half
};

/* state | PTEOS | OBJ -> next, proc*/

#define ta_cur_state  (ta_fsm[2048])

u8 ta_fsm[2049];	//[2048] stores the current state
u32 ta_fsm_cl=7;

static void fill_fsm(ta_state st, s8 pt, s8 obj, ta_state next, u32 proc=0, u32 sz64=0)
{
   unsigned i, j;
	for (i=0;i<8;i++)
	{
		if (pt != -1) i=pt;

		for (j=0;j<32;j++)
		{
			if (obj != -1)
            j=obj;
			ta_fsm[(st<<8)+(i<<5)+j]=next | proc*16 /*| sz64*32*/;
			if (obj != -1)
            break;
		}

		if (pt != -1)
         break;
	}
}

static void fill_fsm(void)
{
   unsigned i;
	//initialise to invalid
	for (i=0;i<2048;i++)
		ta_fsm[i]=(i>>8) | 0x80;

	for (i=0;i<8;i++)
	{
		switch(i)
      {
         case ParamType_End_Of_List:
            {
               //End of list -> process it !
               fill_fsm(TAS_NS,   ParamType_End_Of_List,-1,TAS_NS,1);
               fill_fsm(TAS_PLV32,ParamType_End_Of_List,-1,TAS_NS,1);
               fill_fsm(TAS_PLV64,ParamType_End_Of_List,-1,TAS_NS,1);
               fill_fsm(TAS_MLV64,ParamType_End_Of_List,-1,TAS_NS,1);
            }
            break;

         case ParamType_User_Tile_Clip:
         case ParamType_Object_List_Set:
            {
               //32B commands, no state change
               fill_fsm(TAS_NS,i,-1,TAS_NS);
               fill_fsm(TAS_PLV32,i,-1,TAS_PLV32);
               fill_fsm(TAS_PLV64,i,-1,TAS_PLV64);
               fill_fsm(TAS_MLV64,i,-1,TAS_MLV64);
            }
            break;

         case 3:
         case 6:
            //invalid
            break;

         case ParamType_Polygon_or_Modifier_Volume:
            {
               //right .. its complicated alirte

               for (int k=0;k<32;k++)
               {
                  u32 uid = TaTypeLut::instance().table[k * 4];
                  u32 vt=uid & 0x7f;

                  bool v64 = vt == 5 || vt == 6 || vt == 11 || vt == 12 || vt == 13 || vt == 14;
                  bool p64 = uid >> 31;

                  ta_state nxt = p64 ? (v64 ? TAS_PLHV64 : TAS_PLHV32) :
                     (v64 ? TAS_PLV64  : TAS_PLV32 ) ;

                  fill_fsm(TAS_PLV32,i,k,nxt,0,p64);
                  fill_fsm(TAS_PLV64,i,k,nxt,0,p64);
               }


               //32B command, no state change
               fill_fsm(TAS_MLV64,i,-1,TAS_MLV64);

               //process and start list
               fill_fsm(TAS_NS,i,-1,TAS_NS,1);
            }
            break;

         case ParamType_Sprite:
            {
               //SPR: 32B -> expect 64B data (PL*)
               fill_fsm(TAS_PLV32,i,-1,TAS_PLV64);
               fill_fsm(TAS_PLV64,i,-1,TAS_PLV64);

               //invalid for ML

               //process and start list
               fill_fsm(TAS_NS,i,-1,TAS_NS,1);
            }
            break;

         case ParamType_Vertex_Parameter:
            {
               //VTX: 32 B -> Expect more of it
               fill_fsm(TAS_PLV32,i,-1,TAS_PLV32,0,0);

               //VTX: 64 B -> Expect next 32B
               fill_fsm(TAS_PLV64,i,-1,TAS_PLV64_H,0,1);

               //MVO: 64B -> expect next 32B
               fill_fsm(TAS_MLV64,i,-1,TAS_MLV64_H,0,1);

               //invalid for NS
            }
            break;
      }
	}
	//?

	fill_fsm(TAS_PLHV32,-1,-1,TAS_PLV32);  //64 PH -> expect V32
	fill_fsm(TAS_PLHV64,-1,-1,TAS_PLV64);  //64 PH -> expect V64

	fill_fsm(TAS_PLV64_H,-1,-1,TAS_PLV64); //64 VH -> expect V64
	fill_fsm(TAS_MLV64_H,-1,-1,TAS_MLV64); //64 MH -> expect M64
}

/* The control-word walk (ta_set_walk()). While it is on, every entry of
 * ta_fsm for a block that is not a vertex - any parameter but a vertex,
 * in a state that does not expect the second half of one - has this bit
 * as well: the block is then handed to ta_handle_cmd() by the code that
 * steps the state, which notes where the block is in the frame's table
 * of them (TA_context::walk) and goes on as without it. Vertices, and
 * everything while the walk is off, take the very same way as ever: the
 * table holds no such bit then, and nothing else is looked at. */
#define TA_FSM_WALK 0x40

/* Whether ta_fsm has the bit (the emulation thread's), and whether it is
 * wanted (from any thread: it is put in or taken out at the next list
 * init, on the emulation thread, between two blocks) */
static bool ta_walk_on;
static retro_atomic_int_t ta_walk_wanted = RETRO_ATOMIC_INT_INITIALIZER(0);

/* The bit put in or taken out, the table of every frame being written
 * forgotten: what has been sent to them so far was not walked, or walked
 * without what follows */
static void ta_walk_apply(bool on)
{
	u32 i;
	for (i = 0; i < 2048; i++)
	{
		u8 e = ta_fsm[i] & ~TA_FSM_WALK;
		if (on && (i >> 8) <= TAS_MLV64 && ((i >> 5) & 7) != ParamType_Vertex_Parameter)
			e |= TA_FSM_WALK;
		ta_fsm[i] = e;
	}
	ta_walk_on = on;
	tactx_walk_forget();
}

void ta_set_walk(bool on)
{
	retro_atomic_store_release_int(&ta_walk_wanted, on ? 1 : 0);
}

/* The block before ta_tad.thd_data goes in the frame's table: one store.
 * A table that is full is not to be trusted for the rest of the frame,
 * which is then parsed as it always is. */
static NOINLINE u32 ta_walk_note(u32 trans)
{
	TA_context *ctx = ta_ctx;
	u32 n = ctx->walk_count;

	if (ctx->walk_restart)
	{
		ctx->walk_restart = false;
		n = 0;
	}
	if (n < ctx->walk_size)
		ctx->walk[n++] = (u32)(ta_tad.thd_data - 32 - ta_tad.thd_root) / 32;
	else
		n = TA_WALK_INVALID;
	ctx->walk_count = n;
	return trans & ~TA_FSM_WALK;
}

static const HollyInterruptID ListEndInterrupt[5]=
{
	holly_OPAQUE,
	holly_OPAQUEMOD,
	holly_TRANS,
	holly_TRANSMOD,
	holly_PUNCHTHRU
};


static NOINLINE void DYNACALL ta_handle_cmd(u32 trans)
{
   Ta_Dma* dat=(Ta_Dma*)(ta_tad.thd_data-32);

   if (trans & TA_FSM_WALK)
   {
      trans = ta_walk_note(trans);
      if ((trans & 0xF0) == 0)
      {
         // nothing else to do for it
         ta_cur_state = (ta_state)trans;
         return;
      }
   }

   u32 cmd = trans>>4;
   trans&=7;
   //printf("Process state transition: %d || %d -> %d \n",cmd,state_in,trans&0xF);

   if (cmd != 8)
   {
      if (dat->pcw.ParaType == ParamType_End_Of_List)
      {
         if (ta_fsm_cl==7)
            ta_fsm_cl=dat->pcw.ListType;
         //printf("List %d ended\n",ta_fsm_cl);
         /* There are five lists and an interrupt for each. A list type of
          * 5 to 7 is none of them: the list ends and nothing is raised. */
         if (ta_fsm_cl < 5)
            asic_RaiseInterruptBothCLX(ListEndInterrupt[ta_fsm_cl]);
         ta_fsm_cl=7;
         trans=TAS_NS;
      }
      else if (dat->pcw.ParaType == ParamType_Polygon_or_Modifier_Volume)
      {
         if (ta_fsm_cl==7)
            ta_fsm_cl=dat->pcw.ListType;

         if (!IsModVolList(ta_fsm_cl))
				trans=TAS_PLV32;
			else
				trans=TAS_MLV64;
      }
      else if (dat->pcw.ParaType == ParamType_Sprite)
      {
         if (ta_fsm_cl==7)
				ta_fsm_cl=dat->pcw.ListType;

			//verify(!IsModVolList(ta_fsm_cl));	// fails with "F1 World Grand Prix for Dreamcast" and only with dynarec...
			trans=TAS_PLV32;
      }
   }

   u32 state_in = (trans<<8) | (dat->pcw.ParaType<<5) | (dat->pcw.obj_ctrl>>2)%32;
   ta_cur_state = (ta_state)(ta_fsm[state_in]&0xF);
   verify(ta_cur_state<=7);
}

static OnLoad ol_fillfsm(&fill_fsm);

void ta_vtx_ListCont(void)
{
   SetCurrentTARC(TA_CURRENT_CTX);
   ta_tad.Continue();
   // (where this pass's object lists go: see ta_select_passes())
   ta_tad.ol_base[ta_tad.render_pass_count] = TA_OL_BASE & 0xFFFFE0u;

	ta_cur_state=TAS_NS;
	ta_fsm_cl = 7;
}

void ta_vtx_ListInit(void)
{
	if ((retro_atomic_load_relaxed_int(&ta_walk_wanted) != 0) != ta_walk_on)
		ta_walk_apply(!ta_walk_on);
	SetCurrentTARC(TA_CURRENT_CTX);
	if (ta_ctx && ta_ctx->drawn)
		tactx_Restart(ta_ctx);
   ta_tad.ClearPartial();
   ta_tad.ol_base[0] = TA_OL_BASE & 0xFFFFE0u;
	/* (the data from here on is a new frame's: the table of the old one is
	 * kept for as long as it is what a render draws) */
	if (ta_ctx)
		ta_ctx->walk_restart = true;

	ta_cur_state=TAS_NS;
	ta_fsm_cl = 7;
}
void ta_vtx_SoftReset(void)
{
	ta_cur_state=TAS_NS;
}

// The machine is reset: no list is open and no parameter is half sent
void ta_vtx_Reset(void)
{
	ta_cur_state = TAS_NS;
	ta_fsm_cl    = 7;
}

int ta_vtx_list(void)
{
	return ta_fsm_cl == 7 ? -1 : (int)ta_fsm_cl;
}

bool ta_vtx_half(void)
{
	return ta_cur_state >= TAS_PLHV32;
}

/* A block the careful way: with no list begun, at the very start of a
 * frame's data, or with the buffer full. */
static NOINLINE
void DYNACALL ta_data32_slow(void* data)
{
   if (ta_ctx == NULL)
   {
   	INFO_LOG(PVR, "Warning: data sent to TA prior to ListInit. Ignored");
		return;
   }
	/* Where the next block goes. (Not End(): after the lists are started
	 * again the next block goes to the start of the buffer, whatever the
	 * frame before left in it, as the hardware's write pointer goes back to
	 * the base.) */
	if ((u32)(ta_tad.thd_data - ta_tad.thd_root) >= ta_ctx->data_size)
	{
		INFO_LOG(PVR, "Warning: TA data buffer overflow");
		asic_RaiseInterrupt(holly_MATR_NOMEM);
		return;
	}

   simd256_t *dst = (simd256_t*)ta_tad.thd_data;
   simd256_t *src = (simd256_t*)data; 

   /* The first block since the lists were started again over data that
    * was never drawn: the places where lists were continued are places in
    * that data, and not in this. (Left, the new data was cut into passes
    * at them, and the first pass ran on into what was left of the old.) */
   if (ta_tad.thd_data == ta_tad.thd_root && ta_tad.thd_old_data != ta_tad.thd_root)
      ta_tad.render_pass_count = 0;

   // First byte is PCW
   PCW pcw        = *(PCW*)data;

   /* Copy the TA data */
   if (dst != src)
      *dst = *src;

   ta_tad.thd_data+=32;

   /* Process TA state */
   u32 state_in     = (ta_cur_state<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32;

   u32 trans         = ta_fsm[state_in];
   ta_cur_state     = (ta_state)trans;
   bool must_handle = trans& 0xF0;

   if (must_handle)
      ta_handle_cmd(trans);
}

/* A block from a store queue. With data already in the buffer and room
 * for more - one comparison says both - it is copied and the state moved
 * on here; anything else is ta_data32_slow()'s. */
void DYNACALL ta_vtx_data32(void* data)
{
   TA_context *ctx = ta_ctx;
   u8 *to          = ta_tad.thd_data;

   if (ctx == NULL || (u32)(to - ta_tad.thd_root) - 32 >= ctx->data_size - 32)
   {
      ta_data32_slow(data);
      return;
   }

   PCW pcw = *(PCW*)data;

   *(simd256_t*)to = *(simd256_t*)data;
   ta_tad.thd_data = to + 32;

   u32 trans    = ta_fsm[(ta_cur_state<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32];
   ta_cur_state = (ta_state)trans;
   if (trans & 0xF0)
      ta_handle_cmd(trans);
}

/* Where the next block will be put, if @blocks more can be: something that
 * makes blocks can make them there, and they are then taken by
 * ta_vtx_data() without being copied. NULL: they cannot, or not there. */
u8 *ta_vtx_room(u32 blocks)
{
   TA_context *ctx = ta_ctx;
   u32 used;

   if (ctx == NULL)
      return NULL;
   used = (u32)(ta_tad.thd_data - ta_tad.thd_root);
   if (used > ctx->data_size || blocks > (ctx->data_size - used) / 32)
      return NULL;
   return ta_tad.thd_data;
}

/* The state machine's step for a block that has to be handled, as
 * ta_handle_cmd() takes it but with nothing raised: for walking data
 * again. @cl is the list that is open (ta_fsm_cl). */
static u32 ta_walk_cmd(u32 trans, PCW pcw, u32 *cl)
{
   u32 cmd = trans >> 4;

   trans &= 7;
   if (cmd != 8)
   {
      if (pcw.ParaType == ParamType_End_Of_List)
      {
         *cl  = 7;
         trans = TAS_NS;
      }
      else if (pcw.ParaType == ParamType_Polygon_or_Modifier_Volume)
      {
         if (*cl == 7)
            *cl = pcw.ListType;
         trans = IsModVolList(*cl) ? TAS_MLV64 : TAS_PLV32;
      }
      else if (pcw.ParaType == ParamType_Sprite)
      {
         if (*cl == 7)
            *cl = pcw.ListType;
         trans = TAS_PLV32;
      }
   }
   return ta_fsm[(trans<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32] & 0xF;
}

/* A frame's data that did not come in through here - a state was loaded -
 * walked again, for its table: from the start, with the state machine
 * starting over where the lists were started and continued. (The data
 * goes as far as it was sent; what the tile accelerator was in the
 * middle of is in the state, and the table goes on from there as the
 * next blocks come.) Only while the walk is on. */
void ta_walk_rebuild(TA_context *ctx)
{
   const tad_context &tad = ctx->tad;
   const u8 *p, *end;
   u32 n = 0, state = TAS_NS, cl = 7, pass = 0;

   ctx->walk_count   = TA_WALK_INVALID;
   ctx->walk_restart = false;
   if (!ta_walk_on)
      return;
   end = tad.End();
   for (p = tad.thd_root; p < end; p += 32)
   {
      PCW pcw;
      u32 trans;

      while (pass < tad.render_pass_count && p >= tad.render_passes[pass])
      {
         // ta_vtx_ListCont()
         state = TAS_NS;
         cl    = 7;
         pass++;
      }
      pcw   = *(const PCW *)p;
      trans = ta_fsm[(state<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32];
      if (trans & TA_FSM_WALK)
      {
         if (n == ctx->walk_size)
            return;
         ctx->walk[n++] = (u32)(p - tad.thd_root) / 32;
         trans &= ~TA_FSM_WALK;
      }
      state = (trans & 0xF0) ? ta_walk_cmd(trans, pcw, &cl) : trans;
   }
   ctx->walk_count   = n;
   ctx->walk_restart = tad.thd_data == tad.thd_root;
}

/* Many blocks at once: a DMA transfer, or the NAOMI 2's geometry processor.
 *
 * Where to put the next one and the state are kept in registers across a
 * run of them, and a run of vertices in a 32-byte format - most of what
 * there ever is - changes neither the state nor anything else, so it is
 * found by looking at one byte of each. Blocks made in the buffer itself
 * (ta_vtx_room()) are not copied at all. */
void ta_vtx_data(u32* data, u32 size)
{
	while (size > 0)
	{
		TA_context *ctx = ta_ctx;
		u8 *to          = ta_tad.thd_data;
		u8 *root        = ta_tad.thd_root;
		u32 used        = (u32)(to - root);
		u32 count;
		u32 state;
		const u8 *from, *stop;
		bool handled = false;

		if (ctx == NULL || used >= ctx->data_size)
		{
			ta_data32_slow(data);
			data += 8;
			size--;
			continue;
		}
		// (as in ta_data32_slow())
		if (used == 0 && ta_tad.thd_old_data != root)
			ta_tad.render_pass_count = 0;

		count = (ctx->data_size - used) / 32;
		if (count > size)
			count = size;
		state = ta_cur_state;
		from  = (const u8 *)data;
		stop  = from + count * 32;

		if (from == to)
		{
			/* made in place */
			while (from != stop)
			{
				PCW pcw;
				u32 trans;

				if (state == TAS_PLV32)
				{
					while (from != stop && ((const PCW *)from)->ParaType == ParamType_Vertex_Parameter)
						from += 32;
					if (from == stop)
						break;
				}
				pcw   = *(const PCW *)from;
				trans = ta_fsm[(state<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32];
				from += 32;
				state = trans;
				if (trans & 0xF0)
				{
					handled = true;
					break;
				}
			}
			to = (u8 *)from;
		}
		else
		{
			while (from != stop)
			{
				PCW pcw;
				u32 trans;

				if (state == TAS_PLV32)
				{
					while (from != stop && ((const PCW *)from)->ParaType == ParamType_Vertex_Parameter)
					{
						*(simd256_t *)to = *(const simd256_t *)from;
						to   += 32;
						from += 32;
					}
					if (from == stop)
						break;
				}
				pcw   = *(const PCW *)from;
				trans = ta_fsm[(state<<8) | (pcw.ParaType<<5) | (pcw.obj_ctrl>>2)%32];
				*(simd256_t *)to = *(const simd256_t *)from;
				to   += 32;
				from += 32;
				state = trans;
				if (trans & 0xF0)
				{
					handled = true;
					break;
				}
			}
		}

		ta_tad.thd_data = to;
		ta_cur_state    = (ta_state)state;
		if (handled)
			ta_handle_cmd(state);

		count = (u32)(from - (const u8 *)data) / 32;
		data += count * 8;
		size -= count;
	}
}
