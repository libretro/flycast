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
/* GPU-side TA processing in Direct3D 11: see dx11_ta_decoder.h */
#include "dx11_ta_decoder.h"
#include "dx11context_lr.h"
#include "dx11_nvapi.h"
#include "rend/ta_gpu.h"
#include <algorithm>
#include <vector>

extern int screen_height;

/* The kernels, by TAG_K_*, as shader model 5.0 DXBC made from rend/ta_gpu.h
 * when the core was built: tools/rend/kernels_dx12.sh, which checks that
 * they still are its. Nothing is compiled when the core runs. */
#include "dx11_ta_dxbc.inc"
static_assert(sizeof(dx11_ta_dxbc) / sizeof(dx11_ta_dxbc[0]) == TAG_KERNELS + 1, "a kernel for each TAG_K_*");

// (the renderers draw from the records and commands as they are)
static_assert(TAG_C_WORDS * 4 == sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS), "a record is a draw's arguments");
static_assert(TAG_V_WORDS * 4 == sizeof(Vertex), "a vertex as the renderers draw it");
static_assert(TAG_P_WORDS == 2, "a translucent polygon's words are a pair");

/* The kernels' outputs, in the order of their unordered access views
 * (u0 to u8, as rend/ta_gpu.h has them); then what the decoder has of its
 * own. A compute shader of feature level 11.0 has eight views, u0 to u7:
 * the polygon words (u8) and the sorted indices (u9) are each written by
 * one kernel alone, which has them at u1 in place of the modifier volume
 * triangles that it does not touch (tools/rend/kernels_dx12.sh,
 * ta11_source_of()) and has them bound there (Record()) */
enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
	B_ARGS = B_OUTPUTS,	// the dispatches' arguments, copied out of the frame's words
	B_FRM0,			// the frame's words as each decode starts (fZ_max 1, the rest 0)
	B_ZERO,			// zeroes, copied where a decode wants them
	B_INST,			// 0, 1, 2...: the translucent strips' numbers (DX11TaDraw::instances)
	B_SORT,			// (TA_GPU_SORT) the sorted translucent indices, and the prepass's checks after them
	B_COUNT };

/* The views bound for the kernels: u0 to u7 */
enum { UAV_SLOTS = 8 };
static_assert((int)B_TRD + 1 == (int)UAV_SLOTS && (int)B_TRP == (int)UAV_SLOTS, "the outputs at u0 to u7, and the polygon words after");

/* How many frames' tables there are room for: a frame's piece is written
 * again once the GPU is done with the decode that read it last */
enum { PIECES = 8 };

struct DX11TaDecoder::Resources
{
	/* The frames' input, a dynamic buffer: PIECES pieces of tables, then a
	 * ring the frames' data are copied into, each where the last one's
	 * ended (or from the start, where it does not fit), over nothing the
	 * GPU may still read. Mapped without discarding where the device lets
	 * a buffer read through a view be (noOverwrite); discarded every frame
	 * otherwise, when the driver gives it new memory. */
	ComPtr<ID3D11Buffer> input;
	ComPtr<ID3D11ShaderResourceView> tableViews[PIECES];
	ComPtr<ID3D11ShaderResourceView> ringView;
	u32 tableBytes = 0;		// a piece's
	u32 ringBase = 0, ringBytes = 0;
	u32 ringHead = 0;
	bool noOverwrite = false;
	bool mapped = false;		// (the first map discards: nothing has been written yet)
	/* What each piece's last decode read, and the event after it, polled
	 * (never waited for) to know when it is done */
	ComPtr<ID3D11Query> pieceDone[PIECES];
	bool pieceBusy[PIECES] = {};
	u32 pieceRing[PIECES][2] = {};	// the ring's bytes it read: from, to
	u32 serial = 0;
	u32 piece = 0;			// this frame's

	ComPtr<ID3D11Buffer> buffers[B_COUNT];
	ComPtr<ID3D11UnorderedAccessView> uavs[B_OUTPUTS];
	ComPtr<ID3D11UnorderedAccessView> sortUav;
	ComPtr<ID3D11ShaderResourceView> trParams;
	u64 sizes[B_COUNT] = {};
	TaGpuLayout layout;
	ComPtr<ID3D11ComputeShader> kernels[TAG_KERNELS + 1];
	/* how many workgroups the kernels that go round were dispatched (b0),
	 * which HLSL does not tell them */
	ComPtr<ID3D11Buffer> groups;
	u32 groupsSet = 0;
	/* (TA_GPU_SORT) the CPU's sort's words, written here and given to the
	 * buffer the prepass's kernel changes in place */
	std::vector<u32> sortWords;
	bool overlap = false;		// NVAPI's UAV overlap for the last kernels
};

DX11TaDecoder::DX11TaDecoder() {}
DX11TaDecoder::~DX11TaDecoder()
{
	delete res;
}

/* A buffer of @bytes: @bind, @misc; with @data, made with it */
static bool makeBuffer(ID3D11Device *device, u64 bytes, UINT bind, UINT misc, D3D11_USAGE usage, const void *data,
		ComPtr<ID3D11Buffer>& out)
{
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = (UINT)std::max<u64>((bytes + 15) & ~(u64)15, 16);
	desc.Usage = usage;
	desc.BindFlags = bind;
	desc.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
	desc.MiscFlags = misc;
	D3D11_SUBRESOURCE_DATA initial{};
	initial.pSysMem = data;
	return SUCCEEDED(device->CreateBuffer(&desc, data != nullptr ? &initial : nullptr, out.put()));
}

/* A view of @buffer as @count elements of @format from element @first */
static bool makeSrv(ID3D11Device *device, ID3D11Buffer *buffer, DXGI_FORMAT format, u32 first, u32 count,
		ComPtr<ID3D11ShaderResourceView>& out)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
	desc.Format = format;
	desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	desc.Buffer.FirstElement = first;
	desc.Buffer.NumElements = std::max<u32>(count, 1);
	return SUCCEEDED(device->CreateShaderResourceView(buffer, &desc, out.put()));
}

static bool makeUav(ID3D11Device *device, ID3D11Buffer *buffer, u64 bytes, ComPtr<ID3D11UnorderedAccessView>& out)
{
	D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	desc.Buffer.NumElements = (UINT)std::max<u64>(bytes / 4, 1);
	return SUCCEEDED(device->CreateUnorderedAccessView(buffer, &desc, out.put()));
}

bool DX11TaDecoder::Init()
{
	DX11Context *context = DX11Context::Instance();
	ID3D11Device *device = context->GetDevice();
	if (device == nullptr)
		return false;
	auto no = [this](const char *why) {
		NOTICE_LOG(RENDERER, "DX11: TA processing stays on the CPU (%s)", why);
		delete res;
		res = nullptr;
		failed = true;
		return false;
	};
	/* NVIDIA's multi-draw indirect, for the translucent groups' records;
	 * compute shaders with unordered access views of typed buffers, which
	 * feature level 11.0 has; the device's own context, which runs what it
	 * is given as it is given and maps a buffer without discarding it */
	if (context->GetVendor() != 0x10de)
		return no("GPU TA processing in Direct3D 11 needs an NVIDIA GPU");
	if (!dx11_nvapi_multi_draw_available())
		return no("no NVAPI multi-draw indirect: GPU TA processing in Direct3D 11 needs NVIDIA's driver with NVAPI");
	if (context->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
		return no("feature level 11.0 needed");
	if (!context->CanReadBack())
		return no("the frontend's context is a deferred one");

	TaGpuSizes sz;
	ta_gpu_sizes(&sz);
	res = new Resources();
	Resources *r = res;
	const u32 strips = sz.list[0] + sz.list[1] + sz.list[2];
	u32 work[9];
	const u32 workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	// the translucent draw records: each group's count and room for a draw a vertex
	const u32 trdSlots = sz.verts + sz.headers;

	r->sizes[B_VTX] = (u64)sz.verts * TAG_V_WORDS * 4;
	r->sizes[B_MOD] = (u64)sz.modtrig * 9 * 4 + 4;
	r->sizes[B_IDX] = (u64)sz.idx * 4;
	r->sizes[B_WORK] = (u64)workWords * 4;
	r->sizes[B_REC] = (u64)strips * TAG_C_WORDS * 4;
	// (and a second command for each, for shadows: TAG_T_SHD_CMD)
	r->sizes[B_CMD] = (u64)sz.headers * 2 * TAG_C_WORDS * 4;
	r->sizes[B_FRM] = TAG_O_WORDS * 4;
	r->sizes[B_TRD] = (u64)trdSlots * TAG_C_WORDS * 4;
	r->sizes[B_TRP] = (u64)sz.list[2] * TAG_P_WORDS * 4;
	r->sizes[B_ARGS] = 12 * 4;
	r->sizes[B_FRM0] = TAG_O_WORDS * 4;
	r->sizes[B_ZERO] = std::max<u64>((u64)trdSlots * TAG_C_WORDS * 4, (u64)sz.headers * 4);
	r->sizes[B_INST] = (u64)sz.list[2] * 4;
	/* the sorted indices: three a triangle, for as many triangles as a
	 * frame can have indices, and as many again for the depth prepass
	 * (rend/sorter.h), whose triangles are some of those; and the
	 * prepass's checks after them (TAG_T_PREP), a word and as many checks
	 * as there are translucent strips at most */
	const u64 sortWords = (u64)sz.idx * 2 * 3 + 1 + (u64)sz.list[2] * TAG_PC_WORDS;
	r->sizes[B_SORT] = sortWanted ? sortWords * 4 : 0;

	// the buffers the decode writes and the draws read: each a typed view of words, for both
	static const UINT bind[B_OUTPUTS] = {
		D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_INDEX_BUFFER, 0, 0, 0, 0, 0, D3D11_BIND_SHADER_RESOURCE
	};
	bool ok = true;
	for (int b = 0; ok && b < B_OUTPUTS; b++)
	{
		const UINT misc = b == B_CMD || b == B_TRD ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0;
		ok = makeBuffer(device, r->sizes[b], bind[b] | D3D11_BIND_UNORDERED_ACCESS, misc, D3D11_USAGE_DEFAULT, nullptr, r->buffers[b])
				&& makeUav(device, r->buffers[b], r->sizes[b], r->uavs[b]);
	}
	ok = ok && makeSrv(device, r->buffers[B_TRP], DXGI_FORMAT_R32G32_UINT, 0, sz.list[2], r->trParams);
	// (a buffer with no binding at all is not one every driver makes: the arguments' can be read as well)
	ok = ok && makeBuffer(device, r->sizes[B_ARGS], D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,
			D3D11_USAGE_DEFAULT, nullptr, r->buffers[B_ARGS]);
	{
		/* What is copied in at the start of every decode, and the strips'
		 * numbers: made with what they hold, now */
		std::vector<u32> words(std::max<u64>(r->sizes[B_ZERO], std::max<u64>(r->sizes[B_INST], r->sizes[B_FRM0])) / 4 + 4, 0);
		ok = ok && makeBuffer(device, r->sizes[B_ZERO], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words.data(), r->buffers[B_ZERO]);
		words[TAG_O_ZMAX] = 0x3F800000u;	// fZ_max starts at 1
		ok = ok && makeBuffer(device, r->sizes[B_FRM0], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words.data(), r->buffers[B_FRM0]);
		for (u32 i = 0; i < sz.list[2]; i++)
			words[i] = i;
		ok = ok && makeBuffer(device, r->sizes[B_INST], D3D11_BIND_VERTEX_BUFFER, 0, D3D11_USAGE_IMMUTABLE, words.data(), r->buffers[B_INST]);
	}
	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = 16;
	cb.Usage = D3D11_USAGE_DEFAULT;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	const u32 zero[4] = {};
	D3D11_SUBRESOURCE_DATA cbInitial{};
	cbInitial.pSysMem = zero;
	ok = ok && SUCCEEDED(device->CreateBuffer(&cb, &cbInitial, r->groups.put()));
	r->groupsSet = 0;
	if (!ok)
		return no("its buffers could not be made");

	/* The input: the tables' pieces, then the ring, at least the largest
	 * frame's data and half as much again */
	r->tableBytes = ((sz.table_words + 63) & ~63u) * 4;
	r->ringBase = r->tableBytes * PIECES;
	r->ringBytes = ta_data_size() + ta_data_size() / 2;
	D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
	r->noOverwrite = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
			&& options.MapNoOverwriteOnDynamicBufferSRV;
	ok = makeBuffer(device, (u64)r->ringBase + r->ringBytes, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DYNAMIC, nullptr, r->input)
			&& makeSrv(device, r->input, DXGI_FORMAT_R32_UINT, r->ringBase / 4, r->ringBytes / 4, r->ringView);
	for (int p = 0; ok && p < PIECES; p++)
	{
		D3D11_QUERY_DESC q{};
		q.Query = D3D11_QUERY_EVENT;
		ok = makeSrv(device, r->input, DXGI_FORMAT_R32_UINT, p * r->tableBytes / 4, r->tableBytes / 4, r->tableViews[p])
				&& SUCCEEDED(device->CreateQuery(&q, r->pieceDone[p].put()));
	}
	if (!ok)
		return no("its input buffer could not be made");

	// every kernel, now: none is made while a game runs
	for (int k = 1; ok && k <= TAG_KERNELS; k++)
		ok = SUCCEEDED(device->CreateComputeShader(dx11_ta_dxbc[k], dx11_ta_dxbc_size[k], nullptr, r->kernels[k].put()));
	if (!ok)
		return no("its kernels could not be made");

	/* The sorted indices, in video memory the CPU's words are given to
	 * (UpdateSubresource()) and the prepass's kernel changes in place */
	if (sortWanted)
	{
		r->sortWords.assign(sortWords, 0);
		if (!makeBuffer(device, r->sizes[B_SORT], D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT,
					nullptr, r->buffers[B_SORT])
				|| !makeUav(device, r->buffers[B_SORT], r->sizes[B_SORT], r->sortUav))
		{
			r->buffers[B_SORT].reset();
			r->sortUav.reset();
			r->sortWords.clear();
			r->sortWords.shrink_to_fit();
		}
	}

	r->layout.words[0] = 0;		// (each frame's: where its data are in the ring)
	for (int i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
		return no("the header pass's memory could not be made");
	// (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever)
	sortable = r->buffers[B_SORT] && ta_gpu_sort_reserve(true);
	r->overlap = dx11_nvapi_uav_overlap_available();

	draw.vertices = r->buffers[B_VTX];
	draw.indices = r->buffers[B_IDX];
	draw.modtrig = r->buffers[B_MOD];
	draw.commands = r->buffers[B_CMD];
	draw.shadowCommands = sz.headers * TAG_C_WORDS * 4;
	draw.trDraws = r->buffers[B_TRD];
	draw.trParams = r->trParams;
	draw.instances = r->buffers[B_INST];
	draw.sorted = sortable ? r->buffers[B_SORT].get() : nullptr;
	ready = true;
	NOTICE_LOG(RENDERER, "DX11: TA processing on the GPU (NVIDIA's multi-draw indirect; %u KB of tables a frame, %u pieces, "
			"a ring of %u MB for the frames' data%s; the last kernels %s%s)",
			r->tableBytes / 1024, PIECES, r->ringBytes >> 20, r->noOverwrite ? "" : ", discarded every frame",
			r->overlap ? "overlapped through NVAPI" : "one after another",
			sortable ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

void DX11TaDecoder::Log(const char *when)
{
	NOTICE_LOG(RENDERER, "DX11: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room, %llu whose input the GPU still had); %llu with shadows the GPU decides, "
			"%llu with a sorted pass, %llu multi-draws refused",
			when, (unsigned long long)statFrames, (unsigned long long)statSkipped,
			(unsigned long long)statWhy[TA_GPU_WHY_TABLE], (unsigned long long)statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)statWhy[TA_GPU_WHY_SORT], (unsigned long long)statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)statWhy[TA_GPU_WHY_ROOM], (unsigned long long)statBusy,
			(unsigned long long)statShadowed, (unsigned long long)statSorted, (unsigned long long)statDrawFailed);
}

void DX11TaDecoder::Term()
{
	if (res == nullptr)
		return;
	if (ready)
		Log("");
	// (the renderer is done with the GPU, or the context goes)
	ta_set_gpu_processing(false);
	/* Nothing of it is left bound: the context's record of what is bound
	 * must not name a buffer that goes (dx11context_lr.h) */
	DX11Context *context = DX11Context::Instance();
	if (context->GetDeviceContext() != nullptr)
	{
		DX11Lock lock;
		context->SetGeometry(nullptr, 0, 0, nullptr);
		context->SetVertexBuffer(nullptr, 0, 2);
		context->SetPSResource(5, nullptr);
	}
	delete res;
	res = nullptr;
	ready = false;
	prepared = false;
	sortable = false;
	draw = DX11TaDraw();
}

void DX11TaDecoder::Setup(bool wanted)
{
	if (ready && wanted)
		return;
	if (ready)
		Term();
	if (!wanted)
	{
		failed = false;
		return;
	}
	if (failed)
		return;
	Init();
}

bool DX11TaDecoder::Prepare(TA_context *ctx, u32 want, bool *rv)
{
	prepared = false;
	if (ctx->rend.isRenderFramebuffer)
	{
		statSkipped++;
		statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	Resources& r = *res;
	ID3D11DeviceContext *deviceContext = DX11Context::Instance()->GetDeviceContext();
	/* This frame's piece of the tables, and room in the ring for its data,
	 * where the GPU is done with the decodes that read them (polled, never
	 * waited for); if not, the frame is parsed. (Discarding the input every
	 * frame, the driver sees to that.) */
	const u32 piece = r.noOverwrite ? r.serial % PIECES : 0;
	const u32 bytes = (u32)std::min<size_t>((size_t)(ctx->tad.End() - ctx->tad.thd_root), ta_data_size());
	u32 at = 0;
	if (r.noOverwrite)
	{
		for (int p = 0; p < PIECES; p++)
			if (r.pieceBusy[p] && deviceContext->GetData(r.pieceDone[p], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
				r.pieceBusy[p] = false;
		const u32 room = (bytes + 255) & ~255u;
		at = r.ringHead + room <= r.ringBytes ? r.ringHead : 0;
		bool busy = r.pieceBusy[piece];
		for (int p = 0; p < PIECES && !busy; p++)
			busy = r.pieceBusy[p] && at < r.pieceRing[p][1] && r.pieceRing[p][0] < at + room;
		if (busy)
		{
			statSkipped++;
			statBusy++;
			return false;
		}
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	const D3D11_MAP how = r.noOverwrite && r.mapped ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
	if (FAILED(deviceContext->Map(r.input, 0, how, 0, &mapped)))
	{
		statSkipped++;
		statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	u8 * const input = (u8 *)mapped.pData;
	r.mapped = true;
	flags = want;
	r.layout.words[0] = at / 4;
	prepared = ta_gpu_prepare(ctx, &r.layout, (u32 *)(input + piece * r.tableBytes), r.tableBytes / 4, &frame, flags);
	// (the data as far as the game wrote them: all the decode reads)
	if (prepared && frame.rv)
		memcpy(input + r.ringBase + at, ctx->tad.thd_root, bytes);
	deviceContext->Unmap(r.input, 0);
	if (!prepared)
	{
		statSkipped++;
		statWhy[frame.why < TA_GPU_WHY_COUNT ? frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = frame.rv;
	if (ta_gpu_sorted_indices() != 0)
		statSorted++;
	if (frame.shadows != 0)
		statShadowed++;
	// (nothing to draw: nothing to decode)
	if (!frame.rv)
	{
		prepared = false;
		return true;
	}
	r.piece = piece;
	r.pieceRing[piece][0] = at;
	r.pieceRing[piece][1] = at + ((bytes + 255) & ~255u);
	r.ringHead = r.pieceRing[piece][1];
	r.serial++;
	return true;
}

void DX11TaDecoder::SetGroups(u32 groups)
{
	if (groups == res->groupsSet)
		return;
	const u32 words[4] = { groups, 0, 0, 0 };
	DX11Context::Instance()->GetDeviceContext()->UpdateSubresource(res->groups, 0, nullptr, words, 0, 0);
	res->groupsSet = groups;
}

void DX11TaDecoder::Record(TA_context *ctx)
{
	(void)ctx;
	if (!prepared)
		return;
	prepared = false;
	Resources& r = *res;
	DX11Context *context = DX11Context::Instance();
	ID3D11DeviceContext *dc = context->GetDeviceContext();

	/* The outputs are bound to be written: Direct3D 11 unbinds them from
	 * wherever they are bound to be read, behind the context's back, so
	 * the context unbinds them first (the vertices, the volumes' triangles
	 * and the indices; the per-pixel renderer's polygon words) */
	context->SetGeometry(nullptr, 0, 0, nullptr);
	context->SetPSResource(5, nullptr);

	/* The frame's words as a decode starts them, the slots' heads and the
	 * translucent draw records zeroed: copies */
	D3D11_BOX box{ 0, 0, 0, TAG_O_WORDS * 4, 1, 1 };
	dc->CopySubresourceRegion(r.buffers[B_FRM], 0, 0, 0, 0, r.buffers[B_FRM0], 0, &box);
	if (frame.slots != 0)
	{
		box.right = frame.slots * 4;
		dc->CopySubresourceRegion(r.buffers[B_WORK], 0, r.layout.words[9] * 4, 0, 0, r.buffers[B_ZERO], 0, &box);
	}
	if (frame.trd != 0)
	{
		box.right = frame.trd * TAG_C_WORDS * 4;
		dc->CopySubresourceRegion(r.buffers[B_TRD], 0, 0, 0, 0, r.buffers[B_ZERO], 0, &box);
	}
	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: given to their buffer now, before the
	 * kernel that reads them runs */
	const bool sorted = sortable && ta_gpu_sorted_indices() != 0;
	const u32 checks = sorted ? ta_gpu_prepass_checks() : 0;
	if (sorted)
	{
		const u32 words = std::min<u32>(ta_gpu_sorted_words(), (u32)r.sortWords.size());
		ta_gpu_sorted_write(r.sortWords.data());
		const D3D11_BOX part{ 0, 0, 0, words * 4, 1, 1 };
		dc->UpdateSubresource(r.buffers[B_SORT], 0, &part, r.sortWords.data(), 0, 0);
	}

	ID3D11ShaderResourceView * const inputs[2] = { r.ringView, r.tableViews[r.piece] };
	dc->CSSetShaderResources(0, 2, inputs);
	ID3D11UnorderedAccessView *uavs[UAV_SLOTS];
	for (int b = 0; b < UAV_SLOTS; b++)
		uavs[b] = r.uavs[b];
	dc->CSSetUnorderedAccessViews(0, UAV_SLOTS, uavs, nullptr);
	// (the two kernels that have another buffer at u1 than the modifier volume triangles: bound for each)
	auto atU1 = [&](ID3D11UnorderedAccessView *view) {
		dc->CSSetUnorderedAccessViews(1, 1, &view, nullptr);
	};
	SetGroups(0);
	dc->CSSetConstantBuffers(0, 1, r.groups.addr());

	/* Each dispatch after the one before: Direct3D 11 has a dispatch see
	 * what the ones before it wrote to the views bound */
	auto dispatch = [&](int k, u32 groups) {
		if (groups == 0)
			return;
		dc->CSSetShader(r.kernels[k], nullptr, 0);
		dc->Dispatch(groups, 1, 1);
	};
	// (the dispatches' arguments, a copy of the frame's: TAG_O_ARGS64 on)
	auto indirect = [&](int k, u32 at) {
		dc->CSSetShader(r.kernels[k], nullptr, 0);
		dc->DispatchIndirect(r.buffers[B_ARGS], (at - TAG_O_ARGS64) * 4);
	};
	// (a dispatch of so many workgroups at most, each then taking every so many: told how many there are)
	auto loop = [&](int k, u32 count) {
		const u32 groups = std::min(count, 65535u);
		SetGroups(groups);
		dispatch(k, groups);
	};
	const u32 scanGroups = (frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (frame.verts + frame.modtrig + 63) / 64);
	// (the vertices as unpacked, before TAG_K_PROVOKE moves colours)
	if (checks != 0)
	{
		atU1(r.sortUav);
		loop(TAG_K_PREPASS, checks);
	}
	dispatch(TAG_K_COUNT, scanGroups);
	dispatch(TAG_K_COUNTSUM, 1);
	/* The arguments of the dispatches that go by strips, out of the frame's
	 * words, as they are once TAG_K_COUNTSUM has written them */
	static_assert(TAG_O_ARGSCH == TAG_O_ARGS64 + 3 && TAG_O_ARGS1 == TAG_O_ARGS64 + 6, "the three dispatches' arguments one after the other");
	const D3D11_BOX args{ TAG_O_ARGS64 * 4, 0, 0, (TAG_O_ARGS64 + 9) * 4, 1, 1 };
	dc->CopySubresourceRegion(r.buffers[B_ARGS], 0, 0, 0, 0, r.buffers[B_FRM], 0, &args);

	dispatch(TAG_K_ENDS, scanGroups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (frame.slots + 63) / 64);
	/* The last four, after TAG_K_FINISH, do not depend on one another: each
	 * reads what the kernels before them wrote and writes words none of the
	 * others reads or writes -
	 *   TAG_K_SHADOW  reads the commands' first half (TAG_K_FINISH's), the
	 *                 indices and the vertices' x and y; writes the second
	 *                 half of the commands (TAG_T_SHD_CMD on)
	 *   TAG_K_TRDRAW  reads the records; writes the translucent draw
	 *                 records and the polygon words
	 *   TAG_K_BLEED   reads the records, the indices, the vertices' u, v
	 *                 and z; writes their u and v
	 *   TAG_K_PROVOKE reads the records, the indices and the vertices'
	 *                 four colours; writes those colours
	 * (and all of them the tables, the work buffer and the frame's words,
	 * which none of them writes). So where NVAPI has UAV overlap they run
	 * without the driver waiting between them; it ends before anything
	 * reads their outputs - the views unbound below, then the draws. */
	const u32 tail = (frame.shadows != 0) + (frame.trd != 0) + (screen_height > 480) + ((flags & TA_GPU_PROVOKE) != 0);
	const bool overlapped = r.overlap && tail > 1;
	if (overlapped)
		dx11_nvapi_begin_uav_overlap(dc);
	if (frame.shadows != 0)
		loop(TAG_K_SHADOW, frame.shadows);
	if (frame.trd != 0)
	{
		atU1(r.uavs[B_TRP]);
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	}
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
	if (overlapped)
		dx11_nvapi_end_uav_overlap(dc);

	// for whatever draws from them: unbound from the kernels
	ID3D11UnorderedAccessView * const none[UAV_SLOTS] = {};
	dc->CSSetUnorderedAccessViews(0, UAV_SLOTS, none, nullptr);
	ID3D11ShaderResourceView * const noInputs[2] = {};
	dc->CSSetShaderResources(0, 2, noInputs);
	// the piece and the ring's bytes are read until the event after the decode
	dc->End(r.pieceDone[r.piece]);
	r.pieceBusy[r.piece] = true;
	if (++statFrames % 600 == 0)
		Log(" so far");
}

void DX11TaDecoder::CopyDepthScale(ID3D11Buffer *dst, u32 offset)
{
	const D3D11_BOX box{ TAG_O_ZSCALE * 4, 0, 0, TAG_O_ZSCALE * 4 + 4, 1, 1 };
	DX11Context::Instance()->GetDeviceContext()->CopySubresourceRegion(dst, 0, offset, 0, 0, res->buffers[B_FRM], 0, &box);
}

void DX11TaDecoder::DrawRecords(u32 slot, u32 count)
{
	if (count == 0)
		return;
	const u32 stride = TAG_C_WORDS * 4;
	if (!dx11_nvapi_multi_draw_indexed(DX11Context::Instance()->GetDeviceContext(), count, res->buffers[B_TRD], slot * stride, stride)
			&& statDrawFailed++ == 0)
		WARN_LOG(RENDERER, "DX11: NVAPI's multi-draw indirect failed: a translucent draw group is not drawn");
}
