#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include "dx11_renderer.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include <algorithm>
#include <cmath>

extern int screen_width, screen_height;

static const D3D11_INPUT_ELEMENT_DESC MainLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
static const D3D11_INPUT_ELEMENT_DESC PositionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

static ComPtr<ID3D11Buffer> makeConstantBuffer(ID3D11Device *device, UINT size)
{
	ComPtr<ID3D11Buffer> buffer;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = (size + 15) & ~15u;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
		buffer.reset();
	return buffer;
}

static bool makeTexture(ID3D11Device *device, u32 width, u32 height, DXGI_FORMAT format, UINT bind,
		ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, const void *data = nullptr, UINT pitch = 0)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind;
	D3D11_SUBRESOURCE_DATA initial{};
	initial.pSysMem = data;
	initial.SysMemPitch = pitch;
	view.reset();
	if (FAILED(device->CreateTexture2D(&desc, data != nullptr ? &initial : nullptr, texture.put()))
			|| FAILED(device->CreateShaderResourceView(texture, nullptr, view.put())))
	{
		WARN_LOG(RENDERER, "DX11: texture creation failed (%d x %d, format %d)", width, height, (int)format);
		view.reset();
		texture.reset();
		return false;
	}
	return true;
}

bool DX11Renderer::Init()
{
	DX11Context *context = DX11Context::Instance();
	device = context->GetDevice();
	deviceContext = context->GetDeviceContext();
	if (device == nullptr || deviceContext == nullptr)
	{
		WARN_LOG(RENDERER, "DX11: no device");
		return false;
	}
	shaders = &context->GetShaders();
	samplers = &context->GetSamplers();
	blendStates = &context->GetBlendStates();

	DX11Lock lock;
	bool success = shaders->GetVertexShader(true) != nullptr && shaders->GetModVolVertexShader() != nullptr;
	ID3DBlob *blob = shaders->GetVertexShaderBlob();
	success = success && blob != nullptr
			&& SUCCEEDED(device->CreateInputLayout(MainLayout, ARRAY_SIZE(MainLayout), blob->GetBufferPointer(), blob->GetBufferSize(), mainInputLayout.put()));
	blob = shaders->GetModVolVertexShaderBlob();
	success = success && blob != nullptr
			&& SUCCEEDED(device->CreateInputLayout(PositionLayout, ARRAY_SIZE(PositionLayout), blob->GetBufferPointer(), blob->GetBufferSize(), positionInputLayout.put()));

	vtxConstants = makeConstantBuffer(device, sizeof(VertexConstants));
	fragConstants = makeConstantBuffer(device, sizeof(FragmentConstants));
	polyConstants = makeConstantBuffer(device, sizeof(PolyConstants));
	success = success && vtxConstants && fragConstants && polyConstants;

	// No culling, front faces culled, back faces: ISP's culling mode 0 or 1, 2, 3
	for (int i = 0; i < 3; i++)
	{
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = i == 0 ? D3D11_CULL_NONE : i == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
		desc.FrontCounterClockwise = TRUE;
		desc.ScissorEnable = TRUE;
		// (the depth is the shaders' own, the clip space's is always 1)
		desc.DepthClipEnable = FALSE;
		success = success && SUCCEEDED(device->CreateRasterizerState(&desc, rasterizers[i].put()));
	}

	// The fog table, two rows of 128: see CheckFogTexture()
	success = success && makeTexture(device, 128, 2, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, fogTexture, fogView);
	fogUploadedValid = false;
	// The palette, a row of 1024 colours
	success = success && makeTexture(device, 1024, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, paletteTexture, paletteView);
	palette_updated = true;

	havePicture = false;
	lastScreen = -1;
	screenWidth = screenHeight = 0;
	stateChanges = context->GetStateChanges() - 1;
	if (!success)
	{
		WARN_LOG(RENDERER, "DX11 renderer initialization failed");
		return false;
	}
	return true;
}

void DX11Renderer::Term()
{
	DX11Lock lock;
	if (deviceContext != nullptr)
	{
		deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView *none[4] = {};
		deviceContext->PSSetShaderResources(0, 4, none);
	}
	DX11Context::Instance()->SetFrame(nullptr, nullptr);
	dx11_rtt_term();
	texCache.Clear();
	overlay.Term();
	depthStencilStates.Term();
	fbView.reset();
	fbTexture.reset();
	restoredPicture.reset();
	screenTargets.clear();
	stencilSRV.reset();
	depthSRV.reset();
	depthViewReadOnly.reset();
	depthViewReadStencil.reset();
	depthView.reset();
	depthTex.reset();
	depthWidth = depthHeight = 0;
	paletteView.reset();
	paletteTexture.reset();
	fogView.reset();
	fogTexture.reset();
	for (auto& rasterizer : rasterizers)
		rasterizer.reset();
	polyConstants.reset();
	fragConstants.reset();
	vtxConstants.reset();
	modVolBuffer.reset();
	indexBuffer.reset();
	vertexBuffer.reset();
	vertexBufferSize = indexBufferSize = modVolBufferSize = 0;
	positionInputLayout.reset();
	mainInputLayout.reset();
	device = nullptr;
	deviceContext = nullptr;
}

/* The size rendered at has changed, and the context is the same: the
 * screen's pictures are made again at the new size when the next frame is
 * drawn, and the last picture goes with them - EnsureScreen() */
void DX11Renderer::Resize(int w, int h)
{
}

u64 DX11Renderer::GetTexture(TSP tsp, TCW tcw, int area)
{
	DX11Texture* tf = texCache.getTextureCacheData(tsp, tcw, area);

	if (tf->IsNew())
		tf->Create();
	//update if needed
	if (tf->NeedsUpdate())
		tf->Update();
	else if (tf->IsCustomTextureAvailable())
		tf->CheckCustomTexture();

	return tf->GetIntId();
}

/* A picture kept from the context before: the first render to the screen
 * starts from it */
void DX11Renderer::CheckKeptPicture()
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == nullptr)
		return;
	ComPtr<ID3D11Texture2D> texture;
	if (!makeTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, texture, restoredPicture,
			pixels, width * 4))
		restoredPicture.reset();
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles.cpp */
void DX11Renderer::CheckFogTexture()
{
	u8 texData[256];
	MakeFogTexture(texData);
	if (fogUploadedValid && !memcmp(fogUploaded, texData, sizeof(fogUploaded)))
		return;
	memcpy(fogUploaded, texData, sizeof(fogUploaded));
	fogUploadedValid = true;
	deviceContext->UpdateSubresource(fogTexture, 0, nullptr, texData, 128, 256);
}

void DX11Renderer::CheckPaletteTexture()
{
	if (!palette_updated)
		return;
	palette_updated = false;
	deviceContext->UpdateSubresource(paletteTexture, 0, nullptr, palette32_ram, 1024 * 4, 1024 * 4);
}

bool DX11Renderer::Process(TA_context* ctx)
{
	if (device == nullptr)
		return false;
	DX11Lock lock;

	if (!ctx->rend.isRTT)
		overlay.Prepare();
	if (!ctx->rend.isRTT && !ctx->rend.isRenderFramebuffer)
		CheckKeptPicture();

	if (ctx->rend.isRenderFramebuffer)
		return UploadFramebuffer();

	if (KillTex)
	{
		/* what was rendered to a texture and is not in video memory yet goes there before its picture does */
		rtt_watch_flush();
		texCache.Clear();
	}

	bool result = ta_parse_vdrc(ctx);

	texCache.Cleanup();

	if (result)
	{
		CheckFogTexture();
		CheckPaletteTexture();
	}
	return result;
}

/* ---- What is set, and what was ---- */

void DX11Renderer::ResetState()
{
	memset(&state, 0, sizeof(state));
	state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	currentScissor = D3D11_RECT{ -1, -1, -1, -1 };
	stateChanges = DX11Context::Instance()->GetStateChanges();
}

void DX11Renderer::SetVS(ID3D11VertexShader *vs)
{
	if (vs != state.vs)
	{
		deviceContext->VSSetShader(vs, nullptr, 0);
		state.vs = vs;
	}
}

void DX11Renderer::SetPS(ID3D11PixelShader *ps)
{
	if (ps != state.ps)
	{
		deviceContext->PSSetShader(ps, nullptr, 0);
		state.ps = ps;
	}
}

void DX11Renderer::SetBlend(ID3D11BlendState *blend)
{
	if (blend != state.blend)
	{
		deviceContext->OMSetBlendState(blend, nullptr, 0xffffffff);
		state.blend = blend;
	}
}

void DX11Renderer::SetDepthStencil(ID3D11DepthStencilState *depth, UINT ref)
{
	if (depth != state.depth || ref != state.stencilRef)
	{
		deviceContext->OMSetDepthStencilState(depth, ref);
		state.depth = depth;
		state.stencilRef = ref;
	}
}

void DX11Renderer::SetTexture(int slot, ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler)
{
	if (view != state.srv[slot])
	{
		deviceContext->PSSetShaderResources(slot, 1, &view);
		state.srv[slot] = view;
	}
	if (sampler != state.sampler[slot])
	{
		deviceContext->PSSetSamplers(slot, 1, &sampler);
		state.sampler[slot] = sampler;
	}
}

void DX11Renderer::SetTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (topology != state.topology)
	{
		deviceContext->IASetPrimitiveTopology(topology);
		state.topology = topology;
	}
}

void DX11Renderer::SetCullMode(int mode)
{
	ID3D11RasterizerState *raster = rasterizers[mode == 3 ? 2 : mode == 2 ? 1 : 0];
	if (raster != state.raster)
	{
		deviceContext->RSSetState(raster);
		state.raster = raster;
	}
}

void DX11Renderer::SetScissor(const D3D11_RECT& rect)
{
	if (rect.left != currentScissor.left || rect.top != currentScissor.top
			|| rect.right != currentScissor.right || rect.bottom != currentScissor.bottom)
	{
		deviceContext->RSSetScissorRects(1, &rect);
		currentScissor = rect;
	}
}

void DX11Renderer::SetPolyConstants(const PolyConstants& constants)
{
	if (state.polyValid && !memcmp(&constants, &state.poly, sizeof(constants)))
		return;
	SetConstants(polyConstants.get(), constants);
	state.poly = constants;
	state.polyValid = true;
}

void DX11Renderer::BindMainGeometry()
{
	const UINT stride = sizeof(Vertex);
	const UINT offset = 0;
	deviceContext->IASetInputLayout(mainInputLayout);
	deviceContext->IASetVertexBuffers(0, 1, vertexBuffer.addr(), &stride, &offset);
	deviceContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
}

/* ---- Clipping ---- */

TileClipping DX11Renderer::SetTileClip(u32 val, D3D11_RECT& clipRect)
{
	int rect[4] = {};
	TileClipping clipmode = ::GetTileClip(val, matrices.GetViewportMatrix(), rect);
	if (clipmode == TileClipping::Outside)
		IntersectClipRect(rect, baseScissor.left, baseScissor.top,
				baseScissor.right - baseScissor.left, baseScissor.bottom - baseScissor.top);
	clipRect.left = rect[0];
	clipRect.top = rect[1];
	clipRect.right = rect[0] + rect[2];
	clipRect.bottom = rect[1] + rect[3];

	return clipmode;
}

void DX11Renderer::SetBaseScissor()
{
	bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !matrices.IsClipped();
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
		{
			baseScissor = D3D11_RECT{ 0, 0, 640, 480 };
		}
		else
		{
			float min_x = xform_x(&matrices.GetScissorMatrix(), pvrrc.fb_X_CLIP.min);
			float min_y = xform_y(&matrices.GetScissorMatrix(), pvrrc.fb_Y_CLIP.min);
			float width = xform_w(&matrices.GetScissorMatrix(), pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
			float height = xform_h(&matrices.GetScissorMatrix(), pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
			if (width < 0)
			{
				min_x += width;
				width = -width;
			}
			if (height < 0)
			{
				min_y += height;
				height = -height;
			}
			const LONG x = (LONG)std::max(lroundf(min_x), 0L);
			const LONG y = (LONG)std::max(lroundf(min_y), 0L);
			baseScissor = D3D11_RECT{ x, y, x + (LONG)std::max(lroundf(width), 0L), y + (LONG)std::max(lroundf(height), 0L) };
		}
	}
	else
	{
		baseScissor = D3D11_RECT{ 0, 0, (LONG)screen_width, (LONG)screen_height };
	}
	currentScissor = D3D11_RECT{ -1, -1, -1, -1 };
}

// Direct3D takes a flat shaded triangle's colours from its first vertex.
// On Dreamcast the last vertex is the provoking one so it is copied onto the
// first (as in the Vulkan renderers).
void DX11Renderer::SetProvokingVertices()
{
	auto setProvokingVertex = [](const List<PolyParam>& list) {
		for (int i = 0; i < list.used(); i++)
		{
			const PolyParam& pp = list.head()[i];
			if (!pp.pcw.Gouraud && pp.count > 2)
			{
				for (u32 n = 0; n < pp.count - 2; n++)
				{
					Vertex *vertex = &pvrrc.verts.head()[pvrrc.idx.head()[pp.first + n]];
					Vertex *lastVertex = &pvrrc.verts.head()[pvrrc.idx.head()[pp.first + n + 2]];
					memcpy(vertex->col, lastVertex->col, 4);
					memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
					memcpy(vertex->col1, lastVertex->col1, 4);
					memcpy(vertex->spc1, lastVertex->spc1, 4);
				}
			}
		}
	};
	setProvokingVertex(pvrrc.global_param_op);
	setProvokingVertex(pvrrc.global_param_pt);
	setProvokingVertex(pvrrc.global_param_tr);
}

void DX11Renderer::SortTriangles()
{
	sortedPolys.resize(pvrrc.render_passes.used());
	sortedIndexes.resize(pvrrc.render_passes.used());
	sortedIndexCount = 0;
	RenderPass previousPass = {};

	for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];
		sortedIndexes[render_pass].clear();
		if (current_pass.autosort)
		{
			GenSorted(previousPass.tr_count, current_pass.tr_count - previousPass.tr_count, sortedPolys[render_pass], sortedIndexes[render_pass], true);
			for (auto& poly : sortedPolys[render_pass])
				poly.first += sortedIndexCount;
			sortedIndexCount += sortedIndexes[render_pass].size();
		}
		else
			sortedPolys[render_pass].clear();
		previousPass = current_pass;
	}
}

/* ---- Buffers ---- */

static bool ensureBuffer(ID3D11Device *device, ComPtr<ID3D11Buffer>& buffer, u32& size, u32 needed, UINT bind)
{
	if (buffer && size >= needed)
		return true;
	u32 newSize = std::max(size, 64u * 1024u);
	while (newSize < needed)
		newSize *= 2;
	buffer.reset();
	size = 0;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = newSize;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = bind;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
	{
		WARN_LOG(RENDERER, "DX11: buffer creation failed (%d bytes)", newSize);
		buffer.reset();
		return false;
	}
	size = newSize;
	return true;
}

bool DX11Renderer::UploadGeometry()
{
	D3D11_MAPPED_SUBRESOURCE mapped;

	if (!ensureBuffer(device, vertexBuffer, vertexBufferSize, pvrrc.verts.bytes(), D3D11_BIND_VERTEX_BUFFER)
			|| FAILED(deviceContext->Map(vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, pvrrc.verts.head(), pvrrc.verts.bytes());
	deviceContext->Unmap(vertexBuffer, 0);

	const u32 indexBytes = pvrrc.idx.bytes() + sortedIndexCount * sizeof(u32);
	if (!ensureBuffer(device, indexBuffer, indexBufferSize, indexBytes, D3D11_BIND_INDEX_BUFFER)
			|| FAILED(deviceContext->Map(indexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	u8 *dst = (u8 *)mapped.pData;
	memcpy(dst, pvrrc.idx.head(), pvrrc.idx.bytes());
	dst += pvrrc.idx.bytes();
	for (const std::vector<u32>& idx : sortedIndexes)
		if (!idx.empty())
		{
			memcpy(dst, idx.data(), idx.size() * sizeof(u32));
			dst += idx.size() * sizeof(u32);
		}
	deviceContext->Unmap(indexBuffer, 0);

	if (pvrrc.modtrig.used() != 0)
	{
		if (!ensureBuffer(device, modVolBuffer, modVolBufferSize, pvrrc.modtrig.bytes(), D3D11_BIND_VERTEX_BUFFER)
				|| FAILED(deviceContext->Map(modVolBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		memcpy(mapped.pData, pvrrc.modtrig.head(), pvrrc.modtrig.bytes());
		deviceContext->Unmap(modVolBuffer, 0);
	}
	return true;
}

void DX11Renderer::MakeFragmentConstants(FragmentConstants& constants)
{
	memset(&constants, 0, sizeof(constants));
	//VERT and RAM fog color constants
	u8* fog_colvert_bgra = (u8*)&FOG_COL_VERT;
	u8* fog_colram_bgra = (u8*)&FOG_COL_RAM;
	constants.fogColVert[0] = fog_colvert_bgra[2] / 255.0f;
	constants.fogColVert[1] = fog_colvert_bgra[1] / 255.0f;
	constants.fogColVert[2] = fog_colvert_bgra[0] / 255.0f;

	constants.fogColRam[0] = fog_colram_bgra[2] / 255.0f;
	constants.fogColRam[1] = fog_colram_bgra[1] / 255.0f;
	constants.fogColRam[2] = fog_colram_bgra[0] / 255.0f;

	//Fog density constant
	u8* fog_density = (u8*)&FOG_DENSITY;
	float fog_den_mant = fog_density[1] / 128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	s32 fog_den_exp = (s8)fog_density[0];
	constants.fogDensity = fog_den_mant * powf(2.0f, fog_den_exp);

	constants.colorClampMin[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	constants.colorClampMin[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	constants.colorClampMin[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	constants.colorClampMin[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	constants.colorClampMax[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	constants.colorClampMax[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	constants.colorClampMax[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	constants.colorClampMax[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

	// half a step under the reference: see the same in gles/gles.cpp
	constants.alphaTestValue = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	constants.shadeScale = FPU_SHAD_SCALE.scale_factor / 256.f;
}

void DX11Renderer::SetupFrame()
{
	/* The normal matrix, made for a picture whose first line is the top
	 * one and whose clip space has y going up: the other way up from the
	 * Vulkan renderers', whose matrices these are, and nothing else */
	const xform& normal = matrices.GetNormalMatrix();
	VertexConstants vtx = {
		{ normal.sx, -normal.sy, normal.sz, 0.f },
		{ normal.tx, -normal.ty, normal.tz, 0.f },
	};
	SetConstants(vtxConstants.get(), vtx);
	FragmentConstants frag;
	MakeFragmentConstants(frag);
	SetConstants(fragConstants.get(), frag);

	deviceContext->VSSetConstantBuffers(0, 1, vtxConstants.addr());
	ID3D11Buffer *buffers[] = { fragConstants, polyConstants };
	deviceContext->PSSetConstantBuffers(0, 2, buffers);
	ID3D11ShaderResourceView *views[] = { paletteView, fogView };
	deviceContext->PSSetShaderResources(1, 2, views);
	ID3D11SamplerState *point = samplers->GetPointSampler();
	deviceContext->PSSetSamplers(1, 1, &point);
	state.srv[1] = paletteView;
	state.srv[2] = fogView;
	state.sampler[1] = point;
	deviceContext->GSSetShader(nullptr, nullptr, 0);
	BindMainGeometry();
	// (what was pushed, for a polygon that pushes nothing: anything at all)
	PolyConstants zero = {};
	SetPolyConstants(zero);
}

bool DX11Renderer::EnsureDepth(u32 width, u32 height)
{
	if (depthTex && depthWidth >= width && depthHeight >= height)
		return true;
	width = std::max(width, depthWidth);
	height = std::max(height, depthHeight);
	stencilSRV.reset();
	depthSRV.reset();
	depthViewReadOnly.reset();
	depthViewReadStencil.reset();
	depthView.reset();
	depthTex.reset();
	depthWidth = depthHeight = 0;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	// (a float for the depth, as the Vulkan renderers have it where they can)
	desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, depthTex.put())))
	{
		WARN_LOG(RENDERER, "DX11: depth buffer creation failed (%d x %d)", width, height);
		depthTex.reset();
		return false;
	}
	D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
	dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	bool success = SUCCEEDED(device->CreateDepthStencilView(depthTex, &dsv, depthView.put()));
	dsv.Flags = D3D11_DSV_READ_ONLY_STENCIL;
	success = SUCCEEDED(device->CreateDepthStencilView(depthTex, &dsv, depthViewReadStencil.put())) && success;
	dsv.Flags = D3D11_DSV_READ_ONLY_STENCIL | D3D11_DSV_READ_ONLY_DEPTH;
	success = SUCCEEDED(device->CreateDepthStencilView(depthTex, &dsv, depthViewReadOnly.put())) && success;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	success = SUCCEEDED(device->CreateShaderResourceView(depthTex, &srv, depthSRV.put())) && success;
	srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
	success = SUCCEEDED(device->CreateShaderResourceView(depthTex, &srv, stencilSRV.put())) && success;
	if (!success)
		WARN_LOG(RENDERER, "DX11: depth buffer view creation failed");
	depthWidth = width;
	depthHeight = height;
	return true;
}

/* ---- The screen ---- */

bool DX11Renderer::EnsureScreen(u32 width, u32 height)
{
	const unsigned count = DX11Context::Instance()->GetPresentCount();

	if (width != screenWidth || height != screenHeight)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is (Vulkan's carried picture): nothing is read back or
		 * loaded again. */
		if (havePicture && lastScreen >= 0 && (size_t)lastScreen < screenTargets.size())
			restoredPicture = screenTargets[lastScreen].view;
		screenTargets.clear();
		havePicture = false;
		lastScreen = -1;
		screenWidth = width;
		screenHeight = height;
	}
	if (screenTargets.size() > count)
	{
		if (lastScreen >= (int)count)
		{
			if (havePicture)
				restoredPicture = screenTargets[lastScreen].view;
			havePicture = false;
			lastScreen = -1;
		}
		screenTargets.resize(count);
	}
	while (screenTargets.size() < count)
		screenTargets.push_back(ScreenTarget());
	ScreenTarget& target = screenTargets[screenIndex];
	if (!target.texture)
	{
		if (!makeTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
				target.texture, target.view)
				|| FAILED(device->CreateRenderTargetView(target.texture, nullptr, target.target.put())))
		{
			target = ScreenTarget();
			return false;
		}
		const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		deviceContext->ClearRenderTargetView(target.target, black);
	}
	return EnsureDepth(width, height);
}

void DX11Renderer::DrawLastPicture(u32 width, u32 height)
{
	ID3D11ShaderResourceView *view = LastPictureView();
	if (view == nullptr)
		return;
	const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
	deviceContext->RSSetViewports(1, &vp);
	/* (the picture kept from another context can be of another size) */
	DX11Context::Instance()->GetQuad().Draw(view, havePicture ? samplers->GetPointSampler() : samplers->GetLinearSampler());
	ID3D11ShaderResourceView *none = nullptr;
	deviceContext->PSSetShaderResources(0, 1, &none);
	ResetState();
}

bool DX11Renderer::BeginScreen()
{
	screenIndex = (int)DX11Context::Instance()->GetPresentIndex();
	if (!EnsureScreen(screen_width, screen_height))
		return false;
	ScreenTarget& target = screenTargets[screenIndex];
	targetView = target.target;
	targetWidth = screenWidth;
	targetHeight = screenHeight;

	matrices.CalcMatrices(&pvrrc);
	ResetState();
	if (!BindsScreen())
	{
		SetBaseScissor();
		return true;
	}
	deviceContext->OMSetRenderTargets(1, target.target.addr(), depthView);
	deviceContext->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (KeepsLastPicture())
	{
		if (havePicture && lastScreen == screenIndex)
			;
		else if (havePicture)
			deviceContext->CopyResource(target.texture, screenTargets[lastScreen].texture);
		else
		{
			const float black[4] = { 0.f, 0.f, 0.f, 1.f };
			deviceContext->ClearRenderTargetView(target.target, black);
			DrawLastPicture(screenWidth, screenHeight);
		}
	}
	else
	{
		float color[4] = { 0.f, 0.f, 0.f, 1.f };
		if (pvrrc.clearFramebuffer)
		{
			color[0] = VO_BORDER_COL.Red / 255.f;
			color[1] = VO_BORDER_COL.Green / 255.f;
			color[2] = VO_BORDER_COL.Blue / 255.f;
		}
		deviceContext->ClearRenderTargetView(target.target, color);
	}
	const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)screenWidth, (float)screenHeight, 0.f, 1.f };
	deviceContext->RSSetViewports(1, &vp);
	SetBaseScissor();
	return true;
}

void DX11Renderer::EndScreen()
{
	ScreenTarget& target = screenTargets[screenIndex];
	// what goes over the picture, onto it
	deviceContext->OMSetRenderTargets(1, target.target.addr(), nullptr);
	const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)screenWidth, (float)screenHeight, 0.f, 1.f };
	deviceContext->RSSetViewports(1, &vp);
	overlay.Draw(screenWidth, screenHeight);
	deviceContext->OMSetRenderTargets(0, nullptr, nullptr);

	havePicture = true;
	lastScreen = screenIndex;
	restoredPicture.reset();
	DX11Context::Instance()->SetFrame(target.texture, target.view);
}

void DX11Renderer::KeepPicture()
{
	if (!havePicture || lastScreen < 0 || (size_t)lastScreen >= screenTargets.size())
		return;
	u8 *pixels = last_picture_keep(screenWidth, screenHeight);
	if (pixels != nullptr && !dx11_read_picture(screenTargets[lastScreen].texture, screenWidth, screenHeight, pixels))
		last_picture_drop();
}

/* ---- Renders to textures ---- */

bool DX11Renderer::BeginRTT()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	matrices.CalcMatrices(&pvrrc);
	ResetState();

	rttAddr = FB_W_SOF1 & VRAM_MASK;
	// the frame from its first pixel, as far as the pixel clip goes (see TransformMatrix)
	const u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	const u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	u32 widthPow2 = 8;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;
	const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	upscaledWidth *= scale;
	upscaledHeight *= scale;
	widthPow2 *= scale;
	heightPow2 *= scale;

	if (!EnsureDepth(widthPow2, heightPow2))
		return false;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_watch.h */
	dx11_rtt_supersede(rttAddr, clipX, clipY, clipWidth, clipHeight);
	// TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1
	TCW tcw = { { rttAddr >> 3, 0, 0, 1 } };
	switch (FB_W_CTRL.fb_packmode) {
	case 0:
	case 3:
		tcw.PixelFmt = Pixel1555;
		break;
	case 1:
		tcw.PixelFmt = Pixel565;
		break;
	case 2:
		tcw.PixelFmt = Pixel4444;
		break;
	}

	TSP tsp = { 0 };
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < origWidth; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < origHeight; tsp.TexV++);

	rttTexture = texCache.getTextureCacheData(tsp, tcw);
	if (rttTexture->IsNew())
		rttTexture->Create();

	if (rttTexture->spare)
	{
		D3D11_TEXTURE2D_DESC desc;
		rttTexture->spare->GetDesc(&desc);
		if (desc.Width != widthPow2 || desc.Height != heightPow2 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM
				|| (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || desc.MipLevels != 1)
		{
			rttTexture->spareTarget.reset();
			rttTexture->spareView.reset();
			rttTexture->spare.reset();
		}
	}
	if (!rttTexture->spare
			&& !makeTexture(device, widthPow2, heightPow2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
					rttTexture->spare, rttTexture->spareView))
		return false;
	if (!rttTexture->spareTarget
			&& FAILED(device->CreateRenderTargetView(rttTexture->spare, nullptr, rttTexture->spareTarget.put())))
	{
		WARN_LOG(RENDERER, "DX11: render to texture target creation failed");
		return false;
	}

	targetView = rttTexture->spareTarget;
	targetWidth = upscaledWidth;
	targetHeight = upscaledHeight;

	/* (a texture this render draws with is not to be bound while it is drawn into) */
	ID3D11ShaderResourceView *none[4] = {};
	deviceContext->PSSetShaderResources(0, 4, none);
	if (BindsScreen())
	{
		deviceContext->OMSetRenderTargets(1, rttTexture->spareTarget.addr(), depthView);
		const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		deviceContext->ClearRenderTargetView(rttTexture->spareTarget, black);
		deviceContext->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
		const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)upscaledWidth, (float)upscaledHeight, 0.f, 1.f };
		deviceContext->RSSetViewports(1, &vp);
	}
	// only the pixel clip is written
	baseScissor = D3D11_RECT{ (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale) };
	currentScissor = D3D11_RECT{ -1, -1, -1, -1 };
	return true;
}

void DX11Renderer::EndRTT()
{
	deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
	rttTexture->SwapWithSpare();

	rttTexture->dirty = 0;
	rttTexture->stride_read = 0;	// (rendered, not read)
	libCore_vramlock_Lock(rttTexture->sa_tex, rttTexture->sa + rttTexture->size - 1, rttTexture);
	/* video memory gets it when something first touches it there */
	dx11_rtt_watch(rttTexture, rttAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	rttTexture = nullptr;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

bool DX11Renderer::UploadFramebuffer()
{
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	PixelBuffer<u32> pb;
	int width;
	int height;
	ReadFramebuffer(pb, width, height);

	if (!fbTexture || width != fbWidth || height != fbHeight)
	{
		if (!makeTexture(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, fbTexture, fbView))
			return false;
		fbWidth = width;
		fbHeight = height;
	}
	deviceContext->UpdateSubresource(fbTexture, 0, nullptr, pb.data(), width * 4, width * height * 4);
	return true;
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
void DX11Renderer::DrawFramebuffer()
{
	screenIndex = (int)DX11Context::Instance()->GetPresentIndex();
	if (!EnsureScreen(screen_width, screen_height))
		return;
	ScreenTarget& target = screenTargets[screenIndex];
	matrices.CalcMatrices(&pvrrc);
	deviceContext->OMSetRenderTargets(1, target.target.addr(), nullptr);
	const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	deviceContext->ClearRenderTargetView(target.target, black);
	const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)screenWidth, (float)screenHeight, 0.f, 1.f };
	deviceContext->RSSetViewports(1, &vp);

	const xform& viewport = matrices.GetViewportMatrix();
	const float x0 = xform_x(&viewport, 0.f), x1 = xform_x(&viewport, 640.f);
	const float y0 = xform_y(&viewport, 0.f), y1 = xform_y(&viewport, 480.f);
	DX11Context::Instance()->GetQuad().Draw(fbView, samplers->GetLinearSampler(),
			x0 * 2.f / screenWidth - 1.f, 1.f - y1 * 2.f / screenHeight,
			(x1 - x0) * 2.f / screenWidth, (y1 - y0) * 2.f / screenHeight);
	ID3D11ShaderResourceView *none = nullptr;
	deviceContext->PSSetShaderResources(0, 1, &none);
	EndScreen();
}

/* ---- The frame ---- */

bool DX11Renderer::Render()
{
	if (device == nullptr)
		return false;
	if (pvrrc.isRTT && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		/* A pixel clip that ends before it starts holds no pixel: the
		 * chip writes nothing. (Its size would be negative here.) */
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}
	/* the frontend may still be showing what is in the screen's next
	 * picture: it is waited for, before the context is taken */
	if (!pvrrc.isRTT)
		DX11Context::Instance()->WaitPresentIndex();
	DX11Lock lock;
	deviceContext->ClearState();

	if (pvrrc.isRenderFramebuffer)
	{
		DrawFramebuffer();
		return true;
	}

	SortTriangles();
	if (pvrrc.isRTT ? !BeginRTT() : !BeginScreen())
	{
		rttTexture = nullptr;
		return false;
	}
	SetProvokingVertices();
	if (!UploadGeometry())
	{
		deviceContext->OMSetRenderTargets(0, nullptr, nullptr);
		rttTexture = nullptr;
		return false;
	}
	SetupFrame();

	DrawFrame();

	if (pvrrc.isRTT)
		EndRTT();
	else
		EndScreen();

	return !pvrrc.isRTT;
}

/* ---- The polygons, as the Vulkan renderer's Drawer draws them ---- */

void DX11Renderer::DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
		ShadowPass shadowPass, const D3D11_RECT *within)
{
	D3D11_RECT scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	if (within != nullptr)
	{
		// only the part of it inside this rectangle, and nothing if there is none
		const D3D11_RECT& clip = tileClip == TileClipping::Outside ? scissorRect : baseScissor;
		const D3D11_RECT r = {
			std::max(within->left, clip.left), std::max(within->top, clip.top),
			std::min(within->right, clip.right), std::min(within->bottom, clip.bottom)
		};
		if (r.right <= r.left || r.bottom <= r.top)
			return;
		SetScissor(r);
	}
	else if (tileClip == TileClipping::Outside)
		SetScissor(scissorRect);
	else
		SetScissor(baseScissor);

	float trilinearAlpha = 1.f;
	if (poly.tsp.FilterMode > 1 && poly.pcw.Texture && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1)
	{
		trilinearAlpha = 0.25f * (poly.tsp.MipMapD & 0x3);
		if (poly.tsp.FilterMode == 2)
			// Trilinear pass A
			trilinearAlpha = 1.f - trilinearAlpha;
	}
	const bool palette = BaseTextureCacheData::IsGpuHandledPaletted(poly.tsp, poly.tcw);
	float palette_index = 0.f;
	if (palette)
	{
		if (poly.tcw.PixelFmt == PixelPal4)
			palette_index = (float(poly.tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			palette_index = (float((poly.tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}
	if (tileClip == TileClipping::Inside || trilinearAlpha != 1.f || palette)
	{
		const PolyConstants constants = {
			{ (float)scissorRect.left, (float)scissorRect.top, (float)scissorRect.right, (float)scissorRect.bottom },
			trilinearAlpha,
			palette_index,
			{ 0.f, 0.f }
		};
		SetPolyConstants(constants);
	}

	if (poly.pcw.Texture && poly.texid != 0)
	{
		DX11Texture *texture = (DX11Texture *)(uintptr_t)poly.texid;
		SetTexture(0, texture->textureView, samplers->GetSampler(poly.tsp));
	}

	DX11FragmentShaderParams params = {};
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = poly.tcw.PixelFmt == PixelBumpMap;
	/* By the polygon alone, as the Vulkan renderer has it: the clamping
	 * limits change, and a shader is kept by the polygon's bit only */
	params.clamping = poly.tsp.ColorClamp;
	params.insideClipTest = (poly.tileclip >> 28) == 3;
	params.fog = poly.tsp.FogCtrl;
	params.gouraud = poly.pcw.Gouraud;
	params.ignoreTexAlpha = poly.tsp.IgnoreTexA || poly.tcw.PixelFmt == Pixel565;
	params.offset = poly.pcw.Offset;
	params.shaderInstr = poly.tsp.ShadInstr;
	params.texture = poly.pcw.Texture;
	params.trilinear = poly.pcw.Texture && poly.tsp.FilterMode > 1 && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1;
	params.useAlpha = poly.tsp.UseAlpha;
	params.palette = palette;
	params.shadowed = shadowPass != ShadowPass::None;
	SetVS(shaders->GetVertexShader(poly.pcw.Gouraud));
	SetPS(shaders->GetPixelShader(params));

	// Depth and stencil
	int depthFunc;
	if (listType == ListType_Punch_Through || sortTriangles)
		depthFunc = 6;	// greater or equal
	else
		depthFunc = poly.isp.DepthMode;
	bool depthWrite;
	// Sorted translucent polygons, per triangle or per strip, do not write
	// depth: a strip drawn in sorted order would hide parts of the ones after it.
	if (sortTriangles)
		depthWrite = false;
	// Z Write Disable seems to be ignored for punch-through.
	// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
	else if (listType == ListType_Punch_Through)
		depthWrite = true;
	else
		depthWrite = !poly.isp.ZWriteDis;

	const bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	if (shadowPass != ShadowPass::None)
	{
		/* Drawn again for its shadow: where this polygon is the one showing
		 * (same depth), takes shadows (stencil bit 7) and is in a volume
		 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
		 * marked may be drawn over is what tells the two kinds apart. */
		if (shadowPass == ShadowPass::LaterWins)
			SetDepthStencil(depthStencilStates.Get(true, false, 2, DX11Stencil::ShadowLaterWins), 0xC1);
		else
			SetDepthStencil(depthStencilStates.Get(true, false, 2, DX11Stencil::ShadowEarlierWins), 0x81);
	}
	else if (shadowed)
		SetDepthStencil(depthStencilStates.Get(true, depthWrite, depthFunc, DX11Stencil::Shadow), poly.pcw.Shadow != 0 ? 0x80 : 0);
	else
		SetDepthStencil(depthStencilStates.Get(true, depthWrite, depthFunc));

	// Apparently punch-through polys support blending, or at least some combinations
	// An opaque polygon is blended by its instructions too when it is in a
	// list continuation; the ones of the first render pass have had theirs
	// set to source x 1 and destination x 0 when the list was read.
	if (shadowPass == ShadowPass::None
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| poly.tsp.SrcInstr != 1 || poly.tsp.DstInstr != 0))
		SetBlend(blendStates->Get(true, poly.tsp.SrcInstr, poly.tsp.DstInstr));
	else
		SetBlend(blendStates->Get(false));

	SetCullMode(poly.isp.CullMode);
	SetTopology(sortTriangles && !settings.pvr.Emulation.AlphaSortMode ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	deviceContext->DrawIndexed(count, first, 0);
}

// The polygons of a list that take shadows, drawn again where they are in a
// modifier volume: see rend/shadows.h.
void DX11Renderer::DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
		const ScreenBounds& area, const D3D11_RECT& scissor)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (CanDrawShadowed(listType, pp) && PolyOverlaps(pp, area))
			DrawPoly(listType, false, *pp, pp->first, pp->count,
					ShadowLaterWins(listType, pp) ? ShadowPass::LaterWins : ShadowPass::EarlierWins,
					&scissor);
}

void DX11Renderer::DrawSorted(const std::vector<SortTrigDrawParam>& polys, bool multipass)
{
	for (const SortTrigDrawParam& param : polys)
		DrawPoly(ListType_Translucent, true, *param.ppid, pvrrc.idx.used() + param.first, param.count);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		// Sorted translucent polygons are drawn without writing depth. Where
		// a later render pass needs their depth (Cosmic Smash), it is written
		// now, for those that do not have Z write disabled.
		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		deviceContext->IASetInputLayout(positionInputLayout);
		deviceContext->IASetVertexBuffers(0, 1, vertexBuffer.addr(), &stride, &offset);
		SetVS(shaders->GetModVolVertexShader());
		SetPS(shaders->GetModVolPixelShader());
		SetBlend(blendStates->Get(false, 1, 0, false));
		SetDepthStencil(depthStencilStates.Get(true, true, 6));
		SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (const SortTrigDrawParam& param : polys)
		{
			if (param.count <= 2 || param.ppid->isp.ZWriteDis)
				continue;
			D3D11_RECT scissorRect;
			SetScissor(SetTileClip(param.ppid->tileclip, scissorRect) == TileClipping::Outside ? scissorRect : baseScissor);
			SetCullMode(param.ppid->isp.CullMode);
			deviceContext->DrawIndexed(param.count, pvrrc.idx.used() + param.first, 0);
		}
		BindMainGeometry();
	}
}

void DX11Renderer::DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
	{
		if (pp->count <= 2)
			continue;
		/* Depth mode "never" with nothing to force it to another: nothing of
		 * it is drawn, and no stencil is written where its depth test fails.
		 * Not drawn at all. */
		if (pp->isp.DepthMode == 0 && listType != ListType_Punch_Through && !sortTriangles)
			continue;
		DrawPoly(listType, sortTriangles, *pp, pp->first, pp->count);
	}
}

void DX11Renderer::DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass)
{
	if (count == 0 || pvrrc.modtrig.used() == 0 || !modVolBuffer)
		return;

	const UINT stride = sizeof(float) * 3;
	const UINT offset = 0;
	deviceContext->IASetInputLayout(positionInputLayout);
	deviceContext->IASetVertexBuffers(0, 1, modVolBuffer.addr(), &stride, &offset);
	SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetVS(shaders->GetModVolVertexShader());
	SetPS(shaders->GetModVolPixelShader());
	SetBlend(blendStates->Get(false, 1, 0, false));
	SetScissor(baseScissor);

	ModifierVolumeParam* params = &pvrrc.global_param_mvo.head()[first];

	int mod_base = -1;

	for (int cmv = 0; cmv < count; cmv++)
	{
		ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		{
			D3D11_RECT clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				SetScissor(clipRect);
			else
				SetScissor(baseScissor);
		}

		if (mod_base == -1)
			mod_base = param.first;

		SetCullMode(param.isp.CullMode);
		if (!param.isp.VolumeLast && mv_mode > 0)
			SetDepthStencil(depthStencilStates.Get(true, false, 4, DX11Stencil::Or), 2);	// OR'ing (open volume or quad)
		else
			SetDepthStencil(depthStencilStates.Get(true, false, 4, DX11Stencil::Xor), 2);	// XOR'ing (closed volume)
		deviceContext->Draw(param.count * 3, param.first * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			SetDepthStencil(depthStencilStates.Get(false, false, 7, mv_mode == 1 ? DX11Stencil::Inclusion : DX11Stencil::Exclusion), 1);
			deviceContext->Draw((param.first + param.count - mod_base) * 3, mod_base * 3);
			mod_base = -1;
		}
	}
	SetScissor(baseScissor);
	BindMainGeometry();

	// The polygons that take shadows, again, where they are in a volume
	const ScreenBounds area = ModVolBounds(first, count);
	if (!area.empty())
	{
		int rect[4];
		BoundsToScissor(area, matrices.GetViewportMatrix(), rect);
		const D3D11_RECT scissor = { rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3] };
		DrawShadowed(ListType_Opaque, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count, area, scissor);
		DrawShadowed(ListType_Punch_Through, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count, area, scissor);
		SetScissor(baseScissor);
	}

	// What is left: darkened as it is. The background polygon is the
	// rectangle drawn: the first four vertices' positions.
	const UINT vstride = sizeof(Vertex);
	deviceContext->IASetInputLayout(positionInputLayout);
	deviceContext->IASetVertexBuffers(0, 1, vertexBuffer.addr(), &vstride, &offset);
	SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	SetVS(shaders->GetModVolVertexShader());
	SetPS(shaders->GetModVolPixelShader());
	SetBlend(blendStates->GetAlpha());
	SetDepthStencil(depthStencilStates.Get(false, false, 7, DX11Stencil::Final), 0x81);
	SetCullMode(0);
	deviceContext->DrawIndexed(4, 0, 0);
	BindMainGeometry();
}

void DX11Renderer::DrawFrame()
{
	RenderPass previous_pass = {};
	for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d autosort %d", render_pass + 1,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.mvo_count - previous_pass.mvo_count, current_pass.autosort);
		DrawList(ListType_Opaque, false, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(ListType_Punch_Through, false, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);
		DrawModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, previous_pass, current_pass);
		if (current_pass.autosort)
		{
			if (!settings.pvr.Emulation.AlphaSortMode)
				DrawSorted(sortedPolys[render_pass], render_pass < pvrrc.render_passes.used() - 1);
			else
			{
				SortPParams(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				DrawList(ListType_Translucent, true, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			}
		}
		else
			DrawList(ListType_Translucent, false, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		previous_pass = current_pass;
	}
}

Renderer* rend_D3D11()
{
	return new DX11Renderer();
}
