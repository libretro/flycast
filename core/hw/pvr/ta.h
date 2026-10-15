#pragma once
#include "types.h"
#include "ta_structs.h"

enum
{
   TA_PIXEL_1555 = 0,
   TA_PIXEL_565,
   TA_PIXEL_4444,
   TA_PIXEL_YUV422,
   TA_PIXEL_BUMPMAP,
   TA_PIXEL_4BPP,
   TA_PIXEL_8BPP,
   TA_PIXEL_RESERVED
};

enum
{
   TA_PAL_ARGB1555 = 0,
   TA_PAL_RGB565,
   TA_PAL_ARGB4444,
   TA_PAL_ARGB8888
};

struct TA_context;

void ta_vtx_ListCont();
void ta_vtx_ListInit();
void ta_vtx_SoftReset();
void ta_vtx_Reset();

void DYNACALL ta_vtx_data32(void* data);
void ta_vtx_data(u32* data, u32 size);
u8 *ta_vtx_room(u32 blocks);
// The list that is open (0-4), or -1
int ta_vtx_list(void);
// The second half of a 64-byte parameter is what comes next
bool ta_vtx_half(void);

bool ta_parse_vdrc(TA_context* ctx);
/* Whether the frames parsed from here on mark their flat strips (every
 * vertex at the same depth) as drawn at a single rate, as sprites and the
 * background always are. Off, the decoder does no more than without it. */
void ta_set_flat_strips(bool on);
/* Whether the frames parsed from here on keep the region array's table of
 * tiles (rend_context's region_tiles). Off, the array is walked as it was
 * without it. */
void ta_set_region_table(bool on);

/* The control-word walk (GPU-side TA processing, its CPU side). On, every
 * block the tile accelerator is sent that is not a vertex has its place
 * noted in its frame's table as it comes in, one store each; off (the
 * default) the blocks go through just as they did without it. Taken up
 * at the next list init. (From any thread.) */
void ta_set_walk(bool on);
/* A frame's data came back from a state: its table made again from it */
void ta_walk_rebuild(TA_context *ctx);

class TaTypeLut
{
public:
	static const TaTypeLut& instance() {
		static TaTypeLut _instance;
		return _instance;
	}
	u32 table[256];

private:
	TaTypeLut();
};

#define STRIPS_AS_PPARAMS 1

#include "ta_ctx.h"
