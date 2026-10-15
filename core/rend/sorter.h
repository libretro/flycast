/*
	 This file is part of reicast.

	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.

	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include "hw/pvr/Renderer_if.h"

/* Whether two polygons are drawn the same way, so that GenSorted() makes
 * one draw of triangles of both that come one after the other */
static inline bool SortSameDraw(const PolyParam *pp0, const PolyParam *pp1)
{
	return (pp0->pcw.full & PCW_DRAW_MASK) == (pp1->pcw.full & PCW_DRAW_MASK) && pp0->isp.full == pp1->isp.full
			&& pp0->tcw.full == pp1->tcw.full && pp0->tsp.full == pp1->tsp.full && pp0->tileclip == pp1->tileclip;
}

//Sort based on min-z of each strip
void SortPParams(int first, int count);

struct SortTrigDrawParam
{
	const PolyParam* ppid;
	u32 first;
	u32 count;
};

// Sort based on min-z of each triangle.
// firstProvoking: the renderer takes a flat triangle's colour from its first
// vertex (Vulkan), where the strip's colours have been moved one triangle
// back; otherwise from its last (OpenGL).
void GenSorted(int first, int count, std::vector<SortTrigDrawParam>& pidx_sort, std::vector<u32>& vidx_sort,
		bool firstProvoking = false);

/* The depth prepass of a sorted translucent list (per triangle or per
 * strip). Sorting by triangle or by strip draws a polygon that is behind
 * another after it now and then - a long one reaching back to the horizon
 * sorts as far away - and as these do not write depth, it shows through.
 * The hardware sorts each pixel: a layer whose blending leaves nothing of
 * what is behind it (destination factor zero, or one minus the source
 * alpha with the alpha at 1) hides all that is behind it, whatever the
 * order. So before the colours are drawn, the depth of such pixels is
 * written - colour writes off, a pixel's alpha worked out as the colour
 * pass does and the pixel discarded unless it writes as 255 in 8 bits -
 * and the colour pass then draws nothing behind them. Nothing that was in
 * the right order changes. The depth is written in any order, so the
 * polygons are drawn grouped by what the prepass draws them with, not
 * sorted.
 *
 * What a polygon is drawn with in the prepass: everything that goes into
 * its alpha, its culling and its clipping. Two polygons that agree on all
 * of it are drawn in one draw. */
struct PrepassKey
{
	u64 texid;
	u32 tsp;	// the TSP word without the source factor, fog reduced to whether it is the alpha's (3)
	u32 tcw;
	u32 pcw;
	u32 isp;	// the culling
	u32 tileclip;

	bool operator==(const PrepassKey& o) const {
		return texid == o.texid && tsp == o.tsp && tcw == o.tcw && pcw == o.pcw && isp == o.isp && tileclip == o.tileclip;
	}
	bool operator<(const PrepassKey& o) const {
		if (texid != o.texid) return texid < o.texid;
		if (tsp != o.tsp) return tsp < o.tsp;
		if (tcw != o.tcw) return tcw < o.tcw;
		if (pcw != o.pcw) return pcw < o.pcw;
		if (isp != o.isp) return isp < o.isp;
		return tileclip < o.tileclip;
	}
};

/* Whether a render pass's sorted translucent list gets a prepass: always
 * in the last render pass; in one before it only where the depth of the
 * sorted polygons is written after them anyway for the next pass
 * (TranslucentPolygonDepthMask), and then only for the polygons whose depth
 * that writes - so a later pass never sees a depth it did not see before. */
static inline bool PrepassWanted(bool lastPass)
{
	return lastPass || settings.rend.TranslucentPolygonDepthMask;
}

/* Whether the prepass discards no pixel of the polygon: blended with a
 * destination factor of zero, every pixel hides what is behind it */
static inline bool PrepassAlwaysHides(const PolyParam *pp)
{
	return pp->tsp.DstInstr == 0;
}

/* Whether a polygon of the list can have pixels that hide what is behind
 * them, by its parameters: blended into the picture (not an accumulation
 * buffer) with a destination factor of zero, or of one minus the source
 * alpha where the alpha can reach 1 (not a trilinear pass that scales it
 * down); in a render pass before the last, only one whose depth is
 * written for the next pass (see PrepassWanted()) */
static inline bool PrepassCandidate(const PolyParam *pp, bool lastPass)
{
	if (pp->count <= 2 || pp->tsp.SrcSelect || pp->tsp.DstSelect || (!lastPass && pp->isp.ZWriteDis))
		return false;
	if (pp->tsp.DstInstr == 0)
		return true;
	if (pp->tsp.DstInstr != 5)
		return false;
	// (pass A's alpha is 1 - D / 4, pass B's D / 4 with D at most 3)
	if (pp->pcw.Texture && pp->tsp.FilterMode > 1 && pp->tcw.MipMapped
			&& (pp->tsp.FilterMode != 2 || (pp->tsp.MipMapD & 3) != 0))
		return false;
	return true;
}

static inline PrepassKey PrepassMakeKey(const PolyParam *pp, u64 texid)
{
	TSP tsp = pp->tsp;
	tsp.SrcInstr = 0;
	if (tsp.FogCtrl != 3)
		tsp.FogCtrl = 0;
	ISP_TSP isp;
	isp.full = 0;
	isp.CullMode = pp->isp.CullMode;
	const PrepassKey key = { pp->pcw.Texture ? texid : 0, tsp.full, pp->tcw.full, pp->pcw.full, isp.full, pp->tileclip };
	return key;
}

/* The prepass of render pass list [first, first + count) of the
 * translucent polygons: its draws (ppid: a polygon of the group, which says
 * how all of it is drawn; first and count: of @vidx, triangles) and their
 * triangles' indices, with the provoking vertex as GenSorted() has it.
 * @lastPass: see PrepassWanted(). Neither is cleared when nothing is drawn. */
void GenPrepass(int first, int count, bool lastPass, std::vector<SortTrigDrawParam>& draws, std::vector<u32>& vidx,
		bool firstProvoking = false);
/* (the alpha the prepass compares against: 254.5 / 255, the least that writes as 255) */
#define PREPASS_ALPHA_MIN 0.998039216f
