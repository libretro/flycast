/* Shadows in the per-triangle renderers: what both of them need to know
 * about where on the screen the modifier volumes and the polygons are.
 *
 * Inside a modifier volume the PowerVR2 scales what a polygon is shaded
 * with - its base and offset colours - and then combines them with the
 * texture and fogs the result as usual. Darkening the finished pixel, which
 * is all these renderers used to do, is not the same thing: it also darkens
 * a decal texture, which the hardware leaves alone, and the fog.
 *
 * So once the volumes are in the stencil buffer, the polygons that take
 * shadows are drawn a second time, with a shader that scales the two
 * colours, into the pixels that are theirs (same depth) and in a volume
 * (stencil). Only the polygons that reach into the part of the screen the
 * volumes cover are drawn again, and only that part of them: nothing
 * outside it can be in a volume. The pixels drawn this way are marked, in
 * stencil bit 6, and what is not marked is darkened the old way after:
 * polygons that are blended, which cannot be drawn twice, or that left no
 * depth to be found by. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cmath>
#include <algorithm>
#include "xform.h"

#include "types.h"
#include "hw/pvr/Renderer_if.h"

// The part of the screen something covers, in the coordinates of the vertices
struct ScreenBounds
{
	float min_x, min_y, max_x, max_y;

	void add(float x, float y)
	{
		// written so that a coordinate that is not a number changes nothing
		if (x < min_x) min_x = x;
		if (x > max_x) max_x = x;
		if (y < min_y) min_y = y;
		if (y > max_y) max_y = y;
	}
	bool empty() const
	{
		return !(min_x <= max_x && min_y <= max_y);
	}
	bool overlaps(const ScreenBounds& o) const
	{
		return min_x <= o.max_x && max_x >= o.min_x && min_y <= o.max_y && max_y >= o.min_y;
	}
};

// What the modifier volumes of a render pass cover
static inline ScreenBounds ModVolBounds(int first, int count)
{
	ScreenBounds b = { 1e30f, 1e30f, -1e30f, -1e30f };
	const ModifierVolumeParam *params = &pvrrc.global_param_mvo.head()[first];

	for (int i = 0; i < count; i++)
	{
		const ModTriangle *t = &pvrrc.modtrig.head()[params[i].first];
		for (u32 n = params[i].count; n != 0; n--, t++)
		{
			b.add(t->x0, t->y0);
			b.add(t->x1, t->y1);
			b.add(t->x2, t->y2);
		}
	}
	return b;
}

static inline bool PolyOverlaps(const PolyParam *gp, const ScreenBounds& area)
{
	ScreenBounds b = { 1e30f, 1e30f, -1e30f, -1e30f };
	const u32 *idx = &pvrrc.idx.head()[gp->first];
	const Vertex *verts = pvrrc.verts.head();

	for (u32 n = gp->count; n != 0; n--, idx++)
		b.add(verts[*idx].x, verts[*idx].y);
	return b.overlaps(area);
}

// Bounds to a scissor rectangle (x, y, width, height), a pixel wider all round
static inline void BoundsToScissor(const ScreenBounds& b, const xform& viewport, int *rect)
{
	// huge coordinates are common in volumes; keep them where rounding is defined
	float sx = std::min(std::max(b.min_x - 1.f, -32768.f), 32768.f);
	float sy = std::min(std::max(b.min_y - 1.f, -32768.f), 32768.f);
	float ex = std::min(std::max(b.max_x + 1.f, -32768.f), 32768.f);
	float ey = std::min(std::max(b.max_y + 1.f, -32768.f), 32768.f);

	if (!pvrrc.isRTT)
	{
		sx = xform_x(&viewport, sx);
		sy = xform_y(&viewport, sy);
		ex = xform_x(&viewport, ex);
		ey = xform_y(&viewport, ey);
	}
	else
	{
		sx *= settings.rend.RenderToTextureUpscale;
		sy *= settings.rend.RenderToTextureUpscale;
		ex *= settings.rend.RenderToTextureUpscale;
		ey *= settings.rend.RenderToTextureUpscale;
	}
	const int x0 = std::max(0, (int)floorf(std::min(sx, ex)));
	const int y0 = std::max(0, (int)floorf(std::min(sy, ey)));
	const int x1 = std::max(x0, (int)ceilf(std::max(sx, ex)));
	const int y1 = std::max(y0, (int)ceilf(std::max(sy, ey)));
	rect[0] = x0;
	rect[1] = y0;
	rect[2] = x1 - x0;
	rect[3] = y1 - y0;
}

// Whether a polygon that takes shadows can be drawn a second time for them
static inline bool CanDrawShadowed(u32 listType, const PolyParam *gp)
{
	if (gp->count <= 2 || !gp->pcw.Shadow)
		return false;
	// blended with what was there before: cannot be drawn twice
	if (gp->tsp.SrcInstr != 1 || gp->tsp.DstInstr != 0)
		return false;
	// left no depth of its own to be found by
	if (listType == ListType_Opaque && (gp->isp.ZWriteDis || gp->isp.DepthMode == 0))
		return false;
	return true;
}

/* Where two polygons are at the very same depth, the one that was left
 * showing the first time has to be the one left showing the second time:
 * the later one if its depth test lets equal depths through (equal, less or
 * equal, greater or equal, always; punch-through is always greater or
 * equal), the earlier one if not (less, greater, not equal). */
static inline bool ShadowLaterWins(u32 listType, const PolyParam *gp)
{
	return listType == ListType_Punch_Through || ((0xCC >> gp->isp.DepthMode) & 1);
}
