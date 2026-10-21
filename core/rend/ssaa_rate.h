/* Which polygons SSAA shades once a pixel, in every raster renderer that
 * has it (Vulkan's, Direct3D 12's): the ones drawn at a single rate
 * (rend/tile/pvr_tile.h) - sprites, the background and, where the decoder
 * looks for them (ta_set_flat_strips()), flat strips. Those are shaded at
 * the pixel's centre, as with SSAA off, and a pixel all of whose samples
 * they cover resolves to exactly what SSAA off draws; every other polygon
 * is shaded at each sample. */
#pragma once
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/tile/pvr_tile.h"

/* Whether a polygon is drawn at a single rate. The bit means something for
 * a sprite and the background always, for other strips only while the
 * decoder looks for flat ones: it is what the game left there otherwise. */
static inline bool SsaaSingleRate(const PolyParam& pp)
{
	return (pp.pcw.full & PVR_PCW_SINGLE_RATE) != 0
			&& (ta_flat_strips() || pp.pcw.ParaType == ParamType_Sprite || &pp == pvrrc.global_param_op.head());
}
