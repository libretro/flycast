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
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>
#include <vector>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d9.h>
#include "rend/dx11/comptr.h"
#include "rend/TexCache.h"
#include "rend/vram_budget.h"
#include "dx9_shaders.h"

/* The frontend's Direct3D 9 device, from RETRO_HW_CONTEXT_D3D9's
 * interface (libretro_d3d9.h), and what the renderer shares of it.
 *
 * The device is the frontend's as well, taken in turns (Lock()), and the
 * frame is a picture of this core's handed over as it is (SetFrame()):
 * one for each of the frontend's sync indexes, each drawn into again only
 * once the frontend is done with it.
 *
 * Everything the renderer sets on the device goes through here and is
 * set only when it differs from what the device has: from one render to
 * the next as well, for as long as the frontend has not used the device.
 * When it has, nothing is known of what it left, and the renderer's base
 * state is put back in one call (a state block). */
class DX9Context
{
public:
	static DX9Context *Instance() { return &instance; }

	/* The frontend has made its device (context_reset) */
	bool Init(const struct retro_hw_render_interface_d3d9 *iface);
	/* ...and is about to release it, or reset it, or the game is going */
	void Term();

	IDirect3DDevice9 *GetDevice() const { return device; }
	bool IsEx() const { return ex; }
	const VramBudget& GetVramBudget() const { return vramBudget; }
	DX9Shaders& GetShaders() { return shaders; }
	/* Whether a 16-bit format can be sampled; and whether the card makes
	 * a texture's smaller levels */
	bool IsFormatSupported(TextureType type) const { return (unsigned)type < 5 && formatSupported[(int)type]; }
	bool CanGenerateMips(TextureType type) const { return (unsigned)type < 5 && mipsSupported[(int)type]; }
	static D3DFORMAT TextureFormat(TextureType type);
	D3DFORMAT GetDepthFormat() const { return depthFormat; }
	DWORD GetMaxAnisotropy() const { return maxAnisotropy; }
	/* Textures written by the processor: dynamic ones, written where they
	 * are; or, on a device without them, managed ones */
	bool HasDynamicTextures() const { return dynamicTextures; }

	/* The device, taken from the frontend and given back. May be nested;
	 * the scene begun inside is ended when it is given back. */
	void Lock();
	void Unlock();
	bool IsLocked() const { return lockDepth > 0; }
	/* Changes each time the frontend has had the device: constants the
	 * renderer set before are not there any more */
	u32 Epoch() const { return epoch; }
	/* Draws go between these. StretchRect(), GetRenderTargetData() and
	 * the like outside: EndScene() first. */
	bool BeginScene();
	void EndScene();

	/* ---- What is set ---- */
	void SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
	{
		if ((u32)state < RenderStates)
		{
			if (renderState[state] == value)
				return;
			renderState[state] = value;
		}
		device->SetRenderState(state, value);
	}
	void SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture)
	{
		if (texture != bound.texture[stage])
		{
			device->SetTexture(stage, texture);
			bound.texture[stage] = texture;
		}
	}
	/* A sampler's filter, addressing and level of detail bias, as one word:
	 * see SamplerKey() */
	void SetSampler(DWORD stage, u32 key);
	static u32 SamplerKey(bool linear, u32 anisotropy, D3DTEXTUREADDRESS u, D3DTEXTUREADDRESS v, u32 lodBias)
	{
		return (linear ? 1u : 0u) | ((anisotropy & 31) << 1) | ((u32)u << 6) | ((u32)v << 9) | ((lodBias & 15) << 12) | 0x10000;
	}
	/* Clamped and nearest, or linear: pictures drawn whole */
	static u32 PointSampler() { return SamplerKey(false, 1, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0); }
	static u32 LinearSampler() { return SamplerKey(true, 1, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0); }
	void SetVS(IDirect3DVertexShader9 *vs)
	{
		if (vs != bound.vs)
		{
			device->SetVertexShader(vs);
			bound.vs = vs;
		}
	}
	void SetPS(IDirect3DPixelShader9 *ps)
	{
		if (ps != bound.ps)
		{
			device->SetPixelShader(ps);
			bound.ps = ps;
		}
	}
	void SetDecl(IDirect3DVertexDeclaration9 *decl)
	{
		if (decl != bound.decl)
		{
			device->SetVertexDeclaration(decl);
			bound.decl = decl;
		}
	}
	void SetStream(IDirect3DVertexBuffer9 *buffer, UINT stride)
	{
		if (buffer != bound.stream || stride != bound.stride)
		{
			device->SetStreamSource(0, buffer, 0, stride);
			bound.stream = buffer;
			bound.stride = stride;
		}
	}
	void SetIndices(IDirect3DIndexBuffer9 *indices)
	{
		if (indices != bound.indices)
		{
			device->SetIndices(indices);
			bound.indices = indices;
		}
	}
	/* The pixel shaders' flags, as bits (DX9ShaderBool) */
	void SetBools(u32 bits)
	{
		if (bits != bound.bools)
		{
			float values[16];
			for (int i = 0; i < 16; i++)
				values[i] = (float)((bits >> i) & 1);
			device->SetPixelShaderConstantF(DX9PC_Flags, values, 4);
			bound.bools = bits;
		}
	}
	/* What is drawn into, @width by @height of it from its top left corner
	 * (the viewport, and half a pixel of it for the vertex shaders) */
	void SetTarget(IDirect3DSurface9 *target, IDirect3DSurface9 *depth, u32 width, u32 height);
	/* Within the viewport. False, and nothing set, if nothing of it is:
	 * then nothing is to be drawn. */
	bool SetScissor(const RECT& r)
	{
		const RECT rect = {
			std::max<LONG>(r.left, 0), std::max<LONG>(r.top, 0),
			std::min<LONG>(r.right, (LONG)bound.viewport[0]), std::min<LONG>(r.bottom, (LONG)bound.viewport[1])
		};
		if (rect.right <= rect.left || rect.bottom <= rect.top)
			return false;
		if (!bound.scissorSet || rect.left != bound.scissor.left || rect.top != bound.scissor.top
				|| rect.right != bound.scissor.right || rect.bottom != bound.scissor.bottom)
		{
			device->SetScissorRect(&rect);
			bound.scissor = rect;
			bound.scissorSet = true;
		}
		return true;
	}
	/* What is drawn into cleared, all of it */
	void Clear(DWORD flags, D3DCOLOR color);

	/* The depth and the stencil, as the Vulkan renderers' pipelines use
	 * them: by key (DX9Stencil), set when the key changes */
	void SetDepthStencil(bool depthTest, bool depthWrite, int depthFunc, int stencil, DWORD ref);
	/* Blending by the TSP's instructions, or none */
	void SetBlend(bool enable, u32 src = 1, u32 dst = 0, bool writeColor = true);
	/* By the source's alpha: the crosshairs, the VMUs' screens, shadows */
	void SetAlphaBlend();

	/* ---- Pictures ---- */

	/* A texture of the renderer's: one released before of the same kind
	 * if there is one, or a new one */
	ComPtr<IDirect3DTexture9> MakeTexture(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format);
	/* ...and given back, to be used again */
	void Recycle(ComPtr<IDirect3DTexture9>& texture);
	/* A texture the processor writes all of, with D3DLOCK_DISCARD where
	 * it can: what to lock it with */
	DWORD WriteLockFlags() const { return dynamicTextures ? D3DLOCK_DISCARD : 0; }
	DWORD WriteUsage() const { return dynamicTextures ? D3DUSAGE_DYNAMIC : 0; }

	/* A picture onto what is bound: at x, y, w, h in clip space (y going
	 * up), from (u0, v0) to (u1, v1) of the texture (v going down).
	 * Blended by its alpha times @alpha, or put there as it is. */
	void DrawQuad(IDirect3DTexture9 *texture, bool linear, bool swapRB,
			float x = -1.f, float y = -1.f, float w = 2.f, float h = 2.f,
			float u0 = 0.f, float v0 = 0.f, float u1 = 1.f, float v1 = 1.f,
			bool blend = false, float alpha = 1.f);

	/* ---- The frame ---- */

	/* The picture the next video_refresh is to show, handed over here,
	 * with the device taken: from then on it is the frontend's
	 * (IsHandedOver()) until the frame has been shown */
	void SetFrame(IDirect3DTexture9 *texture);
	bool IsHandedOver(IDirect3DTexture9 *texture) const { return handedOver && texture == frameTexture.get(); }
	/* Just before video_refresh: whether a frame was handed over for it */
	bool Present();
	/* How many pictures the screen is drawn in turn into, and which of
	 * them the next frame is. Before drawing into it, the frontend is
	 * waited for to be done with it - never with the device taken. */
	unsigned GetPresentCount() const;
	unsigned GetPresentIndex() const;
	void WaitPresentIndex();

private:
	static DX9Context instance;
	bool InitLocked(const struct retro_hw_render_interface_d3d9 *iface);

	ComPtr<IDirect3DDevice9> device;
	bool ex = false;
	VramBudget vramBudget;
	pD3DCompile compile = nullptr;
	HMODULE compilerModule = nullptr;
	bool formatSupported[5] = {};
	bool mipsSupported[5] = {};
	bool dynamicTextures = false;
	bool separateAlpha = false;
	D3DFORMAT depthFormat = D3DFMT_D24S8;
	DWORD maxAnisotropy = 1;
	DWORD extraTargets = 0;		// render targets 1 and on
	DX9Shaders shaders;

	void *handle = nullptr;
	bool (*lock_context)(void *) = nullptr;
	void (*unlock_context)(void *) = nullptr;
	void (*set_texture)(void *, LPDIRECT3DTEXTURE9) = nullptr;
	unsigned (*get_sync_index)(void *) = nullptr;
	unsigned (*get_sync_index_mask)(void *) = nullptr;
	void (*wait_sync_index)(void *) = nullptr;
	int lockDepth = 0;
	bool inScene = false;
	u32 epoch = 0;

	/* What the device has, as far as this knows */
	static const u32 RenderStates = D3DRS_BLENDOPALPHA + 1;
	static const DWORD Unknown = 0xfefefefe;
	DWORD renderState[RenderStates];
	struct Bound
	{
		IDirect3DBaseTexture9 *texture[3];
		u32 sampler[3];
		IDirect3DVertexShader9 *vs;
		IDirect3DPixelShader9 *ps;
		IDirect3DVertexDeclaration9 *decl;
		IDirect3DVertexBuffer9 *stream;
		UINT stride;
		IDirect3DIndexBuffer9 *indices;
		u32 bools;
		IDirect3DSurface9 *target;
		IDirect3DSurface9 *depth;
		u32 viewport[2];
		RECT scissor;
		bool scissorSet;
		u32 depthKey;
		u32 blendKey;
	} bound;
	/* Nothing is known: the frontend has had the device */
	void Forget();
	/* What the renderer never changes, set again in one call */
	ComPtr<IDirect3DStateBlock9> baseState;
	void SetBase(bool record);

	/* Textures released, to be made into new ones of the same kind */
	struct Spare
	{
		u32 width, height, levels;
		DWORD usage;
		D3DFORMAT format;
		u32 bytes;
		ComPtr<IDirect3DTexture9> texture;
	};
	std::vector<Spare> spares;
	u32 spareBytes = 0;
	static const size_t MaxSpares = 64;
	static const u32 MaxSpareBytes = 32 * 1024 * 1024;

	/* The quad's vertices, one rectangle after another, started again
	 * (discarded) when full */
	ComPtr<IDirect3DVertexBuffer9> quadBuffer;
	ComPtr<IDirect3DVertexDeclaration9> quadDecl;
	static const UINT QuadCount = 64;
	UINT quadNext = QuadCount;

	ComPtr<IDirect3DTexture9> frameTexture;
	bool handedOver = false;
};

/* The device taken for as long as this lives (see DX9Context::Lock()) */
struct DX9Lock
{
	DX9Lock() { DX9Context::Instance()->Lock(); }
	~DX9Lock() { DX9Context::Instance()->Unlock(); }
};

/* What the stencil buffer is used for (dx11/dx11_renderstate.h): the
 * modifier volumes' bits 0 and 1, bit 7 for a polygon that takes shadows,
 * bit 6 for a pixel drawn again for its shadow (rend/shadows.h). */
enum DX9Stencil
{
	DX9Stencil_None,
	DX9Stencil_Shadow,
	DX9Stencil_ShadowLaterWins,
	DX9Stencil_ShadowEarlierWins,
	DX9Stencil_Xor,
	DX9Stencil_Or,
	DX9Stencil_Inclusion,
	DX9Stencil_Exclusion,
	DX9Stencil_Final,
};
