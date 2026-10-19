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
/* The per-pixel renderer: the translucent polygons' pixels are put in
 * lists, sorted by depth and blended at the end, as the Direct3D 11
 * per-pixel renderer does it (dx11/oit/dx11_oitrenderer.cpp), step for
 * step:
 *
 *  - the opaque and punch-through polygons' depth, and the modifier
 *    volumes into the stencil;
 *  - their colour, where their depth is the one left, darkened where the
 *    stencil says they are in a volume;
 *  - the translucent polygons' pixels into the lists (or, in a list that
 *    is not to be sorted, drawn as they come);
 *  - the translucent modifier volumes, marking the pixels in the lists;
 *  - the lists put together over the colour, into the picture - or, with
 *    a render pass after this one, into a picture the next pass draws on.
 *
 * In bands when a frame has more translucent pixels than the lists hold
 * (ChooseBands()), the counts read back without waiting through a ring of
 * them in a readback buffer, a frame's counts read once the fence says its
 * list is done.
 *
 * Direct3D 12's own: the lists, their heads and the counter are a table of
 * views the root signature has for them (DX12Context::OitFirst); the depth
 * and the stencil are read through views of the depth buffer's planes in
 * that table, each plane in the state its pass needs; what the shaders do
 * to the lists is ordered by barriers between the passes, which Direct3D
 * 11 did by itself; the translucent polygons' parameters go to the GPU in
 * the upload ring, as a root descriptor. The rest is the per-triangle
 * renderer's: the textures, renders to textures, the screen's pictures,
 * the last picture, the batching of the draws. */
#include "../dx12_renderer.h"
#include "../dx12_quad.h"
#include "rend/oit_size.h"
#include "hw/pvr/ta.h"
#include <algorithm>

/* (the most a resource can be: a quarter of the card's memory, but at
 * least 128 MB and at most 2 GB; mingw-w64's header has not the names) */
#ifndef D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM
#define D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM 128
#endif
#ifndef D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM
#define D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM 2048
#endif

class DX12OITRenderer final : public DX12Renderer
{
public:
	bool Init() override
	{
		if (!DX12Renderer::Init())
			return false;
		if (!MakeBuffers(PlannedSize(screen_width, screen_height)))
		{
			WARN_LOG(RENDERER, "DX12 per-pixel renderer initialization failed");
			Term();
			return false;
		}
		PreparePipelines();
		countHead = 0;
		for (u32 i = 0; i < COUNT_SLOTS; i++)
		{
			countWords[i] = 0;
			countFence[i] = 0;
		}
		oitBands[0] = oitBands[1] = 1;
		oitRoomyFrames[0] = oitRoomyFrames[1] = 0;
		band = {};
		return true;
	}

	void Term() override
	{
		if (context != nullptr)
		{
			for (DX12Image& picture : pictures)
				DropImage(picture);
			picturesWidth = picturesHeight = 0;
			DropBuffer(pointers);
			pointersWidth = pointersHeight = 0;
			DropBuffer(pixels);
			DropBuffer(counter);
			if (counts.resource && countsMapped != nullptr)
				counts.resource->Unmap(0, nullptr);
			countsMapped = nullptr;
			DropBuffer(counts);
			bufferSize = room = pixelBufferRoom = 0;
			depthViewed = nullptr;
		}
		DX12Renderer::Term();
	}

protected:
	bool BindsScreen() const override { return false; }
	void DrawFrame() override;
	D3D12_RECT CutScissor(const D3D12_RECT& rect) const override
	{
		if (band.bottom == 0)
			return rect;
		D3D12_RECT cut = rect;
		cut.top = std::max(rect.top, band.top);
		cut.bottom = std::max(std::min(rect.bottom, band.bottom), cut.top);
		return cut;
	}

private:
	enum { COUNT_SLOTS = 4, COUNT_WORDS = 256 };
	struct Buffer
	{
		ComPtr<ID3D12Resource> resource;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	};

	/* For a screen of @width x @height: rend/oit_size.h, and no more than
	 * a resource can be */
	u64 PlannedSize(int width, int height) const
	{
		return oit_buffer_bytes((u32)std::max(width, 0), (u32)std::max(height, 0),
				context->GetVramBudget().available_kib(), (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u);
	}
	bool MakeBuffers(u64 wanted);
	bool MakeBuffer(Buffer& buffer, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
	void DropBuffer(Buffer& buffer);
	bool MakePictures(u32 width, u32 height);
	bool UploadTrPolyParams();
	void PreparePipelines();
	/* The counter back to 0: the lists are empty, the buffer filled from its start */
	void ResetCount();
	/* The counter, as the draws before left it, copied into @word of @slot
	 * of the counts: nothing waits for it */
	void CopyCount(u32 slot, u32 word);
	/* The counts @slot holds, read if the GPU is done with the list that
	 * copied them, and the largest of them in @largest: false when it is
	 * not done, or when nothing was copied there. Never waits. */
	bool ReadCount(u32 slot, u32& largest);
	bool HasCount(u32 slot) const { return slot < COUNT_SLOTS && countWords[slot] != 0; }
	u32 ChooseBands(bool rtt);
	/* What the shaders did to the lists, done before what comes next */
	void ListsBarrier()
	{
		context->UavBarrier(pixels.resource);
		context->UavBarrier(pointers.resource);
		context->UavBarrier(counter.resource);
	}

	void DrawPoly(u32 listType, bool autosort, u32 pass, const PolyParam& poly, u32 first, u32 count);
	void DrawList(u32 listType, bool autosort, u32 pass, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModifierVolumes(bool translucent, int first, int count, const ModifierVolumeParam *params);
	void Resolve(u32 targetRtv, u32 opaqueSrv);
	void ClearStencil()
	{
		DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context->Cmd()->ClearDepthStencilView(context->Dsv().Cpu(depth.rtv), D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, nullptr);
	}

	// the lists (OIT_FRAGMENT_BYTES each), their heads (R32_UINT, a picture's size), the counter (a word)
	Buffer pixels;
	Buffer pointers;
	u32 pointersWidth = 0, pointersHeight = 0;
	Buffer counter;
	u64 bufferSize = 0;
	u32 room = 0;
	// the counts read back: COUNT_SLOTS of COUNT_WORDS, mapped for good
	Buffer counts;
	const u32 *countsMapped = nullptr;
	u32 countWords[COUNT_SLOTS] = {};
	u64 countFence[COUNT_SLOTS] = {};	// the fence value of the list that copied them
	/* What the opaque and punch-through polygons are drawn into, two of
	 * them: with several render passes, each draws on what the last one
	 * put together */
	DX12Image pictures[2];
	u32 picturesWidth = 0;
	u32 picturesHeight = 0;
	// the depth buffer the table's depth and stencil views are of
	ID3D12Resource *depthViewed = nullptr;
	/* The translucent polygons' parameters, in the ring */
	std::vector<u32> trPolyParamsData;
	/* A frame drawn in horizontal bands (DrawFrame()): the band being
	 * drawn, in pixels of the target - from its top to its bottom, which is
	 * 0 when the frame is drawn whole */
	struct { LONG top, bottom; } band = {};
	/* The counts of the frames in flight: the slot the frame being drawn
	 * copies into, and the bands each slot's frame was drawn in and whether
	 * it was a render to a texture */
	u32 countHead = 0;
	u32 countBands[COUNT_SLOTS] = {};
	bool countRtt[COUNT_SLOTS] = {};
	// how many bands frames are drawn in: the screen's [0], renders to a texture [1]
	u32 oitBands[2] = { 1, 1 };
	u32 oitRoomyFrames[2] = {};
};

/* ---- The buffers ---- */

bool DX12OITRenderer::MakeBuffer(Buffer& buffer, u64 bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
	DropBuffer(buffer);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = heapType;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
			__uuidof(ID3D12Resource), (void **)buffer.resource.put())))
	{
		buffer.resource.reset();
		return false;
	}
	buffer.state = state;
	return true;
}

void DX12OITRenderer::DropBuffer(Buffer& buffer)
{
	if (buffer.resource)
	{
		buffer.resource->AddRef();
		context->Release(buffer.resource.get());
		buffer.resource.reset();
	}
	buffer.state = D3D12_RESOURCE_STATE_COMMON;
}

/* The lists: one large buffer (@wanted bytes, or as large as the card will
 * make it), the counter, the counts' readback ring; their views into the
 * table */
bool DX12OITRenderer::MakeBuffers(u64 wanted)
{
	/* (the most a resource can be is a quarter of the card's memory, but
	 * at least 128 MB and at most 2 GB: it is asked for no larger than
	 * that, and halved until the card makes it, a whole number of
	 * pixels each time) */
	const u64 largest = (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u;
	const u64 smallest = (u64)D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u;
	u64 bytes = std::min<u64>(wanted, largest) & ~(u64)(OIT_FRAGMENT_BYTES - 1);
	bool ok = MakeBuffer(pixels, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	while (!ok && bytes > smallest)
	{
		bytes = std::max((bytes / 2) & ~(u64)(OIT_FRAGMENT_BYTES - 1), smallest);
		ok = MakeBuffer(pixels, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: pixel buffer creation failed");
		return false;
	}
	bufferSize = wanted;
	room = pixelBufferRoom = (u32)(bytes / OIT_FRAGMENT_BYTES);
	if (bytes < wanted)
		NOTICE_LOG(RENDERER, "DX12: pixel buffer of %u MB", (u32)(bytes / 1024 / 1024));
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = room;
	uav.Buffer.StructureByteStride = OIT_FRAGMENT_BYTES;
	device->CreateUnorderedAccessView(pixels.resource, nullptr, &uav, context->Srv().Cpu(DX12Context::OitFirst + 0));

	// the counter: a word, counted up by the shaders
	if (!MakeBuffer(counter, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
	{
		WARN_LOG(RENDERER, "DX12: pixel counter creation failed");
		return false;
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC counterUav{};
	counterUav.Format = DXGI_FORMAT_R32_TYPELESS;
	counterUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	counterUav.Buffer.NumElements = 4;
	counterUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	device->CreateUnorderedAccessView(counter.resource, nullptr, &counterUav, context->Srv().Cpu(DX12Context::OitFirst + 2));

	// the counts read back, mapped once
	if (!counts.resource)
	{
		if (!MakeBuffer(counts, COUNT_SLOTS * COUNT_WORDS * sizeof(u32), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))
		{
			WARN_LOG(RENDERER, "DX12: pixel count readback buffer creation failed");
			return false;
		}
		const D3D12_RANGE whole = { 0, COUNT_SLOTS * COUNT_WORDS * sizeof(u32) };
		void *p = nullptr;
		if (FAILED(counts.resource->Map(0, &whole, &p)))
			return false;
		countsMapped = (const u32 *)p;
	}
	return true;
}

bool DX12OITRenderer::MakePictures(u32 width, u32 height)
{
	/* (made again for another size of screen; a render to a texture is not
	 * one, and a frame that wants more is drawn in bands) */
	const u64 wanted = PlannedSize(screen_width, screen_height);
	if (wanted != bufferSize && !MakeBuffers(wanted))
		return false;

	// the lists' heads, a picture of at least this size: none of them a list (EOL)
	if (!pointers.resource || width > pointersWidth || height > pointersHeight)
	{
		width = std::max(width, pointersWidth);
		height = std::max(height, pointersHeight);
		DropBuffer(pointers);
		pointersWidth = pointersHeight = 0;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		pointers.state = D3D12_RESOURCE_STATE_COPY_DEST;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, pointers.state, nullptr,
				__uuidof(ID3D12Resource), (void **)pointers.resource.put())))
		{
			WARN_LOG(RENDERER, "DX12: per-pixel list heads creation failed (%d x %d)", width, height);
			pointers.resource.reset();
			return false;
		}
		// filled with EOL through the ring
		const u32 pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		DX12Upload upload;
		if (!context->Upload(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, upload))
		{
			WARN_LOG(RENDERER, "DX12: the upload ring is too small for the per-pixel list heads (%u KB)", pitch * height / 1024);
			DropBuffer(pointers);
			return false;
		}
		memset(upload.cpu, 0xff, (size_t)pitch * height);
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = pointers.resource;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = upload.buffer;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Offset = upload.offset;
		src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
		src.PlacedFootprint.Footprint.Width = width;
		src.PlacedFootprint.Footprint.Height = height;
		src.PlacedFootprint.Footprint.Depth = 1;
		src.PlacedFootprint.Footprint.RowPitch = pitch;
		context->Cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		context->Transition(pointers.resource, pointers.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_UINT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(pointers.resource, nullptr, &uav, context->Srv().Cpu(DX12Context::OitFirst + 1));
		pointersWidth = width;
		pointersHeight = height;
	}

	if (!pictures[0].resource || width > picturesWidth || height > picturesHeight)
	{
		width = std::max(width, picturesWidth);
		height = std::max(height, picturesHeight);
		picturesWidth = picturesHeight = 0;
		for (DX12Image& picture : pictures)
		{
			DropImage(picture);
			picture.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			picture.width = width;
			picture.height = height;
			if (!dx12_make_texture(device, width, height, 1, DX12FrameFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, picture.state, picture.resource))
			{
				WARN_LOG(RENDERER, "DX12: per-pixel renderer picture creation failed (%d x %d)", width, height);
				return false;
			}
			picture.srv = context->Srv().Alloc();
			picture.rtv = context->Rtv().Alloc();
			if (picture.srv == ~0u || picture.rtv == ~0u)
			{
				DropImage(picture);
				return false;
			}
			context->MakeSrv(picture.resource, DX12FrameFormat, 1, picture.srv);
			device->CreateRenderTargetView(picture.resource, nullptr, context->Rtv().Cpu(picture.rtv));
		}
		picturesWidth = width;
		picturesHeight = height;
	}
	if (!EnsureDepth(width, height))
		return false;
	// the shaders' views of the depth buffer's planes, when it is another
	if (depthViewed != depth.resource.get())
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		view.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
		view.Texture2D.PlaneSlice = 1;
		device->CreateShaderResourceView(depth.resource, &view, context->Srv().Cpu(DX12Context::OitFirst + 3));
		view.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		view.Texture2D.PlaneSlice = 0;
		device->CreateShaderResourceView(depth.resource, &view, context->Srv().Cpu(DX12Context::OitFirst + 4));
		depthViewed = depth.resource;
	}
	return true;
}

/* What the shaders that put the lists together need to know of each
 * translucent polygon: its TSP, depth function and mask, whether it takes
 * shadows, and its second volume's TSP */
bool DX12OITRenderer::UploadTrPolyParams()
{
	const int count = pvrrc.global_param_tr.used();
	trPolyParamsData.resize(std::max(count, 1) * 2);
	trPolyParamsData[0] = trPolyParamsData[1] = 0;
	const PolyParam *pp = pvrrc.global_param_tr.head();
	for (int i = 0; i < count; i++, pp++)
	{
		trPolyParamsData[i * 2] = (pp->tsp.full & 0xffff00c0) | ((pp->isp.full >> 16) & 0xe400) | ((pp->pcw.full >> 7) & 1);
		trPolyParamsData[i * 2 + 1] = pp->tsp1.full;
	}
	const u32 size = (u32)(trPolyParamsData.size() * sizeof(u32));
	DX12Upload upload;
	if (!context->Upload(size, 16, upload))
		return false;
	memcpy(upload.cpu, trPolyParamsData.data(), size);
	context->Cmd()->SetGraphicsRootShaderResourceView(6, upload.gpu);
	return true;
}

/* ---- The counts ---- */

void DX12OITRenderer::ResetCount()
{
	DX12Upload zero;
	if (!context->Upload(4, 4, zero))
		return;
	memset(zero.cpu, 0, 4);
	context->Transition(counter.resource, counter.state, D3D12_RESOURCE_STATE_COPY_DEST);
	context->Cmd()->CopyBufferRegion(counter.resource, 0, zero.buffer, zero.offset, 4);
	context->Transition(counter.resource, counter.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void DX12OITRenderer::CopyCount(u32 slot, u32 word)
{
	if (slot >= COUNT_SLOTS || word >= COUNT_WORDS || !counts.resource)
		return;
	context->Transition(counter.resource, counter.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	context->Cmd()->CopyBufferRegion(counts.resource, ((u64)slot * COUNT_WORDS + word) * sizeof(u32), counter.resource, 0, 4);
	context->Transition(counter.resource, counter.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (word >= countWords[slot])
		countWords[slot] = word + 1;
	// (the list being recorded is the next one submitted)
	countFence[slot] = context->LastSubmitted() + 1;
}

bool DX12OITRenderer::ReadCount(u32 slot, u32& largest)
{
	if (slot >= COUNT_SLOTS || countWords[slot] == 0 || countsMapped == nullptr)
		return false;
	if (context->Completed() < countFence[slot])
		return false;
	const u32 *words = countsMapped + (size_t)slot * COUNT_WORDS;
	largest = 0;
	for (u32 i = 0; i < countWords[slot]; i++)
		largest = std::max(largest, words[i]);
	countWords[slot] = 0;
	return true;
}

/* The bands this frame is drawn in, from the counts of the oldest frame
 * whose list the GPU is done with: never this frame's, which nothing
 * waits for. More at once when that one had pixels that found no room;
 * fewer when a second of frames in a row would have fitted in fewer. Each
 * band is taken to need what the fullest one did, with a quarter more
 * room. The screen's frames and renders to a texture are counted apart. */
u32 DX12OITRenderer::ChooseBands(bool rtt)
{
	constexpr u32 MaxBands = 64;
	const u32 oldest = (countHead + 1) % COUNT_SLOTS;	// when all are in flight
	for (u32 k = 0; k < COUNT_SLOTS; k++)
	{
		const u32 slot = (oldest + k) % COUNT_SLOTS;
		if (!HasCount(slot))
			continue;
		u32 fragments;
		if (!ReadCount(slot, fragments))
			// (still being drawn: the ones after it are younger)
			break;
		const u32 drawnIn = std::max<u32>(std::min<u32>(countBands[slot], MaxBands), 1);
		const u64 roomNow = std::max<u64>(room, 1);
		const u64 frame = (u64)fragments * drawnIn;
		const u32 wanted = (u32)std::max<u64>(std::min<u64>((frame + frame / 4 + roomNow - 1) / roomNow, MaxBands), 1);
		u32& bands = oitBands[countRtt[slot] ? 1 : 0];
		u32& roomy = oitRoomyFrames[countRtt[slot] ? 1 : 0];
		const u32 was = bands;
		if (fragments > roomNow)
		{
			bands = std::min<u32>(std::max({ bands, wanted, drawnIn + 1 }), MaxBands);
			roomy = 0;
		}
		else if (wanted < bands)
		{
			if (++roomy >= 60)
			{
				bands = wanted;
				roomy = 0;
			}
		}
		else
			roomy = 0;
		if (bands != was)
			NOTICE_LOG(RENDERER, "DX12 per-pixel renderer: %u translucent pixels in a band of %u%s, room for %u: %u band%s",
					fragments, drawnIn, countRtt[slot] ? " (a render to a texture)" : "", (u32)roomNow, bands, bands > 1 ? "s" : "");
		break;
	}
	return oitBands[rtt ? 1 : 0];
}

/* ---- The pipelines ---- */

/* The ones the passes draw with, made before a game draws (the
 * per-triangle renderer's are made by the context): the depth pass's with
 * every depth mode, the colour pass's (equal), the lists' (greater or
 * equal, nothing written), the unsorted translucent ones, the lists put
 * together, the translucent volumes */
void DX12OITRenderer::PreparePipelines()
{
	static const u8 blends[][2] = { { 4, 5 }, { 1, 1 }, { 1, 0 }, { 4, 1 }, { 0, 1 }, { 1, 5 } };
	DX12Pipelines& pipelines = context->Pipelines();
	DX12PsoKey k{};
	for (u32 g = 0; g < 2; g++)
		for (u32 cull = 0; cull < 3; cull++)
		{
			k = DX12PsoKey{};
			k.kind = DX12_PSO_OIT;
			k.gouraud = g != 0;
			k.cull = cull;
			k.depthBound = true;
			k.depthTest = true;
			// the depth pass: opaque, every mode; punch-through, blended, greater or equal
			k.pass = DX12_PASS_DEPTH;
			k.stencil = DX12Stencil::Shadow;
			for (u32 func = 1; func < 8; func++)
				for (u32 w = 0; w < 2; w++)
				{
					k.depthFunc = func;
					k.depthWrite = w != 0;
					pipelines.Prepare(k);
				}
			k.blend = true;
			k.writeColor = true;
			k.depthFunc = 6;
			k.depthWrite = true;
			for (const u8 *b : blends)
			{
				k.src = b[0];
				k.dst = b[1];
				pipelines.Prepare(k);
			}
			// the colour pass: where the depth is the one left; the unsorted translucent ones written
			k.pass = DX12_PASS_COLOR;
			k.stencil = DX12Stencil::None;
			k.depthWrite = false;
			k.depthFunc = 2;
			for (const u8 *b : blends)
			{
				k.src = b[0];
				k.dst = b[1];
				pipelines.Prepare(k);
				k.depthFunc = 6;
				k.depthWrite = true;
				pipelines.Prepare(k);
				k.depthFunc = 7;
				pipelines.Prepare(k);
				k.depthWrite = false;
				k.depthFunc = 2;
			}
			// the lists: nothing written but the lists
			k.pass = DX12_PASS_OIT;
			k.blend = false;
			k.writeColor = false;
			k.depthFunc = 6;
			k.depthWrite = false;
			pipelines.Prepare(k);
			// the translucent polygons' depth, for a pass after
			k.pass = DX12_PASS_DEPTH;
			k.depthWrite = true;
			pipelines.Prepare(k);
		}
	k = DX12PsoKey{};
	k.kind = DX12_PSO_OIT_FINAL;
	k.writeColor = true;
	k.depthFunc = 7;
	pipelines.Prepare(k);
	k = DX12PsoKey{};
	k.kind = DX12_PSO_OIT_MODVOL;
	k.depthFunc = 7;
	for (u32 cull = 0; cull < 3; cull++)
	{
		k.cull = cull;
		pipelines.Prepare(k);
	}
}

/* ---- The draws ---- */

void DX12OITRenderer::DrawPoly(u32 listType, bool autosort, u32 pass, const PolyParam& poly, u32 first, u32 count)
{
	D3D12_RECT scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	DrawState state;
	state.scissor = CutScissor(tileClip == TileClipping::Outside ? scissorRect : baseScissor);

	float trilinearAlpha = 1.f;
	const bool trilinear = poly.tsp.FilterMode > 1 && poly.pcw.Texture && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1;
	if (trilinear)
	{
		trilinearAlpha = 0.25f * (poly.tsp.MipMapD & 0x3);
		if (poly.tsp.FilterMode == 2)
			// Trilinear pass A
			trilinearAlpha = 1.f - trilinearAlpha;
	}

	const bool twoVolumes = poly.tsp1.full != (u32)-1 || poly.tcw1.full != (u32)-1;
	const bool keepAll = twoVolumes || poly.tsp.SrcSelect;
	const bool palette = BaseTextureCacheData::IsGpuHandledPaletted(poly.tsp, poly.tcw);
	float palette_index = 0.f;
	if (palette)
	{
		if (poly.tcw.PixelFmt == PixelPal4)
			palette_index = (float(poly.tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			palette_index = (float((poly.tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}

	PolyConstants constants = {};
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	constants.trilinearAlpha = trilinearAlpha;
	constants.paletteIndex = palette_index;
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
	if (poly.tsp.ColorClamp)
		flags |= DX12_PF_CLAMP;
	if (palette)
		flags |= DX12_PF_PALETTE;
	constants.flags = flags;
	// The per-pixel pass drops a fragment whose blend would leave the
	// pixel as it is. It cannot tell that when the source is the second
	// accumulation buffer, or when the polygon has a second volume and
	// which blend applies is known only when the lists are put
	// together: those get a blend (8) that none of its cases take.
	u32 oit = ((keepAll ? 8u : poly.tsp.SrcInstr) << DX12_OF_SRC_SHIFT) | ((keepAll ? 8u : poly.tsp.DstInstr) << DX12_OF_DST_SHIFT);
	if (twoVolumes)
	{
		oit |= DX12_OF_TWO_VOLUMES | ((u32)poly.tsp1.SrcInstr << DX12_OF_SRC1_SHIFT) | ((u32)poly.tsp1.DstInstr << DX12_OF_DST1_SHIFT)
				| ((u32)poly.tsp1.ShadInstr << DX12_OF_SHAD_INSTR1_SHIFT) | ((u32)poly.tsp1.FogCtrl << DX12_OF_FOG1_SHIFT);
		if (poly.tsp1.UseAlpha)
			oit |= DX12_OF_USE_ALPHA1;
		if (poly.tsp1.IgnoreTexA)
			oit |= DX12_OF_IGNORE_TEX_A1;
	}
	constants.oitFlags = oit;
	constants.texture = DX12_SRV_NONE;
	constants.texture1 = DX12_SRV_NONE;
	if (poly.pcw.Texture)
	{
		if (poly.texid != 0)
			constants.texture = ((DX12Texture *)(uintptr_t)poly.texid)->Srv();
		if (twoVolumes && poly.texid1 != 0)
			constants.texture1 = ((DX12Texture *)(uintptr_t)poly.texid1)->Srv();
	}
	constants.sampler = dx12_sampler_index(poly.tsp);
	constants.sampler1 = dx12_sampler_index(poly.tsp1);
	constants.polyNumber = listType == ListType_Translucent ? (u32)(&poly - pvrrc.global_param_tr.head()) : 0;

	// Depth and stencil
	DX12PsoKey key{};
	key.kind = DX12_PSO_OIT;
	key.pass = pass;
	key.gouraud = poly.pcw.Gouraud;
	key.depthBound = true;
	key.depthTest = true;
	if (pass == DX12_PASS_COLOR && !poly.isp.ZWriteDis && listType != ListType_Translucent)
		key.depthFunc = 2;	// equal
	else if (listType == ListType_Punch_Through || autosort)
		key.depthFunc = 6;	// greater or equal
	else
		key.depthFunc = poly.isp.DepthMode;
	/* Depth is written in the depth pass. In the colour pass opaque and
	 * punch-through polygons are drawn where their depth is the one the
	 * depth pass left, and translucent ones that are sorted only into the
	 * lists; the translucent polygons that are not sorted are drawn there,
	 * in order, each tested against the depth the ones before it wrote, so
	 * they still write. */
	const bool colourPassWrites = pass == DX12_PASS_COLOR && !autosort && listType == ListType_Translucent;
	if (pass != DX12_PASS_DEPTH && !colourPassWrites)
		key.depthWrite = false;
	// Z Write Disable seems to be ignored for punch-through.
	// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
	else if (listType == ListType_Punch_Through)
		key.depthWrite = true;
	else
		key.depthWrite = !poly.isp.ZWriteDis;

	state.stencilRef = 0;
	if (pass == DX12_PASS_DEPTH && (listType == ListType_Opaque || listType == ListType_Punch_Through))
	{
		key.stencil = DX12Stencil::Shadow;
		state.stencilRef = poly.pcw.Shadow != 0 ? 0x80 : 0;
	}
	else
		key.stencil = DX12Stencil::None;

	// Apparently punch-through polys support blending, or at least some combinations
	if (listType == ListType_Punch_Through || pass == DX12_PASS_COLOR)
	{
		key.blend = true;
		key.writeColor = true;
		key.src = poly.tsp.SrcInstr;
		key.dst = poly.tsp.DstInstr;
	}
	key.cull = poly.isp.CullMode == 3 ? 2 : poly.isp.CullMode == 2 ? 1 : 0;
	state.pso = context->Pipelines().Get(key);
	state.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

	u32 words[DX12Context::PolyConstantCount];
	memcpy(words, &constants, sizeof(words));
	Draw(state, words, count, first);
}

void DX12OITRenderer::DrawList(u32 listType, bool autosort, u32 pass, const List<PolyParam>& polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (pp->count > 2)
			DrawPoly(listType, autosort, pass, *pp, pp->first, pp->count);
	FlushBatch();
}

void DX12OITRenderer::DrawModifierVolumes(bool translucent, int first, int count, const ModifierVolumeParam *modVolParams)
{
	if (count == 0 || !haveModVols)
		return;
	FlushBatch();
	ID3D12GraphicsCommandList *cmd = context->Cmd();

	// the modifier volumes' triangles, a position each
	context->SetVertexBuffer(modVolView);
	context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->SetScissor(CutScissor(baseScissor));

	const ModifierVolumeParam* params = &modVolParams[first];
	int mod_base = -1;

	for (int cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		{
			D3D12_RECT clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				context->SetScissor(CutScissor(clipRect));
			else
				context->SetScissor(CutScissor(baseScissor));
		}

		if (mod_base == -1)
			mod_base = param.first;

		const u32 cull = param.isp.CullMode == 3 ? 2 : param.isp.CullMode == 2 ? 1 : 0;
		ID3D12PipelineState *pso;
		u32 constants[DX12Context::PolyConstantCount] = {};
		if (translucent)
		{
			// (the lists are marked by the shaders, and nothing else is tested or written)
			DX12PsoKey key{};
			key.kind = DX12_PSO_OIT_MODVOL;
			key.depthFunc = 7;
			key.cull = cull;
			pso = context->Pipelines().Get(key);
			constants[13] = !param.isp.VolumeLast && mv_mode > 0 ? DX12_OF_MV_OR : DX12_OF_MV_XOR;
			if (pso != nullptr)
			{
				context->SetPipeline(pso);
				context->SetRootConstants(constants, DX12Context::PolyConstantCount);
				cmd->DrawInstanced(param.count * 3, 1, param.first * 3, 0);
			}
		}
		else
		{
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
		}

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			if (translucent)
			{
				// (what the volumes before did to the lists is done)
				context->UavBarrier(pixels.resource);
				constants[13] = mv_mode == 1 ? DX12_OF_MV_INCLUSION : DX12_OF_MV_EXCLUSION;
				if (pso != nullptr)
				{
					context->SetRootConstants(constants, DX12Context::PolyConstantCount);
					cmd->DrawInstanced((param.first + param.count - mod_base) * 3, 1, mod_base * 3, 0);
					context->UavBarrier(pixels.resource);
				}
			}
			else
			{
				pso = ModVolPipeline(false, false, 7, mv_mode == 1 ? DX12Stencil::Inclusion : DX12Stencil::Exclusion, cull, false, DX12_BLEND_KEY);
				if (pso != nullptr)
				{
					context->SetPipeline(pso);
					context->SetStencilRef(1);
					cmd->DrawInstanced((param.first + param.count - mod_base) * 3, 1, mod_base * 3, 0);
				}
			}
			mod_base = -1;
		}
	}
	context->SetScissor(CutScissor(baseScissor));
	context->SetVertexBuffer(vertexView);
}

/* The lists put together over the opaque polygons' picture (at @opaqueSrv
 * of the table), into @targetRtv: the whole picture, not the part the game
 * clips its drawing to - what is outside that has to be written too */
void DX12OITRenderer::Resolve(u32 target, u32 opaqueSrv)
{
	FlushBatch();
	context->SetTargets(target, ~0u);
	DX12PsoKey key{};
	key.kind = DX12_PSO_OIT_FINAL;
	key.writeColor = true;
	key.depthFunc = 7;
	ID3D12PipelineState *pso = context->Pipelines().Get(key);
	if (pso == nullptr)
		return;
	context->SetPipeline(pso);
	context->SetScissor(CutScissor(D3D12_RECT{ 0, 0, (LONG)targetWidth, (LONG)targetHeight }));
	context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	u32 constants[DX12Context::PolyConstantCount] = {};
	constants[8] = opaqueSrv;
	context->SetRootConstants(constants, DX12Context::PolyConstantCount);
	context->Cmd()->DrawInstanced(4, 1, 0, 0);
}

void DX12OITRenderer::DrawFrame()
{
	if (!MakePictures(targetWidth, targetHeight) || !UploadTrPolyParams() || !pixels.resource)
		return;
	context->SetViewport((float)targetWidth, (float)targetHeight);
	ID3D12GraphicsCommandList *cmd = context->Cmd();

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	float clear[4] = { 0.f, 0.f, 0.f, 1.f };
	if (pvrrc.clearFramebuffer)
	{
		clear[0] = VO_BORDER_COL.Red / 255.f;
		clear[1] = VO_BORDER_COL.Green / 255.f;
		clear[2] = VO_BORDER_COL.Blue / 255.f;
	}
	for (DX12Image& picture : pictures)
		context->Transition(picture, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cmd->ClearRenderTargetView(context->Rtv().Cpu(pictures[0].rtv), clear, 0, nullptr);
	if (pvrrc.isRTT)
		// (what is outside the pixel clip, as the per-triangle renderer leaves it)
		cmd->ClearRenderTargetView(context->Rtv().Cpu(targetRtv), clear, 0, nullptr);
	DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	cmd->ClearDepthStencilView(context->Dsv().Cpu(depth.rtv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.f, 0, 0, nullptr);

	/* In bands, when the frames before had more translucent pixels than the
	 * lists have room for (ChooseBands()): all of the frame's render passes
	 * once a band, every draw cut to the band by the scissor (CutScissor()),
	 * each band's lists starting empty and put on the picture before the
	 * next band's. Nothing is dropped and nothing is allocated; the opaque
	 * polygons are drawn once a band. (The clears above are of the whole
	 * target: a band's draws touch its own pixels alone, so the others'
	 * depth is still clear when their band comes.) */
	const u32 bands = std::max<u32>(std::min<u32>(ChooseBands(pvrrc.isRTT), std::max<u32>(targetHeight, 1)), 1);
	const u32 bandHeight = (targetHeight + bands - 1) / bands;
	const u32 bandCount = std::max<u32>((targetHeight + bandHeight - 1) / bandHeight, 1);
	countWords[countHead] = 0;
	u32 countWord = 0;
	const int render_pass_count = pvrrc.render_passes.used();
	const D3D12_RECT whole = { 0, 0, (LONG)targetWidth, (LONG)targetHeight };

	for (u32 bandIndex = 0; bandIndex < bandCount; bandIndex++)
	{
	band.top = (LONG)(bandIndex * bandHeight);
	band.bottom = bandCount > 1 ? (LONG)std::min<u32>((bandIndex + 1) * bandHeight, targetHeight) : 0;
	int cur = 0;
	RenderPass previous_pass = {};
	for (int render_pass = 0; render_pass < render_pass_count; render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d", render_pass + 1,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.mvo_count - previous_pass.mvo_count,
				current_pass.mvo_tr_count - previous_pass.mvo_tr_count, current_pass.autosort);

		const bool finalPass = render_pass == render_pass_count - 1;
		if (render_pass != 0)
			// (each pass has its own volumes)
			ClearStencil();

		// Depth and stencil. The pixel counter is reset: the lists are
		// empty, and the buffer is filled from its start.
		ResetCount();
		DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		context->Transition(pictures[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
		context->SetTargets(pictures[cur].rtv, depth.rtv);
		DrawList(ListType_Opaque, false, DX12_PASS_DEPTH, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(ListType_Punch_Through, false, DX12_PASS_DEPTH, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);
		DrawModifierVolumes(false, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());

		// Colour, the stencil read by the shaders
		DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		context->SetTargets(pictures[cur].rtv, dsvReadStencil);

		/* A render that covers only part of the screen leaves the rest as
		 * it was: the last picture goes in first, under everything */
		if (render_pass == 0 && !pvrrc.isRTT && KeepsLastPicture())
		{
			DX12Image *last = LastPictureImage();
			if (last != nullptr && (!context->IsHandedOver(last) || last->state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
			{
				context->Transition(*last, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				// (the quad is drawn with the scissor that is set: the band's)
				context->SetScissor(CutScissor(whole));
				DX12Quad::Draw(last->srv, !havePicture, -1.f, -1.f, 2.f, 2.f);
			}
		}

		// OP + PT
		DrawList(ListType_Opaque, false, DX12_PASS_COLOR, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(ListType_Punch_Through, false, DX12_PASS_COLOR, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);

		// TR
		/* (In the first frame after the per-pixel lists are made, too: they
		 * start empty, and that frame's translucent polygons are drawn like
		 * any other's.) */
		if (current_pass.autosort)
		{
			// into the lists, tested against the depth by the shaders as well
			DepthStates(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
			context->SetTargets(pictures[cur].rtv, dsvReadOnly);
			DrawList(ListType_Translucent, true, DX12_PASS_OIT, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			ListsBarrier();
			// how many pixels this band's pass asked for, copied out
			CopyCount(countHead, countWord++);
		}
		else
			DrawList(ListType_Translucent, false, DX12_PASS_COLOR, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);

		// The translucent modifier volumes, and the lists put together
		const u32 target = finalPass ? targetRtv : pictures[cur ^ 1].rtv;
		context->SetTargets(target, ~0u);
		if (current_pass.mv_op_tr_shared)
			DrawModifierVolumes(true, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());
		else
			DrawModifierVolumes(true, previous_pass.mvo_tr_count, current_pass.mvo_tr_count - previous_pass.mvo_tr_count, pvrrc.global_param_mvo_tr.head());
		ListsBarrier();
		context->Transition(pictures[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		Resolve(target, pictures[cur].srv);
		ListsBarrier();

		if (!finalPass)
		{
			// The translucent polygons' depth, for the passes after this one
			DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			context->Transition(pictures[cur], D3D12_RESOURCE_STATE_RENDER_TARGET);
			context->SetTargets(pictures[cur].rtv, depth.rtv);
			DrawList(ListType_Translucent, current_pass.autosort, DX12_PASS_DEPTH, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			// the next pass draws on what this one put together
			cur ^= 1;
		}

		previous_pass = current_pass;
	}
	}
	band = {};
	// (what is drawn on the picture after this is not cut to the last band; the depth as the base renderer has it)
	context->SetScissor(whole);
	DepthStates(D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	// the frame's counts are in its slot: the next frame's go in the next
	countBands[countHead] = bandCount;
	countRtt[countHead] = pvrrc.isRTT;
	countHead = (countHead + 1) % COUNT_SLOTS;
	// (the target is left bound: the overlay goes onto it, and the end of the render takes it)
	context->SetTargets(targetRtv, depth.rtv);
}

Renderer* rend_OITD3D12()
{
	return new DX12OITRenderer();
}
