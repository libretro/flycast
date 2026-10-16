/* What the reference software renderer's files (refsw.h, refsw.cpp,
 * refsw_pixel.cpp: libswirl's rend/soft, BSD licence in license/bsd) took
 * from the rest of libswirl, given here from this core's headers instead,
 * so that those files compile as they are. Included by refsw.h in place of
 * libswirl's rend/gles/gles.h. See README.
 *
 * - libswirl's types.h has "using namespace std"; this core's does not
 * - the registers it reads as bitfield unions that this core reads as words
 *   (HALF_OFFSET, ISP_BACKGND_T)
 * - its ISP_TSP has a modvol view of the word (VolumeMode in the depth
 *   mode's bits, as the hardware has it for a modifier volume)
 * - text_info / raw_GetTexture: a texture made from video memory with this
 *   core's own texture cache, laid out as libswirl's software renderers
 *   want it (refsw_frame.cpp)
 * - cMutex: refsw.cpp takes a lock around the texture cache; one thread
 *   draws here, so it is nothing */
#pragma once
#define PVR_REGS_FOR_RENDERER
#include "types.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_structs.h"
#include "hw/pvr/ta_ctx.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <vector>
using namespace std;

struct Renderer;

union HALF_OFFSET_type
{
	struct
	{
		u32 fpu_pixel_half_offset : 1;
		u32 tsp_pixel_half_offset : 1;
		u32 texure_pixel_half_offset : 1;
	};
	u32 full;
};
#undef HALF_OFFSET
#define HALF_OFFSET PvrReg(HALF_OFFSET_addr, HALF_OFFSET_type)

union ISP_BACKGND_T_type
{
	struct
	{
		u32 tag_offset   : 3;
		u32 tag_address  : 21;
		u32 skip         : 3;
		u32 shadow       : 1;
		u32 cache_bypass : 1;
	};
	u32 full;
};
#undef ISP_BACKGND_T
#define ISP_BACKGND_T PvrReg(ISP_BACKGND_T_addr, ISP_BACKGND_T_type)

/* libswirl's ISP_TSP, which has the modifier volume view of the word */
union REFSW_ISP_TSP
{
	struct
	{
		u32 Reserved    : 20;
		u32 DCalcCtrl   : 1;
		u32 CacheBypass : 1;
		u32 UV_16b      : 1;
		u32 Gouraud     : 1;
		u32 Offset      : 1;
		u32 Texture     : 1;
		u32 ZWriteDis   : 1;
		u32 CullMode    : 2;
		u32 DepthMode   : 3;
	};
	struct
	{
		u32 res        : 27;
		u32 CullMode   : 2;
		u32 VolumeMode : 3;	// 0 normal polygon, 1 inside last, 2 outside last
	} modvol;
	u32 full;
};
#define ISP_TSP REFSW_ISP_TSP

/* A texture as libswirl's software renderers take it: for each texel, the
 * 2x2 block of texels at (u+1,v+1), (u,v+1), (u+1,v), (u,v) as four 32-bit
 * BGRA words, 16 bytes a texel (its bilinear filter reads the block). */
struct text_info
{
	u32 width;
	u32 height;
	u8 *pdata;
	u32 textype;
};

text_info raw_GetTexture(u8 *vram, TSP tsp, TCW tcw);

struct cMutex
{
	void Lock() {}
	void Unlock() {}
};
