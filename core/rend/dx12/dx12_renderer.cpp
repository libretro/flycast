#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
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
#include "dx12_renderer.h"
#include "dx12_quad.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include "rend/ssaa_pattern.h"
#include "rend/ssaa_rate.h"
#include <algorithm>
#include <cmath>

extern int screen_width, screen_height;

/* An image of the renderer's own: a committed texture, with its place in
 * the bindless table (at @srv if given, one taken otherwise) and a render
 * target view if it is one */
static bool makeImage(DX12Image& image, u32 width, u32 height, DXGI_FORMAT format, bool target, u32 srv = ~0u)
{
	DX12Context *context = DX12Context::Instance();
	image = DX12Image();
	image.width = width;
	image.height = height;
	image.state = target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (!dx12_make_texture(context->GetDevice(), width, height, 1, format,
			target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE, image.state, image.resource))
		return false;
	image.srv = srv != ~0u ? srv : context->Srv().Alloc();
	if (image.srv == ~0u)
	{
		image.resource.reset();
		return false;
	}
	context->MakeSrv(image.resource, format, 1, image.srv);
	if (target)
	{
		image.rtv = context->Rtv().Alloc();
		if (image.rtv == ~0u)
		{
			if (srv == ~0u)
				context->Srv().Free(image.srv);
			image = DX12Image();
			return false;
		}
		context->GetDevice()->CreateRenderTargetView(image.resource, nullptr, context->Rtv().Cpu(image.rtv));
	}
	return true;
}

/* A picture of @samples samples a pixel in the standard pattern (SSAA's),
 * committed: of the frame's colour, with a place in the bindless table (a
 * view of its samples) and a render target view; or of its depth and
 * stencil, with no views (DX12Renderer::SetupSsaa() makes those) */
static bool makeSamples(DX12Image& image, u32 width, u32 height, u32 samples, bool depthStencil)
{
	DX12Context *context = DX12Context::Instance();
	ID3D12Device *device = context->GetDevice();
	image = DX12Image();
	image.width = width;
	image.height = height;
	image.state = depthStencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = depthStencil ? DX12DepthResourceFormat : DX12FrameFormat;
	desc.SampleDesc.Count = samples;
	desc.SampleDesc.Quality = DX12StandardPattern;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = depthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE clear{};
	if (depthStencil)
		clear.Format = DX12DepthFormat;
	else
	{
		clear.Format = DX12FrameFormat;
		clear.Color[3] = 1.f;
	}
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, image.state, &clear,
			__uuidof(ID3D12Resource), (void **)image.resource.put())))
	{
		WARN_LOG(RENDERER, "DX12: SSAA picture creation failed (%d x %d, %d samples)", width, height, samples);
		image = DX12Image();
		return false;
	}
	if (depthStencil)
		return true;
	image.srv = context->Srv().Alloc();
	image.rtv = context->Rtv().Alloc();
	if (image.srv == ~0u || image.rtv == ~0u)
		return false;
	D3D12_SHADER_RESOURCE_VIEW_DESC view{};
	view.Format = DX12FrameFormat;
	view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	device->CreateShaderResourceView(image.resource, &view, context->Srv().Cpu(image.srv));
	device->CreateRenderTargetView(image.resource, nullptr, context->Rtv().Cpu(image.rtv));
	return true;
}

/* Rows into the ring and then into @image, which is left to be sampled */
static bool uploadImage(DX12Image& image, const void *pixels, u32 width, u32 height, u32 bpp)
{
	DX12Context *context = DX12Context::Instance();
	if (!image.resource)
		return false;
	const u32 pitch = (width * bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	DX12Upload upload;
	if (!context->Upload(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, upload))
		return false;
	if (pitch == width * bpp)
		memcpy(upload.cpu, pixels, (size_t)pitch * height);
	else
		for (u32 y = 0; y < height; y++)
			memcpy(upload.cpu + (size_t)y * pitch, (const u8 *)pixels + (size_t)y * width * bpp, (size_t)width * bpp);
	context->Transition(image, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = image.resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = upload.buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload.offset;
	D3D12_RESOURCE_DESC desc = image.resource->GetDesc();
	src.PlacedFootprint.Footprint.Format = desc.Format;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	context->Cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	context->Transition(image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

void DX12Renderer::DropImage(DX12Image& image)
{
	if (image.resource)
	{
		image.resource->AddRef();
		context->Release(image.resource.get());
		image.resource.reset();
	}
	// (the fog's and the palette's places are theirs for good)
	if (image.srv >= DX12_SRV_FIRST_FREE)
		context->ReleaseSrv(image.srv);
	if (image.rtv != ~0u)
		context->Rtv().Free(image.rtv);
	image = DX12Image();
}

bool DX12Renderer::Init()
{
	context = DX12Context::Instance();
	device = context->GetDevice();
	if (device == nullptr)
	{
		WARN_LOG(RENDERER, "DX12: no device");
		return false;
	}
	bool success = true;
	// The fog table, two rows of 128: see CheckFogTexture()
	success = success && makeImage(fogTexture, 128, 2, DXGI_FORMAT_R8_UNORM, false, DX12_SRV_FOG);
	fogUploadedValid = false;
	// The palette, a row of 1024 colours
	success = success && makeImage(paletteTexture, 1024, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, DX12_SRV_PALETTE);
	palette_updated = true;
	// the depth, for the screen at least: made now and not in the first frame
	success = success && EnsureDepth(std::max(screen_width, 640), std::max(screen_height, 480));
	success = success && overlay.Init();
	if (batch == nullptr)
		batch = new DrawArgs[BATCH_MAX];
	batchCount = 0;
	statBatches = statBatched = 0;

	havePicture = false;
	lastScreen = -1;
	lastInExtra = false;
	screenWidth = screenHeight = 0;
	heldFromProcess = false;
	if (!success)
	{
		WARN_LOG(RENDERER, "DX12 renderer initialization failed");
		return false;
	}
	dx12_make_samplers();
	// textures made on the GPU, and the textures themselves, all of them now (rend/tex_pool.h)
	texDecoder.Init(context);
	// (what the decoder has been asked for goes into the list a readback submits early)
	context->SetBeforeSubmit([](void *decoder) { ((DX12TexDecoder *)decoder)->Flush(); }, &texDecoder);
	texPool.Init(context->GetVramBudget(), &texDecoder);
	DX12Texture::SetDecoder(texDecoder.Ready() ? &texDecoder : nullptr);
	DX12Texture::SetPool(&texPool);
	// the pipelines, and SSAA's pictures and pipelines made from them
	PreparePipelines();
	SetupSsaa(screen_width, screen_height);
	/* GPU-side TA processing, if it is on: here, not in a frame; and the
	 * buffer its frames' vertex constants go to (UploadConstants()) */
	if (!gpuVtxConstants)
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		gpuVtxConstantsState = D3D12_RESOURCE_STATE_COPY_DEST;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, gpuVtxConstantsState, nullptr,
				__uuidof(ID3D12Resource), (void **)gpuVtxConstants.put())))
			gpuVtxConstants.reset();
	}
	taDecoder.WantSort(TaSorts());
	taDecoder.Setup(TaFlags() != 0 && gpuVtxConstants && ta_gpu_wanted());
	return true;
}

void DX12Renderer::OptionsChanged()
{
	if (context != nullptr)
		taDecoder.Setup(TaFlags() != 0 && gpuVtxConstants && ta_gpu_wanted());
}

void DX12Renderer::Term()
{
	if (context == nullptr)
		return;
	FlushBatch();
	DropSsaa();
	ta_set_flat_strips(false);
	context->SetFrame(nullptr);
	context->SetBeforeSubmit(nullptr, nullptr);
	dx12_rtt_term();
	texCache.Clear();
	// (what the cache's textures had is back in the pool by now)
	DX12Texture::SetPool(nullptr);
	DX12Texture::SetDecoder(nullptr);
	texPool.Term();
	texDecoder.Term();
	taDecoder.Term();
	if (gpuVtxConstants)
	{
		gpuVtxConstants->AddRef();
		context->Release(gpuVtxConstants.get());
		gpuVtxConstants.reset();
	}
	gpuVtxConstantsState = D3D12_RESOURCE_STATE_COPY_DEST;
	overlay.Term();
	DropImage(fbTexture);
	DropRestored();
	for (ScreenTarget& t : screenTargets)
		DropImage(t.image);
	screenTargets.clear();
	DropImage(extraTarget.image);
	lastInExtra = false;
	if (depth.resource)
	{
		depth.resource->AddRef();
		context->Release(depth.resource.get());
		context->Dsv().Free(depth.rtv);
		context->Dsv().Free(dsvReadStencil);
		context->Dsv().Free(dsvReadOnly);
	}
	depth = DX12Image();
	dsvReadStencil = dsvReadOnly = ~0u;
	DropImage(paletteTexture);
	DropImage(fogTexture);
	delete [] batch;
	batch = nullptr;
	NOTICE_LOG(RENDERER, "DX12: %u draws in %u batches; %u submits, %u stalls", statBatched, statBatches, context->statSubmits, context->statStalls);
	device = nullptr;
	context = nullptr;
}

/* The size rendered at has changed, and the context is the same: the
 * screen's pictures are made again at the new size when the next frame is
 * drawn, and the last picture goes with them - EnsureScreen(). SSAA's are
 * made again now, or SSAA is taken up or left: a change of the option is
 * one of size. */
void DX12Renderer::Resize(int w, int h)
{
	if (device != nullptr)
		SetupSsaa((u32)std::max(w, 0), (u32)std::max(h, 0));
}

/* ---- SSAA ---- */

void DX12Renderer::SetupSsaa(u32 width, u32 height)
{
	u32 samples = 1;
	if (settings.rend.Ssaa > 1 && width != 0 && height != 0)
		samples = context->SsaaSamples(settings.rend.Ssaa, ssaa_per_sample_kib(width, height, 4 * SsaaColorImages() + 8),
				SsaaFixedKib(width, height), BindsScreen() ? "DX12 per-triangle renderer" : "DX12 per-pixel renderer");
	if (samples == ssaaSamples && (samples == 1 || (width == ssaaWidth && height == ssaaHeight)))
		return;
	DropSsaa();
	if (samples <= 1)
		return;
	bool ok = true;
	for (u32 i = 0; i < SsaaColorImages() && i < 2; i++)
		ok = ok && makeSamples(ssaaColor[i], width, height, samples, false);
	ok = ok && makeSamples(otherDepth.image, width, height, samples, true);
	if (ok)
	{
		otherDepth.stencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		otherDepth.image.rtv = context->Dsv().Alloc();
		otherDepth.dsvReadStencil = context->Dsv().Alloc();
		otherDepth.dsvReadOnly = context->Dsv().Alloc();
		ok = otherDepth.image.rtv != ~0u && otherDepth.dsvReadStencil != ~0u && otherDepth.dsvReadOnly != ~0u;
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: SSAA off: its pictures could not be made");
		DropSsaa();
		return;
	}
	D3D12_DEPTH_STENCIL_VIEW_DESC view{};
	view.Format = DX12DepthFormat;
	view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
	device->CreateDepthStencilView(otherDepth.image.resource, &view, context->Dsv().Cpu(otherDepth.image.rtv));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	device->CreateDepthStencilView(otherDepth.image.resource, &view, context->Dsv().Cpu(otherDepth.dsvReadStencil));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL | D3D12_DSV_FLAG_READ_ONLY_DEPTH;
	device->CreateDepthStencilView(otherDepth.image.resource, &view, context->Dsv().Cpu(otherDepth.dsvReadOnly));
	// the per-pixel renderer's views of its samples' stencil and depth (its shaders read them)
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
	device->CreateShaderResourceView(otherDepth.image.resource, &srv, context->Srv().Cpu(DX12Context::OitSsaaFirst + 3));
	srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	device->CreateShaderResourceView(otherDepth.image.resource, &srv, context->Srv().Cpu(DX12Context::OitSsaaFirst + 4));
	ssaaSamples = samples;
	ssaaLog2 = SSAA_LOG2(samples);
	ssaaWidth = width;
	ssaaHeight = height;
	context->Pipelines().MakeForSamples(ssaaLog2);
}

void DX12Renderer::DropSsaa()
{
	if (context == nullptr)
		return;
	SsaaDepth(false);
	for (DX12Image& image : ssaaColor)
		DropImage(image);
	if (otherDepth.image.resource)
	{
		otherDepth.image.resource->AddRef();
		context->Release(otherDepth.image.resource.get());
	}
	for (u32 dsv : { otherDepth.image.rtv, otherDepth.dsvReadStencil, otherDepth.dsvReadOnly })
		if (dsv != ~0u)
			context->Dsv().Free(dsv);
	otherDepth.image = DX12Image();
	otherDepth.dsvReadStencil = otherDepth.dsvReadOnly = ~0u;
	ssaaSamples = 1;
	ssaaLog2 = 0;
	ssaaWidth = ssaaHeight = 0;
}

void DX12Renderer::SsaaDepth(bool in)
{
	if (in == ssaaDepthIn || (in && (ssaaSamples <= 1 || !otherDepth.image.resource)))
		return;
	std::swap(depth, otherDepth.image);
	std::swap(stencilState, otherDepth.stencilState);
	std::swap(dsvReadStencil, otherDepth.dsvReadStencil);
	std::swap(dsvReadOnly, otherDepth.dsvReadOnly);
	ssaaDepthIn = in;
}

void DX12Renderer::SsaaResolve(u32 srv)
{
	DX12PsoKey key{};
	key.kind = DX12_PSO_SSAA_RESOLVE;
	key.writeColor = true;
	key.depthFunc = 7;
	ID3D12PipelineState *pso = context->Pipelines().Get(key);
	if (pso == nullptr)
		return;
	context->SetPipeline(pso);
	context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const u32 constants[2] = { srv, ssaaLog2 };
	context->SetRootConstants(constants, 2);
	context->Cmd()->DrawInstanced(4, 1, 0, 0);
}

u64 DX12Renderer::GetTexture(TSP tsp, TCW tcw, int area)
{
	DX12Texture* tf = texCache.getTextureCacheData(tsp, tcw, area);

	if (tf->IsNew())
		tf->Create();
	//update if needed
	if (tf->NeedsUpdate())
		tf->Update();
	else if (tf->IsCustomTextureAvailable())
		tf->CheckCustomTexture();

	return tf->GetIntId();
}

void DX12Renderer::DropRestored()
{
	DropImage(restoredPicture);
}

/* A picture kept from the context before: the first render to the screen
 * starts from it */
void DX12Renderer::CheckKeptPicture()
{
	int width, height;
	const u8 *pixels = last_picture(&width, &height);
	if (pixels == nullptr)
		return;
	DropRestored();
	if (!makeImage(restoredPicture, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false) || !uploadImage(restoredPicture, pixels, width, height, 4))
		DropRestored();
}

/* Uploaded when it differs from what the card has: see UpdateFogTexture() in gles.cpp */
void DX12Renderer::CheckFogTexture()
{
	u8 texData[256];
	MakeFogTexture(texData);
	if (fogUploadedValid && !memcmp(fogUploaded, texData, sizeof(fogUploaded)))
		return;
	memcpy(fogUploaded, texData, sizeof(fogUploaded));
	fogUploadedValid = true;
	uploadImage(fogTexture, texData, 128, 2, 1);
}

void DX12Renderer::CheckPaletteTexture()
{
	if (!palette_updated)
		return;
	palette_updated = false;
	uploadImage(paletteTexture, palette32_ram, 1024, 1, 4);
}

bool DX12Renderer::Process(TA_context* ctx)
{
	if (device == nullptr)
		return false;
	/* A render to the screen with threaded rendering is handed over to the
	 * other thread when this is done: the frontend is waited for before the
	 * screen's picture is drawn into, and not here, which would keep the
	 * emulation waiting too. */
	const bool keep = ctx->rend.isRTT || !settings.rend.ThreadedRendering;
	if (keep && !ctx->rend.isRTT)
		context->WaitPresentIndex();
	const bool result = ProcessLocked(ctx);
	heldFromProcess = keep && result;
	return result;
}

bool DX12Renderer::ProcessLocked(TA_context* ctx)
{
	taDecoder.BeginFrame();
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

	/* SSAA draws flat strips at a single rate: the parse looks for them
	 * then, and only then */
	ta_set_flat_strips(ssaaSamples > 1);
	/* GPU-side TA processing: the header pass makes the frame ready to be
	 * drawn from the GPU's decode, in the full parse's place, which is
	 * recorded now; or it is parsed as ever */
	bool result;
	if (!taDecoder.Ready() || !taDecoder.Prepare(ctx, TaFlags(), &result))
		result = ta_parse_vdrc(ctx);
	else if (result)
		taDecoder.Record(ctx);
	// the textures the frame's polygons asked for that are made on the GPU, before the draws
	texDecoder.Flush();

	texCache.Cleanup();

	if (result)
	{
		CheckFogTexture();
		CheckPaletteTexture();
	}
	return result;
}

/* ---- Clipping ---- */

TileClipping DX12Renderer::SetTileClip(u32 val, D3D12_RECT& clipRect)
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

void DX12Renderer::SetBaseScissor()
{
	bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !matrices.IsClipped();
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
		{
			baseScissor = D3D12_RECT{ 0, 0, 640, 480 };
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
			baseScissor = D3D12_RECT{ x, y, x + (LONG)std::max(lroundf(width), 0L), y + (LONG)std::max(lroundf(height), 0L) };
		}
	}
	else
	{
		baseScissor = D3D12_RECT{ 0, 0, (LONG)screen_width, (LONG)screen_height };
	}
}

// Direct3D takes a flat shaded triangle's colours from its first vertex.
// On Dreamcast the last vertex is the provoking one so it is copied onto the
// first (as in the Vulkan renderers).
void DX12Renderer::SetProvokingVertices()
{
	// (a frame decoded on the GPU has had it done there: TA_GPU_PROVOKE)
	if (pvrrc.gpu)
		return;
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

void DX12Renderer::SortTriangles()
{
	sortedPolys.resize(pvrrc.render_passes.used());
	sortedIndexes.resize(pvrrc.render_passes.used());
	prepassPolys.resize(pvrrc.render_passes.used());
	sortedIndexCount = 0;
	RenderPass previousPass = {};

	for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];
		sortedIndexes[render_pass].clear();
		prepassPolys[render_pass].clear();
		if (current_pass.autosort)
		{
			GenSorted(previousPass.tr_count, current_pass.tr_count - previousPass.tr_count, sortedPolys[render_pass], sortedIndexes[render_pass], true);
			GenPrepass(previousPass.tr_count, current_pass.tr_count - previousPass.tr_count,
					render_pass == pvrrc.render_passes.used() - 1, prepassPolys[render_pass], sortedIndexes[render_pass], true);
			for (auto& poly : sortedPolys[render_pass])
				poly.first += sortedIndexCount;
			for (auto& poly : prepassPolys[render_pass])
				poly.first += sortedIndexCount;
			sortedIndexCount += sortedIndexes[render_pass].size();
		}
		else
			sortedPolys[render_pass].clear();
		previousPass = current_pass;
	}
}

/* ---- Buffers ---- */

bool DX12Renderer::UploadGeometry()
{
	if (pvrrc.gpu)
	{
		// (the decode's, on the GPU)
		const DX12TaDraw& gpu = taDecoder.Draw();
		vertexView = gpu.vertices;
		indexView = gpu.indices;
		modVolView = gpu.modtrig;
		haveModVols = pvrrc.gpu_modtrig != 0;
		return UploadConstants();
	}
	const u32 vertexBytes = pvrrc.verts.bytes();
	const u32 indexBytes = pvrrc.idx.bytes() + sortedIndexCount * sizeof(u32);
	const u32 modVolBytes = pvrrc.modtrig.bytes();
	/* the vertices, the indexes, the volumes' triangles, each at a multiple
	 * of its size, in one piece of the ring */
	const u32 indexes = (vertexBytes + 3) & ~3u;
	const u32 modVols = (indexes + indexBytes + 11) / 12 * 12;
	const u32 total = modVols + modVolBytes;
	DX12Upload upload;
	if (!context->Upload(total, 64, upload))
	{
		WARN_LOG(RENDERER, "DX12: the render's geometry does not fit the upload ring (%u bytes)", total);
		return false;
	}
	memcpy(upload.cpu, pvrrc.verts.head(), vertexBytes);
	u8 *dst = upload.cpu + indexes;
	memcpy(dst, pvrrc.idx.head(), pvrrc.idx.bytes());
	dst += pvrrc.idx.bytes();
	for (const std::vector<u32>& idx : sortedIndexes)
		if (!idx.empty())
		{
			memcpy(dst, idx.data(), idx.size() * sizeof(u32));
			dst += idx.size() * sizeof(u32);
		}
	if (modVolBytes != 0)
		memcpy(upload.cpu + modVols, pvrrc.modtrig.head(), modVolBytes);

	vertexView.BufferLocation = upload.gpu;
	vertexView.SizeInBytes = vertexBytes;
	vertexView.StrideInBytes = sizeof(Vertex);
	indexView.BufferLocation = upload.gpu + indexes;
	indexView.SizeInBytes = indexBytes;
	indexView.Format = DXGI_FORMAT_R32_UINT;
	modVolView.BufferLocation = upload.gpu + modVols;
	modVolView.SizeInBytes = modVolBytes;
	modVolView.StrideInBytes = sizeof(float) * 3;
	haveModVols = modVolBytes != 0;
	return UploadConstants();
}

bool DX12Renderer::UploadConstants()
{
	/* The constants: the normal matrix, made for a picture whose first line
	 * is the top one and whose clip space has y going up: the other way up
	 * from the Vulkan renderers', whose matrices these are, and nothing
	 * else. A few words in the ring every render, read by the GPU where
	 * they are. */
	const xform& normal = matrices.GetNormalMatrix();
	const VertexConstants vtx = {
		// (and the depth's scale: one over the frame's largest 1/w)
		{ normal.sx, -normal.sy, normal.sz, 1.f / pvrrc.fZ_max },
		{ normal.tx, -normal.ty, normal.tz, 0.f },
	};
	FragmentConstants frag;
	MakeFragmentConstants(frag);
	DX12Upload c;
	// (two whole 256-byte registers: a constant buffer is read by the register)
	if (!context->Upload(2 * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, c))
		return false;
	memcpy(c.cpu, &vtx, sizeof(vtx));
	memcpy(c.cpu + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &frag, sizeof(frag));
	vtxConstantsGpu = c.gpu;
	fragConstantsGpu = c.gpu + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
	if (pvrrc.gpu && gpuVtxConstants)
	{
		/* A frame decoded on the GPU: its fZ_max is the GPU's. The vertex
		 * constants go to the buffer of the default heap, and the depth's
		 * scale (scale[3]) from the decode's words: three copies that do not
		 * overlap, which the transitions put after the last frame's draws
		 * and before this one's. */
		ID3D12GraphicsCommandList *cmd = context->Cmd();
		ID3D12Resource *dst = gpuVtxConstants;
		context->Transition(dst, gpuVtxConstantsState, D3D12_RESOURCE_STATE_COPY_DEST);
		static_assert(offsetof(VertexConstants, scale) == 0 && sizeof(VertexConstants) == 32, "the scale's fourth word is the depth's");
		cmd->CopyBufferRegion(dst, 0, c.buffer, c.offset, 12);
		taDecoder.CopyDepthScale(dst, 12);
		cmd->CopyBufferRegion(dst, 16, c.buffer, c.offset + 16, 16);
		context->Transition(dst, gpuVtxConstantsState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		vtxConstantsGpu = dst->GetGPUVirtualAddress();
	}
	return true;
}

void DX12Renderer::MakeFragmentConstants(FragmentConstants& constants)
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
	constants.pixelBufferCount = pixelBufferRoom;
	constants.ssaaMask = SsaaActive() ? (1u << ssaaSamples) - 1 : 1;
	constants.ssaaLog2 = SsaaActive() ? ssaaLog2 : 0;
}

void DX12Renderer::SetupFrame()
{
	context->BindRaster();
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	cmd->SetGraphicsRootConstantBufferView(1, vtxConstantsGpu);
	cmd->SetGraphicsRootConstantBufferView(2, fragConstantsGpu);
	context->SetVertexBuffer(vertexView);
	context->SetIndexBuffer(indexView);
}

bool DX12Renderer::EnsureDepth(u32 width, u32 height)
{
	if (depth.resource && depth.width >= width && depth.height >= height)
		return true;
	// (SSAA's is made at the screen's size, and with it: SetupSsaa())
	if (ssaaDepthIn)
		return false;
	width = std::max(width, depth.width);
	height = std::max(height, depth.height);
	// (the one there was goes once the GPU is done with it; its views' places are used again)
	u32 dsv = depth.rtv;
	if (depth.resource)
	{
		depth.resource->AddRef();
		context->Release(depth.resource.get());
	}
	depth = DX12Image();
	D3D12_CLEAR_VALUE clear{};
	clear.Format = DX12DepthFormat;
	clear.DepthStencil.Depth = 0.f;
	clear.DepthStencil.Stencil = 0;
	depth.state = stencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if (!dx12_make_texture(device, width, height, 1, DX12DepthResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, depth.state,
			depth.resource, &clear))
	{
		WARN_LOG(RENDERER, "DX12: depth buffer creation failed (%d x %d)", width, height);
		return false;
	}
	if (dsv == ~0u)
	{
		dsv = context->Dsv().Alloc();
		dsvReadStencil = context->Dsv().Alloc();
		dsvReadOnly = context->Dsv().Alloc();
	}
	if (dsv == ~0u || dsvReadStencil == ~0u || dsvReadOnly == ~0u)
	{
		depth = DX12Image();
		return false;
	}
	D3D12_DEPTH_STENCIL_VIEW_DESC view{};
	view.Format = DX12DepthFormat;
	view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView(depth.resource, &view, context->Dsv().Cpu(dsv));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	device->CreateDepthStencilView(depth.resource, &view, context->Dsv().Cpu(dsvReadStencil));
	view.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL | D3D12_DSV_FLAG_READ_ONLY_DEPTH;
	device->CreateDepthStencilView(depth.resource, &view, context->Dsv().Cpu(dsvReadOnly));
	depth.rtv = dsv;
	depth.width = width;
	depth.height = height;
	// (the view is read when a command is recorded: what is bound is bound to the old one)
	context->ForgetTargets();
	return true;
}

void DX12Renderer::DepthStates(D3D12_RESOURCE_STATES depthPlane, D3D12_RESOURCE_STATES stencilPlane)
{
	// (the planes are the subresources: the depth 0, the stencil 1)
	context->Transition(depth.resource, depth.state, depthPlane, 0);
	context->Transition(depth.resource, stencilState, stencilPlane, 1);
}

/* ---- The screen ---- */

void DX12Renderer::ClearTarget(const DX12Image& image, const float color[4])
{
	context->Cmd()->ClearRenderTargetView(context->Rtv().Cpu(image.rtv), color, 0, nullptr);
}

bool DX12Renderer::MakeScreenTarget(ScreenTarget& target, u32 width, u32 height)
{
	if (!makeImage(target.image, width, height, DX12FrameFormat, true))
	{
		target.image = DX12Image();
		return false;
	}
	const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	ClearTarget(target.image, black);
	return true;
}

bool DX12Renderer::EnsureScreen(u32 width, u32 height)
{
	const unsigned count = context->GetPresentCount();

	if (width != screenWidth || height != screenHeight)
	{
		/* Another size. The picture with the last frame goes on as the
		 * picture the next render starts from, at whatever size the screen
		 * now is (Vulkan's carried picture): nothing is read back or
		 * loaded again. */
		DropRestored();
		if (havePicture && lastInExtra)
		{
			restoredPicture = extraTarget.image;
			extraTarget.image = DX12Image();
		}
		else if (havePicture && lastScreen >= 0 && (size_t)lastScreen < screenTargets.size())
		{
			restoredPicture = screenTargets[lastScreen].image;
			screenTargets[lastScreen].image = DX12Image();
		}
		for (ScreenTarget& t : screenTargets)
			DropImage(t.image);
		screenTargets.clear();
		DropImage(extraTarget.image);
		havePicture = false;
		lastInExtra = false;
		lastScreen = -1;
		screenWidth = width;
		screenHeight = height;
	}
	if (screenTargets.size() > count)
	{
		if (lastScreen >= (int)count && !lastInExtra)
		{
			DropRestored();
			if (havePicture)
			{
				restoredPicture = screenTargets[lastScreen].image;
				screenTargets[lastScreen].image = DX12Image();
			}
			havePicture = false;
			lastScreen = -1;
		}
		for (size_t i = count; i < screenTargets.size(); i++)
			DropImage(screenTargets[i].image);
		screenTargets.resize(count);
	}
	while (screenTargets.size() < count)
		screenTargets.push_back(ScreenTarget());
	ScreenTarget& target = screenTargets[screenIndex];
	if (!target.image.resource && !MakeScreenTarget(target, width, height))
		return false;
	if (context->IsHandedOver(&target.image))
	{
		/* A render to the screen after one this frame has handed over
		 * already: this one goes to the extra picture, which takes the
		 * place of the one handed over, and that stays as it is until the
		 * frontend has shown it. (The last picture goes where its picture
		 * goes.) */
		if (!extraTarget.image.resource && !MakeScreenTarget(extraTarget, width, height))
			return false;
		std::swap(target, extraTarget);
		if (lastInExtra)
		{
			lastInExtra = false;
			lastScreen = screenIndex;
		}
		else if (lastScreen == screenIndex)
			lastInExtra = true;
	}
	return EnsureDepth(width, height);
}

void DX12Renderer::DrawLastPicture(u32 width, u32 height)
{
	DX12Image *image = LastPictureImage();
	if (image == nullptr)
		return;
	context->SetViewport((float)width, (float)height);
	const D3D12_RECT whole = { 0, 0, (LONG)width, (LONG)height };
	context->SetScissor(whole);
	/* (the picture kept from another context can be of another size) */
	DX12Quad::Draw(image->srv, !havePicture, -1.f, -1.f, 2.f, 2.f);
}

bool DX12Renderer::BeginScreen()
{
	screenIndex = (int)context->GetPresentIndex();
	// (SSAA: drawn into the samples, with their depth, when they are the screen's size)
	SsaaDepth(ssaaSamples > 1 && ssaaWidth == (u32)screen_width && ssaaHeight == (u32)screen_height);
	if (!EnsureScreen(screen_width, screen_height))
	{
		SsaaDepth(false);
		return false;
	}
	ScreenTarget& target = screenTargets[screenIndex];
	targetRtv = target.image.rtv;
	targetWidth = screenWidth;
	targetHeight = screenHeight;

	matrices.CalcMatrices(&pvrrc);
	if (!BindsScreen())
	{
		// (the per-pixel renderer clears and binds its own pictures, and this one at the end)
		context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
		SetBaseScissor();
		return true;
	}
	if (SsaaActive())
	{
		BeginScreenSsaa(target);
		return true;
	}

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other starts black - unless the render covers only part
	 * of the screen, which leaves the rest as it was: the last picture,
	 * which is in this picture already if the last frame went to it.
	 * (Only for such a render: one that covers the screen pays nothing.) */
	if (KeepsLastPicture() && LastPictureImage() != &target.image)
	{
		DX12Image *last = LastPictureImage();
		/* Copied, where it is the same size and can be read: a picture the
		 * frontend has (version 2) is left in the state it was handed over
		 * in, which is one to copy from with the frontends there are;
		 * drawn otherwise. */
		const bool handedOver = context->IsHandedOver(last);
		if (havePicture && (!handedOver || last->state == D3D12_RESOURCE_STATE_COPY_SOURCE))
		{
			context->Transition(*last, D3D12_RESOURCE_STATE_COPY_SOURCE);
			context->Transition(target.image, D3D12_RESOURCE_STATE_COPY_DEST);
			context->Cmd()->CopyResource(target.image.resource, last->resource);
			context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context->SetTargets(target.image.rtv, depth.rtv);
		}
		else
		{
			context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context->SetTargets(target.image.rtv, depth.rtv);
			const float black[4] = { 0.f, 0.f, 0.f, 1.f };
			ClearTarget(target.image, black);
			if (!handedOver || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			{
				context->Transition(*last, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				DrawLastPicture(screenWidth, screenHeight);
			}
		}
	}
	else
	{
		context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
		context->SetTargets(target.image.rtv, depth.rtv);
		if (!KeepsLastPicture())
		{
			float color[4] = { 0.f, 0.f, 0.f, 1.f };
			if (pvrrc.clearFramebuffer)
			{
				color[0] = VO_BORDER_COL.Red / 255.f;
				color[1] = VO_BORDER_COL.Green / 255.f;
				color[2] = VO_BORDER_COL.Blue / 255.f;
			}
			ClearTarget(target.image, color);
		}
	}
	context->Cmd()->ClearDepthStencilView(context->Dsv().Cpu(depth.rtv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, nullptr);
	context->SetViewport((float)screenWidth, (float)screenHeight);
	SetBaseScissor();
	return true;
}

/* The screen's frame with SSAA: its samples cleared, or, under a render
 * that covers only part of the screen, the last picture drawn into them
 * (each of a pixel's samples its pixel's colour, which resolves to it) */
void DX12Renderer::BeginScreenSsaa(ScreenTarget& target)
{
	DX12Image& samples = ssaaColor[0];
	context->Transition(samples, D3D12_RESOURCE_STATE_RENDER_TARGET);
	context->SetTargets(samples.rtv, depth.rtv, ssaaLog2);
	float color[4] = { 0.f, 0.f, 0.f, 1.f };
	const bool keep = KeepsLastPicture();
	if (!keep && pvrrc.clearFramebuffer)
	{
		color[0] = VO_BORDER_COL.Red / 255.f;
		color[1] = VO_BORDER_COL.Green / 255.f;
		color[2] = VO_BORDER_COL.Blue / 255.f;
	}
	ClearTarget(samples, color);
	DX12Image *last = keep ? LastPictureImage() : nullptr;
	if (last != nullptr)
	{
		/* Drawn from where it is when it can be read there; a picture the
		 * frontend has (version 2), left in the state it was handed over
		 * in, is copied into this frame's picture first, which the resolve
		 * writes all of later */
		const bool handedOver = context->IsHandedOver(last);
		DX12Image *from = nullptr;
		if (!handedOver || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
			from = last;
		else if (havePicture && last->state == D3D12_RESOURCE_STATE_COPY_SOURCE && last->width == target.image.width
				&& last->height == target.image.height)
		{
			context->Transition(target.image, D3D12_RESOURCE_STATE_COPY_DEST);
			context->Cmd()->CopyResource(target.image.resource, last->resource);
			from = &target.image;
		}
		if (from != nullptr)
		{
			context->Transition(*from, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context->SetViewport((float)screenWidth, (float)screenHeight);
			const D3D12_RECT whole = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
			context->SetScissor(whole);
			DX12Quad::Draw(from->srv, !havePicture, -1.f, -1.f, 2.f, 2.f);
		}
	}
	context->Cmd()->ClearDepthStencilView(context->Dsv().Cpu(depth.rtv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, nullptr);
	context->SetViewport((float)screenWidth, (float)screenHeight);
	SetBaseScissor();
}

void DX12Renderer::EndScreen()
{
	FlushBatch();
	ScreenTarget& target = screenTargets[screenIndex];
	const bool ssaa = SsaaActive();
	if (ssaa)
	{
		/* The samples resolved into the picture, all of it. Their contents
		 * are not wanted after that: discarded, which a tiler need not
		 * store (the next frame clears them). */
		DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context->Cmd()->DiscardResource(depth.resource, nullptr);
		SsaaDepth(false);
		// (the per-pixel renderer resolves them itself, as it puts its lists together)
		if (BindsScreen())
		{
			context->Transition(ssaaColor[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
			context->SetTargets(target.image.rtv, ~0u);
			context->SetViewport((float)screenWidth, (float)screenHeight);
			const D3D12_RECT whole = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
			context->SetScissor(whole);
			SsaaResolve(ssaaColor[0].srv);
		}
		for (u32 i = 0; i < SsaaColorImages() && i < 2; i++)
		{
			context->Transition(ssaaColor[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
			context->Cmd()->DiscardResource(ssaaColor[i].resource, nullptr);
		}
	}
	// what goes over the picture, onto it (with the depth test off: whatever depth is bound is not looked at)
	if (!overlay.IsEmpty())
	{
		// (with SSAA, none: the depth of one sample may be smaller than the screen)
		context->SetTargets(target.image.rtv, ssaa ? ~0u : depth.rtv);
		context->SetViewport((float)screenWidth, (float)screenHeight);
		const D3D12_RECT whole = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
		context->SetScissor(whole);
		overlay.Draw(screenWidth, screenHeight);
	}
	context->SetTargets(~0u, ~0u);
	// as the frontend wants it, and off to the GPU
	context->Transition(target.image, context->RequiredState());
	context->Submit();

	havePicture = true;
	lastScreen = screenIndex;
	lastInExtra = false;
	DropRestored();
	context->SetFrame(&target.image);
}

void DX12Renderer::KeepPicture()
{
	if (!havePicture || lastScreen < 0 || (size_t)lastScreen >= screenTargets.size() || context == nullptr)
		return;
	DX12Image *image = LastPictureImage();
	u8 *pixels = last_picture_keep(screenWidth, screenHeight);
	if (pixels != nullptr && (image == nullptr || !dx12_read_picture(image->resource, image->state, screenWidth, screenHeight, pixels)))
		last_picture_drop();
}

/* ---- Renders to textures ---- */

bool DX12Renderer::BeginRTT()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	matrices.CalcMatrices(&pvrrc);
	// (one sample a pixel, SSAA or not)
	SsaaDepth(false);

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
	dx12_rtt_supersede(rttAddr, clipX, clipY, clipWidth, clipHeight);
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

	DX12PooledTexture *spare = rttTexture->Spare();
	if (spare != nullptr && (spare->width != widthPow2 || spare->height != heightPow2 || spare->rtv == ~0u))
	{
		rttTexture->ReleaseSpare();
		spare = nullptr;
	}
	// (one of the pool's, made at the start, where it has one of this shape)
	if (spare == nullptr && !rttTexture->TakeSpare(widthPow2, heightPow2))
	{
		DX12PooledTexture *own = texPool.MakeOwn(DX12TexturePool::TARGET, widthPow2, heightPow2, 1);
		if (own == nullptr)
			return false;
		rttTexture->SetSpare(own);
	}
	spare = rttTexture->Spare();

	targetRtv = spare->rtv;
	targetWidth = upscaledWidth;
	targetHeight = upscaledHeight;

	context->Transition(spare->resource, spare->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (BindsScreen())
	{
		context->SetTargets(spare->rtv, depth.rtv);
		const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		context->Cmd()->ClearRenderTargetView(context->Rtv().Cpu(spare->rtv), black, 0, nullptr);
		context->Cmd()->ClearDepthStencilView(context->Dsv().Cpu(depth.rtv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, nullptr);
	}
	context->SetViewport((float)upscaledWidth, (float)upscaledHeight);
	// only the pixel clip is written
	baseScissor = D3D12_RECT{ (LONG)(clipX * scale), (LONG)(clipY * scale),
			(LONG)((clipX + clipWidth) * scale), (LONG)((clipY + clipHeight) * scale) };
	return true;
}

void DX12Renderer::EndRTT()
{
	FlushBatch();
	context->SetTargets(~0u, ~0u);
	DX12PooledTexture *spare = rttTexture->Spare();
	// to be drawn with, and off to the GPU
	context->Transition(spare->resource, spare->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	context->Submit();
	rttTexture->SwapWithSpare();

	rttTexture->dirty = 0;
	rttTexture->stride_read = 0;	// (rendered, not read)
	libCore_vramlock_Lock(rttTexture->sa_tex, rttTexture->sa + rttTexture->size - 1, rttTexture);
	/* video memory gets it when something first touches it there */
	dx12_rtt_watch(rttTexture, rttAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
	rttTexture = nullptr;
}

/* ---- A frame the game wrote to the framebuffer itself ---- */

bool DX12Renderer::UploadFramebuffer()
{
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return false;

	PixelBuffer<u32> pb;
	int width;
	int height;
	ReadFramebuffer(pb, width, height);

	if (!fbTexture.resource || (u32)width != fbTexture.width || (u32)height != fbTexture.height)
	{
		// (another size: seldom, and between frames)
		DropImage(fbTexture);
		if (!makeImage(fbTexture, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false))
			return false;
	}
	return uploadImage(fbTexture, pb.data(), width, height, 4);
}

/* Into the screen's picture, where the game's 640x480 is, as the OpenGL
 * renderers draw it */
void DX12Renderer::DrawFramebuffer()
{
	screenIndex = (int)context->GetPresentIndex();
	if (!EnsureScreen(screen_width, screen_height))
		return;
	ScreenTarget& target = screenTargets[screenIndex];
	matrices.CalcMatrices(&pvrrc);
	context->Transition(target.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
	context->SetTargets(target.image.rtv, ~0u);
	const float black[4] = { 0.f, 0.f, 0.f, 1.f };
	ClearTarget(target.image, black);
	context->SetViewport((float)screenWidth, (float)screenHeight);
	const D3D12_RECT whole = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
	context->SetScissor(whole);

	const xform& viewport = matrices.GetViewportMatrix();
	const float x0 = xform_x(&viewport, 0.f), x1 = xform_x(&viewport, 640.f);
	const float y0 = xform_y(&viewport, 0.f), y1 = xform_y(&viewport, 480.f);
	DX12Quad::Draw(fbTexture.srv, true,
			x0 * 2.f / screenWidth - 1.f, 1.f - y1 * 2.f / screenHeight,
			(x1 - x0) * 2.f / screenWidth, (y1 - y0) * 2.f / screenHeight);
	EndScreen();
}

/* ---- The frame ---- */

bool DX12Renderer::Render()
{
	const bool held = heldFromProcess;
	heldFromProcess = false;
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
	 * picture: it is waited for, if Process() has not */
	if (!held && !pvrrc.isRTT)
		context->WaitPresentIndex();

	SsaaDepth(false);
	if (pvrrc.isRenderFramebuffer)
	{
		DrawFramebuffer();
		return true;
	}

	/* (a frame decoded on the GPU has its sorted passes sorted already, and
	 * their indices went straight to the decoder's room for them when the
	 * decode was recorded: see TA_GPU_SORT) */
	if (!pvrrc.gpu)
		SortTriangles();
	else
	{
		for (std::vector<u32>& idx : sortedIndexes)
			idx.clear();
		sortedIndexCount = 0;
	}
	if (pvrrc.isRTT ? !BeginRTT() : !BeginScreen())
	{
		rttTexture = nullptr;
		return false;
	}
	SetProvokingVertices();
	if (!UploadGeometry())
	{
		context->SetTargets(~0u, ~0u);
		SsaaDepth(false);
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

/* ---- The draws, batched ---- */

void DX12Renderer::Draw(const DrawState& state, const u32 *constants, u32 indexCount, u32 firstIndex)
{
	if (batchCount != 0 && (batchCount == BATCH_MAX || state.pso != batchState.pso || state.stencilRef != batchState.stencilRef
			|| state.topology != batchState.topology || memcmp(&state.scissor, &batchState.scissor, sizeof(state.scissor))))
		FlushBatch();
	if (batchCount == 0)
		batchState = state;
	DrawArgs& args = batch[batchCount++];
	memcpy(args.constants, constants, sizeof(args.constants));
	args.draw.IndexCountPerInstance = indexCount;
	args.draw.InstanceCount = 1;
	args.draw.StartIndexLocation = firstIndex;
	args.draw.BaseVertexLocation = 0;
	args.draw.StartInstanceLocation = 0;
}

void DX12Renderer::FlushBatch()
{
	if (batchCount == 0)
		return;
	const u32 count = batchCount;
	batchCount = 0;
	if (batchState.pso == nullptr)
		return;
	context->SetPipeline(batchState.pso);
	context->SetScissor(batchState.scissor);
	context->SetStencilRef(batchState.stencilRef);
	context->SetTopology(batchState.topology);
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	statBatches++;
	statBatched += count;
	ID3D12CommandSignature *signature = context->DrawSignature();
	DX12Upload upload;
	if (count > 1 && signature != nullptr && context->Upload(count * sizeof(DrawArgs), 16, upload))
	{
		memcpy(upload.cpu, batch, (size_t)count * sizeof(DrawArgs));
		cmd->ExecuteIndirect(signature, count, upload.buffer, upload.offset, nullptr, 0);
		context->ForgetRootConstants();
		return;
	}
	for (u32 i = 0; i < count; i++)
	{
		const DrawArgs& args = batch[i];
		context->SetRootConstants(args.constants, DX12Context::PolyConstantCount);
		cmd->DrawIndexedInstanced(args.draw.IndexCountPerInstance, 1, args.draw.StartIndexLocation, 0, 0);
	}
}

/* ---- The polygons, as the Direct3D 11 renderer draws them ---- */

void DX12Renderer::DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
		ShadowPass shadowPass, const D3D12_RECT *within)
{
	D3D12_RECT scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	DrawState state;
	if (within != nullptr)
	{
		// only the part of it inside this rectangle, and nothing if there is none
		const D3D12_RECT& clip = tileClip == TileClipping::Outside ? scissorRect : baseScissor;
		const D3D12_RECT r = {
			std::max(within->left, clip.left), std::max(within->top, clip.top),
			std::min(within->right, clip.right), std::min(within->bottom, clip.bottom)
		};
		if (r.right <= r.left || r.bottom <= r.top)
			return;
		state.scissor = r;
	}
	else if (tileClip == TileClipping::Outside)
		state.scissor = scissorRect;
	else
		state.scissor = baseScissor;
	state.scissor = CutScissor(state.scissor);

	float trilinearAlpha = 1.f;
	const bool trilinear = poly.pcw.Texture && poly.tsp.FilterMode > 1 && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1;
	if (trilinear)
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

	/* The polygon's root constants: its bits, what the Direct3D 11
	 * renderer made a shader for */
	PolyConstants constants = {};
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	constants.trilinearAlpha = trilinearAlpha;
	constants.paletteIndex = palette_index;
	constants.prepassAlpha = prepassDraw ? (PrepassAlwaysHides(&poly) ? -1.f : PREPASS_ALPHA_MIN) : 0.f;
	u32 flags = 0;
	if (listType == ListType_Punch_Through)
		flags |= DX12_PF_ALPHA_TEST;
	if ((poly.tileclip >> 28) == 3)
		flags |= DX12_PF_CLIP_INSIDE;
	if (poly.tsp.UseAlpha)
		flags |= DX12_PF_USE_ALPHA;
	if (poly.pcw.Texture)
		flags |= DX12_PF_TEXTURE;
	if (poly.tsp.IgnoreTexA || poly.tcw.PixelFmt == Pixel565)
		flags |= DX12_PF_IGNORE_TEX_A;
	flags |= (poly.tsp.ShadInstr & 3) << DX12_PF_SHAD_INSTR_SHIFT;
	if (poly.pcw.Offset)
		flags |= DX12_PF_OFFSET;
	flags |= (poly.tsp.FogCtrl & 3) << DX12_PF_FOG_SHIFT;
	if (poly.tcw.PixelFmt == PixelBumpMap)
		flags |= DX12_PF_BUMPMAP;
	/* By the polygon alone, as the Vulkan renderer has it: the clamping
	 * limits change, and a shader is kept by the polygon's bit only */
	if (poly.tsp.ColorClamp)
		flags |= DX12_PF_CLAMP;
	if (trilinear)
		flags |= DX12_PF_TRILINEAR;
	if (palette)
		flags |= DX12_PF_PALETTE;
	if (shadowPass != ShadowPass::None)
		flags |= DX12_PF_SHADOWED;
	constants.flags = flags;
	constants.texture = DX12_SRV_NONE;
	if (poly.pcw.Texture && poly.texid != 0)
		constants.texture = ((DX12Texture *)(uintptr_t)poly.texid)->Srv();
	constants.sampler = dx12_sampler_index(poly.tsp);

	// Depth and stencil
	DX12PsoKey key{};
	key.kind = DX12_PSO_POLY;
	key.gouraud = poly.pcw.Gouraud;
	key.depthBound = true;
	key.depthTest = true;
	if (listType == ListType_Punch_Through || sortTriangles)
		key.depthFunc = 6;	// greater or equal
	else
		key.depthFunc = poly.isp.DepthMode;
	// Sorted translucent polygons, per triangle or per strip, do not write
	// depth: a strip drawn in sorted order would hide parts of the ones after it.
	// Their depth prepass does.
	if (prepassDraw)
		key.depthWrite = true;
	else if (sortTriangles)
		key.depthWrite = false;
	// Z Write Disable seems to be ignored for punch-through.
	// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
	else if (listType == ListType_Punch_Through)
		key.depthWrite = true;
	else
		key.depthWrite = !poly.isp.ZWriteDis;

	const bool shadowed = listType == ListType_Opaque || listType == ListType_Punch_Through;
	state.stencilRef = 0;
	if (shadowPass != ShadowPass::None)
	{
		/* Drawn again for its shadow: where this polygon is the one showing
		 * (same depth), takes shadows (stencil bit 7) and is in a volume
		 * (bit 0). Bit 6 marks the pixels drawn this way; whether one already
		 * marked may be drawn over is what tells the two kinds apart. */
		key.depthWrite = false;
		key.depthFunc = 2;
		if (shadowPass == ShadowPass::LaterWins)
		{
			key.stencil = DX12Stencil::ShadowLaterWins;
			state.stencilRef = 0xC1;
		}
		else
		{
			key.stencil = DX12Stencil::ShadowEarlierWins;
			state.stencilRef = 0x81;
		}
	}
	else if (shadowed)
	{
		key.stencil = DX12Stencil::Shadow;
		state.stencilRef = poly.pcw.Shadow != 0 ? 0x80 : 0;
	}
	else
		key.stencil = DX12Stencil::None;

	// Apparently punch-through polys support blending, or at least some combinations
	// An opaque polygon is blended by its instructions too when it is in a
	// list continuation; the ones of the first render pass have had theirs
	// set to source x 1 and destination x 0 when the list was read.
	key.writeColor = !prepassDraw;
	if (!prepassDraw && shadowPass == ShadowPass::None
			&& (listType == ListType_Translucent || listType == ListType_Punch_Through
				|| poly.tsp.SrcInstr != 1 || poly.tsp.DstInstr != 0))
	{
		key.blend = true;
		key.src = poly.tsp.SrcInstr;
		key.dst = poly.tsp.DstInstr;
	}
	key.cull = poly.isp.CullMode == 3 ? 2 : poly.isp.CullMode == 2 ? 1 : 0;
	// SSAA: shaded at each sample, or once a pixel
	key.samples = context->TargetSamples();
	if (key.samples != 0)
		key.rate = SsaaSingleRate(poly) ? DX12_RATE_PIXEL : DX12_RATE_SAMPLE;
	state.pso = context->Pipelines().Get(key);
	state.topology = prepassDraw || (sortTriangles && !settings.pvr.Emulation.AlphaSortMode)
			? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

	static_assert(sizeof(PolyConstants) == DX12Context::PolyConstantCount * 4, "the polygon's root constants");
	u32 words[DX12Context::PolyConstantCount];
	memcpy(words, &constants, sizeof(words));
	// (a frame decoded on the GPU: a draw group by its arguments there)
	if (pvrrc.gpu && !gpuSorted)
		DrawGpu(state, words, listType, shadowPass != ShadowPass::None, first, count);
	else
		Draw(state, words, count, first);
}

void DX12Renderer::DrawGpu(const DrawState& state, const u32 *constants, u32 listType, bool shadow, u32 first, u32 count)
{
	FlushBatch();
	if (state.pso == nullptr)
		return;
	context->SetPipeline(state.pso);
	context->SetScissor(state.scissor);
	context->SetStencilRef(state.stencilRef);
	context->SetTopology(state.topology);
	context->SetRootConstants(constants, DX12Context::PolyConstantCount);
	const DX12TaDraw& gpu = taDecoder.Draw();
	const u64 stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	statBatches++;
	statBatched++;
	if (listType == ListType_Translucent)
		cmd->ExecuteIndirect(gpu.drawIndexed, count, gpu.trDraws, (first + 1) * stride, gpu.trDraws, first * stride);
	else
		cmd->ExecuteIndirect(gpu.drawIndexed, 1, gpu.commands, (shadow ? gpu.shadowCommands : 0) + first * stride, nullptr, 0);
}

// The polygons of a list that take shadows, drawn again where they are in a
// modifier volume: see rend/shadows.h.
void DX12Renderer::DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
		const ScreenBounds& area, const D3D12_RECT& scissor)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		/* (of a frame decoded on the GPU, the GPU has said which groups reach
		 * into the volumes: the others' second commands draw nothing) */
		if (CanDrawShadowed(listType, pp) && (pvrrc.gpu || PolyOverlaps(pp, area)))
			DrawPoly(listType, false, *pp, pp->first, pp->count,
					ShadowLaterWins(listType, pp) ? ShadowPass::LaterWins : ShadowPass::EarlierWins,
					&scissor);
}

ID3D12PipelineState *DX12Renderer::ModVolPipeline(bool depthTest, bool depthWrite, u32 depthFunc, DX12Stencil stencil, u32 cull, bool writeColor, u32 special)
{
	DX12PsoKey key{};
	key.kind = DX12_PSO_MODVOL;
	key.depthBound = true;
	key.depthTest = depthTest;
	key.depthWrite = depthWrite;
	key.depthFunc = depthFunc;
	key.stencil = stencil;
	key.cull = cull;
	key.writeColor = writeColor;
	key.special = special;
	key.samples = context->TargetSamples();
	return context->Pipelines().Get(key);
}

void DX12Renderer::DrawSorted(const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	/* (a frame decoded on the GPU: its sorted indices are this frame's in
	 * the decoder's room for them, and name the GPU's vertices) */
	const u32 base = pvrrc.gpu ? 0 : pvrrc.idx.used();
	if (pvrrc.gpu)
	{
		if (count == 0)
			return;
		FlushBatch();
		context->SetIndexBuffer(taDecoder.Draw().sorted);
		gpuSorted = true;
	}
	for (u32 i = 0; i < count; i++)
		DrawPoly(ListType_Translucent, true, *polys[i].ppid, base + polys[i].first, polys[i].count);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		// Sorted translucent polygons are drawn without writing depth. Where
		// a later render pass needs their depth (Cosmic Smash), it is written
		// now, for those that do not have Z write disabled.
		FlushBatch();
		ID3D12GraphicsCommandList *cmd = context->Cmd();
		context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (u32 i = 0; i < count; i++)
		{
			const SortTrigDrawParam& param = polys[i];
			if (param.count <= 2 || param.ppid->isp.ZWriteDis)
				continue;
			D3D12_RECT scissorRect;
			context->SetScissor(SetTileClip(param.ppid->tileclip, scissorRect) == TileClipping::Outside ? scissorRect : baseScissor);
			const u32 cull = param.ppid->isp.CullMode == 3 ? 2 : param.ppid->isp.CullMode == 2 ? 1 : 0;
			ID3D12PipelineState *pso = ModVolPipeline(true, true, 6, DX12Stencil::None, cull, false, DX12_BLEND_KEY);
			if (pso == nullptr)
				continue;
			context->SetPipeline(pso);
			cmd->DrawIndexedInstanced(param.count, 1, base + param.first, 0, 0);
		}
	}
	if (pvrrc.gpu)
	{
		FlushBatch();
		context->SetIndexBuffer(indexView);
		gpuSorted = false;
	}
}

// The depth prepass of a sorted list: rend/sorter.h
void DX12Renderer::DrawPrepass(const SortTrigDrawParam *polys, u32 count)
{
	if (count == 0)
		return;
	// (as DrawSorted(): of a frame decoded on the GPU, the indices are in the decoder's room)
	const u32 base = pvrrc.gpu ? 0 : pvrrc.idx.used();
	if (pvrrc.gpu)
	{
		FlushBatch();
		context->SetIndexBuffer(taDecoder.Draw().sorted);
		gpuSorted = true;
	}
	prepassDraw = true;
	for (u32 i = 0; i < count; i++)
		DrawPoly(ListType_Translucent, true, *polys[i].ppid, base + polys[i].first, polys[i].count);
	prepassDraw = false;
	if (pvrrc.gpu)
	{
		FlushBatch();
		context->SetIndexBuffer(indexView);
		gpuSorted = false;
	}
}

void DX12Renderer::DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last)
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

void DX12Renderer::DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass, int render_pass)
{
	if (count == 0 || !haveModVols)
		return;
	FlushBatch();
	ID3D12GraphicsCommandList *cmd = context->Cmd();

	// the modifier volumes' triangles, a position each
	context->SetVertexBuffer(modVolView);
	context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->SetScissor(baseScissor);

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
			D3D12_RECT clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				context->SetScissor(clipRect);
			else
				context->SetScissor(baseScissor);
		}

		if (mod_base == -1)
			mod_base = param.first;

		const u32 cull = param.isp.CullMode == 3 ? 2 : param.isp.CullMode == 2 ? 1 : 0;
		ID3D12PipelineState *pso;
		if (!param.isp.VolumeLast && mv_mode > 0)
			pso = ModVolPipeline(true, false, 4, DX12Stencil::Or, cull, false, DX12_BLEND_KEY);	// OR'ing (open volume or quad)
		else
			pso = ModVolPipeline(true, false, 4, DX12Stencil::Xor, cull, false, DX12_BLEND_KEY);	// XOR'ing (closed volume)
		if (pso != nullptr)
		{
			context->SetPipeline(pso);
			context->SetStencilRef(2);
			cmd->DrawInstanced(param.count * 3, 1, param.first * 3, 0);
		}

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			pso = ModVolPipeline(false, false, 7, mv_mode == 1 ? DX12Stencil::Inclusion : DX12Stencil::Exclusion, cull, false, DX12_BLEND_KEY);
			if (pso != nullptr)
			{
				context->SetPipeline(pso);
				context->SetStencilRef(1);
				cmd->DrawInstanced((param.first + param.count - mod_base) * 3, 1, mod_base * 3, 0);
			}
			mod_base = -1;
		}
	}
	context->SetScissor(baseScissor);
	context->SetVertexBuffer(vertexView);

	/* The polygons that take shadows, again, where they are in a volume
	 * (whose bounds the header pass has worked out for a frame decoded on
	 * the GPU, which has the volumes' triangles: TA_GPU_SHADOWS) */
	const f32 *ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
	const ScreenBounds area = pvrrc.gpu ? ScreenBounds{ ga[0], ga[1], ga[2], ga[3] } : ModVolBounds(first, count);
	if (!area.empty())
	{
		int rect[4];
		BoundsToScissor(area, matrices.GetViewportMatrix(), rect);
		const D3D12_RECT scissor = { rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3] };
		DrawShadowed(ListType_Opaque, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count, area, scissor);
		DrawShadowed(ListType_Punch_Through, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count, area, scissor);
		FlushBatch();
		context->SetScissor(baseScissor);
	}

	// What is left: darkened as it is. The background polygon is the
	// rectangle drawn: the first four vertices' positions.
	ID3D12PipelineState *pso = ModVolPipeline(false, false, 7, DX12Stencil::Final, 0, true, DX12_BLEND_ALPHA);
	if (pso != nullptr)
	{
		context->SetPipeline(pso);
		context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->SetStencilRef(0x81);
		cmd->DrawIndexedInstanced(4, 1, 0, 0, 0);
	}
}

void DX12Renderer::DrawFrame()
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
		DrawModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, previous_pass, current_pass, render_pass);
		if (current_pass.autosort)
		{
			// (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT)
			if (pvrrc.gpu)
			{
				u32 n;
				const SortTrigDrawParam *prepass = ta_gpu_prepass(render_pass, &n);
				DrawPrepass(prepass, n);
				const SortTrigDrawParam *sorted = ta_gpu_sorted(render_pass, &n);
				DrawSorted(sorted, n, render_pass < pvrrc.render_passes.used() - 1);
			}
			else if (!settings.pvr.Emulation.AlphaSortMode)
			{
				DrawPrepass(prepassPolys[render_pass].data(), (u32)prepassPolys[render_pass].size());
				DrawSorted(sortedPolys[render_pass].data(), (u32)sortedPolys[render_pass].size(), render_pass < pvrrc.render_passes.used() - 1);
			}
			else
			{
				DrawPrepass(prepassPolys[render_pass].data(), (u32)prepassPolys[render_pass].size());
				SortPParams(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				DrawList(ListType_Translucent, true, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			}
		}
		else
			DrawList(ListType_Translucent, false, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		previous_pass = current_pass;
	}
	FlushBatch();
}

Renderer* rend_D3D12()
{
	return new DX12Renderer();
}
