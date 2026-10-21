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
/* A picture drawn onto what is bound, as a rectangle: the last frame's
 * into the next, the VMUs' screens and the crosshairs over the picture,
 * the framebuffer the game wrote itself, a render to a texture brought
 * down to the console's size. No vertex buffer: the rectangle is in the
 * root constants (shaders/dx12_raster.hlsl, DX12_QUAD_VS). */
#include "dx12context_lr.h"
#include "dx12_shader_flags.h"
#include <cstring>

struct DX12Quad
{
	/* The picture at @srv of the bindless table, sampled with the linear
	 * sampler or the nearest one; @x, @y, @w, @h in clip space (y going
	 * up); its coordinates from (@u0, @v0) to (@u1, @v1); blended by
	 * @special (DX12_BLEND_KEY: not blended; DX12_BLEND_ALPHA; DX12_BLEND_FACTOR
	 * with @factor) */
	static void Draw(u32 srv, bool linear, float x, float y, float w, float h,
			float u0 = 0.f, float v0 = 0.f, float u1 = 1.f, float v1 = 1.f, u32 special = DX12_BLEND_KEY, const float *factor = nullptr)
	{
		DX12Context *context = DX12Context::Instance();
		context->BindRaster();
		DX12PsoKey key{};
		key.kind = DX12_PSO_QUAD;
		key.writeColor = true;
		key.special = special;
		key.depthBound = context->HasDepthBound();
		key.samples = context->TargetSamples();
		ID3D12PipelineState *pso = context->Pipelines().Get(key);
		if (pso == nullptr)
			return;
		context->SetPipeline(pso);
		context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		if (special == DX12_BLEND_FACTOR && factor != nullptr)
			context->SetBlendFactor(factor);
		const float values[8] = { x, y, w, h, u0, v0, u1, v1 };
		u32 constants[10];
		memcpy(constants, values, sizeof(values));
		constants[8] = srv;
		constants[9] = linear ? DX12_QF_LINEAR : 0;
		context->SetRootConstants(constants, 10);
		context->Cmd()->DrawInstanced(4, 1, 0, 0);
	}
};
