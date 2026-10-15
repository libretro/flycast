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
#include <algorithm>
#include <vector>
#include "../comptr.h"
#include "../dx11context_lr.h"

extern u64 pixel_buffer_size;

/* The per-pixel lists: every translucent pixel drawn, in one large buffer
 * (as large as the "Accumulation pixel buffer size" option says, or as
 * large as the card will make it), and for each screen pixel the first of
 * its list. */
class DX11OITBuffers
{
public:
	bool Init(ID3D11Device *device)
	{
		this->device = device;
		pixelsView.reset();
		pixels.reset();
		counterView.reset();
		counter.reset();
		constants.reset();
		size = pixel_buffer_size;

		/* (the most a resource can be is a quarter of the card's memory, but
		 * at least 128 MB and at most 2 GB: it is asked for no larger than
		 * that, and halved until the card makes it, a whole number of
		 * pixels each time) */
		const UINT largest = D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u;
		const UINT smallest = D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = (UINT)std::min<u64>(size, largest) & ~15u;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = 16;	// sizeof(struct Pixel)
		HRESULT hr = device->CreateBuffer(&desc, nullptr, pixels.put());
		while (FAILED(hr) && desc.ByteWidth > smallest)
		{
			desc.ByteWidth = std::max((desc.ByteWidth / 2) & ~15u, smallest);
			hr = device->CreateBuffer(&desc, nullptr, pixels.put());
		}
		if (FAILED(hr))
		{
			WARN_LOG(RENDERER, "DX11: pixel buffer creation failed (%x)", (unsigned)hr);
			pixels.reset();
			return false;
		}
		if (desc.ByteWidth < size)
			NOTICE_LOG(RENDERER, "DX11: pixel buffer of %u MB", desc.ByteWidth / 1024 / 1024);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_UNKNOWN;
		uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav.Buffer.FirstElement = 0;
		uav.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;
		hr = device->CreateUnorderedAccessView(pixels, &uav, pixelsView.put());
		if (FAILED(hr))
		{
			WARN_LOG(RENDERER, "DX11: pixel buffer view creation failed (%x)", (unsigned)hr);
			pixelsView.reset();
			pixels.reset();
			return false;
		}

		/* How many of them are taken, counted up by the shaders as the
		 * Vulkan renderer's are; and how many there are room for */
		D3D11_BUFFER_DESC counterDesc{};
		counterDesc.ByteWidth = 16;
		counterDesc.Usage = D3D11_USAGE_DEFAULT;
		counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC counterUav{};
		counterUav.Format = DXGI_FORMAT_R32_TYPELESS;
		counterUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		counterUav.Buffer.NumElements = 4;
		counterUav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		const u32 count[4] = { uav.Buffer.NumElements, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = count;
		D3D11_BUFFER_DESC constDesc{};
		constDesc.ByteWidth = 16;
		constDesc.Usage = D3D11_USAGE_IMMUTABLE;
		constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(device->CreateBuffer(&counterDesc, nullptr, counter.put()))
				|| FAILED(device->CreateUnorderedAccessView(counter, &counterUav, counterView.put()))
				|| FAILED(device->CreateBuffer(&constDesc, &initial, constants.put())))
		{
			WARN_LOG(RENDERER, "DX11: pixel counter creation failed");
			Term();
			return false;
		}
		return true;
	}

	/* The first pixels of the lists, for a picture of at least this size.
	 * They start as no list at all, and the pass that puts the lists
	 * together leaves them that way. */
	bool Resize(u32 width, u32 height)
	{
		if (pointers && width <= this->width && height <= this->height)
			return true;
		width = std::max(width, this->width);
		height = std::max(height, this->height);
		pointersView.reset();
		pointers.reset();
		this->width = this->height = 0;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		std::vector<u32> none((size_t)width * height, 0xffffffffu);
		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = none.data();
		initial.SysMemPitch = width * sizeof(u32);
		if (FAILED(device->CreateTexture2D(&desc, &initial, pointers.put()))
				|| FAILED(device->CreateUnorderedAccessView(pointers, nullptr, pointersView.put())))
		{
			WARN_LOG(RENDERER, "DX11: per-pixel list heads creation failed (%d x %d)", width, height);
			pointersView.reset();
			pointers.reset();
			return false;
		}
		this->width = width;
		this->height = height;
		return true;
	}

	/* With one render target, the lists at u1, u2 and u3, and how large
	 * they can be at b2 */
	void Bind(DX11Context *context, ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
	{
		ID3D11UnorderedAccessView * const views[] = { pixelsView, pointersView, counterView };
		context->SetTargetsAndUAVs(target, depth, views);
		context->SetPSConstants(2, constants);
	}
	/* The buffer starts filling from its beginning again */
	void ResetCount(ID3D11DeviceContext *deviceContext)
	{
		const UINT zero[4] = {};
		deviceContext->ClearUnorderedAccessViewUint(counterView, zero);
	}

	bool IsValid() const { return pixelsView && pointersView && counterView; }
	u64 GetSize() const { return size; }

	void Term()
	{
		pointersView.reset();
		pointers.reset();
		pixelsView.reset();
		pixels.reset();
		counterView.reset();
		counter.reset();
		constants.reset();
		width = height = 0;
	}

private:
	ID3D11Device *device = nullptr;
	u64 size = 0;
	ComPtr<ID3D11Buffer> pixels;
	ComPtr<ID3D11UnorderedAccessView> pixelsView;
	ComPtr<ID3D11Buffer> counter;
	ComPtr<ID3D11UnorderedAccessView> counterView;
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11Texture2D> pointers;
	ComPtr<ID3D11UnorderedAccessView> pointersView;
	u32 width = 0;
	u32 height = 0;
};
