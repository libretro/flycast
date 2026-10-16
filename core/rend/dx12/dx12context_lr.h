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
#pragma once
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstring>
#include "types.h"
#include <libretro.h>
#include <libretro_d3d12.h>
#include "rend/dx11/comptr.h"
#include "rend/vram_budget.h"
#include "rend/TexCache.h"
#include "dx12_pso.h"

/* The frontend's Direct3D 12 device and queue, from RETRO_HW_CONTEXT_D3D12's
 * interface, and what the renderers share of them.
 *
 * Everything is recorded into one command list at a time, opened when
 * something is first recorded (Cmd()) and executed on the frontend's queue
 * when a render is done (Submit()), which signals this context's fence.
 * The fence is only ever polled - nothing waits for it but a readback the
 * game asked for, and the frontend's own wait_sync_index (version 2) - and
 * what the GPU still reads (the command allocators, the upload ring, a
 * resource let go of) is used again once the fence says it is done with it.
 *
 * Version 1 of the interface hands the frame over with set_texture, and the
 * frontend copies it inside video_refresh, on the same queue: one picture.
 * Version 2 (asked for through the negotiation interface, as Direct3D 11's
 * version 2 is) hands it over with set_texture_fenced, the fence and the
 * value its render signals, and the frontend reads it where it is, with no
 * copy: a picture for each of its sync indexes. */

/* A texture or picture of the renderer's, and the state it was left in */
struct DX12Image
{
	ComPtr<ID3D12Resource> resource;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
	u32 srv = ~0u;		// its place in the bindless table
	u32 rtv = ~0u;		// in the render target heap, if it has one
	u32 width = 0;
	u32 height = 0;
};

/* A piece of the upload ring: where the CPU writes, where the GPU reads */
struct DX12Upload
{
	u8 *cpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
	ID3D12Resource *buffer = nullptr;
	u64 offset = 0;		// in the buffer
};

/* A heap of descriptors with places handed out and back: a stack of the
 * free ones, made with the heap (nothing allocated after) */
class DX12DescriptorHeap
{
public:
	bool Init(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shaderVisible, u32 reserved = 0);
	void Term();
	/* ~0u when there is none left */
	u32 Alloc();
	void Free(u32 index);
	D3D12_CPU_DESCRIPTOR_HANDLE Cpu(u32 index) const { D3D12_CPU_DESCRIPTOR_HANDLE h = cpuStart; h.ptr += (SIZE_T)index * increment; return h; }
	D3D12_GPU_DESCRIPTOR_HANDLE Gpu(u32 index) const { D3D12_GPU_DESCRIPTOR_HANDLE h = gpuStart; h.ptr += (UINT64)index * increment; return h; }
	ID3D12DescriptorHeap *Heap() const { return heap; }
	/* The places from @first on handed out from here, those before kept */
	void Range(u32 first, u32 end);

private:
	ComPtr<ID3D12DescriptorHeap> heap;
	D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
	u32 increment = 0;
	u32 *freeList = nullptr;
	u32 freeCount = 0;
	u32 capacity = 0;
};

class DX12Context
{
public:
	static DX12Context *Instance() { return &instance; }

	/* The frontend has made its device (context_reset) */
	bool Init(const struct retro_hw_render_interface_d3d12 *iface);
	/* ...and is about to destroy it, or the game is going */
	void Term();

	ID3D12Device *GetDevice() const { return device; }
	ID3D12CommandQueue *GetQueue() const { return queue; }
	// what video memory there was to use when the context was set up
	const VramBudget& GetVramBudget() const { return vramBudget; }
	/* Whether the 16-bit formats can be sampled */
	bool IsFormatSupported(TextureType type) const { return (unsigned)type < 5 && formatSupported[(int)type]; }
	bool CanRenderTo(DXGI_FORMAT format) const;
	bool CanWriteUAV(DXGI_FORMAT format) const;

	/* ---- Recording and submitting ---- */

	/* The command list things are recorded into, opened if none is */
	ID3D12GraphicsCommandList *Cmd();
	bool IsRecording() const { return recording; }
	/* What is recorded, executed on the frontend's queue; the fence then
	 * signalled with the value returned (the last one, if nothing was
	 * recorded). What is waiting to be recorded (BeforeSubmit) goes first. */
	u64 Submit();
	/* Called before a list is submitted: the uploads and the texture kernels
	 * that are waiting (the renderer's) */
	void SetBeforeSubmit(void (*fn)(void *), void *arg) { beforeSubmit = fn; beforeSubmitArg = arg; }
	/* What the GPU has done, polled */
	u64 Completed();
	u64 LastSubmitted() const { return fenceValue; }
	/* Waits until the GPU has done @value: for a readback only */
	void WaitFor(u64 value);
	ID3D12Fence *GetFence() const { return fence; }

	/* Bytes of the upload ring, mapped for good, for this list's commands
	 * to read: at a multiple of @align (a power of two). The GPU reads it
	 * until the list it is for has been executed; the ring takes it back
	 * then. False if it can never be had (larger than the ring). */
	bool Upload(u32 bytes, u32 align, DX12Upload& out);
	u64 UploadRingSize() const { return ringSize; }
	D3D12_GPU_VIRTUAL_ADDRESS UploadRingGpu() const { return ringGpu; }

	/* An object the GPU may still use, released once it is done with it */
	void Release(IUnknown *object);
	/* A place in the bindless table the GPU may still read, free again once it is done with it */
	void ReleaseSrv(u32 index);
	/* A readback buffer of at least @bytes (made larger if it is not;
	 * waits for the GPU then), mapped; null if it cannot be had */
	ID3D12Resource *ReadbackBuffer(u64 bytes);

	/* A transition, if the image is not in that state already */
	void Transition(DX12Image& image, D3D12_RESOURCE_STATES state);
	void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES state);

	/* ---- Descriptors ---- */
	DX12DescriptorHeap& Srv() { return srvHeap; }			// shader visible: textures (the bindless table), the kernels' UAVs
	DX12DescriptorHeap& Samplers() { return samplerHeap; }	// shader visible: the polygons' samplers
	DX12DescriptorHeap& Rtv() { return rtvHeap; }
	DX12DescriptorHeap& Dsv() { return dsvHeap; }
	/* A view of a 2D texture into the table, at @index */
	void MakeSrv(ID3D12Resource *resource, DXGI_FORMAT format, u32 levels, u32 index);
	/* The polygons' root signature, the table and the samplers bound, if
	 * they are not; ForgetRaster() when another root signature was set */
	void BindRaster();
	void ForgetRaster() { bound.rasterBound = false; }
	/* A place in the render target heap for a view read when a command
	 * is recorded and not after: the next of a ring of them */
	u32 TransientRtv() { return transientRtv = (transientRtv + 1) % TRANSIENT_RTVS; }
	static const u32 TRANSIENT_RTVS = 64;
	/* The bindless table starts here; the UAVs after it */
	static const u32 UavFirst = DX12_SRV_TABLE_SIZE;
	static const u32 UavCount = 4096;

	/* ---- Root signatures, pipelines ---- */
	ID3D12RootSignature *RasterRootSignature() const { return rasterRoot; }
	ID3D12RootSignature *DecodeRootSignature() const { return decodeRoot; }
	DX12Pipelines& Pipelines() { return pipelines; }
	/* ExecuteIndirect of draws that change the polygon's root constants
	 * (null if the device cannot) */
	ID3D12CommandSignature *DrawSignature() const { return drawSignature; }

	/* What is bound, for the list being recorded: set only when it differs */
	void SetPipeline(ID3D12PipelineState *pso)
	{
		if (pso != bound.pso)
		{
			cmdList->SetPipelineState(pso);
			bound.pso = pso;
		}
	}
	/* (a pipeline was set that is not one of the table's) */
	void ForgetPipeline() { bound.pso = nullptr; }
	void SetTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
	{
		if (topology != bound.topology)
		{
			cmdList->IASetPrimitiveTopology(topology);
			bound.topology = topology;
		}
	}
	void SetScissor(const D3D12_RECT& rect)
	{
		if (!bound.scissorSet || memcmp(&rect, &bound.scissor, sizeof(rect)))
		{
			cmdList->RSSetScissorRects(1, &rect);
			bound.scissor = rect;
			bound.scissorSet = true;
		}
	}
	void SetViewport(float width, float height)
	{
		if (!bound.viewportSet || width != bound.viewport[0] || height != bound.viewport[1])
		{
			const D3D12_VIEWPORT vp = { 0.f, 0.f, width, height, 0.f, 1.f };
			cmdList->RSSetViewports(1, &vp);
			bound.viewport[0] = width;
			bound.viewport[1] = height;
			bound.viewportSet = true;
		}
	}
	void SetStencilRef(UINT ref)
	{
		if (!bound.stencilRefSet || ref != bound.stencilRef)
		{
			cmdList->OMSetStencilRef(ref);
			bound.stencilRef = ref;
			bound.stencilRefSet = true;
		}
	}
	void SetVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& view)
	{
		if (memcmp(&view, &bound.vertexBuffer, sizeof(view)))
		{
			cmdList->IASetVertexBuffers(0, 1, &view);
			bound.vertexBuffer = view;
		}
	}
	void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& view)
	{
		if (memcmp(&view, &bound.indexBuffer, sizeof(view)))
		{
			cmdList->IASetIndexBuffer(&view);
			bound.indexBuffer = view;
		}
	}
	void SetBlendFactor(const float factor[4])
	{
		if (!bound.blendFactorSet || memcmp(factor, bound.blendFactor, sizeof(bound.blendFactor)))
		{
			cmdList->OMSetBlendFactor(factor);
			memcpy(bound.blendFactor, factor, sizeof(bound.blendFactor));
			bound.blendFactorSet = true;
		}
	}
	/* (what an ExecuteIndirect changed is not known after it) */
	void ForgetRootConstants() { bound.rootConstantsSet = false; }
	void SetRootConstants(const u32 *values, u32 count)
	{
		if (!bound.rootConstantsSet || memcmp(values, bound.rootConstants, count * 4))
		{
			cmdList->SetGraphicsRoot32BitConstants(0, count, values, 0);
			memcpy(bound.rootConstants, values, count * 4);
			bound.rootConstantsSet = true;
		}
	}
	/* The render target and depth (or none), each with a view in its heap
	 * (a transient render target view is set whatever was bound) */
	void SetTargets(u32 rtv, u32 dsv);
	void ForgetTargets() { bound.rtv = bound.dsv = ~0u - 1; }
	bool HasDepthBound() const { return bound.dsv != ~0u && bound.dsv != ~0u - 1; }

	/* ---- The frame ---- */

	/* The picture the next video_refresh is to show, drawn and left in
	 * required_state by a render that has been submitted: with version 2 it
	 * is handed over here, and is the frontend's (IsHandedOver()) until the
	 * frame has been shown */
	void SetFrame(const DX12Image *image);
	bool IsHandedOver(const DX12Image *image) const { return v2 && handedOver && image != nullptr && image->resource.get() == frameResource.get(); }
	/* Just before video_refresh(RETRO_HW_FRAME_BUFFER_VALID) */
	void Present();
	/* How many pictures the screen is drawn in turn into, and which of them
	 * the next frame is; before drawing into it, the frontend is waited for
	 * to be done with it (version 2's wait_sync_index) */
	unsigned GetPresentCount() const;
	unsigned GetPresentIndex() const;
	void WaitPresentIndex();
	D3D12_RESOURCE_STATES RequiredState() const { return requiredState; }
	bool IsV2() const { return v2; }

	/* For measuring: the waits there were that the protocol does not ask
	 * for (a full ring, every allocator still in use) */
	u32 statStalls = 0;
	u32 statSubmits = 0;

private:
	static DX12Context instance;
	bool MakeRootSignatures();
	void ReclaimReleased(u64 done);
	void ResetBound();

	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	VramBudget vramBudget;
	bool formatSupported[5] = {};
	HMODULE d3d12Module = nullptr;

	// the fence, and the event a readback waits on
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	u64 fenceValue = 0;
	u64 completed = 0;

	// the command allocators, in turn, each with the value its last list signalled
	enum { ALLOCATORS = 8 };
	ComPtr<ID3D12CommandAllocator> allocators[ALLOCATORS];
	u64 allocatorFence[ALLOCATORS] = {};
	u32 allocatorNext = 0;
	ComPtr<ID3D12GraphicsCommandList> cmdList;
	bool recording = false;
	void (*beforeSubmit)(void *) = nullptr;
	void *beforeSubmitArg = nullptr;

	/* The upload ring: positions only ever grow, the place in the buffer is
	 * the position modulo its size. What the GPU may still read starts at
	 * the position the oldest list not done yet was submitted after. */
	ComPtr<ID3D12Resource> ring;
	u8 *ringCpu = nullptr;
	D3D12_GPU_VIRTUAL_ADDRESS ringGpu = 0;
	u64 ringSize = 0;
	u64 ringHead = 0;
	enum { RING_MARKS = 64 };
	struct RingMark { u64 fence; u64 head; };
	RingMark ringMarks[RING_MARKS];
	u32 ringMarkFirst = 0, ringMarkCount = 0;
	u64 ringTail = 0;
	u64 ringSubmitHead = 0;		// the head when the last list was submitted

	// released once the GPU is done: an object, or a place in the bindless table
	enum { RELEASES = 1024 };
	struct Released { IUnknown *object; u32 srv; u64 fence; };
	Released released[RELEASES];
	u32 releasedFirst = 0, releasedCount = 0;

	DX12DescriptorHeap srvHeap, samplerHeap, rtvHeap, dsvHeap;
	ComPtr<ID3D12RootSignature> rasterRoot;
	ComPtr<ID3D12RootSignature> decodeRoot;
	ComPtr<ID3D12CommandSignature> drawSignature;
	DX12Pipelines pipelines;

	struct Bound
	{
		ID3D12PipelineState *pso;
		D3D12_PRIMITIVE_TOPOLOGY topology;
		D3D12_RECT scissor;
		bool scissorSet;
		float viewport[2];
		bool viewportSet;
		UINT stencilRef;
		bool stencilRefSet;
		u32 rootConstants[16];
		bool rootConstantsSet;
		u32 rtv, dsv;
		D3D12_VERTEX_BUFFER_VIEW vertexBuffer;
		D3D12_INDEX_BUFFER_VIEW indexBuffer;
		float blendFactor[4];
		bool blendFactorSet;
		bool rasterBound;
	} bound;
	u32 transientRtv = 0;

	// a picture read back, through this buffer, mapped
	ComPtr<ID3D12Resource> readback;
	u64 readbackSize = 0;

	// the frame: a reference to its picture, which stays whatever the renderer does with its own
	ComPtr<ID3D12Resource> frameResource;
	u64 frameFence = 0;
	bool handedOver = false;
	D3D12_RESOURCE_STATES requiredState = D3D12_RESOURCE_STATE_COPY_SOURCE;

	// the interface
	bool v2 = false;
	void *handle = nullptr;
	void (*set_texture)(void *, ID3D12Resource *, DXGI_FORMAT) = nullptr;
	unsigned (*get_sync_index)(void *) = nullptr;
	unsigned (*get_sync_index_mask)(void *) = nullptr;
	void (*wait_sync_index)(void *) = nullptr;
	void (*set_texture_fenced)(void *, ID3D12Resource *, DXGI_FORMAT, ID3D12Fence *, UINT64) = nullptr;
};

/* What a render's draw calls and the format of every picture are */
static const DXGI_FORMAT DX12FrameFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT DX12DepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

/* The depth and stencil: a float for the depth, as the Vulkan renderers have it */
static const DXGI_FORMAT DX12DepthResourceFormat = DXGI_FORMAT_R32G8X24_TYPELESS;

/* A committed 2D texture in the default heap */
bool dx12_make_texture(ID3D12Device *device, u32 width, u32 height, u32 levels, DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out,
		const D3D12_CLEAR_VALUE *clear = nullptr);
