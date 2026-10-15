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
#include "dx9_renderer.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include <algorithm>
#include <cmath>

extern int screen_width, screen_height;

/* (the vertices' colours are R G B A bytes; D3DCOLOR has them the other
 * way round, which the vertex shader undoes) */
static const D3DVERTEXELEMENT9 MainElements[] =
{
	{ 0, (WORD)offsetof(Vertex, x),       D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, (WORD)offsetof(Vertex, col),     D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
	{ 0, (WORD)offsetof(Vertex, vtx_spc), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    1 },
	{ 0, (WORD)offsetof(Vertex, u),       D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	D3DDECL_END()
};
static const D3DVERTEXELEMENT9 PositionElements[] =
{
	{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	D3DDECL_END()
};

/* As much as the tile accelerator's lists can hold of a frame (ta_ctx.h),
 * and the sorted triangles' indexes, three for each index at most */
static void maxGeometry(u32& vertexBytes, u32& indexBytes)
{
	const u32 scale = std::max<u32>(1, ta_data_size() / TA_DATA_SIZE);
	vertexBytes = 4 * 1024 * 1024 * scale + 16384 * scale * (u32)sizeof(ModTriangle) + 4096;
	indexBytes = 120 * 1024 * scale * 4 * (u32)sizeof(u32) + 4096;
}

bool DX9Renderer::MakeBuffers(u32 vertexBytes, u32 indexBytes)
{
	vertexBuffer.reset();
	indexBuffer.reset();
	vertexBufferSize = indexBufferSize = 0;
	vertexBufferUsed = indexBufferUsed = 0;
	if (FAILED(device->CreateVertexBuffer(vertexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
				vertexBuffer.put(), nullptr))
			|| FAILED(device->CreateIndexBuffer(indexBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT,
				indexBuffer.put(), nullptr)))
	{
		WARN_LOG(RENDERER, "DX9: buffer creation failed (%d and %d bytes)", vertexBytes, indexBytes);
		vertexBuffer.reset();
		indexBuffer.reset();
		return false;
	}
	vertexBufferSize = vertexBytes;
	indexBufferSize = indexBytes;
	return true;
}

bool DX9Renderer::Init()
{
	context = DX9Context::Instance();
	device = context->GetDevice();
	if (device == nullptr)
	{
		WARN_LOG(RENDERER, "DX9: no device");
		return false;
	}
	shaders = &context->GetShaders();

	DX9Lock lock;
	bool success = SUCCEEDED(device->CreateVertexDeclaration(MainElements, mainDecl.put()))
			&& SUCCEEDED(device->CreateVertexDeclaration(PositionElements, positionDecl.put()));
	u32 vertexBytes, indexBytes;
	maxGeometry(vertexBytes, indexBytes);
	success = success && MakeBuffers(vertexBytes, indexBytes);

	// The fog table, two rows of 128: see CheckFogTexture()
	fogTexture = context->MakeTexture(128, 2, 1, context->WriteUsage(), D3DFMT_A8);
	fogUploadedValid = false;
	// The palette, a row of 1024 colours
	paletteTexture = context->MakeTexture(1024, 1, 1, context->WriteUsage(), D3DFMT_A8R8G8B8);
	palette_updated = true;
	success = success && fogTexture && paletteTexture;

	lastPicture.reset();
	restoredPicture.reset();
	screenWidth = screenHeight = 0;
	polyEpoch = frameEpoch = 0;
	heldFromProcess = false;
	/* The depth buffer and the screen's pictures now, at the size the
	 * screen is, and not when the first frame comes */
	if (success && screen_width > 0 && screen_height > 0)
	{
		success = EnsureDepth(screen_width, screen_height);
		screenWidth = screen_width;
		screenHeight = screen_height;
		screenSlots.resize(context->GetPresentCount());
		for (ScreenSlot& slot : screenSlots)
			success = success && MakeScreenTarget(slot.target, screenWidth, screenHeight);
	}
	if (!success)
	{
		WARN_LOG(RENDERER, "DX9 renderer initialization failed");
		return false;
	}
	return true;
}

void DX9Renderer::Term()
{
	DX9Lock lock;
	if (device != nullptr)
		context->SetFrame(nullptr);
	dx9_rtt_term();
	texCache.Clear();
	overlay.Term();
	fbTexture.reset();
	restoredPicture.reset();
	lastPicture.reset();
	screenSlots.clear();
	depthSurface.reset();
	depthWidth = depthHeight = 0;
	paletteTexture.reset();
	fogTexture.reset();
	vertexBuffer.reset();
	indexBuffer.reset();
	vertexBufferSize = indexBufferSize = 0;
	positionDecl.reset();
	mainDecl.reset();
	device = nullptr;
}

/* The size rendered at has changed, and the device is the same: the
 * screen's pictures are made again at the new size when they are next
 * drawn into, and the last picture goes with them - EnsureScreen() */
void DX9Renderer::Resize(int w, int h)
{
}

u64 DX9Renderer::GetTexture(TSP tsp, TCW tcw, int area)
{
	DX9Texture* tf = texCache.getTextureCacheData(tsp, tcw, area);

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
void DX9Renderer::CheckKeptPicture()
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == nullptr)
		return;
	context->Recycle(restoredPicture);
	restoredPicture = context->MakeTexture(width, height, 1, context->WriteUsage(), D3DFMT_A8R8G8B8);
	D3DLOCKED_RECT rect;
	if (!restoredPicture || FAILED(restoredPicture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
	{
		restoredPicture.reset();
		return;
	}
	for (int y = 0; y < height; y++)
		memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, pixels + (size_t)y * width * 4, (size_t)width * 4);
	restoredPicture->UnlockRect(0);
	restoredSwap = true;
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles.cpp */
void DX9Renderer::CheckFogTexture()
{
	u8 texData[256];
	MakeFogTexture(texData);
	if (fogUploadedValid && !memcmp(fogUploaded, texData, sizeof(fogUploaded)))
		return;
	D3DLOCKED_RECT rect;
	if (FAILED(fogTexture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
		return;
	memcpy(rect.pBits, texData, 128);
	memcpy((u8 *)rect.pBits + rect.Pitch, texData + 128, 128);
	fogTexture->UnlockRect(0);
	memcpy(fogUploaded, texData, sizeof(fogUploaded));
	fogUploadedValid = true;
}

void DX9Renderer::CheckPaletteTexture()
{
	if (!palette_updated)
		return;
	D3DLOCKED_RECT rect;
	if (FAILED(paletteTexture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
		return;
	// (R G B A bytes, which the shader reads the other way round)
	memcpy(rect.pBits, palette32_ram, 1024 * 4);
	paletteTexture->UnlockRect(0);
	palette_updated = false;
}

bool DX9Renderer::Process(TA_context* ctx)
{
	if (device == nullptr)
		return false;
	/* The device is taken here and kept for the Render() that follows,
	 * which is then one lock with this. Except when a render to the screen
	 * is handed over to the other thread when this is done (threaded
	 * rendering): the frontend is waited for before the screen's picture
	 * is drawn into, and never with the device taken - and not here,
	 * which would keep the emulation waiting too. */
	const bool keep = ctx->rend.isRTT || !settings.rend.ThreadedRendering;
	if (keep && !ctx->rend.isRTT)
		context->WaitPresentIndex();
	context->Lock();
	const bool result = ProcessLocked(ctx);
	heldFromProcess = keep && result;
	if (!heldFromProcess)
		context->Unlock();
	return result;
}

bool DX9Renderer::ProcessLocked(TA_context* ctx)
{
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

/* ---- What is set ---- */

/* ISP's culling mode 0 or 1: none; 2: counter-clockwise faces; 3: clockwise ones */
void DX9Renderer::SetCullMode(int mode)
{
	context->SetRenderState(D3DRS_CULLMODE, mode == 3 ? D3DCULL_CW : mode == 2 ? D3DCULL_CCW : D3DCULL_NONE);
}

void DX9Renderer::SetPolyConstants(const PolyConstants& constants)
{
	if (polyEpoch == context->Epoch() && !memcmp(&constants, &polyUploaded, sizeof(constants)))
		return;
	device->SetPixelShaderConstantF(DX9PC_ClipTest, constants.clipTest, 2);
	polyUploaded = constants;
	polyEpoch = context->Epoch();
}

/* ---- Clipping ---- */

TileClipping DX9Renderer::SetTileClip(u32 val, RECT& clipRect)
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

void DX9Renderer::SetBaseScissor()
{
	bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !matrices.IsClipped();
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
		{
			baseScissor = RECT{ 0, 0, 640, 480 };
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
			baseScissor = RECT{ x, y, x + (LONG)std::max(lroundf(width), 0L), y + (LONG)std::max(lroundf(height), 0L) };
		}
	}
	else
	{
		baseScissor = RECT{ 0, 0, (LONG)screen_width, (LONG)screen_height };
	}
}

// Direct3D takes a flat shaded triangle's colours from its first vertex.
// On Dreamcast the last vertex is the provoking one so it is copied onto the
// first (as in the Vulkan renderers).
void DX9Renderer::SetProvokingVertices()
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

void DX9Renderer::SortTriangles()
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

bool DX9Renderer::UploadGeometry()
{
	const u32 vertexBytes = pvrrc.verts.bytes();
	const u32 indexBytes = pvrrc.idx.bytes() + sortedIndexCount * sizeof(u32);
	const u32 modVolBytes = pvrrc.modtrig.bytes();
	const u32 modVolSize = sizeof(float) * 3;
	/* Each at a multiple of its size, as the draws count them; after what
	 * the renders before this one have put there, and not over any of it -
	 * the buffer started again (discarded) when it is full */
	auto lay = [&](u32 start, u32& vertices, u32& modVols) {
		vertices = (start + sizeof(Vertex) - 1) / sizeof(Vertex) * sizeof(Vertex);
		modVols = (vertices + vertexBytes + modVolSize - 1) / modVolSize * modVolSize;
		return modVols + modVolBytes;
	};
	u32 vertices, modVols;
	u32 vertexEnd = lay(vertexBufferUsed, vertices, modVols);
	DWORD vertexFlags = D3DLOCK_NOOVERWRITE;
	if (vertexEnd > vertexBufferSize)
	{
		vertexFlags = D3DLOCK_DISCARD;
		vertexEnd = lay(0, vertices, modVols);
	}
	u32 indexStart = indexBufferUsed;
	DWORD indexFlags = D3DLOCK_NOOVERWRITE;
	if (indexStart + indexBytes > indexBufferSize)
	{
		indexFlags = D3DLOCK_DISCARD;
		indexStart = 0;
	}
	if (vertexEnd > vertexBufferSize || indexBytes > indexBufferSize)
	{
		/* (more than the tile accelerator can make of a frame: it cannot
		 * happen, but if it does, larger buffers once) */
		WARN_LOG(RENDERER, "DX9: a frame of %d bytes of vertices and %d of indexes: buffers made larger", vertexEnd, indexBytes);
		if (!MakeBuffers(std::max(vertexEnd, vertexBufferSize) * 2, std::max(indexBytes, indexBufferSize) * 2))
			return false;
		vertexFlags = indexFlags = D3DLOCK_DISCARD;
		vertexEnd = lay(0, vertices, modVols);
		indexStart = 0;
	}

	void *data;
	if (vertexEnd > vertices)
	{
		if (FAILED(vertexBuffer->Lock(vertices, vertexEnd - vertices, &data, vertexFlags)))
			return false;
		memcpy(data, pvrrc.verts.head(), vertexBytes);
		if (modVolBytes != 0)
			memcpy((u8 *)data + (modVols - vertices), pvrrc.modtrig.head(), modVolBytes);
		vertexBuffer->Unlock();
	}
	if (indexBytes != 0)
	{
		if (FAILED(indexBuffer->Lock(indexStart, indexBytes, &data, indexFlags)))
			return false;
		u8 *dst = (u8 *)data;
		memcpy(dst, pvrrc.idx.head(), pvrrc.idx.bytes());
		dst += pvrrc.idx.bytes();
		for (const std::vector<u32>& idx : sortedIndexes)
			if (!idx.empty())
			{
				memcpy(dst, idx.data(), idx.size() * sizeof(u32));
				dst += idx.size() * sizeof(u32);
			}
		indexBuffer->Unlock();
	}
	vertexBufferUsed = vertexEnd;
	indexBufferUsed = indexStart + indexBytes;

	vertexBase = (INT)(vertices / sizeof(Vertex));
	vertexCount = std::max<UINT>(1, pvrrc.verts.used());
	indexBase = indexStart / sizeof(u32);
	modVolBase = modVols / modVolSize;
	haveModVols = modVolBytes != 0;
	return true;
}

void DX9Renderer::SetupFrame()
{
	/* The normal matrix, made for a picture whose first line is the top
	 * one and whose clip space has y going up: the other way up from the
	 * Vulkan renderers', whose matrices these are, and nothing else */
	const xform& normal = matrices.GetNormalMatrix();
	const float vtx[2][4] = {
		{ normal.sx, -normal.sy, normal.sz, 0.f },
		{ normal.tx, -normal.ty, normal.tz, 0.f },
	};
	device->SetVertexShaderConstantF(DX9VC_NormalScale, &vtx[0][0], 2);

	float frag[5][4] = {};
	// the clamping limits
	for (int i = 0; i < 3; i++)
	{
		frag[DX9PC_ColorClampMin][i] = ((pvrrc.fog_clamp_min >> (16 - 8 * i)) & 0xFF) / 255.0f;
		frag[DX9PC_ColorClampMax][i] = ((pvrrc.fog_clamp_max >> (16 - 8 * i)) & 0xFF) / 255.0f;
	}
	frag[DX9PC_ColorClampMin][3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;
	frag[DX9PC_ColorClampMax][3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;
	//VERT and RAM fog color constants
	const u8* fog_colvert_bgra = (const u8*)&FOG_COL_VERT;
	const u8* fog_colram_bgra = (const u8*)&FOG_COL_RAM;
	for (int i = 0; i < 3; i++)
	{
		frag[DX9PC_FogColVert][i] = fog_colvert_bgra[2 - i] / 255.0f;
		frag[DX9PC_FogColRam][i] = fog_colram_bgra[2 - i] / 255.0f;
	}
	// half a step under the reference: see the same in gles/gles.cpp
	frag[DX9PC_FrameParams][0] = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	//Fog density constant
	const u8* fog_density = (const u8*)&FOG_DENSITY;
	const float fog_den_mant = fog_density[1] / 128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	const s32 fog_den_exp = (s8)fog_density[0];
	frag[DX9PC_FrameParams][1] = fog_den_mant * powf(2.0f, fog_den_exp);
	frag[DX9PC_FrameParams][2] = FPU_SHAD_SCALE.scale_factor / 256.f;
	device->SetPixelShaderConstantF(DX9PC_ColorClampMin, &frag[0][0], 5);

	context->SetDecl(mainDecl);
	context->SetStream(vertexBuffer, sizeof(Vertex));
	context->SetIndices(indexBuffer);
	context->SetTexture(DX9S_Palette, paletteTexture);
	context->SetTexture(DX9S_Fog, fogTexture);
	context->SetVS(shaders->GetVS(DX9VS_Main));
}

bool DX9Renderer::EnsureDepth(u32 width, u32 height)
{
	if (depthSurface && depthWidth >= width && depthHeight >= height)
		return true;
	width = std::max(width, depthWidth);
	height = std::max(height, depthHeight);
	depthSurface.reset();
	depthWidth = depthHeight = 0;
	HRESULT hr = device->CreateDepthStencilSurface(width, height, context->GetDepthFormat(), D3DMULTISAMPLE_NONE, 0,
			TRUE, depthSurface.put(), nullptr);
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX9: depth buffer creation failed (%x, %d x %d)", (unsigned)hr, width, height);
		depthSurface.reset();
		return false;
	}
	depthWidth = width;
	depthHeight = height;
	return true;
}

/* ---- The screen ---- */

bool DX9Renderer::MakeScreenTarget(ScreenTarget& target, u32 width, u32 height)
{
	target = ScreenTarget();
	target.texture = context->MakeTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
	if (!target.texture || FAILED(target.texture->GetSurfaceLevel(0, target.surface.put())))
	{
		target = ScreenTarget();
		return false;
	}
	target.width = width;
	target.height = height;
	device->ColorFill(target.surface, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
	return true;
}

bool DX9Renderer::EnsureScreen(u32 width, u32 height)
{
	if (width != screenWidth || height != screenHeight)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is: nothing is read back or loaded again. The pictures of the
		 * old size are made again each when it is next drawn into, after
		 * the frontend is done with it. */
		if (lastPicture)
		{
			restoredPicture = lastPicture;
			restoredSwap = false;
		}
		lastPicture.reset();
		screenWidth = width;
		screenHeight = height;
	}
	const unsigned count = context->GetPresentCount();
	if (screenSlots.size() < count)
		screenSlots.resize(count);
	ScreenSlot& slot = screenSlots[screenIndex];
	if (slot.target.texture && (slot.target.width != width || slot.target.height != height))
		slot.target = ScreenTarget();
	if (slot.extra.texture && (slot.extra.width != width || slot.extra.height != height))
		slot.extra = ScreenTarget();
	if (!slot.target.texture)
	{
		context->EndScene();
		if (!MakeScreenTarget(slot.target, width, height))
			return false;
	}
	if (context->IsHandedOver(slot.target.texture))
	{
		/* A render to the screen after one this frame has handed over
		 * already: this one goes to the slot's other picture, and the one
		 * handed over stays as it is until the frontend has shown it */
		if (!slot.extra.texture)
		{
			context->EndScene();
			if (!MakeScreenTarget(slot.extra, width, height))
				return false;
		}
		std::swap(slot.target, slot.extra);
	}
	return EnsureDepth(width, height);
}

void DX9Renderer::DrawLastPicture()
{
	IDirect3DTexture9 *picture = lastPicture ? lastPicture.get() : restoredPicture.get();
	if (picture == nullptr)
		return;
	/* (the picture kept from another context can be of another size) */
	context->DrawQuad(picture, !lastPicture, lastPicture ? false : restoredSwap);
}

bool DX9Renderer::BeginScreen()
{
	screenIndex = (int)context->GetPresentIndex();
	if (!EnsureScreen(screen_width, screen_height))
		return false;
	ScreenTarget& target = screenSlots[screenIndex].target;

	matrices.CalcMatrices(&pvrrc);
	context->SetTarget(target.surface, depthSurface, screenWidth, screenHeight);
	if (!context->BeginScene())
		return false;

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (KeepsLastPicture())
	{
		context->Clear(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0);
		if (lastPicture.get() != target.texture.get())
		{
			if (!lastPicture)
				context->Clear(D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0));
			DrawLastPicture();
		}
	}
	else
	{
		D3DCOLOR color = D3DCOLOR_ARGB(255, 0, 0, 0);
		if (pvrrc.clearFramebuffer)
			color = D3DCOLOR_ARGB(255, VO_BORDER_COL.Red, VO_BORDER_COL.Green, VO_BORDER_COL.Blue);
		context->Clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, color);
	}
	SetBaseScissor();
	return true;
}

void DX9Renderer::EndScreen()
{
	ScreenTarget& target = screenSlots[screenIndex].target;
	// what goes over the picture, onto it (with the depth test off)
	if (!overlay.IsEmpty())
		overlay.Draw(screenWidth, screenHeight);

	lastPicture = target.texture;
	restoredPicture.reset();
	context->SetFrame(target.texture);
}

void DX9Renderer::KeepPicture()
{
	IDirect3DTexture9 *picture = lastPicture.get();
	if (picture == nullptr || screenWidth == 0 || screenHeight == 0)
		return;
	u8 *pixels = last_picture_keep(screenWidth, screenHeight);
	if (pixels != nullptr && !dx9_read_picture(picture, screenWidth, screenHeight, pixels))
		last_picture_drop();
}

/* ---- Renders to textures ---- */

bool DX9Renderer::BeginRTT()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	matrices.CalcMatrices(&pvrrc);

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

	context->EndScene();
	if (!EnsureDepth(widthPow2, heightPow2))
		return false;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_watch.h */
	dx9_rtt_supersede(rttAddr, clipX, clipY, clipWidth, clipHeight);
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
		D3DSURFACE_DESC desc;
		if (FAILED(rttTexture->spare->GetLevelDesc(0, &desc)) || desc.Width != widthPow2 || desc.Height != heightPow2
				|| desc.Format != D3DFMT_A8R8G8B8 || (desc.Usage & D3DUSAGE_RENDERTARGET) == 0
				|| rttTexture->spare->GetLevelCount() != 1)
		{
			rttTexture->spareSurface.reset();
			context->Recycle(rttTexture->spare);
		}
	}
	if (!rttTexture->spare)
		rttTexture->spare = context->MakeTexture(widthPow2, heightPow2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
	if (!rttTexture->spare)
		return false;
	if (!rttTexture->spareSurface && FAILED(rttTexture->spare->GetSurfaceLevel(0, rttTexture->spareSurface.put())))
	{
		WARN_LOG(RENDERER, "DX9: render to texture target creation failed");
		return false;
	}

	/* (a texture this render draws with is not to be bound while it is
	 * drawn into) */
	context->SetTexture(DX9S_Texture, nullptr);
	context->SetTarget(rttTexture->spareSurface, depthSurface, upscaledWidth, upscaledHeight);
	if (!context->BeginScene())
		return false;
	context->Clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(255, 0, 0, 0));
	// only the pixel clip is written
	baseScissor = RECT{ (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale) };
	return true;
}

void DX9Renderer::EndRTT()
{
	/* (left bound: whatever binds the texture to be read from binds
	 * another target first) */
	rttTexture->SwapWithSpare();

	rttTexture->dirty = 0;
	rttTexture->stride_read = 0;	// (rendered, not read)
	libCore_vramlock_Lock(rttTexture->sa_tex, rttTexture->sa + rttTexture->size - 1, rttTexture);
	/* video memory gets it when something first touches it there */
	dx9_rtt_watch(rttTexture, rttAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	rttTexture = nullptr;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

bool DX9Renderer::UploadFramebuffer()
{
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	PixelBuffer<u32> pb;
	int width;
	int height;
	ReadFramebuffer(pb, width, height);

	if (!fbTexture || width != fbWidth || height != fbHeight)
	{
		/* (written by the processor every frame: one the card reads where it
		 * is written, with no copy of it made on the way) */
		context->Recycle(fbTexture);
		fbTexture = context->MakeTexture(width, height, 1, context->WriteUsage(), D3DFMT_A8R8G8B8);
		if (!fbTexture)
			return false;
		fbWidth = width;
		fbHeight = height;
	}
	D3DLOCKED_RECT rect;
	if (FAILED(fbTexture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
		return false;
	const u8 *src = (const u8 *)pb.data();
	u8 *dst = (u8 *)rect.pBits;
	if ((u32)rect.Pitch == (u32)width * 4)
		memcpy(dst, src, (size_t)width * height * 4);
	else
		for (int y = 0; y < height; y++)
			memcpy(dst + (size_t)y * rect.Pitch, src + (size_t)y * width * 4, (size_t)width * 4);
	fbTexture->UnlockRect(0);
	return true;
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
void DX9Renderer::DrawFramebuffer()
{
	screenIndex = (int)context->GetPresentIndex();
	if (!EnsureScreen(screen_width, screen_height))
		return;
	ScreenTarget& target = screenSlots[screenIndex].target;
	matrices.CalcMatrices(&pvrrc);
	context->SetTarget(target.surface, nullptr, screenWidth, screenHeight);
	if (!context->BeginScene())
		return;
	context->Clear(D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0));

	const xform& viewport = matrices.GetViewportMatrix();
	const float x0 = xform_x(&viewport, 0.f), x1 = xform_x(&viewport, 640.f);
	const float y0 = xform_y(&viewport, 0.f), y1 = xform_y(&viewport, 480.f);
	// (R G B A bytes)
	context->DrawQuad(fbTexture, true, true,
			x0 * 2.f / screenWidth - 1.f, 1.f - y1 * 2.f / screenHeight,
			(x1 - x0) * 2.f / screenWidth, (y1 - y0) * 2.f / screenHeight);
	EndScreen();
}

/* ---- The frame ---- */

bool DX9Renderer::Render()
{
	/* the device, taken by Process() and kept for this, or taken here -
	 * and given back on the way out */
	const bool held = heldFromProcess;
	heldFromProcess = false;
	struct Unlocker
	{
		bool locked;
		~Unlocker() { if (locked) DX9Context::Instance()->Unlock(); }
	} unlocker = { held };
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
	if (!held)
	{
		/* the frontend may still be showing what is in the screen's next
		 * picture: it is waited for, before the device is taken */
		if (!pvrrc.isRTT)
			context->WaitPresentIndex();
		context->Lock();
		unlocker.locked = true;
	}

	if (pvrrc.isRenderFramebuffer)
	{
		DrawFramebuffer();
		return true;
	}

	SortTriangles();
	SetProvokingVertices();
	/* (the buffers are written before the scene begins) */
	if (!UploadGeometry())
		return false;
	if (pvrrc.isRTT ? !BeginRTT() : !BeginScreen())
	{
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

void DX9Renderer::DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
		ShadowPass shadowPass, const RECT *within)
{
	RECT scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	if (within != nullptr)
	{
		// only the part of it inside this rectangle, and nothing if there is none
		const RECT& clip = tileClip == TileClipping::Outside ? scissorRect : baseScissor;
		const RECT r = {
			std::max(within->left, clip.left), std::max(within->top, clip.top),
			std::min(within->right, clip.right), std::min(within->bottom, clip.bottom)
		};
		if (r.right <= r.left || r.bottom <= r.top || !context->SetScissor(r))
			return;
	}
	else if (!context->SetScissor(tileClip == TileClipping::Outside ? scissorRect : baseScissor))
		return;

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
			{ trilinearAlpha, palette_index, 0.f, 0.f }
		};
		SetPolyConstants(constants);
	}

	const bool textured = poly.pcw.Texture && poly.texid != 0;
	bool swapRB = false;
	if (textured)
	{
		DX9Texture *texture = (DX9Texture *)(uintptr_t)poly.texid;
		context->SetTexture(DX9S_Texture, texture->texture);
		swapRB = texture->swapRB;
		const D3DTEXTUREADDRESS u = poly.tsp.ClampU ? D3DTADDRESS_CLAMP : poly.tsp.FlipU ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
		const D3DTEXTUREADDRESS v = poly.tsp.ClampV ? D3DTADDRESS_CLAMP : poly.tsp.FlipV ? D3DTADDRESS_MIRROR : D3DTADDRESS_WRAP;
		context->SetSampler(DX9S_Texture, DX9Context::SamplerKey(poly.tsp.FilterMode != 0,
				poly.tsp.FilterMode != 0 ? (u32)settings.rend.AnisotropicFiltering : 1, u, v, poly.tsp.MipMapD));
	}

	/* What the other renderers make a shader for, as the shader's
	 * flags. By the polygon alone, as the Vulkan renderer has it: the
	 * clamping limits change, and a shader is kept by the polygon's bit only */
	const bool shadowedPass = shadowPass != ShadowPass::None;
	const u32 shadInstr = poly.tsp.ShadInstr & 3;
	const u32 fog = poly.tsp.FogCtrl & 3;
	const u32 bools = ((u32)poly.tsp.UseAlpha << DX9B_UseAlpha)
			| ((u32)(poly.tsp.IgnoreTexA || poly.tcw.PixelFmt == Pixel565) << DX9B_IgnoreTexA)
			| ((u32)poly.pcw.Offset << DX9B_Offset)
			| ((u32)(poly.tcw.PixelFmt == PixelBumpMap) << DX9B_BumpMap)
			| ((u32)poly.tsp.ColorClamp << DX9B_ColorClamp)
			| ((u32)(poly.pcw.Texture && poly.tsp.FilterMode > 1 && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1) << DX9B_TriLinear)
			| ((u32)((poly.tileclip >> 28) == 3) << DX9B_ClipInside)
			| ((u32)(listType == ListType_Punch_Through) << DX9B_AlphaTest)
			| ((u32)shadowedPass << DX9B_Shadowed)
			| ((shadInstr & 1) << DX9B_ShadInstr0) | ((shadInstr >> 1) << DX9B_ShadInstr1)
			| ((fog & 1) << DX9B_Fog0) | ((fog >> 1) << DX9B_Fog1)
			| ((u32)swapRB << DX9B_SwapRB);
	context->SetBools(bools);
	context->SetPS(shaders->GetPS(!poly.pcw.Texture ? DX9PS_Plain : palette ? DX9PS_Palette : DX9PS_Texture));
	context->SetRenderState(D3DRS_SHADEMODE, poly.pcw.Gouraud ? D3DSHADE_GOURAUD : D3DSHADE_FLAT);

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
			context->SetDepthStencil(true, false, 2, DX9Stencil_ShadowLaterWins, 0xC1);
		else
			context->SetDepthStencil(true, false, 2, DX9Stencil_ShadowEarlierWins, 0x81);
	}
	else if (shadowed)
		context->SetDepthStencil(true, depthWrite, depthFunc, DX9Stencil_Shadow, poly.pcw.Shadow != 0 ? 0x80 : 0);
	else
		context->SetDepthStencil(true, depthWrite, depthFunc, DX9Stencil_None, 0);

	// Apparently punch-through polys support blending, or at least some combinations
	// An opaque polygon is blended by its instructions too when it is in a
	// list continuation; the ones of the first render pass have had theirs
	// set to source x 1 and destination x 0 when the list was read.
	if (shadowPass == ShadowPass::None
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| poly.tsp.SrcInstr != 1 || poly.tsp.DstInstr != 0))
		context->SetBlend(true, poly.tsp.SrcInstr, poly.tsp.DstInstr);
	else
		context->SetBlend(false);

	SetCullMode(poly.isp.CullMode);
	Draw(sortTriangles && !settings.pvr.Emulation.AlphaSortMode ? D3DPT_TRIANGLELIST : D3DPT_TRIANGLESTRIP, first, count);
}

// The polygons of a list that take shadows, drawn again where they are in a
// modifier volume: see rend/shadows.h.
void DX9Renderer::DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
		const ScreenBounds& area, const RECT& scissor)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (CanDrawShadowed(listType, pp) && PolyOverlaps(pp, area))
			DrawPoly(listType, false, *pp, pp->first, pp->count,
					ShadowLaterWins(listType, pp) ? ShadowPass::LaterWins : ShadowPass::EarlierWins,
					&scissor);
}

void DX9Renderer::DrawSorted(const std::vector<SortTrigDrawParam>& polys, bool multipass)
{
	for (const SortTrigDrawParam& param : polys)
		DrawPoly(ListType_Translucent, true, *param.ppid, pvrrc.idx.used() + param.first, param.count);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		// Sorted translucent polygons are drawn without writing depth. Where
		// a later render pass needs their depth (Cosmic Smash), it is written
		// now, for those that do not have Z write disabled.
		context->SetDecl(positionDecl);
		context->SetVS(shaders->GetVS(DX9VS_ModVol));
		context->SetPS(shaders->GetPS(DX9PS_ModVol));
		context->SetBlend(false, 1, 0, false);
		context->SetDepthStencil(true, true, 6, DX9Stencil_None, 0);
		for (const SortTrigDrawParam& param : polys)
		{
			if (param.count <= 2 || param.ppid->isp.ZWriteDis)
				continue;
			RECT scissorRect;
			if (!context->SetScissor(SetTileClip(param.ppid->tileclip, scissorRect) == TileClipping::Outside ? scissorRect : baseScissor))
				continue;
			SetCullMode(param.ppid->isp.CullMode);
			Draw(D3DPT_TRIANGLELIST, pvrrc.idx.used() + param.first, param.count);
		}
		context->SetDecl(mainDecl);
		context->SetVS(shaders->GetVS(DX9VS_Main));
	}
}

void DX9Renderer::DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last)
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

void DX9Renderer::DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass)
{
	// (nothing of the frame on the target: nothing to do)
	if (count == 0 || !haveModVols || !context->SetScissor(baseScissor))
		return;

	// (the modifier volumes' triangles, after the vertices in the buffer)
	context->SetDecl(positionDecl);
	context->SetStream(vertexBuffer, sizeof(float) * 3);
	context->SetVS(shaders->GetVS(DX9VS_ModVol));
	context->SetPS(shaders->GetPS(DX9PS_ModVol));
	context->SetBlend(false, 1, 0, false);

	ModifierVolumeParam* params = &pvrrc.global_param_mvo.head()[first];

	int mod_base = -1;

	for (int cmv = 0; cmv < count; cmv++)
	{
		ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		// (and with nothing of it left, nothing of it drawn)
		RECT clipRect;
		const bool visible = context->SetScissor(SetTileClip(param.tileclip, clipRect) == TileClipping::Outside ? clipRect : baseScissor);

		if (mod_base == -1)
			mod_base = param.first;

		SetCullMode(param.isp.CullMode);
		if (!param.isp.VolumeLast && mv_mode > 0)
			context->SetDepthStencil(true, false, 4, DX9Stencil_Or, 2);	// OR'ing (open volume or quad)
		else
			context->SetDepthStencil(true, false, 4, DX9Stencil_Xor, 2);	// XOR'ing (closed volume)
		if (visible)
			device->DrawPrimitive(D3DPT_TRIANGLELIST, modVolBase + param.first * 3, param.count);

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			context->SetDepthStencil(false, false, 7, mv_mode == 1 ? DX9Stencil_Inclusion : DX9Stencil_Exclusion, 1);
			if (visible)
				device->DrawPrimitive(D3DPT_TRIANGLELIST, modVolBase + mod_base * 3, param.first + param.count - mod_base);
			mod_base = -1;
		}
	}
	context->SetScissor(baseScissor);
	context->SetDecl(mainDecl);
	context->SetStream(vertexBuffer, sizeof(Vertex));
	context->SetVS(shaders->GetVS(DX9VS_Main));

	// The polygons that take shadows, again, where they are in a volume
	const ScreenBounds area = ModVolBounds(first, count);
	if (!area.empty())
	{
		int rect[4];
		BoundsToScissor(area, matrices.GetViewportMatrix(), rect);
		const RECT scissor = { rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3] };
		DrawShadowed(ListType_Opaque, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count, area, scissor);
		DrawShadowed(ListType_Punch_Through, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count, area, scissor);
		context->SetScissor(baseScissor);
	}

	// What is left: darkened as it is. The background polygon is the
	// rectangle drawn: the first four vertices' positions.
	context->SetDecl(positionDecl);
	context->SetVS(shaders->GetVS(DX9VS_ModVol));
	context->SetPS(shaders->GetPS(DX9PS_ModVol));
	context->SetAlphaBlend();
	context->SetDepthStencil(false, false, 7, DX9Stencil_Final, 0x81);
	SetCullMode(0);
	Draw(D3DPT_TRIANGLESTRIP, 0, 4);
	context->SetDecl(mainDecl);
	context->SetVS(shaders->GetVS(DX9VS_Main));
}

void DX9Renderer::DrawFrame()
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

Renderer* rend_D3D9()
{
	return new DX9Renderer();
}
