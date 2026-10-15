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
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include <map>
#include <string>
#include <cstdio>
#include "comptr.h"

class DX11TexDecoder;

/* A texture of the texture pool's: a view of it to sample, and of each
 * level for the decoder to write through - an unsigned integer view of
 * the 32-bit and 8-bit ones (made typeless for that), a render target view
 * of the 16-bit ones - where the decoder makes textures of its format */
struct DX11PooledTexture : TexPoolEntry
{
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> view;
	ComPtr<ID3D11UnorderedAccessView> uavs[TEXD_MAX_LEVELS];
	ComPtr<ID3D11RenderTargetView> targets[TEXD_MAX_LEVELS];
	u32 levels = 1;
	bool writable = false;
	DX11PooledTexture *all = nullptr;		// every one there is, for Term()
};

/* The cache's textures (rend/tex_pool.h): made when the renderer starts,
 * and handed out to the textures that need one of their shape. The pool's
 * formats are the texture types, by their numbers. A texture is given
 * back as soon as it is done with: Direct3D sees to it that what the GPU
 * still has to do with it is done first. */
class DX11TexturePool
{
public:
	void Init(ID3D11Device *device, const VramBudget& budget, const DX11TexDecoder *decoder);
	void Term();
	/* A texture of @type, @width by @height, with its levels or not: a
	 * free one of that shape, or one made now if there is none. Null if
	 * the shape is not one of the pool's. */
	DX11PooledTexture *Take(TextureType type, u32 width, u32 height, bool mips);
	void Give(DX11PooledTexture *texture) { book.Give(texture); }
	const TexPoolBook& Book() const { return book; }

private:
	DX11PooledTexture *Make(int cls, bool late);

	TexPoolBook book;
	ID3D11Device *device = nullptr;
	bool ready = false;
	bool usable[5] = {};
	bool writable[5] = {};
	DX11PooledTexture *all = nullptr;
};

class DX11Texture : public BaseTextureCacheData
{
public:
	~DX11Texture() { ReleasePooled(); }
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	bool GpuDecode(const texd_plan& plan) override;
	bool Delete() override;
	/* The renderer's, while it has them */
	static void SetDecoder(DX11TexDecoder *decoder) { DX11Texture::decoder = decoder; }
	static void SetPool(DX11TexturePool *pool) { DX11Texture::pool = pool; }
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
		// (the pool's texture is not one to be rendered to: it goes back)
		ReleasePooled();
		std::swap(texture, spare);
		std::swap(textureView, spareView);
		spareTarget.reset();
	}

private:
	/* The pool's texture for this shape, the one it has if it is that;
	 * false if the pool has none of this shape */
	bool TakePooled(u32 width, u32 height, bool mips);
	void ReleasePooled();
	/* What was asked of the decoder for it is not done after all */
	void CancelDecode();

	DX11PooledTexture *pooled = nullptr;
	u32 decodeJob = 0, decodeCount = 0, decodeSerial = 0;
	static DX11TexDecoder *decoder;
	static DX11TexturePool *pool;
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
