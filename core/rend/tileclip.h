/*
	Copyright 2020 flyinghead
	This file is part of Flycast.
    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include <cmath>
#include <algorithm>
#include "xform.h"

#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_regs.h"

enum class TileClipping {
	Inside,			// Render stuff outside the region
	Off,    		// Always passes
	Outside    		// Render stuff inside the region
};

// clip_rect[] will contain x, y, width, height
static inline TileClipping GetTileClip(u32 val, const xform& viewport, int *clip_rect)
{
	u32 clipmode = val >> 28;
	if (clipmode < 2)
		return TileClipping::Off;	//always passes

	TileClipping tileClippingMode;
	if (clipmode & 1)
		tileClippingMode = TileClipping::Inside;   //render stuff outside the region
	else
		tileClippingMode = TileClipping::Outside;  //render stuff inside the region

	float csx = (float)(val & 63);
	float cex = (float)((val >> 6) & 63);
	float csy = (float)((val >> 12) & 31);
	float cey = (float)((val >> 17) & 31);
	csx = csx * 32;
	cex = cex * 32 + 32;
	csy = csy * 32;
	cey = cey * 32 + 32;

	if (csx <= 0 && csy <= 0 && cex >= 640 && cey >= 480)
		return TileClipping::Off;

	if (!pvrrc.isRTT)
	{
		csx = xform_x(&viewport, csx);
		csy = xform_y(&viewport, csy);
		cey = xform_y(&viewport, cey);
		cex = xform_x(&viewport, cex);
	}
	else
	{
		// (across, by half that where the scaler halves what is drawn: transform_matrix.h)
		const float across = settings.rend.RenderToTextureUpscale / (SCALER_CTL.hscale ? 2.f : 1.f);
		csx *= across;
		csy *= settings.rend.RenderToTextureUpscale;
		cex *= across;
		cey *= settings.rend.RenderToTextureUpscale;
	}
	clip_rect[0] = std::max(0, (int)lroundf(csx));
	clip_rect[1] = std::max(0, (int)lroundf(std::min(csy, cey)));
	clip_rect[2] = std::max(0, (int)lroundf(cex - csx));
	clip_rect[3] = std::max(0, (int)lroundf(std::abs(cey - csy)));

	return tileClippingMode;
}
