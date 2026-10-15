/*
	Copyright 2021 flyinghead

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
#include <d3d9.h>
#include "types.h"
#include "rend/dx11/comptr.h"

/* What goes over the picture: the VMUs' screens and the light guns'
 * crosshairs, where the core options put them (as the Vulkan renderers'
 * vulkan/vmu.cpp does it). */
class DX9Overlay
{
public:
	/* Before the frame: the textures of what has changed */
	void Prepare();
	/* Onto the target that is bound, @width by @height */
	void Draw(int width, int height);
	/* Nothing to draw */
	bool IsEmpty() const
	{
		for (int i = 0; i < 4; i++)
			if (vmus[i] || crosshairs[i])
				return false;
		return true;
	}
	void Term();

private:
	/* (B G R A, as the card reads it) */
	bool Upload(ComPtr<IDirect3DTexture9>& texture, int width, int height, const u8 *bgra);

	ComPtr<IDirect3DTexture9> vmus[4];
	ComPtr<IDirect3DTexture9> crosshairs[4];
};
