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
/* The render passes of @ctx the render being started draws (rend.pass_first,
 * pass_last, keep): those whose object lists its region array points to,
 * by where each pass's lists were written. A frame of one pass, or whose
 * passes cannot be told apart that way, is drawn whole, as it always was.
 * (Emulation thread, as the render is started.) */
void ta_select_passes(TA_context* ctx);
/* Whether the frames parsed from here on mark their flat strips (every
 * vertex at the same depth) as drawn at a single rate, as sprites and the
 * background always are. Off, the decoder does no more than without it. */
void ta_set_flat_strips(bool on);
// ...and whether they are looked for now: a strip's single-rate bit means something only then
bool ta_flat_strips(void);
/* Whether the frames parsed from here on keep the region array's table of
 * tiles (rend_context's region_tiles). Off, the array is walked as it was
 * without it. */
void ta_set_region_table(bool on);
/* Whether a frame's modifier volume triangles far off the screen are cut
 * for the raster renderers (rend_context's mv_clip: rend/modvol_clip.h) */
void ta_set_modvol_clip(bool on);

/* The control-word walk (GPU-side TA processing, its CPU side). On, every
 * block the tile accelerator is sent that is not a vertex has its place
 * noted in its frame's table as it comes in, one store each; off (the
 * default) the blocks go through just as they did without it. Taken up
 * at the next list init. (From any thread.) */
void ta_set_walk(bool on);
/* A frame's data came back from a state: its table made again from it */
void ta_walk_rebuild(TA_context *ctx);
/* The header pass: the frame's polygon parameters, texture lookups, render
 * passes, region settings and draw groups, from the walk's table and the
 * parameters alone, never a vertex. false: the frame has no table to be
 * trusted (or more than there is room for), and only the full parse can
 * do it. Its results are for the GPU decode to come; they do not change
 * the frame. (From the thread that renders.) */
bool ta_walk_parse(TA_context *ctx);
/* What it works in, made once for frames of the machine's size (before
 * the frames that use it: when the check is turned on), and given back;
 * false: no memory */
bool ta_walk_init(void);
void ta_walk_term(void);
/* Whether every frame is also put through the header pass and the two
 * compared, and both timed (logged). Turns the walk on and off with it.
 * (From the thread that renders, between frames.) */
void ta_set_walk_check(bool on);

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
