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
#include <libretro.h>
#include <libretro_d3d.h>
#include "comptr.h"
#include "dx11_shaders.h"
#include "dx11_texture.h"
#include "dx11_renderstate.h"
#include "dx11_quad.h"

/* The frontend's Direct3D 11 device, from RETRO_HW_CONTEXT_D3D11's
 * interface, and what the renderers share of it.
 *
 * With version 1 of the interface the frame is the shader resource view
 * left at the pixel shader's slot 0 when the frame is handed over. With
 * version 2 the context is the frontend's as well, taken in turns
 * (lock_context), the frame is named (set_texture), and the frontend may
 * still be reading a frame when the next one is drawn: a picture for each
 * of its sync indexes. */
class DX11Context
{
public:
	static DX11Context *Instance() { return &instance; }

	/* The frontend has made its context (context_reset) */
	bool Init(const struct retro_hw_render_interface_d3d11 *iface);
	/* ...and is about to destroy it, or the game is going */
	void Term();

	ID3D11Device *GetDevice() const { return device; }
	ID3D11DeviceContext *GetDeviceContext() const { return deviceContext; }
	D3D_FEATURE_LEVEL GetFeatureLevel() const { return featureLevel; }
	/* Unordered access views in pixel shaders, and a context that can be read back from */
	bool HasPerPixel() const { return device && featureLevel >= D3D_FEATURE_LEVEL_11_0 && immediate; }
	/* A deferred context (version 1, a frontend that draws on a thread of
	 * its own) records commands, and cannot hand anything back */
	bool CanReadBack() const { return immediate; }
	/* Whether the 16-bit formats can be sampled, and the smaller levels of
	 * a texture made on the card */
	bool IsFormatSupported(TextureType type) const;
	bool CanGenerateMips(TextureType type) const;

	pD3DCompile GetCompiler() const { return compile; }
	DX11Shaders& GetShaders() { return shaders; }
	DX11Samplers& GetSamplers() { return samplers; }
	DX11Quad& GetQuad() { return quad; }
	DX11BlendStates& GetBlendStates() { return blendStates; }

	/* Something other than the renderer set up the context: whatever the
	 * renderer keeps of what it has set is not so any more */
	void StateChanged() { stateChanges++; }
	u32 GetStateChanges() const { return stateChanges; }

	/* Version 2: the context, taken from the frontend and given back.
	 * May be nested. Nothing with version 1. */
	void Lock();
	void Unlock();

	/* The picture the next video_refresh is to show */
	void SetFrame(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view);
	/* Just before video_refresh(RETRO_HW_FRAME_BUFFER_VALID): it is handed over */
	void Present();
	/* How many pictures the screen is to be drawn in turn into, and which
	 * of them the next frame is. Before drawing into it, the frontend is
	 * waited for to be done with it - never with the context taken. */
	unsigned GetPresentCount() const;
	unsigned GetPresentIndex() const;
	void WaitPresentIndex();

private:
	static DX11Context instance;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> deviceContext;
	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
	bool immediate = false;
	pD3DCompile compile = nullptr;
	HMODULE compilerModule = nullptr;
	bool formatSupported[5] = {};
	bool mipsSupported[5] = {};

	DX11Shaders shaders;
	DX11Samplers samplers;
	DX11Quad quad;
	DX11BlendStates blendStates;
	u32 stateChanges = 0;

	ComPtr<ID3D11Texture2D> frameTexture;
	ComPtr<ID3D11ShaderResourceView> frameView;

	// version 2
	bool v2 = false;
	void *handle = nullptr;
	bool (*lock_context)(void *) = nullptr;
	void (*unlock_context)(void *) = nullptr;
	void (*set_texture)(void *, ID3D11Texture2D *) = nullptr;
	unsigned (*get_sync_index)(void *) = nullptr;
	unsigned (*get_sync_index_mask)(void *) = nullptr;
	void (*wait_sync_index)(void *) = nullptr;
	int lockDepth = 0;
};

/* The context taken for as long as this lives (see DX11Context::Lock()) */
struct DX11Lock
{
	DX11Lock() { DX11Context::Instance()->Lock(); }
	~DX11Lock() { DX11Context::Instance()->Unlock(); }
};
