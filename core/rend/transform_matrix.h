/*
    Created on: Oct 22, 2019

	Copyright 2019 flyinghead

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
#include "hw/pvr/ta_ctx.h"

extern float fb_scale_x, fb_scale_y;
extern int screen_width, screen_height;

template<bool invertY>
class TransformMatrix
{
public:
	TransformMatrix() = default;
	TransformMatrix(const rend_context& renderingContext)
	{
		CalcMatrices(&renderingContext);
	}

	bool IsClipped() const
	{
		return renderingContext->fb_X_CLIP.min != 0
				|| lroundf((renderingContext->fb_X_CLIP.max + 1) / scale_x) != 640L
				|| renderingContext->fb_Y_CLIP.min != 0
				|| lroundf((renderingContext->fb_Y_CLIP.max + 1) / scale_y) != 480L;
	}

	const xform& GetNormalMatrix() const {
		return normalMatrix;
	}
	const xform& GetScissorMatrix() const {
		return scissorMatrix;
	}
	const xform& GetViewportMatrix() const {
		return viewportMatrix;
	}

	float GetSidebarWidth() const {
		return sidebarWidth;
	}

	vec2f GetDreamcastViewport() const {
		return dcViewport;
	}

	void CalcMatrices(const rend_context *renderingContext)
	{
		this->renderingContext = renderingContext;

		GetFramebufferScaling(false, scale_x, scale_y);

		if (renderingContext->isRTT)
		{
			dcViewport.x = renderingContext->fb_X_CLIP.max - renderingContext->fb_X_CLIP.min + 1;
			dcViewport.y = renderingContext->fb_Y_CLIP.max - renderingContext->fb_Y_CLIP.min + 1;
			xform_set(&normalMatrix, 2.0f / dcViewport.x, 2.0f / dcViewport.y, 1.f, -1.f, -1.f, 0.f);
			scissorMatrix = normalMatrix;
			sidebarWidth = 0;
		}
		else
		{
			dcViewport.x = 640.f * scale_x;
			dcViewport.y = 480.f * scale_y;

			float startx = 0;
			float starty = 0;
#if 0
			const bool vga = FB_R_CTRL.vclk_div == 1;
			switch (SPG_LOAD.hcount)
			{
				case 857: // NTSC, VGA
					startx = VO_STARTX.HStart - (vga ? 0xa8 : 0xa4);
					break;
				case 863: // PAL
					startx = VO_STARTX.HStart - 0xae;
					break;
				default:
					INFO_LOG(PVR, "unknown video mode: hcount %d", SPG_LOAD.hcount);
					break;
			}
			switch (SPG_LOAD.vcount)
			{
				case 524: // NTSC, VGA
					starty = VO_STARTY.VStart_field1 - (vga ? 0x28 : 0x12);
					break;
				case 262: // NTSC 240p
					starty = VO_STARTY.VStart_field1 - 0x11;
					break;
				case 624: // PAL
					starty = VO_STARTY.VStart_field1 - 0x2d;
					break;
				case 312: // PAL 240p
					starty = VO_STARTY.VStart_field1 - 0x2e;
					break;
				default:
					INFO_LOG(PVR, "unknown video mode: vcount %d", SPG_LOAD.vcount);
					break;
			}
			// some heuristic...
			startx *= 0.8;
			starty *= 1.1;
#endif
			xform_set(&normalMatrix, 1.f, 1.f, 1.f, startx, starty, 0.f);
			scissorMatrix = normalMatrix;

			float scissoring_scale_x, scissoring_scale_y;
			GetFramebufferScaling(true, scissoring_scale_x, scissoring_scale_y);

			float dc2s_scale_h = screen_height / 480.0f;
			sidebarWidth =  (screen_width - dc2s_scale_h * 640.0f) / 2;
			float x_coef = 2.0f / (screen_width / dc2s_scale_h * scale_x);
			float y_coef = 2.0f / dcViewport.y * (invertY ? -1 : 1);
			// scaled, then moved to where the picture starts
			xform to_clip;
			xform_set(&to_clip, x_coef, y_coef, 1.f,
					-1 + 2 * sidebarWidth / screen_width, invertY ? 1.f : -1.f, 0.f);
			xform_mul(&normalMatrix, &to_clip, &normalMatrix);
			xform_set(&to_clip, x_coef * scissoring_scale_x, y_coef * scissoring_scale_y, 1.f,
					-1 + 2 * sidebarWidth / screen_width, invertY ? 1.f : -1.f, 0.f);
			xform_mul(&scissorMatrix, &to_clip, &scissorMatrix);
		}
		xform depth;
		xform_set(&depth, 1.f, 1.f, 1 / settings.rend.ExtraDepthScale, 0.f, 0.f, 0.f);
		xform_mul(&normalMatrix, &depth, &normalMatrix);

		// clip space to pixels: moved by 1, then scaled by half the size
		xform vp_trans, half;
		xform_set(&vp_trans, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f);
		if (renderingContext->isRTT)
			xform_set(&half, dcViewport.x / 2, dcViewport.y / 2, 1.f, 0.f, 0.f, 0.f);
		else
			xform_set(&half, screen_width / 2, screen_height / 2, 1.f, 0.f, 0.f, 0.f);
		xform_mul(&vp_trans, &half, &vp_trans);
		xform_mul(&viewportMatrix, &vp_trans, &normalMatrix);
		xform_mul(&scissorMatrix, &vp_trans, &scissorMatrix);
	}

private:
	void GetFramebufferScaling(bool scissor, float& scale_x, float& scale_y)
	{
		scale_x = 1.f;
		scale_y = 1.f;

		if (!renderingContext->isRTT && !renderingContext->isRenderFramebuffer)
		{
			if (!scissor)
			{
				scale_x = fb_scale_x;
				scale_y = fb_scale_y;
			}
			if (SCALER_CTL.vscalefactor > 0x400)
			{
				// Interlace mode A (single framebuffer)
				if (SCALER_CTL.interlace == 0 && !scissor)
					scale_y *= roundf((float)SCALER_CTL.vscalefactor / 0x400);
				else if (SCALER_CTL.interlace == 1 && scissor)
					// Interlace mode B (alternating framebuffers)
					scale_y *= roundf((float)SCALER_CTL.vscalefactor / 0x400);
			}

			// VO pixel doubling is done after fb rendering/clipping
			// so it should be used for scissoring as well
			if (VO_CONTROL.pixel_double && !scissor)
				scale_x *= 0.5f;

			// the X Scaler halves the horizontal resolution but
			// before clipping/scissoring
			if (SCALER_CTL.hscale)
				scale_x *= 2.f;
		}
	}

	const rend_context *renderingContext = nullptr;

	xform normalMatrix = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
	xform scissorMatrix = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
	xform viewportMatrix = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
	vec2f dcViewport = { 0.f, 0.f };
	float scale_x = 0;
	float scale_y = 0;
	float sidebarWidth = 0;
};
