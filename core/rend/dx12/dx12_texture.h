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
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include "rend/dx11/comptr.h"
#include "dx12_shader_flags.h"
#include "dx12context_lr.h"
#include <string>
#include <cstdio>

class DX12TexDecoder;

/* A texture of the texture pool's (rend/tex_pool.h): its place in the
 * bindless table, which is its for good, and the state the GPU last left
 * it in. The decoder writes its levels through unordered access views
 * (32-bit and 8-bit texels, into a typeless texture) or draws them through
 * render target views (16-bit texels), made when they are needed in the
 * context's rings of them: a view is a few words in a heap, not an
 * object. */
struct DX12PooledTexture : TexPoolEntry
{
	ComPtr<ID3D12Resource> resource;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	u32 srv = ~0u;
	u32 rtv = ~0u;			// a picture rendered to: its render target view, for good
	u32 width = 0;
	u32 height = 0;
	u32 levels = 1;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;	// the view's
	bool writable = false;
	bool own = false;		// not one of the pool's shapes: made for one texture, and goes with it
	DX12PooledTexture *all = nullptr;		// every one there is, for Term()
};

/* The cache's textures: made when the renderer starts, and handed out to
 * the textures that need one of their shape. The pool's formats are the
 * texture types, by their numbers, and TARGET: the pictures rendered to,
 * RGBA8 with a render target view. A texture is given back as soon as it
 * is done with: everything is recorded into one command list, in order,
 * so what the GPU still has to draw with it is drawn before it is written
 * again. */
class DX12TexturePool
{
public:
	void Init(const VramBudget& budget, const DX12TexDecoder *decoder);
	void Term();
	/* A texture of @type, @width by @height, with its levels or not: a
	 * free one of that shape, or one made now if there is none. Null if
	 * the shape is not one of the pool's. */
	DX12PooledTexture *Take(TextureType type, u32 width, u32 height, bool mips);
	/* A picture to render to, @width by @height; null if the pool has
	 * none of that shape */
	DX12PooledTexture *TakeTarget(u32 width, u32 height);
	/* One of a shape the pool has not, for one texture: @levels levels of
	 * @type (TARGET for a picture to render to). Given back like any other,
	 * and goes then. */
	DX12PooledTexture *MakeOwn(u32 type, u32 width, u32 height, u32 levels);
	void Give(DX12PooledTexture *texture);
	enum { FORMATS = 6, TARGET = 5 };
	const TexPoolBook& Book() const { return book; }

private:
	DX12PooledTexture *Make(int cls, bool late);
	bool MakeResource(DX12PooledTexture *t, u32 f, u32 width, u32 height, u32 levels);

	TexPoolBook book;
	bool ready = false;
	bool usable[FORMATS] = {};
	bool writable[FORMATS] = {};
	DX12PooledTexture *all = nullptr;
};

class DX12Texture : public BaseTextureCacheData
{
public:
	~DX12Texture() { ReleasePooled(); ReleaseSpare(); }
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	bool GpuDecode(const texd_plan& plan) override;
	bool Delete() override;
	/* The renderer's, while it has them */
	static void SetDecoder(DX12TexDecoder *decoder) { DX12Texture::decoder = decoder; }
	static void SetPool(DX12TexturePool *pool) { DX12Texture::pool = pool; }
	static DX12TexturePool *Pool() { return pool; }
	bool Force32BitTexture(TextureType type) const override;
	std::string GetId() override { char s[20]; sprintf(s, "%p", (void *)this); return s; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	bool IsNew() const { return pooled == nullptr; }

	/* The texture, and its place in the bindless table (DX12_SRV_NONE
	 * when it has none: sampled as zero) */
	DX12PooledTexture *Image() const { return pooled; }
	u32 Srv() const { return pooled != nullptr ? pooled->srv : DX12_SRV_NONE; }

	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done: the polygons of the render that
	 * are drawn with the texture itself see the picture it had before, as
	 * the other renderers' do, and no picture is made anew for every
	 * render. */
	DX12PooledTexture *Spare() const { return spare; }
	void SwapWithSpare()
	{
		// (the texture it had goes back to the pool: the spare is that now)
		ReleasePooled();
		pooled = spare;
		spare = nullptr;
	}
	/* The spare, from the pool if it has one of this shape */
	bool TakeSpare(u32 width, u32 height);
	/* ...or this one, made for it */
	void SetSpare(DX12PooledTexture *t) { ReleaseSpare(); spare = t; }
	void ReleaseSpare();

private:
	/* The pool's texture for this shape, the one it has if it is that;
	 * false if the pool has none of this shape */
	bool TakePooled(u32 width, u32 height, bool mips);
	void ReleasePooled();
	/* What was asked of the decoder for it is not done after all */
	void CancelDecode();

	DX12PooledTexture *pooled = nullptr;
	DX12PooledTexture *spare = nullptr;
	u32 decodeJob = 0, decodeCount = 0, decodeSerial = 0;
	// UploadMemory() handed out a piece of the upload ring for the next UploadToGPU()
	u8 *uploadCpu = nullptr;
	DX12Upload uploadPiece;
	u32 uploadPitch = 0;
	static DX12TexDecoder *decoder;
	static DX12TexturePool *pool;
};

class DX12TextureCache : public BaseTextureCache<DX12Texture>
{
public:
	void Cleanup() { CollectCleanup(); }
};

/* The polygons' samplers: all of them in the sampler table when the
 * renderer starts, by the TSP's MipMapD (four bits), FilterMode (two),
 * ClampV, ClampU, FlipV, FlipU - ten bits, DX12_SAMPLER_TABLE_SIZE of
 * them. The fog table's and the palette's are the root signature's own. */
static inline u32 dx12_sampler_index(TSP tsp)
{
	return ((tsp.full >> 8) & 0xF) | (((tsp.full >> 13) & 0x3F) << 4);
}
void dx12_make_samplers();

/* The Direct3D 12 renderers' side of rend/rtt_watch.h, as vulkan/rtt_read.h
 * is the Vulkan renderers': a render to a texture stays in the texture,
 * and is brought to video memory when the game first touches it there. */
void dx12_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx12_rtt_watch(DX12Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx12_rtt_term(void);
/* width by height pixels of a picture of that size (left in @state, and
 * left as it was), R G B A, top line first. Submits what is recorded and
 * waits for the card: a readback. */
bool dx12_read_picture(ID3D12Resource *texture, D3D12_RESOURCE_STATES& state, u32 width, u32 height, u8 *rgba);
