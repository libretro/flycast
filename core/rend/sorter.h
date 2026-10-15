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
