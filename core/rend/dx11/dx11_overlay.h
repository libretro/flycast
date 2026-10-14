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
#include <d3d11.h>
#include <d3dcompiler.h>
#include "types.h"
#include "comptr.h"

/* What goes over the picture: the VMUs' screens and the light guns'
 * crosshairs, where the core options put them (as the Vulkan renderers'
 * vulkan/vmu.cpp does it). */
class DX11Overlay
{
public:
	/* Before the frame: the textures of what has changed */
	void Prepare();
	/* Onto the target that is bound, @width by @height */
	void Draw(int width, int height);
	void Term();

private:
	struct Picture
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11ShaderResourceView> view;
	};
	bool Upload(Picture& picture, int width, int height, const u8 *rgba);

	Picture vmus[4];
	Picture crosshairs[4];
};
