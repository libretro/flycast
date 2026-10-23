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
/* The Direct3D 12 renderer's pipeline state objects.
 *
 * One pixel shader draws every polygon (dx12_shaders.h: the polygon's bits
 * are root constants; the per-pixel renderer's has one a pass), so a
 * pipeline is the fixed-function state a polygon is drawn with and nothing
 * more: its key (DX12PsoKey) is the blend, depth, stencil and cull state,
 * the interpolation, and which shaders. Every pipeline is made from its
 * key alone.
 *
 * When. At the start (DX12Context::Init()), every pipeline a frame can
 * need whatever it draws - the modifier volumes' in every mode and cull
 * mode, the depth prepass's, the quads', the texture kernels' - and every
 * key kept from earlier runs (the key log, below), so that a game's own
 * are there before it draws. Direct3D 12 has no dynamic blend or depth
 * state, so a polygon drawn with a key never seen before has no pipeline
 * that draws the same pixels to stand in for it, as Vulkan's general
 * pipelines do: that pipeline is made there and then, once, and its key is
 * logged (statMadeInFrame). The next run makes it at the start.
 *
 * The keys and the device's own compiled pipelines are kept on disk in the
 * system directory (dc/d3d12_pipelines.bin): a header, the keys, then an
 * ID3D12PipelineLibrary, which hands a pipeline back without compiling it
 * again. A library the driver no longer takes (a new driver) is started
 * again empty; the keys stay.
 *
 * SSAA (dx12_renderer.h): a pipeline also has the samples a pixel of what
 * it draws into, and for a polygon whether it is shaded at each sample or
 * once a pixel. When SSAA is taken up (the renderer starts, or changes
 * size), every pipeline of one sample there is gets made for the count
 * taken, at both rates (MakeForSamples()): a frame draws into the screen's
 * samples with what a frame without SSAA draws with, so it finds them made.
 *
 * The table is looked up by key for every draw: open addressing, made with
 * the context; nothing is allocated to find or add a pipeline. Only the
 * thread that renders touches it. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "dx12_shader_flags.h"
#include <vector>

/* (mingw-w64's d3d12.h has ID3D12Device1, which makes them, and not the
 * interface itself: as the Windows SDK has it) */
#ifndef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
#define __ID3D12PipelineLibrary_INTERFACE_DEFINED__
struct ID3D12PipelineLibrary : public ID3D12DeviceChild
{
	virtual HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name, ID3D12PipelineState *pipeline) = 0;
	virtual HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
			REFIID riid, void **pipeline) = 0;
	virtual HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
			REFIID riid, void **pipeline) = 0;
	virtual SIZE_T STDMETHODCALLTYPE GetSerializedSize() = 0;
	virtual HRESULT STDMETHODCALLTYPE Serialize(void *data, SIZE_T size) = 0;
};
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12PipelineLibrary, 0xc64226a8, 0x9201, 0x46af, 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f)
#endif
#endif

/* What the stencil buffer is used for: as the Direct3D 11 renderer's
 * (dx11/dx11_renderstate.h, DX11Stencil) */
enum class DX12Stencil
{
	None,
	Shadow,
	ShadowLaterWins,
	ShadowEarlierWins,
	Xor,
	Or,
	Inclusion,
	Exclusion,
	Final,
	Count
};

/* What the special blends are: the key's own factors; by the source's
 * alpha (the crosshairs); by the blend factor's (the VMUs' screens) */
enum { DX12_BLEND_KEY = 0, DX12_BLEND_ALPHA = 1, DX12_BLEND_FACTOR = 2 };
/* Which shaders: the per-triangle renderer's polygons, the modifier
 * volumes, the quads; the per-pixel renderer's polygons (in a pass, DX12_PASS_),
 * its lists put together, its translucent modifier volumes; SSAA's resolve */
enum { DX12_PSO_POLY = 0, DX12_PSO_MODVOL = 1, DX12_PSO_QUAD = 2, DX12_PSO_OIT = 3, DX12_PSO_OIT_FINAL = 4, DX12_PSO_OIT_MODVOL = 5,
	DX12_PSO_SSAA_RESOLVE = 6 };
/* SSAA's rates (DX12PsoKey::rate): none; a polygon shaded at each sample;
 * once a pixel. (The lists put together: at each sample into samples, or
 * resolved into a picture of one; a translucent volume: with SSAA.) */
enum { DX12_RATE_NONE = 0, DX12_RATE_SAMPLE = 1, DX12_RATE_PIXEL = 2 };

struct DX12PsoKey
{
	u32 kind;			// DX12_PSO_
	bool gouraud;
	bool blend;
	u32 src, dst;		// the TSP's instructions, 0 to 7
	bool writeColor;
	bool depthTest;
	bool depthWrite;
	u32 depthFunc;		// the ISP's depth mode, 0 never to 7 always
	DX12Stencil stencil;
	u32 cull;			// 0 none, 1 front faces, 2 back faces
	u32 special;		// DX12_BLEND_
	bool depthBound;	// a depth and stencil is bound (its format is in the pipeline)
	u32 pass;			// DX12_PASS_, of a DX12_PSO_OIT pipeline
	u32 samples;		// SSAA: log2 of the samples a pixel of what it draws into (0: one)
	u32 rate;			// SSAA: DX12_RATE_

	u64 Pack() const
	{
		return (u64)(kind & 7) | ((u64)gouraud << 3) | ((u64)blend << 4) | ((u64)(src & 7) << 5) | ((u64)(dst & 7) << 8)
				| ((u64)writeColor << 11) | ((u64)depthTest << 12) | ((u64)depthWrite << 13) | ((u64)(depthFunc & 7) << 14)
				| ((u64)((u32)stencil & 15) << 17) | ((u64)(cull & 3) << 21) | ((u64)(special & 3) << 23)
				| ((u64)depthBound << 25) | ((u64)(pass & 3) << 26)
				// (never 0: an empty slot's)
				| (1ull << 28)
				// (after it: a key without SSAA is the one it was before SSAA)
				| ((u64)(samples & 3) << 29) | ((u64)(rate & 3) << 31);
	}
	static DX12PsoKey Unpack(u64 v)
	{
		DX12PsoKey k;
		k.kind = v & 7;
		k.gouraud = (v >> 3) & 1;
		k.blend = (v >> 4) & 1;
		k.src = (v >> 5) & 7;
		k.dst = (v >> 8) & 7;
		k.writeColor = (v >> 11) & 1;
		k.depthTest = (v >> 12) & 1;
		k.depthWrite = (v >> 13) & 1;
		k.depthFunc = (v >> 14) & 7;
		k.stencil = (DX12Stencil)((v >> 17) & 15);
		k.cull = (v >> 21) & 3;
		k.special = (v >> 23) & 3;
		k.depthBound = (v >> 25) & 1;
		k.pass = (v >> 26) & 3;
		k.samples = (v >> 29) & 3;
		k.rate = (v >> 31) & 3;
		return k;
	}
	/* Whether a key read from a file is one this core makes */
	static bool Valid(u64 v)
	{
		const DX12PsoKey k = Unpack(v);
		return (v >> 33) == 0 && ((v >> 28) & 1) == 1 && k.kind <= DX12_PSO_SSAA_RESOLVE && (u32)k.stencil < (u32)DX12Stencil::Count
				&& k.cull <= 2 && k.special <= DX12_BLEND_FACTOR && k.pass <= DX12_PASS_OIT && k.rate <= DX12_RATE_PIXEL;
	}
};

class DX12Pipelines
{
public:
	/* The table, the shaders, the library from the file; @vendor the
	 * adapter's PCI vendor id */
	bool Init(ID3D12Device *device, ID3D12RootSignature *root, UINT vendor);
	/* The library and the keys written to the file, if pipelines were made */
	void Term();
	/* Everything a frame can need, and the keys kept from earlier runs */
	void MakeAtStart();
	/* One more a renderer wants made before a game draws */
	void Prepare(const DX12PsoKey& key) { Add(key.Pack()); }
	/* SSAA of 2^@log2n samples taken up: every pipeline of one sample made
	 * for that many, at each rate it can be drawn at, and the keys of that
	 * many kept from earlier runs (those made already are found made) */
	void MakeForSamples(u32 log2n);

	/* The pipeline of @key: the table's, or made now (and counted) */
	ID3D12PipelineState *Get(const DX12PsoKey& key)
	{
		const u64 k = key.Pack();
		u32 i = Hash(k);
		for (;;)
		{
			Slot& s = slots[i];
			if (s.key == k)
				return s.pso;
			if (s.key == 0)
				return Make(k, i, true);
			i = (i + 1) & (SLOTS - 1);
		}
	}

	// what has been done so far, for measuring
	u32 statMade = 0;			// all of them
	u32 statMadeInFrame = 0;	// of them, while a game ran
	u32 statFromLibrary = 0;	// handed back by the library, not compiled
	u32 statPostZ = 0;			// SSAA's lists' made with post-depth coverage (NVAPI)
	u32 statOwnTest = 0;		// ...made to test their samples themselves

private:
	enum { SLOTS = 8192 };	// (SSAA makes each pipeline of a polygon again at two rates)
	struct Slot
	{
		u64 key;
		ID3D12PipelineState *pso;
	};
	static u32 Hash(u64 k) { k ^= k >> 29; k *= 0x9E3779B97F4A7C15ull; return (u32)(k >> 40) & (SLOTS - 1); }
	ID3D12PipelineState *Make(u64 key, u32 slot, bool inFrame);
	void Add(u64 key);
	bool Build(u64 key, ID3D12PipelineState **out);
	void LoadFile();
	void SaveFile();

	ID3D12Device *device = nullptr;
	ID3D12Device1 *device1 = nullptr;
	ID3D12RootSignature *root = nullptr;
	Slot *slots = nullptr;
	u32 used = 0;
	ID3D12PipelineLibrary *library = nullptr;
	std::vector<u8> libraryData;		// what the library was made from: it reads it for as long as it lives
	std::vector<u64> keys;				// the file's, then those made
	size_t fileKeys = 0;				// how many of them are the file's
	u32 storedSinceLoad = 0;
	bool started = false;
	/* SSAA's lists' pipelines made with NVIDIA's post-depth coverage
	 * (dx12_nvapi.h): on an NVIDIA GPU whose driver has NVAPI, until it
	 * would not make one */
	bool postZCoverage = false;
};
