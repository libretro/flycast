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
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <algorithm>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d.h>
#include "comptr.h"
#include "dx11_shaders.h"
#include "dx11_texture.h"
#include "dx11_renderstate.h"
#include "dx11_quad.h"
#include "rend/vram_budget.h"

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
	// what video memory there was to use when the context was set up
	const VramBudget& GetVramBudget() const { return vramBudget; }
	// the adapter's PCI vendor id (0x10de: NVIDIA's), 0 when it is not known
	UINT GetVendor() const { return vendor; }
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

	/* The frontend draws with the context (version 1: in the video_refresh
	 * the frame is handed over in): nothing bound is what this core bound.
	 * The context is cleared when the core next takes it (Lock()). */
	void StateChanged() { stateChanges++; }

	/* Version 2: the context, taken from the frontend and given back.
	 * May be nested. With version 1 only what the frontend has done to the
	 * context is looked at. */
	void Lock();
	void Unlock();

	/* ---- What is bound ----
	 *
	 * Everything this core binds goes through these, and is set only when
	 * it differs from what is bound already: from one render to the next
	 * as well, for as long as the frontend has not used the context.
	 * (Lock() clears the context when the frontend has, and this then
	 * knows exactly what is bound: nothing.) The geometry shader is never
	 * anything but none. */
	void SetInputLayout(ID3D11InputLayout *layout)
	{
		if (layout != bound.layout)
		{
			deviceContext->IASetInputLayout(layout);
			bound.layout = layout;
		}
	}
	/* The same buffer at vertex slots 0 and 1, with those strides, and as the index buffer (or none) */
	void SetGeometry(ID3D11Buffer *buffer, UINT stride0, UINT stride1, ID3D11Buffer *indexBuffer)
	{
		SetGeometry(buffer, stride0, buffer, stride1, indexBuffer);
	}
	/* ...or a buffer each (GPU-side TA processing's) */
	void SetGeometry(ID3D11Buffer *buffer0, UINT stride0, ID3D11Buffer *buffer1, UINT stride1, ID3D11Buffer *indexBuffer)
	{
		if (buffer0 != bound.vertexBuffer[0] || buffer1 != bound.vertexBuffer[1]
				|| stride0 != bound.stride[0] || stride1 != bound.stride[1])
		{
			ID3D11Buffer * const buffers[2] = { buffer0, buffer1 };
			const UINT strides[2] = { stride0, stride1 };
			const UINT offsets[2] = { 0, 0 };
			deviceContext->IASetVertexBuffers(0, 2, buffers, strides, offsets);
			bound.vertexBuffer[0] = buffer0;
			bound.vertexBuffer[1] = buffer1;
			bound.stride[0] = stride0;
			bound.stride[1] = stride1;
		}
		SetIndexBuffer(indexBuffer);
	}
	void SetIndexBuffer(ID3D11Buffer *indexBuffer)
	{
		if (indexBuffer != bound.indexBuffer)
		{
			deviceContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
			bound.indexBuffer = indexBuffer;
		}
	}
	void SetVertexBuffer(ID3D11Buffer *buffer, UINT stride, UINT slot = 0)
	{
		if (buffer != bound.vertexBuffer[slot] || stride != bound.stride[slot])
		{
			const UINT offset = 0;
			deviceContext->IASetVertexBuffers(slot, 1, &buffer, &stride, &offset);
			bound.vertexBuffer[slot] = buffer;
			bound.stride[slot] = stride;
		}
	}
	void SetTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
	{
		if (topology != bound.topology)
		{
			deviceContext->IASetPrimitiveTopology(topology);
			bound.topology = topology;
		}
	}
	void SetVS(ID3D11VertexShader *vs)
	{
		if (vs != bound.vs)
		{
			deviceContext->VSSetShader(vs, nullptr, 0);
			bound.vs = vs;
		}
	}
	void SetPS(ID3D11PixelShader *ps)
	{
		if (ps != bound.ps)
		{
			deviceContext->PSSetShader(ps, nullptr, 0);
			bound.ps = ps;
		}
	}
	void SetVSConstants(ID3D11Buffer *buffer)
	{
		if (buffer != bound.vsConstants)
		{
			deviceContext->VSSetConstantBuffers(0, 1, &buffer);
			bound.vsConstants = buffer;
		}
	}
	void SetPSConstants(UINT slot, ID3D11Buffer *buffer)
	{
		if (buffer != bound.psConstants[slot])
		{
			deviceContext->PSSetConstantBuffers(slot, 1, &buffer);
			bound.psConstants[slot] = buffer;
		}
	}
	void SetPSResource(UINT slot, ID3D11ShaderResourceView *view)
	{
		if (view != bound.srv[slot])
		{
			deviceContext->PSSetShaderResources(slot, 1, &view);
			bound.srv[slot] = view;
		}
	}
	/* Several at once: in one call, for those of them that differ */
	void SetPSConstants(UINT start, UINT count, ID3D11Buffer * const *buffers)
	{
		UINT first = count, last = 0;
		for (UINT i = 0; i < count; i++)
			if (buffers[i] != bound.psConstants[start + i])
			{
				first = std::min(first, i);
				last = i;
			}
		if (first == count)
			return;
		deviceContext->PSSetConstantBuffers(start + first, last - first + 1, buffers + first);
		for (UINT i = first; i <= last; i++)
			bound.psConstants[start + i] = buffers[i];
	}
	void SetPSResources(UINT start, UINT count, ID3D11ShaderResourceView * const *views)
	{
		UINT first = count, last = 0;
		for (UINT i = 0; i < count; i++)
			if (views[i] != bound.srv[start + i])
			{
				first = std::min(first, i);
				last = i;
			}
		if (first == count)
			return;
		deviceContext->PSSetShaderResources(start + first, last - first + 1, views + first);
		for (UINT i = first; i <= last; i++)
			bound.srv[start + i] = views[i];
	}
	void SetPSSampler(UINT slot, ID3D11SamplerState *sampler)
	{
		if (sampler != bound.sampler[slot])
		{
			deviceContext->PSSetSamplers(slot, 1, &sampler);
			bound.sampler[slot] = sampler;
		}
	}
	/* Pixel shader resources 0 to @count - 1 unbound */
	void UnbindPSResources(UINT count)
	{
		UINT first = count;
		for (UINT i = 0; i < count; i++)
			if (bound.srv[i] != nullptr)
			{
				first = i;
				break;
			}
		if (first == count)
			return;
		ID3D11ShaderResourceView *none[ResourceSlots] = {};
		deviceContext->PSSetShaderResources(first, count - first, none);
		for (UINT i = first; i < count; i++)
			bound.srv[i] = nullptr;
	}
	void SetRasterizer(ID3D11RasterizerState *state)
	{
		if (state != bound.raster)
		{
			deviceContext->RSSetState(state);
			bound.raster = state;
		}
	}
	void SetViewport(float width, float height)
	{
		if (!bound.viewportSet || width != bound.viewport[0] || height != bound.viewport[1])
		{
			const D3D11_VIEWPORT vp = { 0.f, 0.f, width, height, 0.f, 1.f };
			deviceContext->RSSetViewports(1, &vp);
			bound.viewport[0] = width;
			bound.viewport[1] = height;
			bound.viewportSet = true;
		}
	}
	void SetScissor(const D3D11_RECT& rect)
	{
		if (!bound.scissorSet || rect.left != bound.scissor.left || rect.top != bound.scissor.top
				|| rect.right != bound.scissor.right || rect.bottom != bound.scissor.bottom)
		{
			deviceContext->RSSetScissorRects(1, &rect);
			bound.scissor = rect;
			bound.scissorSet = true;
		}
	}
	/* (no factor: what the state is given without one, all ones) */
	void SetBlend(ID3D11BlendState *state, const float *factor = nullptr)
	{
		static const float ones[4] = { 1.f, 1.f, 1.f, 1.f };
		if (factor == nullptr)
			factor = ones;
		if (state != bound.blend || memcmp(factor, bound.blendFactor, sizeof(bound.blendFactor)))
		{
			deviceContext->OMSetBlendState(state, factor, 0xffffffff);
			bound.blend = state;
			memcpy(bound.blendFactor, factor, sizeof(bound.blendFactor));
		}
	}
	void SetDepthStencil(ID3D11DepthStencilState *state, UINT ref = 0)
	{
		if (state != bound.depth || ref != bound.stencilRef)
		{
			deviceContext->OMSetDepthStencilState(state, ref);
			bound.depth = state;
			bound.stencilRef = ref;
		}
	}
	/* One render target (or none) and the depth and stencil (or none).
	 * Unordered access views bound before are not looked at again. */
	void SetTargets(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
	{
		if (target != bound.target || depth != bound.depthTarget || bound.uavsSet)
		{
			deviceContext->OMSetRenderTargets(target != nullptr ? 1 : 0, target != nullptr ? &target : nullptr, depth);
			TargetsChanged(target, depth);
			bound.uavsSet = false;
		}
	}
	/* ...and with the unordered access views at slots 1 to 3, their counters kept */
	void SetTargetsAndUAVs(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth, ID3D11UnorderedAccessView * const uavs[3])
	{
		if (target != bound.target || depth != bound.depthTarget || !bound.uavsSet
				|| memcmp(uavs, bound.uavs, sizeof(bound.uavs)))
		{
			const UINT counts[3] = { (UINT)-1, (UINT)-1, (UINT)-1 };
			deviceContext->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, depth, 1, 3, uavs, counts);
			TargetsChanged(target, depth);
			memcpy(bound.uavs, uavs, sizeof(bound.uavs));
			bound.uavsSet = true;
		}
	}

	/* The picture the next video_refresh is to show. With version 2 it is
	 * handed over here, with the context taken: from then on it is the
	 * frontend's (IsHandedOver()), until the frame has been shown. */
	void SetFrame(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view);
	bool IsHandedOver(ID3D11Texture2D *texture) const { return v2 && handedOver && texture == frameTexture.get(); }
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
	VramBudget vramBudget;
	UINT vendor = 0;
	bool immediate = false;
	pD3DCompile compile = nullptr;
	HMODULE compilerModule = nullptr;
	bool formatSupported[5] = {};
	bool mipsSupported[5] = {};

	DX11Shaders shaders;
	DX11Samplers samplers;
	DX11Quad quad;
	DX11BlendStates blendStates;
	/* times the frontend has used the context, and how many of them the
	 * context has been cleared for */
	u32 stateChanges = 0;
	u32 stateCleared = 0;

	static const UINT ResourceSlots = 6;
	struct Bound
	{
		ID3D11InputLayout *layout;
		ID3D11Buffer *vertexBuffer[3];
		UINT stride[3];
		ID3D11Buffer *indexBuffer;
		D3D11_PRIMITIVE_TOPOLOGY topology;
		ID3D11VertexShader *vs;
		ID3D11PixelShader *ps;
		ID3D11Buffer *vsConstants;
		ID3D11Buffer *psConstants[3];
		ID3D11ShaderResourceView *srv[ResourceSlots];
		ID3D11SamplerState *sampler[4];
		ID3D11RasterizerState *raster;
		float viewport[2];
		bool viewportSet;
		D3D11_RECT scissor;
		bool scissorSet;
		ID3D11BlendState *blend;
		float blendFactor[4];
		ID3D11DepthStencilState *depth;
		UINT stencilRef;
		ID3D11RenderTargetView *target;
		ID3D11DepthStencilView *depthTarget;
		ID3D11UnorderedAccessView *uavs[3];
		bool uavsSet;
	} bound;
	/* What ClearState() leaves */
	void ClearBound()
	{
		memset(&bound, 0, sizeof(bound));
		bound.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		for (float& f : bound.blendFactor)
			f = 1.f;
	}
	/* A view of a texture bound to be drawn into is unbound by Direct3D
	 * from wherever it is bound to be read: the views at the slots textures
	 * and pictures go to are not known after that */
	void TargetsChanged(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
	{
		bound.target = target;
		bound.depthTarget = depth;
		ID3D11ShaderResourceView * const unknown = reinterpret_cast<ID3D11ShaderResourceView *>(1);
		bound.srv[0] = bound.srv[3] = bound.srv[4] = unknown;
	}

	ComPtr<ID3D11Texture2D> frameTexture;
	ComPtr<ID3D11ShaderResourceView> frameView;
	bool handedOver = false;

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
