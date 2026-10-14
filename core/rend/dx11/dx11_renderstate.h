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
#include <unordered_map>
#include "comptr.h"

/* What the stencil buffer is used for, as the Vulkan renderers' pipelines
 * use it (vulkan/pipeline.cpp): the modifier volumes' bits 0 and 1, bit 7
 * for a polygon that takes shadows, bit 6 for a pixel drawn again for its
 * shadow (rend/shadows.h). */
enum class DX11Stencil
{
	None,
	Shadow,				// a polygon that takes shadows, or not: bit 7 (the reference) written
	ShadowLaterWins,	// drawn again for its shadow: see PipelineManager::ShadowPass
	ShadowEarlierWins,
	Xor,				// a closed volume
	Or,					// an open one, or a quad
	Inclusion,			// the area summed
	Exclusion,
	Final,				// what is left in a volume, darkened
	Count
};

class DX11DepthStencilStates
{
public:
	/* @depthFunc: the ISP's depth mode, 0 never to 7 always */
	ID3D11DepthStencilState *Get(bool depthTest, bool depthWrite, int depthFunc, DX11Stencil stencil = DX11Stencil::None);
	void Term() { states.clear(); }

private:
	std::unordered_map<u32, ComPtr<ID3D11DepthStencilState>> states;
};

class DX11BlendStates
{
public:
	/* The TSP's instructions, 0 to 7 */
	ID3D11BlendState *Get(bool enable, u32 src = 1, u32 dst = 0, bool writeColor = true);
	/* by the source's alpha: the crosshairs */
	ID3D11BlendState *GetAlpha();
	/* by the alpha of the blend factor: the VMUs' screens */
	ID3D11BlendState *GetFactorAlpha();
	void Term() { states.clear(); alpha.reset(); factorAlpha.reset(); }

private:
	ID3D11BlendState *Make(const D3D11_BLEND_DESC& desc, ComPtr<ID3D11BlendState>& state);

	std::unordered_map<u32, ComPtr<ID3D11BlendState>> states;
	ComPtr<ID3D11BlendState> alpha;
	ComPtr<ID3D11BlendState> factorAlpha;
};
