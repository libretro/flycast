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
/* The compute renderer on Direct3D 12: see dx12_compute.h */
#include "dx12_compute.h"
#include "dx12context_lr.h"
#include "dx12_texture.h"
#include "rend/ssaa_pattern.h"
#include "rend/ta_gpu.h"

#include <algorithm>

/* The kernels, by PVRC_K_* (the walk's sorted variant for more than one
 * sample after the last, by SSAA_LOG2() of its samples, then its unsorted
 * variant for 1, 2, 4 and 8: pvr_compute.h's PVRC_V_*), as DXIL made from
 * rend/tile/pvr_compute.h when the source was: tools/rend/kernels_dx12.sh,
 * which checks they still are its. Nothing is compiled when the core runs. */
#include "dx12_compute_dxil.inc"
static_assert(sizeof(dx12_compute_dxil) / sizeof(dx12_compute_dxil[0]) == PVRC_KERNELS + 8, "a kernel for each PVRC_K_*, and the walk's variants");
// (the walk's unsorted variant for @samples, in the table)
static inline u32 walkUnsortedDxil(u32 samples) { return PVRC_KERNELS + 4 + SSAA_LOG2(samples); }
static_assert(PVRC_TAG_C_WORDS == TAG_C_WORDS && PVRC_TAG_C_COUNT == TAG_C_COUNT && PVRC_TAG_C_FIRST == TAG_C_FIRST
		&& PVRC_TAG_O_OVERRUN == TAG_O_OVERRUN && PVRC_TAG_O_ZSCALE == TAG_O_ZSCALE, "the decode's words, as the kernels read them");
static_assert(PVRC_TEX_SLOTS <= 0x10000 && DX12_SRV_TABLE_SIZE <= 0x10000 && DX12_SAMPLER_TABLE_SIZE <= 0x10000,
		"a slot's word: the texture's place | its sampler's << 16");

/* The bin pool's entries: a run for each tile a primitive touches, for a
 * frame's every primitive (4 MiB), as Vulkan's */
static const u32 PoolEntries = 2u << 20;
/* The pieces of the ring: frames in flight at once */
static const u32 Slots = DX12Context::ComputeTables;
/* The kernels' views (the table's), in the order of their registers: the
 * buffers u0 to u9, then the picture (u0, space 2) */
enum { V_FRAME, V_REC, V_BIN, V_VTX, V_IDX, V_MOD, V_CMD, V_TRD, V_DFRM, V_SLOTS, V_PICTURE, V_COUNT };
static_assert(V_COUNT == DX12Context::ComputeTableSize, "the context's tables have room for the views");
/* (the translucent fragments the walk keeps at a time: what the kernels
 * were made for, pvrc_fragments_kept() of what every Direct3D 12 device
 * gives a workgroup) */
static const u32 SharedBytes = 32768;

/* The textures a frame's polygons have, each given a slot: a table of a
 * fixed size, emptied by a new generation each frame (Vulkan's), each
 * slot's word the texture's place in the bindless table and its sampler's */
struct DX12SlotTable : public ComputeFrame::Slots
{
	static const u32 Size = 2 * PVRC_TEX_SLOTS;
	struct Entry
	{
		u64 texid;
		u32 sampler;
		u32 gen;
		u32 slot;
	};
	Entry entries[Size];
	u32 *words = nullptr;		// this frame's piece of the ring: a word a slot
	DX12PooledTexture *images[PVRC_TEX_SLOTS];
	u32 gen = 0;
	u32 used = 0;

	DX12SlotTable() { memset(entries, 0, sizeof(entries)); }
	void Reset(u32 *words)
	{
		if (++gen == 0)
		{
			memset(entries, 0, sizeof(entries));
			gen = 1;
		}
		used = 0;
		this->words = words;
	}
	u32 Slot(u64 texid, TSP tsp) override
	{
		const u32 sampler = dx12_sampler_index(tsp);
		u32 h = (u32)(texid >> 4) * 2654435761u ^ sampler * 40503u;
		for (u32 i = 0; i < Size; i++)
		{
			Entry& e = entries[(h + i) & (Size - 1)];
			if (e.gen != gen)
			{
				if (used == PVRC_TEX_SLOTS)
					return PVRC_NONE;
				e.gen = gen;
				e.texid = texid;
				e.sampler = sampler;
				e.slot = used;
				const DX12Texture *texture = texid != 0 ? (const DX12Texture *)(uintptr_t)texid : nullptr;
				images[used] = texture != nullptr ? texture->Image() : nullptr;
				words[used] = (texture != nullptr ? texture->Srv() : DX12_SRV_NONE) | (sampler << 16);
				return used++;
			}
			if (e.texid == texid && e.sampler == sampler)
				return e.slot;
		}
		return PVRC_NONE;
	}
};

struct DX12ComputeDrawer::Resources
{
	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> pipelines[PVRC_KERNELS + 1];
	// the walk's unsorted variant (pvr_compute.h: PVRC_V_*; the sorted is pipelines[PVRC_K_WALK])
	ComPtr<ID3D12PipelineState> walkUnsorted;
	// with SSAA, the walk for renders to a texture, at one sample a pixel, both variants
	ComPtr<ID3D12PipelineState> walk1, walk1Unsorted;
	ComPtr<ID3D12CommandSignature> dispatch;
	/* the ring: a piece a slot, each the frame's words then the slots' */
	ComPtr<ID3D12Resource> ring;
	u32 *ringMap = nullptr;
	u32 frameWords = 0, pieceWords = 0;
	u64 pieceFence[Slots] = {};
	u32 slot = 0;		// this frame's piece
	ComPtr<ID3D12Resource> records;
	ComPtr<ID3D12Resource> bins;
	ComPtr<ID3D12Resource> zero;		// zeros, for the bins' counts and starts each frame
	ComPtr<ID3D12Resource> args;		// the dispatches' arguments: the bins' counts, copied
	ComPtr<ID3D12Resource> across;		// the walk's dispatches across, pvrc_walk_x() for each tile size and count of samples, copied to them
	ComPtr<ID3D12Resource> stats;		// the counts for the log, a piece a slot, read when it comes round
	/* the picture a frame starts from when the one before cannot be read
	 * where it is (CopyStart()): the screen's size, a place in the table */
	ComPtr<ID3D12Resource> start;
	D3D12_RESOURCE_STATES startState = D3D12_RESOURCE_STATE_COPY_DEST;
	u32 startSrv = ~0u;
	const u32 *statsMap = nullptr;
	bool statsWritten[Slots] = {};
	u32 binWords = 0;
	u32 runs = 0, chunks = 0, tileLog2 = 5;
	u32 keep = 4;
	u64 recordBytes = 0;
	DX12SlotTable slots;
	bool gpu = false;	// this frame's is the decode's
	bool rtt = false;	// ...a render to a texture
	u32 last = ~0u;		// the picture to start from, in the table
	bool lastScaled = false;
	// this frame's, between Prepare and Record
	u32 words = 0, prims = 0;
	u64 skipped = 0, dropped = 0, hiz = 0, clipped = 0;
	u32 peels = 0;
};

DX12ComputeDrawer::DX12ComputeDrawer() {}
DX12ComputeDrawer::~DX12ComputeDrawer() { Term(); }

const char *DX12ComputeDrawer::DeviceLacks()
{
	ID3D12Device *device = DX12Context::Instance()->GetDevice();
	if (device == nullptr)
		return "no device";
	D3D12_FEATURE_DATA_SHADER_MODEL model{};
	model.HighestShaderModel = D3D_SHADER_MODEL_6_0;
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) || model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
		return "no shader model 6.0";
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) || !options1.Int64ShaderOps)
		return "no 64-bit integers in shaders";
	// (the bindless table and the samplers are unbounded ranges of the root signature)
	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
			|| options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
		return "resource binding tier 1";
	return nullptr;
}

static bool makeBuffer(ID3D12Device *device, D3D12_HEAP_TYPE type, u64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
		ComPtr<ID3D12Resource>& out)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = type;
	if (type == D3D12_HEAP_TYPE_CUSTOM)
	{
		/* written by the CPU, read in place by the GPU, and the decode's
		 * frame written by PVRC_K_GPUSRC: write-combined memory of the host's
		 * that unordered access views can have */
		heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
		heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
	}
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = std::max<u64>(bytes, 256);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = flags;
	return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
			__uuidof(ID3D12Resource), (void **)out.put()));
}

bool DX12ComputeDrawer::Init(u32 width, u32 height, u32 samples)
{
	Term();
	if (failed)
		return false;
	DX12Context *context = DX12Context::Instance();
	ID3D12Device *device = context->GetDevice();
	this->width = width;
	this->height = height;
	this->samples = samples > 4 ? 8 : samples > 2 ? 4 : samples > 1 ? 2 : 1;

	auto fail = [&](const char *why) {
		INFO_LOG(RENDERER, "Direct3D 12 compute renderer: not on this device (%s); the per-pixel renderer draws", why);
		failed = true;
		delete res;
		res = nullptr;
		return false;
	};
	const char *lacks = DeviceLacks();
	if (lacks != nullptr)
		return fail(lacks);
	Resources *r = new Resources();
	res = r;
	r->keep = pvrc_fragments_kept(SharedBytes);
	r->tileLog2 = ComputeFrame::TileLog2(width);
	const u32 tiles = ((width + (1u << r->tileLog2) - 1) >> r->tileLog2) * ((height + (1u << r->tileLog2) - 1) >> r->tileLog2);
	r->chunks = (ComputeFrame::MaxPrims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	// (room for the screen's tiles, and a texture rendered to's, as Vulkan's)
	r->runs = r->chunks * std::max(tiles, ComputeFrame::MaxRttTiles());
	r->binWords = pvrc_bin_words(r->runs, PoolEntries);
	r->frameWords = (ComputeFrame::MaxWords() + 63) & ~63u;
	r->pieceWords = r->frameWords + PVRC_TEX_SLOTS;
	r->recordBytes = (u64)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4;
	const u64 deviceBytes = r->recordBytes + (u64)r->binWords * 4 + (u64)PVRC_B_POOL(r->runs) * 4 + (u64)width * height * 4;
	if ((deviceBytes >> 10) > context->GetVramBudget().available_kib())
		return fail("not enough video memory");

	/* The root signature: the pictures' places in the table (b0); the table
	 * of views (u0 to u9, the picture u0 of space 2); the bindless table
	 * (t0 on, space 1) and the samplers (s0 on, space 1); the linear
	 * sampler for a picture of another size to start from (s0) */
	{
		D3D12_DESCRIPTOR_RANGE ranges[4]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, V_PICTURE, 0, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2, V_PICTURE };
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
		ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 1, 0 };
		D3D12_ROOT_PARAMETER params[4]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, 2 };
		for (int i = 0; i < 3; i++)
		{
			params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1 + i].DescriptorTable.NumDescriptorRanges = i == 0 ? 2 : 1;
			params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i == 0 ? 0 : i + 1];
		}
		D3D12_STATIC_SAMPLER_DESC linear{};
		linear.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		linear.MaxLOD = D3D12_FLOAT32_MAX;
		linear.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = 4;
		desc.pParameters = params;
		desc.NumStaticSamplers = 1;
		desc.pStaticSamplers = &linear;
		typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
		HMODULE lib = GetModuleHandleA("d3d12.dll");
		serialize_fn serialize = lib != nullptr ? (serialize_fn)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : nullptr;
		ComPtr<ID3DBlob> blob, errors;
		if (serialize == nullptr || FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()))
				|| FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)r->root.put())))
			return fail("no root signature");
	}
	/* every kernel, now, for this many samples, and the walk for one when
	 * that is not one (renders to a texture): none is made while a game runs */
	auto make = [&](u32 index, ComPtr<ID3D12PipelineState>& out) {
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = r->root;
		desc.CS = { dx12_compute_dxil[index], dx12_compute_dxil_size[index] };
		return SUCCEEDED(device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), (void **)out.put()));
	};
	for (u32 k = 1; k <= PVRC_KERNELS; k++)
		if (!make(k == PVRC_K_WALK && this->samples > 1 ? PVRC_KERNELS + SSAA_LOG2(this->samples) : k, r->pipelines[k]))
			return fail("a kernel's pipeline could not be made");
	if (!make(walkUnsortedDxil(this->samples), r->walkUnsorted))
		return fail("a kernel's pipeline could not be made");
	if (this->samples > 1 && (!make(PVRC_K_WALK, r->walk1) || !make(walkUnsortedDxil(1), r->walk1Unsorted)))
		return fail("a kernel's pipeline could not be made");
	{
		D3D12_INDIRECT_ARGUMENT_DESC arg{};
		arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		D3D12_COMMAND_SIGNATURE_DESC desc{};
		desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
		desc.NumArgumentDescs = 1;
		desc.pArgumentDescs = &arg;
		if (FAILED(device->CreateCommandSignature(&desc, nullptr, __uuidof(ID3D12CommandSignature), (void **)r->dispatch.put())))
			return fail("no command signature");
	}

	const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	bool ok = makeBuffer(device, D3D12_HEAP_TYPE_CUSTOM, (u64)r->pieceWords * 4 * Slots, uav, ua, r->ring)
			&& makeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, r->recordBytes, uav, ua, r->records)
			// (made zero: a committed resource's memory is)
			&& makeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, (u64)r->binWords * 4, uav, ua, r->bins)
			&& makeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, (u64)PVRC_B_POOL(r->runs) * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE, r->zero)
			&& makeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, PVRC_ST_ARGS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, r->args)
			&& makeBuffer(device, D3D12_HEAP_TYPE_UPLOAD, 8 * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, r->across)
			&& makeBuffer(device, D3D12_HEAP_TYPE_READBACK, (u64)PVRC_STATS * 4 * Slots, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, r->stats);
	if (ok)
	{
		/* (tiles of 2^5 or 2^6 pixels, ComputeFrame::TileLog2(); 1, 2, 4 or 8 samples) */
		u32 *across = nullptr;
		const D3D12_RANGE none{};
		ok = SUCCEEDED(r->across->Map(0, &none, (void **)&across));
		if (ok)
		{
			for (u32 i = 0; i < 8; i++)
				across[i] = pvrc_walk_x(5 + i / 4, 1u << (i % 4));
			r->across->Unmap(0, nullptr);
		}
	}
	if (ok)
	{
		ok = dx12_make_texture(device, width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, r->startState, r->start);
		r->startSrv = ok ? context->Srv().Alloc() : ~0u;
		ok = ok && r->startSrv != ~0u;
		if (ok)
			context->MakeSrv(r->start, DXGI_FORMAT_R8G8B8A8_UNORM, 1, r->startSrv);
	}
	if (!ok)
		return fail("its buffers could not be made");
	const D3D12_RANGE none = { 0, 0 };
	void *p = nullptr;
	if (FAILED(r->ring->Map(0, &none, &p)))
		return fail("its ring could not be mapped");
	r->ringMap = (u32 *)p;
	if (FAILED(r->stats->Map(0, nullptr, &p)))
		return fail("its counts could not be mapped");
	r->statsMap = (const u32 *)p;

	/* Each piece's table of views of the buffers (the decode's are written
	 * when a frame of the decode's has the piece) */
	for (u32 s = 0; s < Slots; s++)
	{
		const u32 table = DX12Context::ComputeFirst + s * DX12Context::ComputeTableSize;
		auto view = [&](u32 v, ID3D12Resource *buffer, u64 firstWord, u64 words) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = firstWord;
			desc.Buffer.NumElements = (UINT)words;
			desc.Buffer.StructureByteStride = 4;
			device->CreateUnorderedAccessView(buffer, nullptr, &desc, context->Srv().Cpu(table + v));
		};
		view(V_FRAME, r->ring, (u64)s * r->pieceWords, r->frameWords);
		view(V_REC, r->records, 0, r->recordBytes / 4);
		view(V_BIN, r->bins, 0, r->binWords);
		for (u32 v = V_VTX; v <= V_DFRM; v++)
			view(v, r->ring, (u64)s * r->pieceWords, r->frameWords);
		view(V_SLOTS, r->ring, (u64)s * r->pieceWords + r->frameWords, PVRC_TEX_SLOTS);
	}

	ready = true;
	NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: %u x %u, %u sample%s a pixel, tiles of %u, %u translucent fragments a sample at a time; %u KiB of video memory, %u KiB of host memory",
			width, height, this->samples, this->samples > 1 ? "s" : "", 1u << r->tileLog2, r->keep,
			(u32)(deviceBytes >> 10), (u32)(((u64)r->pieceWords * 4 * Slots) >> 10));
	return true;
}

void DX12ComputeDrawer::Term()
{
	if (res != nullptr)
	{
		if (statFrames + statFallback != 0)
		{
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: %llu frames drawn (%llu of them the GPU's TA decode's, %llu renders to a texture), %llu left to the per-pixel renderer; %llu runs walked over their chunk, %llu triangles at no depth left out, %llu with a vertex behind the eye clipped, at most %u rounds of translucent fragments, %llu primitives passed over by the tiles' depths",
					(unsigned long long)statFrames, (unsigned long long)statGpuFrames, (unsigned long long)statRttFrames, (unsigned long long)statFallback, (unsigned long long)res->skipped,
					(unsigned long long)res->dropped, (unsigned long long)res->clipped, res->peels, (unsigned long long)res->hiz);
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: fallbacks: %llu renders to a texture it cannot write, %llu starting from a picture it cannot take, %llu with more render passes than it takes, %llu with more textures than its slots, %llu too large, %llu the decode's without its buffers, %llu with its piece of the ring still the GPU's",
					(unsigned long long)statWhy[ComputeFrame::Why_Rtt], (unsigned long long)statWhy[ComputeFrame::Why_Picture],
					(unsigned long long)statWhy[ComputeFrame::Why_Passes], (unsigned long long)statWhy[ComputeFrame::Why_Textures],
					(unsigned long long)statWhy[ComputeFrame::Why_Room], (unsigned long long)statWhy[ComputeFrame::Why_Decode],
					(unsigned long long)statBusy);
		}
		DX12Context *context = DX12Context::Instance();
		/* (what a frame still on the GPU has goes once it is done with it) */
		auto release = [&](ComPtr<ID3D12Resource>& b) {
			if (b)
			{
				b->AddRef();
				context->Release(b.get());
				b.reset();
			}
		};
		if (res->ring && res->ringMap != nullptr)
			res->ring->Unmap(0, nullptr);
		if (res->stats && res->statsMap != nullptr)
			res->stats->Unmap(0, nullptr);
		release(res->ring);
		release(res->records);
		release(res->bins);
		release(res->zero);
		release(res->args);
		release(res->across);
		release(res->stats);
		release(res->start);
		if (res->startSrv != ~0u)
			context->ReleaseSrv(res->startSrv);
		for (ComPtr<ID3D12PipelineState>& pso : res->pipelines)
			if (pso)
			{
				pso->AddRef();
				context->Release(pso.get());
				pso.reset();
			}
		if (res->walk1)
		{
			res->walk1->AddRef();
			context->Release(res->walk1.get());
			res->walk1.reset();
		}
		if (res->root)
		{
			res->root->AddRef();
			context->Release(res->root.get());
			res->root.reset();
		}
		if (res->dispatch)
		{
			res->dispatch->AddRef();
			context->Release(res->dispatch.get());
			res->dispatch.reset();
		}
		delete res;
		res = nullptr;
	}
	ready = false;
	statFrames = statFallback = statGpuFrames = statRttFrames = statBusy = 0;
	memset(statWhy, 0, sizeof(statWhy));
}

u32 DX12ComputeDrawer::StartSrv() const
{
	return res != nullptr ? res->startSrv : ~0u;
}

void DX12ComputeDrawer::CopyStart(ID3D12Resource *from)
{
	Resources& r = *res;
	DX12Context *context = DX12Context::Instance();
	context->Transition(r.start, r.startState, D3D12_RESOURCE_STATE_COPY_DEST);
	context->Cmd()->CopyResource(r.start, from);
	context->Transition(r.start, r.startState, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
}

bool DX12ComputeDrawer::CanDraw() const
{
	if (!ready || pvrrc.isRenderFramebuffer || (pvrrc.gpu && (decoder == nullptr || !decoder->Ready())))
		return false;
	return true;
}

bool DX12ComputeDrawer::Prepare(ComputeFrame& frame, u32 last, bool scaled, u32 rttWidth, u32 rttHeight)
{
	Resources& r = *res;
	DX12Context *context = DX12Context::Instance();
	/* The next piece of the ring, if the GPU is done with the frame that
	 * had it (and the counts it left, read); else the per-pixel renderer
	 * draws this one */
	const u32 slot = (r.slot + 1) % Slots;
	if (r.pieceFence[slot] > context->Completed())
	{
		if (statBusy++ == 0)
			NOTICE_LOG(RENDERER, "Direct3D 12 compute renderer: the GPU still has the piece of the ring this frame would take (an earlier frame's): the per-pixel renderer draws it (and others like it, not logged)");
		Fallback(ComputeFrame::Why_None);
		return false;
	}
	if (r.statsWritten[slot])
	{
		const u32 *st = r.statsMap + slot * PVRC_STATS;
		r.skipped += st[PVRC_ST_SKIPPED];
		r.dropped += st[PVRC_ST_DROPPED];
		r.clipped += st[PVRC_ST_CLIPPED];
		r.hiz += st[PVRC_ST_HIZ];
		r.peels = std::max(r.peels, st[PVRC_ST_PEELS]);
		r.statsWritten[slot] = false;
	}

	ComputeFrame::Layout layout;
	const bool rtt = pvrrc.isRTT;
	layout.width = rtt ? rttWidth : width;
	layout.height = rtt ? rttHeight : height;
	layout.tileLog2 = rtt ? ComputeFrame::TileLog2(layout.width) : r.tileLog2;
	layout.pool = PoolEntries;
	layout.runs = r.runs;
	layout.chunks = r.chunks;
	layout.lastPicture = last != ~0u && !rtt;
	layout.lastScaled = layout.lastPicture && scaled;
	layout.rttScale = rtt ? ComputeFrame::RttScale() : 0;
	// (a texture rendered to larger than the bins are made for: none the PowerVR's registers ask for)
	const u32 tiles = ((layout.width + (1u << layout.tileLog2) - 1) >> layout.tileLog2) * ((layout.height + (1u << layout.tileLog2) - 1) >> layout.tileLog2);
	if (layout.width == 0 || layout.height == 0 || (u64)tiles * r.chunks > r.runs)
	{
		Fallback(ComputeFrame::Why_Room);
		return false;
	}
	u32 *words = r.ringMap + (size_t)slot * r.pieceWords;
	r.slots.Reset(words + r.frameWords);
	r.words = frame.Pack(layout, r.slots, words, r.frameWords, &r.prims);
	if (r.words == 0)
	{
		Fallback(frame.why);
		return false;
	}
	r.slot = slot;
	r.gpu = pvrrc.gpu;
	r.rtt = rtt;
	r.last = layout.lastPicture ? last : ~0u;
	r.lastScaled = layout.lastScaled;
	return true;
}

void DX12ComputeDrawer::Record(ID3D12Resource *picture)
{
	Resources& r = *res;
	DX12Context *context = DX12Context::Instance();
	ID3D12Device *device = context->GetDevice();
	ID3D12GraphicsCommandList *cmd = context->Cmd();
	const u32 slot = r.slot;
	const u32 *words = r.ringMap + (size_t)slot * r.pieceWords;
	const u32 table = DX12Context::ComputeFirst + slot * DX12Context::ComputeTableSize;

	/* The piece's views: the picture, and what the frame's vertices,
	 * indices, volumes and the decode's words are read from - the ring's
	 * piece, or the decode's buffers (views are a few words in the heap,
	 * written where they change; the GPU is done with this piece's) */
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(picture, nullptr, &desc, context->Srv().Cpu(table + V_PICTURE));
		auto view = [&](u32 v, ID3D12Resource *buffer, u64 firstWord, u64 count) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			d.Buffer.FirstElement = firstWord;
			d.Buffer.NumElements = (UINT)count;
			d.Buffer.StructureByteStride = 4;
			device->CreateUnorderedAccessView(buffer, nullptr, &d, context->Srv().Cpu(table + v));
		};
		if (r.gpu)
		{
			const DX12TaDraw& draw = decoder->Draw();
			view(V_VTX, draw.vertexResource, 0, draw.vertices.SizeInBytes / 4);
			view(V_IDX, draw.indexResource, 0, draw.indices.SizeInBytes / 4);
			view(V_MOD, draw.modtrigResource, 0, draw.modtrig.SizeInBytes / 4);
			view(V_CMD, draw.commands, 0, draw.commands->GetDesc().Width / 4);
			view(V_TRD, draw.trDraws, 0, draw.trDraws->GetDesc().Width / 4);
			view(V_DFRM, draw.frame, 0, draw.frame->GetDesc().Width / 4);
			// the decode's outputs where the kernels read them
			decoder->ForKernels();
		}
		else
			for (u32 v = V_VTX; v <= V_DFRM; v++)
				view(v, r.ring, (u64)slot * r.pieceWords, r.frameWords);
	}
	/* The frame's textures, the palette and the picture to start from, for
	 * compute shaders too (left in the states the pixel shaders read them
	 * in as well) */
	const D3D12_RESOURCE_STATES read = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	for (u32 i = 0; i < r.slots.used; i++)
	{
		DX12PooledTexture *t = r.slots.images[i];
		if (t != nullptr && t->state != read)
			context->Transition(t->resource, t->state, read);
	}

	/* The bins' counts and starts of this frame's runs, and the counts for
	 * the log, 0; the pool is 0 after the frame before's clear */
	const u32 used = (r.gpu ? r.chunks : words[PVRC_F_CHUNKS]) * words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	D3D12_RESOURCE_STATES binState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_COPY_DEST);
	if (used >= r.runs)
		cmd->CopyBufferRegion(r.bins, 0, r.zero, 0, (u64)PVRC_B_POOL(r.runs) * 4);
	else
	{
		if (used != 0)
		{
			cmd->CopyBufferRegion(r.bins, (u64)PVRC_B_COUNT(r.runs) * 4, r.zero, 0, (u64)used * 4);
			cmd->CopyBufferRegion(r.bins, (u64)PVRC_B_START(r.runs) * 4, r.zero, 0, (u64)used * 4);
			cmd->CopyBufferRegion(r.bins, (u64)PVRC_B_CURSOR(r.runs) * 4, r.zero, 0, (u64)used * 4);
		}
		cmd->CopyBufferRegion(r.bins, (u64)PVRC_B_STATS(r.runs) * 4, r.zero, 0, (u64)(PVRC_B_POOL(r.runs) - PVRC_B_STATS(r.runs)) * 4);
	}
	context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	ID3D12DescriptorHeap * const heaps[2] = { context->Srv().Heap(), context->Samplers().Heap() };
	cmd->SetDescriptorHeaps(2, heaps);
	cmd->SetComputeRootSignature(r.root);
	const u32 places[2] = { r.last != ~0u ? r.last : DX12_SRV_NONE, DX12_SRV_PALETTE };
	cmd->SetComputeRoot32BitConstants(0, 2, places, 0);
	cmd->SetComputeRootDescriptorTable(1, context->Srv().Gpu(table));
	cmd->SetComputeRootDescriptorTable(2, context->Srv().Gpu(0));
	cmd->SetComputeRootDescriptorTable(3, context->Samplers().Gpu(0));

	auto barrier = [&]() {
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		cmd->ResourceBarrier(1, &b);
	};
	// the bins' counts, as the GPU made them, to the dispatches' arguments
	D3D12_RESOURCE_STATES argState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
	auto arguments = [&]() {
		context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		context->Transition(r.args, argState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyBufferRegion(r.args, 0, r.bins, (u64)PVRC_B_STATS(r.runs) * 4, PVRC_ST_ARGS * 4);
		const u64 across = ((words[PVRC_F_TILE_LOG2] - 5) * 4 + SSAA_LOG2(r.rtt ? 1u : samples)) * 4;
		cmd->CopyBufferRegion(r.args, PVRC_ST_WALK_U_X * 4, r.across, across, 4);
		cmd->CopyBufferRegion(r.args, PVRC_ST_WALK_S_X * 4, r.across, across, 4);
		context->Transition(r.args, argState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	};
	// (a render to a texture is drawn at one sample a pixel)
	const u32 n = r.rtt ? 1 : samples;
	auto step = [&](int kernel, u32 x, u32 y) {
		cmd->SetPipelineState(kernel == PVRC_K_WALK && n != samples ? r.walk1 : r.pipelines[kernel]);
		if (x != 0 && y != 0)
			cmd->Dispatch(x, y, 1);
		barrier();
	};
	auto indirect = [&](int kernel, u32 at) {
		cmd->SetPipelineState(r.pipelines[kernel]);
		cmd->ExecuteIndirect(r.dispatch, 1, r.args, (u64)at * 4, nullptr, 0);
		barrier();
	};
	const u32 tiles = words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	const u32 groups = (r.prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP;
	if (r.gpu)
	{
		/* A frame of the decode's: its numbering, and the dispatches of
		 * the kernels whose sizes are the GPU's, from it */
		step(PVRC_K_GPUSRC, 1, 1);
		arguments();
		indirect(PVRC_K_SETUP, PVRC_ST_SETUP_X);
		step(PVRC_K_SCAN, 1, 1);
		indirect(PVRC_K_SCATTER, PVRC_ST_SETUP_X);
		indirect(PVRC_K_SORT, PVRC_ST_SORT_X);
	}
	else
	{
		step(PVRC_K_SETUP, groups, 1);
		step(PVRC_K_SCAN, 1, 1);
		step(PVRC_K_SCATTER, groups, 1);
		step(PVRC_K_SORT, tiles, words[PVRC_F_CHUNKS]);
	}
	/* (the walk's two variants, each over the coarse tiles of its list,
	 * the sort's: nothing between them; the clear's size is the scan's) */
	arguments();
	cmd->SetPipelineState(n != samples ? r.walk1Unsorted : r.walkUnsorted);
	cmd->ExecuteIndirect(r.dispatch, 1, r.args, (u64)PVRC_ST_WALK_U_X * 4, nullptr, 0);
	cmd->SetPipelineState(n != samples ? r.walk1 : r.pipelines[PVRC_K_WALK]);
	cmd->ExecuteIndirect(r.dispatch, 1, r.args, (u64)PVRC_ST_WALK_S_X * 4, nullptr, 0);
	barrier();
	indirect(PVRC_K_CLEAR, PVRC_ST_CLEAR_X);
	// the counts, for when this piece comes round
	context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyBufferRegion(r.stats, (u64)slot * PVRC_STATS * 4, r.bins, (u64)PVRC_B_STATS(r.runs) * 4, PVRC_STATS * 4);
	context->Transition(r.bins, binState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	r.statsWritten[slot] = true;
	// (the list this is in signals the next value when it is submitted)
	r.pieceFence[slot] = context->LastSubmitted() + 1;
	// (another root signature and table were bound for compute: the raster's are bound again when they are drawn with)
	context->ForgetRaster();
	statFrames++;
	if (r.gpu)
		statGpuFrames++;
	if (r.rtt)
		statRttFrames++;
}
