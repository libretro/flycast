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
#include <string>
#include <cstdio>
#include "rend/dx11/comptr.h"

/* A texture in video memory, written by the processor where it lies: a
 * dynamic texture (a managed one on a device without them), locked with
 * D3DLOCK_DISCARD, which the texture cache converts the console's texture
 * straight into when it can (UploadMemory()). */
class DX9Texture : public BaseTextureCacheData
{
public:
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	bool Delete() override;
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
		std::swap(texture, spare);
		spareSurface.reset();
		swapRB = false;
	}

private:
	bool Prepare(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format);
	/* Locked by UploadMemory() for the UploadToGPU() that follows */
	void *lockedBits = nullptr;
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
