/*
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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "dx12context_lr.h"

/* What goes over the picture: the VMUs' screens and the light guns'
 * crosshairs, where the core options put them (as the Vulkan renderers'
 * vulkan/vmu.cpp does it). The eight pictures are made when the renderer
 * starts, and written through the upload ring when they change. */
class DX12Overlay
{
public:
	bool Init();
	/* Before the frame: the pictures of what has changed */
	void Prepare();
	/* Onto the target that is bound, @width by @height */
	void Draw(int width, int height);
	/* Nothing to draw */
	bool IsEmpty() const
	{
		for (int i = 0; i < 4; i++)
			if (vmus[i].shown || crosshairs[i].shown)
				return false;
		return true;
	}
	void Term();

private:
	struct Picture
	{
		DX12Image image;
		bool shown = false;
	};
	bool Make(Picture& picture, u32 width, u32 height);
	bool Upload(Picture& picture, int width, int height, const u8 *rgba);

	Picture vmus[4];
	Picture crosshairs[4];
};
