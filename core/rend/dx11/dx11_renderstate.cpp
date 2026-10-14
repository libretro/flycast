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
#include "dx11_renderstate.h"
#include "dx11context_lr.h"

static const D3D11_COMPARISON_FUNC depthFuncs[] =
{
	D3D11_COMPARISON_NEVER,				//0	Never
	D3D11_COMPARISON_LESS,				//1	Less
	D3D11_COMPARISON_EQUAL,				//2	Equal
	D3D11_COMPARISON_LESS_EQUAL,		//3	Less Or Equal
	D3D11_COMPARISON_GREATER,			//4	Greater
	D3D11_COMPARISON_NOT_EQUAL,			//5	Not Equal
	D3D11_COMPARISON_GREATER_EQUAL,		//6	Greater Or Equal
	D3D11_COMPARISON_ALWAYS,			//7	Always
};

ID3D11DepthStencilState *DX11DepthStencilStates::Get(bool depthTest, bool depthWrite, int depthFunc, DX11Stencil stencil)
{
	const u32 key = (u32)depthTest | ((u32)depthWrite << 1) | ((u32)(depthFunc & 7) << 2) | ((u32)stencil << 5);
	auto it = states.find(key);
	if (it != states.end())
		return it->second;

	D3D11_DEPTH_STENCIL_DESC desc{};
	desc.DepthEnable = depthTest;
	desc.DepthWriteMask = depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = depthFuncs[depthFunc & 7];
	desc.StencilEnable = stencil != DX11Stencil::None;
	D3D11_DEPTH_STENCILOP_DESC& op = desc.FrontFace;
	op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	op.StencilFunc = D3D11_COMPARISON_ALWAYS;
	// (the reference is set with the state, OMSetDepthStencilState())
	switch (stencil)
	{
	case DX11Stencil::None:
	case DX11Stencil::Count:
		break;
	case DX11Stencil::Shadow:
		op.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 0x80;
		break;
	case DX11Stencil::ShadowLaterWins:
		op.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0x81;
		desc.StencilWriteMask = 0x40;
		break;
	case DX11Stencil::ShadowEarlierWins:
		op.StencilPassOp = D3D11_STENCIL_OP_INVERT;
		op.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x40;
		break;
	case DX11Stencil::Xor:
		op.StencilPassOp = D3D11_STENCIL_OP_INVERT;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 2;
		break;
	case DX11Stencil::Or:
		op.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 2;
		desc.StencilWriteMask = 2;
		break;
	case DX11Stencil::Inclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D11_STENCIL_OP_ZERO;
		op.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX11Stencil::Exclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D11_STENCIL_OP_ZERO;
		op.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX11Stencil::Final:
		// takes shadows, in a volume, and not drawn again for it (bit 6);
		// clears the volume bits and that mark
		op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D11_STENCIL_OP_ZERO;
		op.StencilFunc = D3D11_COMPARISON_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x43;
		break;
	}
	desc.BackFace = desc.FrontFace;

	ComPtr<ID3D11DepthStencilState>& state = states[key];
	ID3D11Device *device = DX11Context::Instance()->GetDevice();
	if (device == nullptr || FAILED(device->CreateDepthStencilState(&desc, state.put())))
		WARN_LOG(RENDERER, "Depth stencil state creation failed");
	return state;
}

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const D3D11_BLEND srcBlend[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_DEST_COLOR,
	D3D11_BLEND_INV_DEST_COLOR,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
};
static const D3D11_BLEND dstBlend[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_SRC_COLOR,
	D3D11_BLEND_INV_SRC_COLOR,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
};
static const D3D11_BLEND srcBlendAlpha[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
};
static const D3D11_BLEND dstBlendAlpha[] =
{
	D3D11_BLEND_ZERO,
	D3D11_BLEND_ONE,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_SRC_ALPHA,
	D3D11_BLEND_INV_SRC_ALPHA,
	D3D11_BLEND_DEST_ALPHA,
	D3D11_BLEND_INV_DEST_ALPHA,
};

ID3D11BlendState *DX11BlendStates::Make(const D3D11_BLEND_DESC& desc, ComPtr<ID3D11BlendState>& state)
{
	ID3D11Device *device = DX11Context::Instance()->GetDevice();
	if (device == nullptr || FAILED(device->CreateBlendState(&desc, state.put())))
		WARN_LOG(RENDERER, "Blend state creation failed");
	return state;
}

ID3D11BlendState *DX11BlendStates::Get(bool enable, u32 src, u32 dst, bool writeColor)
{
	src &= 7;
	dst &= 7;
	if (!enable)
		src = dst = 0;
	const u32 key = (u32)enable | (src << 1) | (dst << 4) | ((u32)writeColor << 7);
	auto it = states.find(key);
	if (it != states.end())
		return it->second;

	D3D11_BLEND_DESC desc{};
	D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
	rt.BlendEnable = enable;
	rt.SrcBlend = enable ? srcBlend[src] : D3D11_BLEND_ONE;
	rt.DestBlend = enable ? dstBlend[dst] : D3D11_BLEND_ZERO;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = enable ? srcBlendAlpha[src] : D3D11_BLEND_ONE;
	rt.DestBlendAlpha = enable ? dstBlendAlpha[dst] : D3D11_BLEND_ZERO;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = writeColor ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	return Make(desc, states[key]);
}

ID3D11BlendState *DX11BlendStates::GetAlpha()
{
	if (!alpha)
	{
		D3D11_BLEND_DESC desc{};
		D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		Make(desc, alpha);
	}
	return alpha;
}

ID3D11BlendState *DX11BlendStates::GetFactorAlpha()
{
	if (!factorAlpha)
	{
		D3D11_BLEND_DESC desc{};
		D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
		rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
		rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		Make(desc, factorAlpha);
	}
	return factorAlpha;
}
