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
#include "rend/TexCache.h"
#include <map>
#include <string>
#include <cstdio>
#include "comptr.h"

class DX11Texture : public BaseTextureCacheData
{
public:
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	bool Delete() override;
	bool Force32BitTexture(TextureType type) const override;
	std::string GetId() override { char s[20]; sprintf(s, "%p", (void *)this); return s; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	bool IsNew() const { return !texture; }

	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> textureView;

	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done: the polygons of the render that
	 * are drawn with the texture itself see the picture it had before, as
	 * the other renderers' do, and no picture is made anew for every
	 * render. */
	ComPtr<ID3D11Texture2D> spare;
	ComPtr<ID3D11ShaderResourceView> spareView;
	ComPtr<ID3D11RenderTargetView> spareTarget;
	void SwapWithSpare()
	{
		std::swap(texture, spare);
		std::swap(textureView, spareView);
		spareTarget.reset();
	}
};

class DX11TextureCache : public BaseTextureCache<DX11Texture>
{
public:
	void Cleanup() { CollectCleanup(); }
};

class DX11Samplers
{
public:
	/* A polygon's: MipMapD, FilterMode, ClampU, ClampV, FlipU, FlipV */
	ID3D11SamplerState *GetSampler(TSP tsp)
	{
		ID3D11SamplerState *sampler = samplers[(tsp.full & TSP_Mask) >> 8];
		return sampler != nullptr ? sampler : MakeSampler(tsp);
	}
	/* Clamped, nearest: the fog table's and the palette's */
	ID3D11SamplerState *GetPointSampler();
	/* Clamped, linear: pictures drawn whole onto another */
	ID3D11SamplerState *GetLinearSampler();
	void Term()
	{
		for (auto& sampler : samplers)
			sampler.reset();
		linear.reset();
	}

	static const u32 TSP_Mask = 0x7ef00;

private:
	ID3D11SamplerState *MakeSampler(TSP tsp);
	/* (looked up for every polygon: by the bits themselves) */
	ComPtr<ID3D11SamplerState> samplers[(TSP_Mask >> 8) + 1];
	ComPtr<ID3D11SamplerState> linear;
};

/* The Direct3D 11 renderers' side of rend/rtt_watch.h, as vulkan/rtt_read.h
 * is the Vulkan renderers': a render to a texture stays in the texture,
 * and is brought to video memory when the game first touches it there. */
void dx11_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx11_rtt_watch(DX11Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx11_rtt_term(void);
/* width by height pixels of a picture of that size, R G B A, top line
 * first. Waits for the card. False if the frontend's context cannot be
 * read from. */
bool dx11_read_picture(ID3D11Texture2D *texture, u32 width, u32 height, u8 *rgba);
