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
#include <cstring>
#include "comptr.h"
#include "dx11_shaders.h"

/* A picture drawn onto what is bound, as a rectangle: the last frame's
 * into the next, the VMUs' screens and the crosshairs over the picture,
 * the framebuffer the game wrote itself. */
class DX11Quad
{
public:
	bool Init(ID3D11Device *device, ID3D11DeviceContext *deviceContext, DX11Shaders *shaders)
	{
		this->deviceContext = deviceContext;
		vertexShader = shaders->GetQuadVertexShader();
		pixelShader = shaders->GetQuadPixelShader();
		ID3DBlob *blob = shaders->GetQuadVertexShaderBlob();
		if (!vertexShader || !pixelShader || blob == nullptr)
			return false;

		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 2 * sizeof(float), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		if (FAILED(device->CreateInputLayout(layout, 2, blob->GetBufferPointer(), blob->GetBufferSize(), inputLayout.put())))
			return false;

		D3D11_RASTERIZER_DESC rast{};
		rast.FillMode = D3D11_FILL_SOLID;
		rast.CullMode = D3D11_CULL_NONE;
		rast.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rast, rasterizerState.put())))
			return false;

		D3D11_DEPTH_STENCIL_DESC ds{};
		ds.DepthEnable = FALSE;
		ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
		ds.StencilEnable = FALSE;
		ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		ds.BackFace = ds.FrontFace;
		if (FAILED(device->CreateDepthStencilState(&ds, depthStencilState.put())))
			return false;

		D3D11_BLEND_DESC blend{};
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blend, opaque.put())))
			return false;

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(Vertex) * 4;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		return SUCCEEDED(device->CreateBuffer(&desc, nullptr, vertexBuffer.put()));
	}

	void Term()
	{
		vertexBuffer.reset();
		opaque.reset();
		depthStencilState.reset();
		rasterizerState.reset();
		inputLayout.reset();
		pixelShader = nullptr;
		vertexShader = nullptr;
		deviceContext = nullptr;
	}

	/* At x, y, w, h in clip space (y going up), from (u0, v0) to (u1, v1)
	 * of the texture (v going down). Without a blend state, it is put
	 * there as it is. */
	void Draw(ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler,
			float x = -1.f, float y = -1.f, float w = 2.f, float h = 2.f,
			float u0 = 0.f, float v0 = 0.f, float u1 = 1.f, float v1 = 1.f,
			ID3D11BlendState *blend = nullptr, const float *blendFactor = nullptr)
	{
		if (!vertexBuffer)
			return;
		const Vertex vertices[4] = {
			{ x,     y,     u0, v1 },
			{ x,     y + h, u0, v0 },
			{ x + w, y,     u1, v1 },
			{ x + w, y + h, u1, v0 },
		};
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (FAILED(deviceContext->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		memcpy(mapped.pData, vertices, sizeof(vertices));
		deviceContext->Unmap(vertexBuffer, 0);

		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		deviceContext->IASetInputLayout(inputLayout);
		deviceContext->IASetVertexBuffers(0, 1, vertexBuffer.addr(), &stride, &offset);
		deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		deviceContext->VSSetShader(vertexShader, nullptr, 0);
		deviceContext->PSSetShader(pixelShader, nullptr, 0);
		deviceContext->GSSetShader(nullptr, nullptr, 0);
		deviceContext->RSSetState(rasterizerState);
		deviceContext->OMSetDepthStencilState(depthStencilState, 0);
		deviceContext->OMSetBlendState(blend != nullptr ? blend : opaque.get(), blendFactor, 0xffffffff);
		deviceContext->PSSetShaderResources(0, 1, &view);
		deviceContext->PSSetSamplers(0, 1, &sampler);
		deviceContext->Draw(4, 0);
	}

private:
	struct Vertex {
		float x, y, u, v;
	};

	ID3D11DeviceContext *deviceContext = nullptr;
	ID3D11VertexShader *vertexShader = nullptr;
	ID3D11PixelShader *pixelShader = nullptr;
	ComPtr<ID3D11InputLayout> inputLayout;
	ComPtr<ID3D11Buffer> vertexBuffer;
	ComPtr<ID3D11RasterizerState> rasterizerState;
	ComPtr<ID3D11DepthStencilState> depthStencilState;
	ComPtr<ID3D11BlendState> opaque;
};
