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
/* GPU-side TA processing in Direct3D 12: see dx12_ta_decoder.h */
#include "dx12_ta_decoder.h"
#include "dx12context_lr.h"
#include "rend/ta_gpu.h"
#include <algorithm>

extern int screen_height;

/* The kernels, by TAG_K_*, as DXBC made from rend/ta_gpu.h when the core
 * was built: tools/rend/kernels_dx12.sh, which checks that they still are
 * its. Nothing is compiled when the core runs. */
#include "dx12_ta_dxbc.inc"
static_assert(sizeof(dx12_ta_dxbc) / sizeof(dx12_ta_dxbc[0]) == TAG_KERNELS + 1, "a kernel for each TAG_K_*");

// (the renderers draw from the records and commands as they are)
static_assert(TAG_C_WORDS * 4 == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), "a record is a draw's arguments");
static_assert(TAG_V_WORDS * 4 == sizeof(Vertex), "a vertex as the renderers draw it");

typedef HRESULT (WINAPI *SerializeRootSignature_t)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

/* The kernels' outputs, in the order of their unordered access views
 * (u0 to u8); then what the decoder has of its own */
enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
	B_ARGS = B_OUTPUTS,	// the dispatches' arguments, copied out of the frame's words
	B_FRM0,			// the frame's words as each decode starts (fZ_max 1, the rest 0)
	B_ZERO,			// zeroes, copied where a decode wants them
	B_INST,			// 0, 1, 2...: the translucent strips' numbers (DX12TaDraw::instances)
	B_COUNT };

/* How many pieces the tables have: a frame's is written again once the GPU
 * is done with the frame that read it last */
enum { TABLE_PIECES = 8 };

struct DX12TaDecoder::Resources
{
	ComPtr<ID3D12Resource> pool;		// the TA contexts' data
	u8 *poolMap = nullptr;
	u32 slice = 0, slices = 0;
	ComPtr<ID3D12Resource> tables;		// mapped: a piece for each frame
	u32 *tablesMap = nullptr;
	u32 tableWords = 0;
	u64 pieceFence[TABLE_PIECES] = {};	// the list that last read each piece
	ComPtr<ID3D12Resource> buffers[B_COUNT];
	D3D12_RESOURCE_STATES states[B_COUNT];
	u64 sizes[B_COUNT] = {};
	TaGpuLayout layout;
	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> kernels[TAG_KERNELS + 1];
	ComPtr<ID3D12CommandSignature> dispatch;
	ComPtr<ID3D12CommandSignature> drawIndexed;
	u32 serial = 0;
	/* (TA_GPU_SORT) room for the sorted translucent indices of a frame, and
	 * the prepass's checks after them, for each piece of the tables, in
	 * memory the CPU writes them to once (they are its sort's) and the GPU
	 * reads once, but for the prepass's triangles the prepass's kernel
	 * makes nothing of (TAG_K_PREPASS), which it writes in place */
	ComPtr<ID3D12Resource> sorted;
	D3D12_RESOURCE_STATES sortedState = D3D12_RESOURCE_STATE_COMMON;
	u8 *sortedMap = nullptr;
	u64 sortSlot = 0;		// bytes; 0: none
};

DX12TaDecoder::DX12TaDecoder() {}
DX12TaDecoder::~DX12TaDecoder()
{
	delete res;
}

/* A committed buffer of @bytes in a heap of @props, in @state */
static bool makeBuffer(ID3D12Device *device, const D3D12_HEAP_PROPERTIES& props, u64 bytes, D3D12_RESOURCE_FLAGS flags,
		D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = std::max<u64>(bytes, 256);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	return SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
			__uuidof(ID3D12Resource), (void **)out.put()));
}

bool DX12TaDecoder::Init()
{
	DX12Context *context = DX12Context::Instance();
	ID3D12Device *device = context->GetDevice();
	if (device == nullptr)
		return false;
#ifdef MSB_FIRST
	// (the kernels read the blocks little-endian)
	failed = true;
	return false;
#endif
	TaGpuSizes sz;
	ta_gpu_sizes(&sz);
	Resources *r = new Resources();
	const u32 strips = sz.list[0] + sz.list[1] + sz.list[2];
	u32 work[9];
	const u32 workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	/* The contexts: every list the GPU may still be behind with, one being
	 * drawn, and the few the game writes to at a time and renders to
	 * textures with; and spares, for a game that writes to more at once, as
	 * many as a thirty-second of the memory budget left allows, up to four
	 * more: a frame that finds them all taken is drawn from the CPU's parse,
	 * in a context of its own memory (tactx_gpu_pool_attach() makes one) */
	r->slice = ta_data_size();
	const u64 budget = (u64)context->GetVramBudget().available_kib() * 1024 / 32;
	const u32 spares = (u32)std::min<u64>(budget / r->slice, 4);
	r->slices = std::min<u32>(TABLE_PIECES + 4 + spares, 16);
	r->tableWords = (sz.table_words + 63) & ~63u;
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

	D3D12_HEAP_PROPERTIES local{};
	local.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_HEAP_PROPERTIES upload{};
	upload.Type = D3D12_HEAP_TYPE_UPLOAD;
	/* The SH4 reads the control words back and states copy the data:
	 * memory without the host's cache in front of it is too slow for that */
	D3D12_HEAP_PROPERTIES cached{};
	cached.Type = D3D12_HEAP_TYPE_CUSTOM;
	cached.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
	cached.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

	bool ok = makeBuffer(device, cached, (u64)r->slice * r->slices, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, r->pool)
			&& SUCCEEDED(r->pool->Map(0, nullptr, (void **)&r->poolMap));
	if (!ok)
	{
		INFO_LOG(RENDERER, "DX12: TA processing stays on the CPU (no host-cached memory)");
		delete r;
		failed = true;
		return false;
	}
	const D3D12_RANGE none = { 0, 0 };
	ok = makeBuffer(device, upload, (u64)r->tableWords * 4 * TABLE_PIECES, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, r->tables)
			&& SUCCEEDED(r->tables->Map(0, &none, (void **)&r->tablesMap));
	for (int b = 0; ok && b < B_COUNT; b++)
	{
		r->states[b] = b < B_OUTPUTS ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
		ok = makeBuffer(device, local, r->sizes[b], b < B_OUTPUTS ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
				r->states[b], r->buffers[b]);
	}
	if (!ok)
	{
		INFO_LOG(RENDERER, "DX12: TA processing stays on the CPU (its buffers could not be made)");
		delete r;
		failed = true;
		return false;
	}

	r->layout.words[0] = 0;		// (each slice is bound from its start)
	for (int i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;

	/* The kernels' root signature: how many workgroups were dispatched
	 * (b0), the frame's data and its tables (t0, t1), the outputs (u0 to
	 * u8) and the sorted indices (u9), all root descriptors */
	HMODULE lib = GetModuleHandleA("d3d12.dll");
	SerializeRootSignature_t serialize = lib != nullptr ? (SerializeRootSignature_t)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : nullptr;
	D3D12_ROOT_PARAMETER params[13]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, 1 };
	for (int i = 0; i < 2; i++)
	{
		params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1 + i].Descriptor = { (UINT)i, 0 };
	}
	for (int i = 0; i < 10; i++)
	{
		params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[3 + i].Descriptor = { (UINT)i, 0 };
	}
	for (D3D12_ROOT_PARAMETER& p : params)
		p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 13;
	desc.pParameters = params;
	ComPtr<ID3DBlob> blob, errors;
	ok = serialize != nullptr && SUCCEEDED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()))
			&& SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
					__uuidof(ID3D12RootSignature), (void **)r->root.put()));
	// every kernel, now: none is made while a game runs
	for (int k = 1; ok && k <= TAG_KERNELS; k++)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
		cs.pRootSignature = r->root;
		cs.CS.pShaderBytecode = dx12_ta_dxbc[k];
		cs.CS.BytecodeLength = dx12_ta_dxbc_size[k];
		ok = SUCCEEDED(device->CreateComputePipelineState(&cs, __uuidof(ID3D12PipelineState), (void **)r->kernels[k].put()));
	}
	D3D12_INDIRECT_ARGUMENT_DESC arg{};
	D3D12_COMMAND_SIGNATURE_DESC sig{};
	sig.NumArgumentDescs = 1;
	sig.pArgumentDescs = &arg;
	arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
	sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	ok = ok && SUCCEEDED(device->CreateCommandSignature(&sig, nullptr, __uuidof(ID3D12CommandSignature), (void **)r->dispatch.put()));
	arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	sig.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	ok = ok && SUCCEEDED(device->CreateCommandSignature(&sig, nullptr, __uuidof(ID3D12CommandSignature), (void **)r->drawIndexed.put()));
	if (!ok)
	{
		INFO_LOG(RENDERER, "DX12: TA processing stays on the CPU (its kernels could not be made)");
		delete r;
		failed = true;
		return false;
	}

	/* The sorted indices: three a triangle, for as many triangles as a frame
	 * can have indices, and as many again for the depth prepass
	 * (rend/sorter.h), whose triangles are some of those, and its checks
	 * after them (TAG_T_PREP); in the host's memory, uncached, which the
	 * GPU reads and the prepass's kernel writes as it is */
	if (sortWanted)
	{
		D3D12_HEAP_PROPERTIES combined{};
		combined.Type = D3D12_HEAP_TYPE_CUSTOM;
		combined.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		combined.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
		const u64 slot = ((u64)sz.idx * 2 * 3 * 4 + 255) & ~(u64)255;
		if (makeBuffer(device, combined, slot * TABLE_PIECES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COMMON, r->sorted)
				&& SUCCEEDED(r->sorted->Map(0, &none, (void **)&r->sortedMap)))
			r->sortSlot = slot;
		else
			r->sorted.reset();
	}

	if (!tactx_gpu_pool_attach(r->poolMap, r->slice, r->slices))
	{
		delete r;
		failed = true;
		return false;
	}
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		delete r;
		failed = true;
		return false;
	}

	/* What is copied in at the start of every decode, and the strips'
	 * numbers: through the ring once, now */
	const u32 frm0Bytes = TAG_O_WORDS * 4, instBytes = (u32)r->sizes[B_INST];
	DX12Upload up;
	if (!context->Upload(frm0Bytes + instBytes, 16, up))
	{
		INFO_LOG(RENDERER, "DX12: TA processing stays on the CPU (the upload ring is too small)");
		ta_set_gpu_processing(false);
		tactx_gpu_pool_detach();
		delete r;
		failed = true;
		return false;
	}
	u32 *words = (u32 *)up.cpu;
	memset(words, 0, frm0Bytes);
	words[TAG_O_ZMAX] = 0x3F800000u;	// fZ_max starts at 1
	for (u32 i = 0; i < sz.list[2]; i++)
		words[TAG_O_WORDS + i] = i;
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	cmd->CopyBufferRegion(r->buffers[B_FRM0], 0, up.buffer, up.offset, frm0Bytes);
	cmd->CopyBufferRegion(r->buffers[B_INST], 0, up.buffer, up.offset + frm0Bytes, instBytes);
	D3D12_RESOURCE_BARRIER barriers[3]{};
	const int done[3] = { B_FRM0, B_INST, B_ARGS };
	const D3D12_RESOURCE_STATES after[3] = { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
	for (int i = 0; i < 3; i++)
	{
		barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[i].Transition.pResource = r->buffers[done[i]];
		barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[i].Transition.StateBefore = r->states[done[i]];
		barriers[i].Transition.StateAfter = after[i];
		r->states[done[i]] = after[i];
	}
	cmd->ResourceBarrier(3, barriers);
	r->states[B_ZERO] = D3D12_RESOURCE_STATE_COPY_DEST;
	context->Transition(r->buffers[B_ZERO], r->states[B_ZERO], D3D12_RESOURCE_STATE_COPY_SOURCE);

	// (without the CPU's room to sort in, a frame with a sorted pass is parsed and sorted as ever)
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	sortable = r->sortSlot != 0;
	draw.vertices = { r->buffers[B_VTX]->GetGPUVirtualAddress(), (UINT)r->sizes[B_VTX], sizeof(Vertex) };
	draw.indices = { r->buffers[B_IDX]->GetGPUVirtualAddress(), (UINT)r->sizes[B_IDX], DXGI_FORMAT_R32_UINT };
	draw.modtrig = { r->buffers[B_MOD]->GetGPUVirtualAddress(), (UINT)r->sizes[B_MOD], sizeof(float) * 3 };
	draw.commands = r->buffers[B_CMD];
	draw.shadowCommands = (u64)sz.headers * TAG_C_WORDS * 4;
	draw.trDraws = r->buffers[B_TRD];
	draw.trParams = r->buffers[B_TRP]->GetGPUVirtualAddress();
	draw.instances = { r->buffers[B_INST]->GetGPUVirtualAddress(), (UINT)r->sizes[B_INST], 4 };
	draw.frame = r->buffers[B_FRM];
	draw.drawIndexed = r->drawIndexed;
	res = r;
	decoding = true;
	ready = true;
	NOTICE_LOG(RENDERER, "DX12: TA processing on the GPU (%u frames of %u MB in host-cached memory, %u KB of tables a frame%s)",
			res->slices, res->slice >> 20, res->tableWords / 256, sortable ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

void DX12TaDecoder::Log(const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "DX12: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room, %llu whose tables the GPU still had); pool: %u frames found it full, %u contexts allocated; "
			"%llu with shadows the GPU decides, %llu with a sorted pass",
			when, (unsigned long long)statFrames, (unsigned long long)statSkipped,
			(unsigned long long)statWhy[TA_GPU_WHY_TABLE], (unsigned long long)statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)statWhy[TA_GPU_WHY_SORT], (unsigned long long)statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)statWhy[TA_GPU_WHY_ROOM], (unsigned long long)statBusy, shortFrames, allocated,
			(unsigned long long)statShadowed, (unsigned long long)statSorted);
}

void DX12TaDecoder::Term()
{
	if (!ready)
		return;
	Log("");
	// (the renderer is done with the GPU, or the context goes: the frames in the pool go on in memory of their own)
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	DX12Context *context = DX12Context::Instance();
	/* the buffers go once the GPU is done with them */
	if (context->GetDevice() != nullptr)
	{
		auto later = [context](IUnknown *object) {
			if (object != nullptr)
			{
				object->AddRef();
				context->Release(object);
			}
		};
		later(res->pool);
		later(res->tables);
		for (ComPtr<ID3D12Resource>& b : res->buffers)
			later(b);
		later(res->root);
		for (ComPtr<ID3D12PipelineState>& k : res->kernels)
			later(k);
		later(res->dispatch);
		later(res->sorted);
		later(res->drawIndexed);
	}
	delete res;
	res = nullptr;
	ready = false;
	prepared = false;
	sortable = false;
}

void DX12TaDecoder::Setup(bool wanted)
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

void DX12TaDecoder::BeginFrame()
{
	if (!ready)
		return;
	prepared = false;
	// every list up to the one the fence has reached is done, and the frames it read
	tactx_gpu_done((u32)DX12Context::Instance()->Completed());
}

bool DX12TaDecoder::Prepare(TA_context *ctx, u32 want, bool *rv)
{
	prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer)
	{
		// (a frame that found the pool full is in memory the GPU cannot read)
		statSkipped++;
		statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	/* This frame's piece of the tables, if the GPU is done with the frame
	 * that last read it; if not, the frame is parsed (never waited for) */
	DX12Context *context = DX12Context::Instance();
	index = res->serial % TABLE_PIECES;
	if (res->pieceFence[index] > context->Completed())
	{
		statSkipped++;
		statBusy++;
		return false;
	}
	flags = want;
	prepared = ta_gpu_prepare(ctx, &res->layout, res->tablesMap + (size_t)index * res->tableWords, res->tableWords, &frame, flags);
	if (!prepared)
	{
		statSkipped++;
		statWhy[frame.why < TA_GPU_WHY_COUNT ? frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	res->serial++;
	*rv = frame.rv;
	if (ta_gpu_sorted_indices() != 0)
		statSorted++;
	if (sortable)
		draw.sorted = { res->sorted->GetGPUVirtualAddress() + index * res->sortSlot, (UINT)res->sortSlot, DXGI_FORMAT_R32_UINT };
	if (frame.shadows != 0)
		statShadowed++;
	// (nothing to draw: nothing to decode)
	if (!frame.rv)
		prepared = false;
	return true;
}

/* The outputs into the kernels' state (@decoding), or into the states the
 * draws read them in: transitions all at once */
void DX12TaDecoder::States(bool decoding)
{
	if (decoding == this->decoding)
		return;
	this->decoding = decoding;
	static const D3D12_RESOURCE_STATES read[B_OUTPUTS] = {
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,	// vertices
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,	// modifier volume triangles
		D3D12_RESOURCE_STATE_INDEX_BUFFER,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,			// the work buffer: the kernels' own
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,			// the records: as well
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,			// the commands
		D3D12_RESOURCE_STATE_COPY_SOURCE,			// the frame's words: 1/fZ_max copied out
		D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,			// the translucent draw records
		(D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
	};
	D3D12_RESOURCE_BARRIER barriers[B_OUTPUTS];
	UINT n = 0;
	for (int b = 0; b < B_OUTPUTS; b++)
	{
		const D3D12_RESOURCE_STATES to = decoding ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : read[b];
		if (res->states[b] == to)
			continue;
		D3D12_RESOURCE_BARRIER& barrier = barriers[n++];
		barrier = D3D12_RESOURCE_BARRIER{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = res->buffers[b];
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = res->states[b];
		barrier.Transition.StateAfter = to;
		res->states[b] = to;
	}
	D3D12_RESOURCE_BARRIER sortedBarrier{};
	const D3D12_RESOURCE_STATES sortedTo = decoding ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_INDEX_BUFFER;
	if (res->sorted && res->sortedState != sortedTo)
	{
		sortedBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		sortedBarrier.Transition.pResource = res->sorted;
		sortedBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		sortedBarrier.Transition.StateBefore = res->sortedState;
		sortedBarrier.Transition.StateAfter = sortedTo;
		res->sortedState = sortedTo;
		DX12Context::Instance()->Cmd()->ResourceBarrier(1, &sortedBarrier);
	}
	if (n != 0)
		DX12Context::Instance()->Cmd()->ResourceBarrier(n, barriers);
}

void DX12TaDecoder::Record(TA_context *ctx)
{
	if (!prepared)
		return;
	prepared = false;
	Resources& r = *res;
	DX12Context *context = DX12Context::Instance();
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	/* The outputs are one copy: whatever read the last frame's (its draws)
	 * is done before they are written again - the transitions say so */
	States(true);

	/* The frame's words as a decode starts them, the slots' heads and the
	 * translucent draw records zeroed: copies, the targets in COPY_DEST */
	auto toState = [&](int b, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_BARRIER& barrier) {
		barrier = D3D12_RESOURCE_BARRIER{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = r.buffers[b];
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = r.states[b];
		barrier.Transition.StateAfter = state;
		r.states[b] = state;
	};
	D3D12_RESOURCE_BARRIER barriers[3];
	UINT n = 0;
	toState(B_FRM, D3D12_RESOURCE_STATE_COPY_DEST, barriers[n++]);
	if (frame.slots != 0)
		toState(B_WORK, D3D12_RESOURCE_STATE_COPY_DEST, barriers[n++]);
	if (frame.trd != 0)
		toState(B_TRD, D3D12_RESOURCE_STATE_COPY_DEST, barriers[n++]);
	cmd->ResourceBarrier(n, barriers);
	cmd->CopyBufferRegion(r.buffers[B_FRM], 0, r.buffers[B_FRM0], 0, TAG_O_WORDS * 4);
	if (frame.slots != 0)
		cmd->CopyBufferRegion(r.buffers[B_WORK], (u64)r.layout.words[9] * 4, r.buffers[B_ZERO], 0, (u64)frame.slots * 4);
	if (frame.trd != 0)
		cmd->CopyBufferRegion(r.buffers[B_TRD], 0, r.buffers[B_ZERO], 0, (u64)frame.trd * TAG_C_WORDS * 4);
	n = 0;
	toState(B_FRM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[n++]);
	if (frame.slots != 0)
		toState(B_WORK, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[n++]);
	if (frame.trd != 0)
		toState(B_TRD, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[n++]);
	cmd->ResourceBarrier(n, barriers);

	cmd->SetComputeRootSignature(r.root);
	cmd->SetComputeRoot32BitConstant(0, 0, 0);
	cmd->SetComputeRootShaderResourceView(1, r.pool->GetGPUVirtualAddress() + (u64)ctx->pool * r.slice);
	cmd->SetComputeRootShaderResourceView(2, r.tables->GetGPUVirtualAddress() + (u64)index * r.tableWords * 4);
	for (int b = 0; b < B_OUTPUTS; b++)
		cmd->SetComputeRootUnorderedAccessView(3 + b, r.buffers[b]->GetGPUVirtualAddress());
	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: written now, in this frame's piece, before
	 * the kernel that reads them is (the host's writes before the list is
	 * executed are the GPU's); u9 for the prepass's kernel */
	const bool sorted = sortable && ta_gpu_sorted_indices() != 0;
	const u32 checks = sorted ? ta_gpu_prepass_checks() : 0;
	if (sorted)
	{
		ta_gpu_sorted_write((u32 *)(r.sortedMap + index * r.sortSlot));
		cmd->SetComputeRootUnorderedAccessView(12, r.sorted->GetGPUVirtualAddress() + index * r.sortSlot);
	}

	D3D12_RESOURCE_BARRIER step{};
	step.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	auto dispatch = [&](int k, u32 groups) {
		cmd->SetPipelineState(r.kernels[k]);
		if (groups != 0)
			cmd->Dispatch(groups, 1, 1);
		cmd->ResourceBarrier(1, &step);
	};
	// (the dispatches' arguments, a copy of the frame's: TAG_O_ARGS64 on)
	auto indirect = [&](int k, u32 at) {
		cmd->SetPipelineState(r.kernels[k]);
		cmd->ExecuteIndirect(r.dispatch, 1, r.buffers[B_ARGS], (u64)(at - TAG_O_ARGS64) * 4, nullptr, 0);
		cmd->ResourceBarrier(1, &step);
	};
	// (a dispatch of so many workgroups at most, each then taking every so many: told how many there are)
	auto loop = [&](int k, u32 count) {
		const u32 groups = std::min(count, 65535u);
		cmd->SetComputeRoot32BitConstant(0, groups, 0);
		dispatch(k, groups);
	};
	const u32 scanGroups = (frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (frame.verts + frame.modtrig + 63) / 64);
	// (the vertices as unpacked, before TAG_K_PROVOKE moves colours)
	if (checks != 0)
		loop(TAG_K_PREPASS, checks);
	dispatch(TAG_K_COUNT, scanGroups);
	dispatch(TAG_K_COUNTSUM, 1);
	/* The arguments of the dispatches that go by strips, out of the frame's
	 * words: an indirect argument buffer is not one the kernels write */
	n = 0;
	toState(B_FRM, D3D12_RESOURCE_STATE_COPY_SOURCE, barriers[n++]);
	toState(B_ARGS, D3D12_RESOURCE_STATE_COPY_DEST, barriers[n++]);
	cmd->ResourceBarrier(n, barriers);
	cmd->CopyBufferRegion(r.buffers[B_ARGS], 0, r.buffers[B_FRM], TAG_O_ARGS64 * 4, 9 * 4);
	n = 0;
	toState(B_FRM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, barriers[n++]);
	toState(B_ARGS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, barriers[n++]);
	cmd->ResourceBarrier(n, barriers);
	static_assert(TAG_O_ARGSCH == TAG_O_ARGS64 + 3 && TAG_O_ARGS1 == TAG_O_ARGS64 + 6, "the three dispatches' arguments one after the other");

	dispatch(TAG_K_ENDS, scanGroups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (frame.slots + 63) / 64);
	if (frame.shadows != 0)
		loop(TAG_K_SHADOW, frame.shadows);
	if (frame.trd != 0)
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);

	// for whatever draws from them
	States(false);
	// (the pipeline bound is not one the context knows)
	context->ForgetPipeline();
	// the context's slice and this piece of the tables are read until this list is done
	const u64 list = context->LastSubmitted() + 1;
	ctx->gpu_serial = (u32)list;
	r.pieceFence[index] = list;
	if (++statFrames % 600 == 0)
		Log(" so far");
}

void DX12TaDecoder::CopyDepthScale(ID3D12Resource *dst, u64 offset)
{
	DX12Context::Instance()->Cmd()->CopyBufferRegion(dst, offset, res->buffers[B_FRM], TAG_O_ZSCALE * 4, 4);
}
