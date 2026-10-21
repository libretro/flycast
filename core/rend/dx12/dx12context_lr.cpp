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
#include "dx12context_lr.h"
#include "rend/dxgi_budget.h"
#include "rend/ssaa_pattern.h"
#include <algorithm>

DX12Context DX12Context::instance;

typedef HRESULT (WINAPI *SerializeRootSignature_t)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *CreateDXGIFactory1_t)(REFIID, void **);

static const DXGI_FORMAT textureFormats[5] = {
	DXGI_FORMAT_B5G6R5_UNORM,		// TextureType::_565
	DXGI_FORMAT_B5G5R5A1_UNORM,		// TextureType::_5551
	DXGI_FORMAT_B4G4R4A4_UNORM,		// TextureType::_4444
	DXGI_FORMAT_R8G8B8A8_UNORM,		// TextureType::_8888
	DXGI_FORMAT_R8_UNORM,			// TextureType::_8
};

/* ---- Descriptor heaps ---- */

bool DX12DescriptorHeap::Init(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shaderVisible, u32 reserved)
{
	Term();
	D3D12_DESCRIPTOR_HEAP_DESC desc{};
	desc.Type = type;
	desc.NumDescriptors = count;
	desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), (void **)heap.put())))
	{
		heap.reset();
		return false;
	}
	cpuStart = heap->GetCPUDescriptorHandleForHeapStart();
	if (shaderVisible)
		gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
	increment = device->GetDescriptorHandleIncrementSize(type);
	capacity = count;
	freeList = new u32[count];
	Range(reserved, count);
	return true;
}

void DX12DescriptorHeap::Range(u32 first, u32 end)
{
	freeCount = 0;
	// (the lowest first)
	for (u32 i = end; i > first; i--)
		freeList[freeCount++] = i - 1;
}

void DX12DescriptorHeap::Term()
{
	delete [] freeList;
	freeList = nullptr;
	freeCount = capacity = 0;
	heap.reset();
}

u32 DX12DescriptorHeap::Alloc()
{
	return freeCount != 0 ? freeList[--freeCount] : ~0u;
}

void DX12DescriptorHeap::Free(u32 index)
{
	if (index < capacity && freeCount < capacity)
		freeList[freeCount++] = index;
}

/* ---- The context ---- */

bool dx12_make_texture(ID3D12Device *device, u32 width, u32 height, u32 levels, DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out, const D3D12_CLEAR_VALUE *clear)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = (UINT16)levels;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
			__uuidof(ID3D12Resource), (void **)out.put())))
	{
		WARN_LOG(RENDERER, "DX12: texture creation failed (%d x %d, %d levels, format %d)", width, height, levels, (int)format);
		out.reset();
		return false;
	}
	return true;
}

bool DX12Context::CanRenderTo(DXGI_FORMAT format) const
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
	return device && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
			&& (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
}

bool DX12Context::CanWriteUAV(DXGI_FORMAT format) const
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
	return device && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
			&& (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0
			&& (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

u32 DX12Context::SsaaSamples(u32 requested, u32 perSampleKib, u32 fixedKib, const char *what) const
{
	if (requested <= 1 || !device)
		return 1;
	// the counts the frame's colour and depth can both be drawn with, in the standard pattern
	u32 counts = 1;
	for (u32 n = 2; n <= SSAA_MAX_SAMPLES; n <<= 1)
	{
		bool ok = true;
		for (DXGI_FORMAT format : { DX12FrameFormat, DX12DepthFormat })
		{
			D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
			levels.Format = format;
			levels.SampleCount = n;
			ok = ok && SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)))
					&& levels.NumQualityLevels > 0;
		}
		if (ok)
			counts |= n;
	}
	const u32 deviceSamples = ssaa_device_samples(requested, counts);
	const u32 samples = ssaa_clamp_samples(deviceSamples, vramBudget.available_kib(), fixedKib, perSampleKib);
	NOTICE_LOG(RENDERER, "%s: SSAA %u samples a pixel (asked %u, the device can draw %u; %u MB a sample, %u MB of the budget left)",
			what, samples, requested, deviceSamples, perSampleKib >> 10, vramBudget.available_kib() >> 10);
	return samples;
}

bool DX12Context::Init(const struct retro_hw_render_interface_d3d12 *iface)
{
	Term();
	if (iface == nullptr || iface->device == nullptr || iface->queue == nullptr)
		return false;
	iface->device->AddRef();
	device.reset(iface->device);
	iface->queue->AddRef();
	queue.reset(iface->queue);
	requiredState = iface->required_state;
	handle = iface->handle;
	set_texture = iface->set_texture;
	if (iface->interface_version >= RETRO_HW_RENDER_INTERFACE_D3D12_VERSION_2
			&& iface->get_sync_index != nullptr && iface->get_sync_index_mask != nullptr
			&& iface->wait_sync_index != nullptr && iface->set_texture_fenced != nullptr)
	{
		v2 = true;
		get_sync_index = iface->get_sync_index;
		get_sync_index_mask = iface->get_sync_index_mask;
		wait_sync_index = iface->wait_sync_index;
		set_texture_fenced = iface->set_texture_fenced;
	}
	else if (set_texture == nullptr)
	{
		ERROR_LOG(RENDERER, "DX12: the frontend's interface has no set_texture");
		Term();
		return false;
	}

	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
			|| options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
	{
		/* (tier 1 has no table of thousands of textures: the Direct3D 11
		 * renderer is for those devices) */
		ERROR_LOG(RENDERER, "DX12: resource binding tier 2 is needed (the device has %d)", (int)options.ResourceBindingTier);
		Term();
		return false;
	}
	for (int i = 0; i < 5; i++)
	{
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { textureFormats[i] };
		const UINT want = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
		formatSupported[i] = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
				&& (support.Support1 & want) == want;
	}

	// the adapter's budget (rend/dxgi_budget.h), found by its LUID
	{
		vramBudget = VramBudget();
		UINT vendor = 0;
		HMODULE dxgi = LoadLibraryA("dxgi.dll");
		CreateDXGIFactory1_t createFactory = dxgi != nullptr ? (CreateDXGIFactory1_t)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
		IDXGIFactory4 *factory = nullptr;
		if (createFactory != nullptr && SUCCEEDED(createFactory(__uuidof(IDXGIFactory4), (void **)&factory)))
		{
			IDXGIAdapter *adapter = nullptr;
			if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), __uuidof(IDXGIAdapter), (void **)&adapter)))
			{
				DXGI_ADAPTER_DESC desc;
				const bool haveDesc = SUCCEEDED(adapter->GetDesc(&desc));
				if (haveDesc)
					vendor = desc.VendorId;
				vramBudget = dxgi_vram_budget(adapter, haveDesc ? &desc : nullptr);
				adapter->Release();
			}
			factory->Release();
		}
		if (dxgi != nullptr)
			FreeLibrary(dxgi);
		if (vramBudget.budget_kib == 0)
			vramBudget.budget_kib = 512 * 1024;
		NOTICE_LOG(RENDERER, "DX12: vendor %04x, resource binding tier %d, interface version %d, required state %x",
				vendor, (int)options.ResourceBindingTier, v2 ? 2 : 1, (unsigned)requiredState);
		vram_budget_log("DX12", vramBudget);
	}

	bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)fence.put()));
	fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	ok = ok && fenceEvent != nullptr;
	fenceValue = completed = 0;
	for (u32 i = 0; ok && i < ALLOCATORS; i++)
	{
		ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)allocators[i].put()));
		allocatorFence[i] = 0;
	}
	allocatorNext = 0;
	ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr,
			__uuidof(ID3D12GraphicsCommandList), (void **)cmdList.put()));
	if (ok)
		cmdList->Close();
	recording = false;

	/* The upload ring: what a frame's renders take to the GPU, vertices,
	 * indexes, constants, textures as they are in video memory, the
	 * textures the CPU converts - a share of the video memory there is,
	 * 32 to 96 MB, mapped once and for good */
	if (ok)
	{
		u64 bytes = (u64)vramBudget.available_kib() * 1024 / 32;
		bytes = std::max<u64>(bytes, 32ull << 20);
		bytes = std::min<u64>(bytes, 96ull << 20);
		ringSize = bytes & ~0xffffull;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = ringSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr, __uuidof(ID3D12Resource), (void **)ring.put()));
		const D3D12_RANGE none = { 0, 0 };
		void *p = nullptr;
		ok = ok && SUCCEEDED(ring->Map(0, &none, &p));
		ringCpu = (u8 *)p;
		ringGpu = ok ? ring->GetGPUVirtualAddress() : 0;
		ringHead = ringTail = ringSubmitHead = 0;
		ringMarkFirst = ringMarkCount = 0;
	}
	releasedFirst = releasedCount = 0;

	// the heaps
	ok = ok && srvHeap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SrvHeapSize, true, DX12_SRV_FIRST_FREE);
	if (ok)
		// (the table's own; the UAVs' are handed out by DX12TexDecoder from UavFirst on)
		srvHeap.Range(DX12_SRV_FIRST_FREE, UavFirst);
	ok = ok && samplerHeap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, true, DX12_SAMPLER_TABLE_SIZE);
	ok = ok && rtvHeap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, false, TRANSIENT_RTVS);
	transientRtv = 0;
	// (the depth's three views, and SSAA's depth's)
	ok = ok && dsvHeap.Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8, false);
	if (ok)
	{
		// every place in the table something: a view of nothing
		for (u32 i = 0; i < UavFirst; i++)
			MakeSrv(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 1, i);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R8G8B8A8_UINT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		for (u32 i = UavFirst; i < SrvHeapSize; i++)
			device->CreateUnorderedAccessView(nullptr, nullptr, &uav, srvHeap.Cpu(i));
	}
	ok = ok && MakeRootSignatures();
	if (!ok)
	{
		ERROR_LOG(RENDERER, "DX12: initialization failed");
		Term();
		return false;
	}
	pipelines.Init(device, rasterRoot);
	pipelines.MakeAtStart();
	frameResource.reset();
	handedOver = false;
	statStalls = statSubmits = 0;
	return true;
}

bool DX12Context::MakeRootSignatures()
{
	HMODULE lib = GetModuleHandleA("d3d12.dll");
	if (lib == nullptr)
		lib = d3d12Module = LoadLibraryA("d3d12.dll");
	SerializeRootSignature_t serialize = lib != nullptr ? (SerializeRootSignature_t)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : nullptr;
	if (serialize == nullptr)
		return false;
	auto make = [&](const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& out) {
		ComPtr<ID3DBlob> blob, errors;
		if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put()))
				|| FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
						__uuidof(ID3D12RootSignature), (void **)out.put())))
		{
			ERROR_LOG(RENDERER, "DX12: root signature: %s", errors ? (const char *)errors->GetBufferPointer() : "");
			out.reset();
			return false;
		}
		return true;
	};

	/* the polygons', the modifier volumes', the quads', and the per-pixel
	 * renderer's (see shaders/dx12_raster.hlsl): its lists, heads and
	 * counter (u1 to u3), the stencil (t4) and the depth (t6) are one table
	 * at OitFirst of the heap, its translucent polygons' parameters (t5) a
	 * root descriptor */
	D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, DX12_SRV_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE samplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, DX12_SAMPLER_TABLE_SIZE, 0, 1, 0 };
	D3D12_DESCRIPTOR_RANGE oitRanges[3] = {
		{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 1, 0, 0 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 3 },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 4 },
	};
	D3D12_ROOT_PARAMETER params[7]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 1, 0, PolyConstantCount };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[1].Descriptor = { 0, 0 };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor = { 2, 0 };
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[3].DescriptorTable = { 1, &srvRange };
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable = { 1, &samplerRange };
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[5].DescriptorTable = { 3, oitRanges };
	params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[6].Descriptor = { 5, 0 };
	params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC statics[2]{};
	for (int i = 0; i < 2; i++)
	{
		D3D12_STATIC_SAMPLER_DESC& s = statics[i];
		s.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.MaxAnisotropy = 1;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderRegister = i;
		s.RegisterSpace = 0;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}
	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 7;
	desc.pParameters = params;
	desc.NumStaticSamplers = 2;
	desc.pStaticSamplers = statics;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	if (!make(desc, rasterRoot))
		return false;

	// the texture kernels' (rend/texdecode.h: b0, t0 the ring, u0 the level)
	D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER kparams[3]{};
	kparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	kparams[0].Constants = { 0, 0, 8 };
	kparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	kparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	kparams[1].Descriptor = { 0, 0 };
	kparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	kparams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	kparams[2].DescriptorTable = { 1, &uavRange };
	kparams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC kdesc{};
	kdesc.NumParameters = 3;
	kdesc.pParameters = kparams;
	if (!make(kdesc, decodeRoot))
		return false;

	/* Draws that set the polygon's root constants each, recorded as one
	 * ExecuteIndirect (DX12Renderer's batches). A device that cannot has
	 * its draws recorded one by one. */
	D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
	args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	args[0].Constant.RootParameterIndex = 0;
	args[0].Constant.DestOffsetIn32BitValues = 0;
	args[0].Constant.Num32BitValuesToSet = PolyConstantCount;
	args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	D3D12_COMMAND_SIGNATURE_DESC sig{};
	sig.ByteStride = PolyConstantCount * 4 + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	sig.NumArgumentDescs = 2;
	sig.pArgumentDescs = args;
	if (FAILED(device->CreateCommandSignature(&sig, rasterRoot, __uuidof(ID3D12CommandSignature), (void **)drawSignature.put())))
	{
		INFO_LOG(RENDERER, "DX12: no ExecuteIndirect of root constants on this device: draws recorded one by one");
		drawSignature.reset();
	}
	return true;
}

void DX12Context::MakeSrv(ID3D12Resource *resource, DXGI_FORMAT format, u32 levels, u32 index)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
	desc.Format = format;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Texture2D.MipLevels = levels;
	device->CreateShaderResourceView(resource, &desc, srvHeap.Cpu(index));
}

void DX12Context::Term()
{
	if (device)
	{
		// everything done: what is released below is not used any more
		if (recording)
			Submit();
		WaitFor(fenceValue);
		ReclaimReleased(fenceValue);
	}
	pipelines.Term();
	drawSignature.reset();
	decodeRoot.reset();
	rasterRoot.reset();
	dsvHeap.Term();
	rtvHeap.Term();
	samplerHeap.Term();
	srvHeap.Term();
	if (ring && ringCpu != nullptr)
		ring->Unmap(0, nullptr);
	ringCpu = nullptr;
	ring.reset();
	ringSize = ringHead = ringTail = 0;
	ringMarkFirst = ringMarkCount = 0;
	readback.reset();
	readbackSize = 0;
	cmdList.reset();
	for (auto& a : allocators)
		a.reset();
	recording = false;
	fence.reset();
	if (fenceEvent != nullptr)
		CloseHandle(fenceEvent);
	fenceEvent = nullptr;
	fenceValue = completed = 0;
	queue.reset();
	device.reset();
	if (d3d12Module != nullptr)
		FreeLibrary(d3d12Module);
	d3d12Module = nullptr;
	frameResource.reset();
	handedOver = false;
	v2 = false;
	handle = nullptr;
	set_texture = nullptr;
	get_sync_index = nullptr;
	get_sync_index_mask = nullptr;
	wait_sync_index = nullptr;
	set_texture_fenced = nullptr;
	beforeSubmit = nullptr;
	beforeSubmitArg = nullptr;
}

/* ---- Recording ---- */

void DX12Context::ResetBound()
{
	memset(&bound, 0, sizeof(bound));
	bound.topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	bound.rtv = bound.dsv = ~0u - 1;
	bound.vertexBuffer.BufferLocation = ~0ull;
	bound.indexBuffer.BufferLocation = ~0ull;
}

ID3D12GraphicsCommandList *DX12Context::Cmd()
{
	if (recording)
		return cmdList;
	const u32 slot = allocatorNext % ALLOCATORS;
	if (allocatorFence[slot] > Completed())
	{
		/* Every allocator holds a list the GPU has not done yet: the GPU is
		 * that many lists behind */
		statStalls++;
		WaitFor(allocatorFence[slot]);
	}
	allocators[slot]->Reset();
	cmdList->Reset(allocators[slot], nullptr);
	ID3D12DescriptorHeap * const heaps[2] = { srvHeap.Heap(), samplerHeap.Heap() };
	cmdList->SetDescriptorHeaps(2, heaps);
	ResetBound();
	recording = true;
	return cmdList;
}

void DX12Context::BindRaster()
{
	ID3D12GraphicsCommandList *cmd = Cmd();
	if (bound.rasterBound)
		return;
	bound.rasterBound = true;
	cmd->SetGraphicsRootSignature(rasterRoot);
	cmd->SetGraphicsRootDescriptorTable(3, srvHeap.Gpu(0));
	cmd->SetGraphicsRootDescriptorTable(4, samplerHeap.Gpu(0));
	cmd->SetGraphicsRootDescriptorTable(5, srvHeap.Gpu(oitTableSsaa ? OitSsaaFirst : OitFirst));
	bound.oitSsaa = oitTableSsaa;
	bound.rootConstantsSet = false;
	bound.pso = nullptr;
}

u64 DX12Context::Submit()
{
	if (beforeSubmit != nullptr)
		beforeSubmit(beforeSubmitArg);
	if (!recording)
		return fenceValue;
	recording = false;
	if (FAILED(cmdList->Close()))
		WARN_LOG(RENDERER, "DX12: the command list could not be closed");
	ID3D12CommandList * const lists[1] = { cmdList.get() };
	queue->ExecuteCommandLists(1, lists);
	fenceValue++;
	queue->Signal(fence, fenceValue);
	allocatorFence[allocatorNext % ALLOCATORS] = fenceValue;
	allocatorNext++;
	/* What the ring had when the list before this one was submitted is
	 * the GPU's until this list is done: a piece written for this list
	 * may be read by the next one as well (a texture converted into the
	 * ring before its copy is recorded; the decoder's bytes, when a
	 * readback submits the list between the two), so a piece is let go of
	 * one list later than the one it was written for. */
	if (ringMarkCount == RING_MARKS)
	{
		// (the oldest mark merged into the next: its place freed later, not sooner)
		ringMarkFirst = (ringMarkFirst + 1) % RING_MARKS;
		ringMarkCount--;
	}
	ringMarks[(ringMarkFirst + ringMarkCount) % RING_MARKS] = { fenceValue, ringSubmitHead };
	ringMarkCount++;
	ringSubmitHead = ringHead;
	statSubmits++;
	Completed();
	return fenceValue;
}

u64 DX12Context::Completed()
{
	if (!fence)
		return 0;
	const u64 done = fence->GetCompletedValue();
	// (a device removed answers UINT64_MAX)
	completed = std::min(done, fenceValue);
	while (ringMarkCount != 0 && ringMarks[ringMarkFirst].fence <= completed)
	{
		ringTail = ringMarks[ringMarkFirst].head;
		ringMarkFirst = (ringMarkFirst + 1) % RING_MARKS;
		ringMarkCount--;
	}
	// (everything submitted done: the last list's pieces too)
	if (ringMarkCount == 0 && completed == fenceValue)
		ringTail = ringSubmitHead;
	ReclaimReleased(completed);
	return completed;
}

void DX12Context::WaitFor(u64 value)
{
	if (!fence || value == 0 || fence->GetCompletedValue() >= value)
	{
		Completed();
		return;
	}
	if (SUCCEEDED(fence->SetEventOnCompletion(value, fenceEvent)))
		WaitForSingleObject(fenceEvent, 10000);
	Completed();
}

bool DX12Context::Upload(u32 bytes, u32 align, DX12Upload& out)
{
	if (ringCpu == nullptr || bytes > ringSize)
		return false;
	u64 pos = (ringHead + align - 1) & ~(u64)(align - 1);
	// (never across the end: from the start again)
	if (pos % ringSize + bytes > ringSize)
		pos = (pos / ringSize + 1) * ringSize;
	while (pos + bytes - ringTail > ringSize)
	{
		// the GPU still reads what it would write over
		if (ringMarkCount == 0)
		{
			/* (what the list being recorded has already: more than the ring
			 * in one render) */
			WARN_LOG(RENDERER, "DX12: the upload ring is too small for this render (%u bytes more)", bytes);
			return false;
		}
		if (Completed() >= ringMarks[ringMarkFirst].fence)
			continue;
		statStalls++;
		WaitFor(ringMarks[ringMarkFirst].fence);
	}
	ringHead = pos + bytes;
	const u64 at = pos % ringSize;
	out.cpu = ringCpu + at;
	out.gpu = ringGpu + at;
	out.buffer = ring;
	out.offset = at;
	return true;
}

void DX12Context::Release(IUnknown *object)
{
	if (object == nullptr)
		return;
	const u64 last = recording ? fenceValue + 1 : fenceValue;
	if (last <= Completed())
	{
		object->Release();
		return;
	}
	if (releasedCount == RELEASES)
	{
		statStalls++;
		WaitFor(released[releasedFirst].fence);
		ReclaimReleased(completed);
	}
	released[(releasedFirst + releasedCount) % RELEASES] = { object, ~0u, last };
	releasedCount++;
}

void DX12Context::ReleaseSrv(u32 index)
{
	if (index < DX12_SRV_FIRST_FREE || index >= UavFirst)
		return;
	const u64 last = recording ? fenceValue + 1 : fenceValue;
	if (last <= Completed())
	{
		srvHeap.Free(index);
		return;
	}
	if (releasedCount == RELEASES)
	{
		statStalls++;
		WaitFor(released[releasedFirst].fence);
		ReclaimReleased(completed);
	}
	released[(releasedFirst + releasedCount) % RELEASES] = { nullptr, index, last };
	releasedCount++;
}

void DX12Context::ReclaimReleased(u64 done)
{
	while (releasedCount != 0 && released[releasedFirst].fence <= done)
	{
		Released& r = released[releasedFirst];
		if (r.object != nullptr)
			r.object->Release();
		else
			srvHeap.Free(r.srv);
		releasedFirst = (releasedFirst + 1) % RELEASES;
		releasedCount--;
	}
}

ID3D12Resource *DX12Context::ReadbackBuffer(u64 bytes)
{
	if (readback && readbackSize >= bytes)
		return readback;
	// (the one there is may be being read into: a readback waits anyway)
	if (readback)
	{
		Submit();
		WaitFor(fenceValue);
		readback.reset();
	}
	readbackSize = 0;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = (bytes + 0xffff) & ~0xffffull;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, __uuidof(ID3D12Resource), (void **)readback.put())))
	{
		WARN_LOG(RENDERER, "DX12: readback buffer creation failed (%u KB)", (u32)(bytes / 1024));
		readback.reset();
		return nullptr;
	}
	readbackSize = desc.Width;
	return readback;
}

void DX12Context::UavBarrier(ID3D12Resource *resource)
{
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource;
	Cmd()->ResourceBarrier(1, &barrier);
}

void DX12Context::Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES state, UINT subresource)
{
	if (current == state || resource == nullptr)
		return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = subresource;
	barrier.Transition.StateBefore = current;
	barrier.Transition.StateAfter = state;
	Cmd()->ResourceBarrier(1, &barrier);
	current = state;
}

void DX12Context::Transition(DX12Image& image, D3D12_RESOURCE_STATES state)
{
	Transition(image.resource, image.state, state);
}

void DX12Context::SetTargets(u32 rtv, u32 dsv, u32 samples)
{
	// (a list started here forgets what was bound: before the samples are noted)
	ID3D12GraphicsCommandList *cmd = Cmd();
	bound.samples = samples;
	if (rtv == bound.rtv && dsv == bound.dsv && rtv >= TRANSIENT_RTVS)
		return;
	const D3D12_CPU_DESCRIPTOR_HANDLE r = rtvHeap.Cpu(rtv);
	const D3D12_CPU_DESCRIPTOR_HANDLE d = dsvHeap.Cpu(dsv);
	cmd->OMSetRenderTargets(rtv != ~0u ? 1 : 0, rtv != ~0u ? &r : nullptr, FALSE, dsv != ~0u ? &d : nullptr);
	bound.rtv = rtv;
	bound.dsv = dsv;
}

/* ---- The frame ---- */

void DX12Context::SetFrame(const DX12Image *image)
{
	frameResource.reset();
	if (image != nullptr && image->resource)
	{
		image->resource->AddRef();
		frameResource.reset(image->resource.get());
	}
	frameFence = fenceValue;
	handedOver = v2 && frameResource;
	if (handedOver)
		set_texture_fenced(handle, frameResource, DX12FrameFormat, fence, frameFence);
}

void DX12Context::Present()
{
	if (!device || !frameResource)
		return;
	if (v2)
	{
		/* (a frame with no render to the screen in it shows the last one again) */
		if (!handedOver)
			set_texture_fenced(handle, frameResource, DX12FrameFormat, fence, frameFence);
		handedOver = false;
	}
	else
		/* the frontend copies it in video_refresh, on the same queue, after
		 * the render that drew it */
		set_texture(handle, frameResource, DX12FrameFormat);
}

unsigned DX12Context::GetPresentCount() const
{
	if (!v2)
		return 1;
	const unsigned mask = get_sync_index_mask(handle);
	unsigned count = 1;
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned DX12Context::GetPresentIndex() const
{
	if (!v2)
		return 0;
	const unsigned index = get_sync_index(handle);
	return index < 32 ? index : 0;
}

void DX12Context::WaitPresentIndex()
{
	if (v2)
		wait_sync_index(handle);
}
