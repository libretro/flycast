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
#include "dx11context_lr.h"
#include <dxgi.h>
#include <dxgi1_4.h>
#include "rend/dxgi_budget.h"

DX11Context DX11Context::instance;

static const DXGI_FORMAT textureFormats[5] = {
	DXGI_FORMAT_B5G6R5_UNORM,		// TextureType::_565
	DXGI_FORMAT_B5G5R5A1_UNORM,		// TextureType::_5551
	DXGI_FORMAT_B4G4R4A4_UNORM,		// TextureType::_4444
	DXGI_FORMAT_R8G8B8A8_UNORM,		// TextureType::_8888
	DXGI_FORMAT_R8_UNORM,			// TextureType::_8
};

bool DX11Context::Init(const struct retro_hw_render_interface_d3d11 *iface)
{
	Term();
	if (iface == nullptr || iface->device == nullptr || iface->context == nullptr)
		return false;

	iface->device->AddRef();
	device.reset(iface->device);
	iface->context->AddRef();
	deviceContext.reset(iface->context);
	featureLevel = device->GetFeatureLevel();
	immediate = deviceContext->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;

	compile = iface->D3DCompile;
	if (compile == nullptr)
	{
		/* (a frontend that does not hand one over: the system's) */
		compilerModule = LoadLibraryA("d3dcompiler_47.dll");
		if (compilerModule != nullptr)
			compile = (pD3DCompile)(void *)GetProcAddress(compilerModule, "D3DCompile");
		if (compile == nullptr)
		{
			ERROR_LOG(RENDERER, "DX11: no shader compiler (d3dcompiler_47.dll)");
			Term();
			return false;
		}
	}

	if (iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D11_VERSION_2
			&& iface->lock_context != nullptr && iface->unlock_context != nullptr && iface->set_texture != nullptr
			&& iface->get_sync_index != nullptr && iface->get_sync_index_mask != nullptr && iface->wait_sync_index != nullptr)
	{
		v2 = true;
		handle = iface->handle;
		lock_context = iface->lock_context;
		unlock_context = iface->unlock_context;
		set_texture = iface->set_texture;
		get_sync_index = iface->get_sync_index;
		get_sync_index_mask = iface->get_sync_index_mask;
		wait_sync_index = iface->wait_sync_index;
	}

	{
		UINT vendor = 0;
		IDXGIDevice *dxgiDevice = nullptr;
		vramBudget = VramBudget();
		if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)))
		{
			IDXGIAdapter *adapter = nullptr;
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
			{
				const bool haveDesc = SUCCEEDED(adapter->GetDesc(&desc));
				if (haveDesc)
					vendor = desc.VendorId;
				vramBudget = dxgi_vram_budget(adapter, haveDesc ? &desc : nullptr);
				adapter->Release();
			}
			dxgiDevice->Release();
		}
		NOTICE_LOG(RENDERER, "DX11: feature level %x, vendor %04x, %s context, interface version %d",
				(unsigned)featureLevel, vendor, immediate ? "immediate" : "deferred", v2 ? 2 : 1);
		vram_budget_log("DX11", vramBudget);
	}
	if (!immediate)
		WARN_LOG(RENDERER, "DX11: the frontend's context is a deferred one: renders to textures cannot reach video memory");

	for (int i = 0; i < 5; i++)
	{
		UINT support = 0;
		const UINT sample = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
		const UINT mips = D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
		if (FAILED(device->CheckFormatSupport(textureFormats[i], &support)))
			support = 0;
		formatSupported[i] = (support & sample) == sample;
		mipsSupported[i] = formatSupported[i] && (support & mips) == mips;
	}
	// what has to be, for the rest
	if (!formatSupported[(int)TextureType::_8888] || !formatSupported[(int)TextureType::_8]
			|| featureLevel < D3D_FEATURE_LEVEL_10_0)
	{
		ERROR_LOG(RENDERER, "DX11: feature level 10.0 or later needed");
		Term();
		return false;
	}

	/* (what is bound is not known until the context is first taken and cleared) */
	stateChanges = stateCleared + 1;
	shaders.Init(device, compile);
	if (!quad.Init(device, deviceContext, &shaders))
	{
		ERROR_LOG(RENDERER, "DX11: initialization failed");
		Term();
		return false;
	}
	return true;
}

void DX11Context::Term()
{
	frameView.reset();
	frameTexture.reset();
	handedOver = false;
	quad.Term();
	blendStates.Term();
	samplers.Term();
	shaders.Term();
	if (deviceContext && immediate)
	{
		DX11Lock lock;
		deviceContext->ClearState();
		deviceContext->Flush();
	}
	deviceContext.reset();
	device.reset();
	if (compilerModule != nullptr)
		FreeLibrary(compilerModule);
	compilerModule = nullptr;
	compile = nullptr;
	immediate = false;
	v2 = false;
	handle = nullptr;
	lock_context = nullptr;
	unlock_context = nullptr;
	set_texture = nullptr;
	get_sync_index = nullptr;
	get_sync_index_mask = nullptr;
	wait_sync_index = nullptr;
	lockDepth = 0;
}

bool DX11Context::IsFormatSupported(TextureType type) const
{
	return (unsigned)type < 5 && formatSupported[(int)type];
}

bool DX11Context::CanGenerateMips(TextureType type) const
{
	return (unsigned)type < 5 && mipsSupported[(int)type];
}

void DX11Context::Lock()
{
	if (lockDepth++ != 0)
		return;
	if (v2 && lock_context(handle))
		/* the frontend has drawn with it since: nothing is as it was left */
		stateChanges++;
	if (stateChanges != stateCleared && deviceContext)
	{
		deviceContext->ClearState();
		ClearBound();
		stateCleared = stateChanges;
	}
}

void DX11Context::Unlock()
{
	if (lockDepth > 0 && --lockDepth == 0 && v2)
		unlock_context(handle);
}

void DX11Context::SetFrame(ID3D11Texture2D *texture, ID3D11ShaderResourceView *view)
{
	if (texture != nullptr)
		texture->AddRef();
	frameTexture.reset(texture);
	if (view != nullptr)
		view->AddRef();
	frameView.reset(view);
	/* (with the context taken, by the render that drew it: no other lock
	 * of it is needed for this) */
	handedOver = v2 && texture != nullptr;
	if (handedOver)
		set_texture(handle, texture);
}

void DX11Context::Present()
{
	if (!deviceContext)
		return;
	if (v2)
	{
		/* (a frame with no render to the screen in it shows the last one again) */
		if (!handedOver)
		{
			DX11Lock lock;
			set_texture(handle, frameTexture);
		}
		/* from here on the frontend has what it was handed: the next
		 * render waits for it to be done with it (WaitPresentIndex()) */
		handedOver = false;
	}
	else
	{
		DX11Lock lock;
		/* (a view of a picture that is also bound to be drawn into is not
		 * bound) */
		SetTargets(nullptr, nullptr);
		SetPSResource(0, frameView);
		/* the frontend draws with the context in video_refresh */
		StateChanged();
	}
}

unsigned DX11Context::GetPresentCount() const
{
	if (!v2)
		return 1;
	unsigned mask = get_sync_index_mask(handle);
	unsigned count = 1;
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned DX11Context::GetPresentIndex() const
{
	if (!v2)
		return 0;
	const unsigned index = get_sync_index(handle);
	return index < 32 ? index : 0;
}

void DX11Context::WaitPresentIndex()
{
	if (v2 && lockDepth == 0)
		wait_sync_index(handle);
}

void DX11Quad::Draw(ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler,
		float x, float y, float w, float h, float u0, float v0, float u1, float v1,
		ID3D11BlendState *blend, const float *blendFactor)
{
	if (!vertexBuffer)
		return;
	const Vertex vertices[4] = {
		{ x,     y,     u0, v1 },
		{ x,     y + h, u0, v0 },
		{ x + w, y,     u1, v1 },
		{ x + w, y + h, u1, v0 },
	};
	DX11Context *context = DX11Context::Instance();
	D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
	/* (a deferred context's commands are each a list of their own) */
	if (next >= QuadCount || !context->CanReadBack())
	{
		mode = D3D11_MAP_WRITE_DISCARD;
		next = 0;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(deviceContext->Map(vertexBuffer, 0, mode, 0, &mapped)))
		return;
	memcpy((Vertex *)mapped.pData + next * 4, vertices, sizeof(vertices));
	deviceContext->Unmap(vertexBuffer, 0);

	context->SetInputLayout(inputLayout);
	context->SetVertexBuffer(vertexBuffer, sizeof(Vertex));
	context->SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->SetVS(vertexShader);
	context->SetPS(pixelShader);
	context->SetRasterizer(rasterizerState);
	context->SetDepthStencil(depthStencilState);
	context->SetBlend(blend != nullptr ? blend : opaque.get(), blendFactor);
	context->SetPSResource(0, view);
	context->SetPSSampler(0, sampler);
	deviceContext->Draw(4, next * 4);
	next++;
}
