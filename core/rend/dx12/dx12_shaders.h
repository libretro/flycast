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
/* The Direct3D 12 renderer's shaders: DXBC made when the core is built
 * (dx12_dxbc.inc, from shaders/dx12_raster.hlsl, by
 * tools/rend/kernels_dx12.sh), and the texture kernels the Direct3D 11
 * renderer has (rend/texdecode_dxbc.inc). Nothing is compiled when the
 * core runs, and it needs no compiler. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>

/* By their number in dx12_dxbc.inc */
enum class DX12Shader
{
	PolyVS = 1,			// smooth
	PolyFlatVS = 2,		// flat
	PolyPS = 3,
	PolyFlatPS = 4,
	ModVolVS = 5,
	ModVolPS = 6,
	QuadVS = 7,
	QuadPS = 8,
	// the per-pixel renderer's
	OITVS = 9,
	OITFlatVS = 10,
	OITDepthPS = 11,		// DX12_PASS_DEPTH; smooth, then flat; then the colour pass's, then the lists'
	OITDepthFlatPS = 12,
	OITColorPS = 13,
	OITColorFlatPS = 14,
	OITListPS = 15,
	OITListFlatPS = 16,
	OITFinalVS = 17,
	OITFinalPS = 18,
	OITModVolPS = 19,
	/* SSAA's (DX12_SSAA): each of these four is smooth at each sample, flat
	 * at each sample, smooth once a pixel, flat once a pixel */
	SsaaPolyPS = 20,
	SsaaOITDepthPS = 24,
	SsaaOITColorPS = 28,
	SsaaOITListPS = 32,			// once a pixel: smooth, flat
	SsaaOITFinalPS = 34,		// the lists put together and the samples resolved
	SsaaOITFinalSamplePS = 35,	// ...put together into each sample
	SsaaOITModVolPS = 36,
	SsaaResolvePS = 37,
};

D3D12_SHADER_BYTECODE dx12_shader(DX12Shader shader);
/* rend/texdecode.h's, by TEXD_KERNEL_D3D (1 to 4) */
D3D12_SHADER_BYTECODE dx12_texdecode_kernel(int kernel);
