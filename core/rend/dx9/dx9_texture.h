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
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include <string>
#include <cstdio>
#include "rend/dx11/comptr.h"

class DX9Context;
class DX9TexDecoder;

/* A texture of the texture pool's, and a surface of each of its levels
 * where it is drawn into */
struct DX9PooledTexture : TexPoolEntry
{
	ComPtr<IDirect3DTexture9> texture;
	ComPtr<IDirect3DSurface9> surfaces[TEXD_MAX_LEVELS];
	u32 levels = 1;
	DX9PooledTexture *all = nullptr;		// every one there is, for Term()
};

/* The cache's textures (rend/tex_pool.h): made when the renderer starts,
 * and handed out to the textures that need one of their shape. Of three
 * kinds: the ones the processor writes (dynamic, as the renderer's own are:
 * the formats 0 to 4, by TextureType); the ones the pixel shader draws
 * into (render targets, DRAWN + TextureType), made only when it is to;
 * and the pictures rendered to (TARGET, A8R8G8B8). A texture is given back
 * as soon as it is done with: Direct3D sees to it that what the GPU still
 * has to do with it is done first. */
class DX9TexturePool
{
public:
	/* (the render targets of the formats @decoder draws into, if there is one) */
	void Init(DX9Context *context, const DX9TexDecoder *decoder);
	void Term();
	enum { DRAWN = 5, TARGET = 10, FORMATS = 11 };
	/* A texture of the pool's format @f, @width by @height, with its levels
	 * or not: a free one of that shape, or one made now if there is none.
	 * Null if the shape or the format is not one of the pool's. */
	DX9PooledTexture *Take(u32 f, u32 width, u32 height, bool mips);
	void Give(DX9PooledTexture *texture) { book.Give(texture); }
	const TexPoolBook& Book() const { return book; }

private:
	DX9PooledTexture *Make(int cls, bool late);

	TexPoolBook book;
	DX9Context *context = nullptr;
	bool ready = false;
	bool usable[FORMATS] = {};
	DX9PooledTexture *all = nullptr;
};

/* A texture in video memory, written by the processor where it lies: a
 * dynamic texture (a managed one on a device without them), locked with
 * D3DLOCK_DISCARD, which the texture cache converts the console's texture
 * straight into when it can (UploadMemory()). */
class DX9Texture : public BaseTextureCacheData
{
public:
	~DX9Texture() { ReleasePooled(); ReleaseSpare(); }
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	bool GpuDecode(const texd_plan& plan) override;
	bool Delete() override;
	/* The renderer's, while it has them */
	static void SetDecoder(DX9TexDecoder *decoder) { DX9Texture::decoder = decoder; }
	static void SetPool(DX9TexturePool *pool) { DX9Texture::pool = pool; }
	bool Force32BitTexture(TextureType type) const override;
	std::string GetId() override { char s[20]; sprintf(s, "%p", (void *)this); return s; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	bool IsNew() const { return !texture; }

	ComPtr<IDirect3DTexture9> texture;
	/* The texture holds R G B A bytes, which the card reads as B G R A:
	 * the shader swaps them */
	bool swapRB = false;

	/* A render to this texture is drawn into this other picture, and the
	 * two change places when it is done (as the Direct3D 11 renderer's) */
	ComPtr<IDirect3DTexture9> spare;
	ComPtr<IDirect3DSurface9> spareSurface;
	void SwapWithSpare()
	{
		// (the texture it had goes back to the pool, if it is the pool's: the spare is that now)
		ReleasePooled();
		pooled = sparePooled;
		sparePooled = nullptr;
		std::swap(texture, spare);
		spareSurface.reset();
		swapRB = false;
	}
	/* The spare, from the pool if it has one of this shape */
	bool TakeSpare(u32 width, u32 height);
	void ReleaseSpare();

private:
	bool Prepare(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format);
	/* The pool's texture of format @f and this shape, the one it has if it
	 * is that; false if the pool has none */
	bool TakePooled(u32 f, u32 width, u32 height, bool mips);
	void ReleasePooled();
	/* What was asked of the decoder for it is not done after all */
	void CancelDecode();
	/* Locked by UploadMemory() for the UploadToGPU() that follows */
	void *lockedBits = nullptr;
	DX9PooledTexture *pooled = nullptr;
	DX9PooledTexture *sparePooled = nullptr;
	u32 decodeJob = 0, decodeCount = 0, decodeSerial = 0;
	static DX9TexDecoder *decoder;
	static DX9TexturePool *pool;
};

class DX9TextureCache : public BaseTextureCache<DX9Texture>
{
public:
	void Cleanup() { CollectCleanup(); }
};

/* The renderer's side of rend/rtt_watch.h: a render to a texture stays in
 * the texture, and is brought to video memory when the game first touches
 * it there. */
void dx9_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx9_rtt_watch(DX9Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height);
void dx9_rtt_term(void);
/* width by height pixels from the top left of a render target texture,
 * R G B A, top line first. Waits for the card. */
bool dx9_read_picture(IDirect3DTexture9 *texture, u32 width, u32 height, u8 *rgba);
