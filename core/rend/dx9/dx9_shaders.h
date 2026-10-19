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
#include <d3dcompiler.h>
#include <vector>
#include "types.h"
#include "rend/dx11/comptr.h"

/* The Direct3D 9 renderer's shaders, shader model 3.
 *
 * Every one of them is compiled when the renderer is first set up, never
 * while a game runs: what a polygon's pixel shader would have been made
 * for in the other renderers (the shading instruction, the fog, the
 * offset colour, the clip...) is a flag in a constant here, which the
 * shader branches on - every pixel of a polygon the same way, which costs
 * the card next to nothing - so that three pixel shaders cover all
 * polygons: untextured, textured, and textured through the palette.
 *
 * What is compiled is kept: a new device after a lost one makes its
 * shaders from it again, without compiling anything. */
enum DX9PixelShader
{
	DX9PS_Plain,		// no texture
	DX9PS_Texture,
	DX9PS_Palette,		// a texture of palette indexes
	DX9PS_ModVol,		// the modifier volumes, and the depth alone
	DX9PS_Quad,			// a picture drawn whole
	DX9PS_Count
};

enum DX9VertexShader
{
	DX9VS_Main,
	DX9VS_ModVol,		// positions alone
	DX9VS_Quad,
	DX9VS_Count
};

/* The main pixel shader's flags: bit i is component i % 4 of constant
 * register c8 + i / 4, 0 or 1 */
enum DX9ShaderBool
{
	DX9B_UseAlpha,
	DX9B_IgnoreTexA,
	DX9B_Offset,
	DX9B_BumpMap,
	DX9B_ColorClamp,
	DX9B_TriLinear,
	DX9B_ClipInside,
	DX9B_AlphaTest,
	DX9B_Shadowed,		// drawn a second time where it is in a modifier volume: rend/shadows.h
	DX9B_ShadInstr0,	// the shading instruction, 0 to 3
	DX9B_ShadInstr1,
	DX9B_Fog0,			// the fog control, 0 to 3
	DX9B_Fog1,
	DX9B_SwapRB,		// a texture that holds R G B A bytes (and the card reads B G R A)
	DX9B_Count
};

/* The constant registers */
enum
{
	// vertex shaders
	DX9VC_NormalScale = 0,
	DX9VC_NormalOffset = 1,
	DX9VC_HalfPixel = 2,		// half a pixel of the viewport, in clip space
	DX9VC_DepthScale = 3,		// 1/fZ_max, 1 / the normal matrix's z scale (rend/depth_scale.h)
	// pixel shaders: the frame's, c0 to c4, and the polygon's, c5 and c6
	DX9PC_ColorClampMin = 0,
	DX9PC_ColorClampMax = 1,
	DX9PC_FogColRam = 2,
	DX9PC_FogColVert = 3,
	DX9PC_FrameParams = 4,		// alpha test value, fog density, shadow scale
	DX9PC_ClipTest = 5,
	DX9PC_PolyParams = 6,		// trilinear alpha, palette index
	DX9PC_Flags = 8,			// c8 to c11: DX9ShaderBool
	DX9PC_Quad = 7,				// the quad's colour multiplier
};

/* Samplers: the polygon's texture, the palette, the fog table */
enum { DX9S_Texture = 0, DX9S_Palette = 1, DX9S_Fog = 2 };

class DX9Shaders
{
public:
	/* Compiled, once for the life of the core; and made for @device */
	bool Init(IDirect3DDevice9 *device, pD3DCompile compile);
	/* The device's shaders released; what was compiled is kept */
	void Term();

	IDirect3DPixelShader9 *GetPS(DX9PixelShader which) const { return ps[which]; }
	IDirect3DVertexShader9 *GetVS(DX9VertexShader which) const { return vs[which]; }

private:
	bool Compile(pD3DCompile compile);
	ComPtr<IDirect3DPixelShader9> ps[DX9PS_Count];
	ComPtr<IDirect3DVertexShader9> vs[DX9VS_Count];
	std::vector<DWORD> psCode[DX9PS_Count];
	std::vector<DWORD> vsCode[DX9VS_Count];
	bool compiled = false;
};
