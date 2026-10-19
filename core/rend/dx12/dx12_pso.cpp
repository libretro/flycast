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
#include "dx12_pso.h"
#include "dx12context_lr.h"
#include "dx12_shaders.h"
#include "hw/pvr/ta_ctx.h"
#include <streams/file_stream.h>
#include <algorithm>
#include <cstdio>
#include <string>

extern std::string get_writable_data_path(const std::string& filename);

static const char *PipelineFileName = "d3d12_pipelines.bin";

/* The file: this header, the keys, then the library's data */
struct DX12PsoFileHeader
{
	char magic[8];
	u32 version;	// of the keys' layout (DX12PsoKey)
	u32 keyCount;
	u64 libraryBytes;
};
static const char PsoFileMagic[8] = { 'F', 'C', 'D', '1', '2', 'P', 'S', 'O' };
static const u32 PsoFileVersion = 1;
static const u32 MaxKeys = 8192;

static const D3D12_COMPARISON_FUNC depthFuncs[] =
{
	D3D12_COMPARISON_FUNC_NEVER,			//0	Never
	D3D12_COMPARISON_FUNC_LESS,				//1	Less
	D3D12_COMPARISON_FUNC_EQUAL,			//2	Equal
	D3D12_COMPARISON_FUNC_LESS_EQUAL,		//3	Less Or Equal
	D3D12_COMPARISON_FUNC_GREATER,			//4	Greater
	D3D12_COMPARISON_FUNC_NOT_EQUAL,		//5	Not Equal
	D3D12_COMPARISON_FUNC_GREATER_EQUAL,	//6	Greater Or Equal
	D3D12_COMPARISON_FUNC_ALWAYS,			//7	Always
};

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const D3D12_BLEND srcBlend[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND dstBlend[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND srcBlendAlpha[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};
static const D3D12_BLEND dstBlendAlpha[] =
{
	D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
	D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA,
};

static const D3D12_INPUT_ELEMENT_DESC MainLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};
/* (a position alone: the modifier volumes' triangles, and the polygons'
 * vertices read for their position only - the stride is the view's) */
static const D3D12_INPUT_ELEMENT_DESC PositionLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

static void stencilDesc(DX12Stencil stencil, D3D12_DEPTH_STENCIL_DESC& desc)
{
	desc.StencilEnable = stencil != DX12Stencil::None;
	desc.StencilReadMask = 0xff;
	desc.StencilWriteMask = 0xff;
	D3D12_DEPTH_STENCILOP_DESC& op = desc.FrontFace;
	op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	op.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	// (the reference is set with the draw, OMSetStencilRef())
	switch (stencil)
	{
	case DX12Stencil::None:
	case DX12Stencil::Count:
		break;
	case DX12Stencil::Shadow:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 0x80;
		break;
	case DX12Stencil::ShadowLaterWins:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0x81;
		desc.StencilWriteMask = 0x40;
		break;
	case DX12Stencil::ShadowEarlierWins:
		op.StencilPassOp = D3D12_STENCIL_OP_INVERT;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x40;
		break;
	case DX12Stencil::Xor:
		op.StencilPassOp = D3D12_STENCIL_OP_INVERT;
		desc.StencilReadMask = 0;
		desc.StencilWriteMask = 2;
		break;
	case DX12Stencil::Or:
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		desc.StencilReadMask = 2;
		desc.StencilWriteMask = 2;
		break;
	case DX12Stencil::Inclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		op.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX12Stencil::Exclusion:
		op.StencilFailOp = op.StencilDepthFailOp = D3D12_STENCIL_OP_ZERO;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 3;
		desc.StencilWriteMask = 3;
		break;
	case DX12Stencil::Final:
		// takes shadows, in a volume, and not drawn again for it (bit 6);
		// clears the volume bits and that mark
		op.StencilFailOp = op.StencilDepthFailOp = op.StencilPassOp = D3D12_STENCIL_OP_ZERO;
		op.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
		desc.StencilReadMask = 0xC1;
		desc.StencilWriteMask = 0x43;
		break;
	}
	desc.BackFace = desc.FrontFace;
}

bool DX12Pipelines::Build(u64 packed, ID3D12PipelineState **out)
{
	const DX12PsoKey key = DX12PsoKey::Unpack(packed);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root;
	DX12Shader vs, ps;
	switch (key.kind)
	{
	case DX12_PSO_POLY:
		vs = key.gouraud ? DX12Shader::PolyVS : DX12Shader::PolyFlatVS;
		ps = key.gouraud ? DX12Shader::PolyPS : DX12Shader::PolyFlatPS;
		desc.InputLayout = { MainLayout, (UINT)ARRAY_SIZE(MainLayout) };
		break;
	case DX12_PSO_MODVOL:
		vs = DX12Shader::ModVolVS;
		ps = DX12Shader::ModVolPS;
		desc.InputLayout = { PositionLayout, (UINT)ARRAY_SIZE(PositionLayout) };
		break;
	default:
		vs = DX12Shader::QuadVS;
		ps = DX12Shader::QuadPS;
		break;
	}
	desc.VS = dx12_shader(vs);
	desc.PS = dx12_shader(ps);

	D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
	rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.LogicOp = D3D12_LOGIC_OP_NOOP;
	rt.RenderTargetWriteMask = key.writeColor ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
	if (key.special == DX12_BLEND_ALPHA)
	{
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
		rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	}
	else if (key.special == DX12_BLEND_FACTOR)
	{
		rt.BlendEnable = TRUE;
		rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
		rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
	}
	else
	{
		rt.BlendEnable = key.blend;
		rt.SrcBlend = key.blend ? srcBlend[key.src] : D3D12_BLEND_ONE;
		rt.DestBlend = key.blend ? dstBlend[key.dst] : D3D12_BLEND_ZERO;
		rt.SrcBlendAlpha = key.blend ? srcBlendAlpha[key.src] : D3D12_BLEND_ONE;
		rt.DestBlendAlpha = key.blend ? dstBlendAlpha[key.dst] : D3D12_BLEND_ZERO;
	}
	desc.SampleMask = UINT_MAX;

	D3D12_RASTERIZER_DESC& rs = desc.RasterizerState;
	rs.FillMode = D3D12_FILL_MODE_SOLID;
	rs.CullMode = key.cull == 1 ? D3D12_CULL_MODE_FRONT : key.cull == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
	rs.FrontCounterClockwise = TRUE;
	// (the depth is the shaders' own, the clip space's is always 1)
	rs.DepthClipEnable = key.kind == DX12_PSO_QUAD;

	D3D12_DEPTH_STENCIL_DESC& ds = desc.DepthStencilState;
	ds.DepthEnable = key.depthTest;
	ds.DepthWriteMask = key.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = depthFuncs[key.depthFunc & 7];
	stencilDesc(key.stencil, ds);
	if (!key.depthBound)
	{
		ds.DepthEnable = FALSE;
		ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		ds.StencilEnable = FALSE;
	}

	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DX12FrameFormat;
	desc.DSVFormat = key.depthBound ? DX12DepthFormat : DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;

	wchar_t name[24];
	swprintf(name, 24, L"fc%016llx", (unsigned long long)packed);
	if (library != nullptr && SUCCEEDED(library->LoadGraphicsPipeline(name, &desc, __uuidof(ID3D12PipelineState), (void **)out)))
	{
		statFromLibrary++;
		return true;
	}
	if (FAILED(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void **)out)))
	{
		*out = nullptr;
		return false;
	}
	if (library != nullptr && SUCCEEDED(library->StorePipeline(name, *out)))
		storedSinceLoad++;
	return true;
}

ID3D12PipelineState *DX12Pipelines::Make(u64 key, u32 slot, bool inFrame)
{
	ID3D12PipelineState *pso = nullptr;
	if (used >= SLOTS * 3 / 4 || !Build(key, &pso))
	{
		WARN_LOG(RENDERER, "DX12: pipeline %016llx could not be made", (unsigned long long)key);
		return nullptr;
	}
	slots[slot].key = key;
	slots[slot].pso = pso;
	used++;
	statMade++;
	if (inFrame && started)
	{
		statMadeInFrame++;
		NOTICE_LOG(RENDERER, "DX12: pipeline %016llx made in a frame", (unsigned long long)key);
	}
	if (keys.size() < MaxKeys)
		keys.push_back(key);
	return pso;
}

void DX12Pipelines::Add(u64 key)
{
	u32 i = Hash(key);
	for (;;)
	{
		Slot& s = slots[i];
		if (s.key == key)
			return;
		if (s.key == 0)
		{
			Make(key, i, false);
			return;
		}
		i = (i + 1) & (SLOTS - 1);
	}
}

bool DX12Pipelines::Init(ID3D12Device *device, ID3D12RootSignature *root)
{
	Term();
	this->device = device;
	this->root = root;
	slots = new Slot[SLOTS]();
	used = 0;
	statMade = statMadeInFrame = statFromLibrary = 0;
	storedSinceLoad = 0;
	keys.clear();
	keys.reserve(MaxKeys);
	if (FAILED(device->QueryInterface(__uuidof(ID3D12Device1), (void **)&device1)))
		device1 = nullptr;
	LoadFile();
	return true;
}

void DX12Pipelines::MakeAtStart()
{
	started = false;
	const size_t fromFile = keys.size();
	DX12PsoKey k{};
	k.depthBound = true;
	// the modifier volumes: the volumes themselves, the area summed, what is left darkened
	k.kind = DX12_PSO_MODVOL;
	for (u32 cull = 0; cull < 3; cull++)
	{
		k.cull = cull;
		k.blend = false;
		k.writeColor = false;
		k.depthTest = true;
		k.depthWrite = false;
		k.depthFunc = 4;
		k.stencil = DX12Stencil::Or;
		Add(k.Pack());
		k.stencil = DX12Stencil::Xor;
		Add(k.Pack());
		k.depthTest = false;
		k.depthFunc = 7;
		k.stencil = DX12Stencil::Inclusion;
		Add(k.Pack());
		k.stencil = DX12Stencil::Exclusion;
		Add(k.Pack());
		// the depth of sorted polygons for a later render pass
		k.depthTest = true;
		k.depthWrite = true;
		k.depthFunc = 6;
		k.stencil = DX12Stencil::None;
		Add(k.Pack());
	}
	k = DX12PsoKey{};
	k.kind = DX12_PSO_MODVOL;
	k.depthBound = true;
	k.special = DX12_BLEND_ALPHA;
	k.writeColor = true;
	k.depthFunc = 7;
	k.stencil = DX12Stencil::Final;
	Add(k.Pack());
	// the depth prepass of sorted polygons
	k = DX12PsoKey{};
	k.kind = DX12_PSO_POLY;
	k.depthBound = true;
	k.depthTest = true;
	k.depthWrite = true;
	k.depthFunc = 6;
	for (u32 g = 0; g < 2; g++)
		for (u32 cull = 0; cull < 3; cull++)
		{
			k.gouraud = g != 0;
			k.cull = cull;
			Add(k.Pack());
		}
	/* The polygons' pipelines games mostly draw with (DX12Renderer::DrawPoly()):
	 * opaque ones with every depth mode, punch-through and translucent ones
	 * with the blends games use, the sorted translucent ones, and the
	 * shadow passes. A game's others are made as it first draws with them,
	 * and are in the key log for its next run. */
	static const u8 blends[][2] = { { 4, 5 }, { 1, 1 }, { 1, 0 }, { 4, 1 }, { 0, 1 }, { 1, 5 } };
	for (u32 g = 0; g < 2; g++)
		for (u32 cull = 0; cull < 3; cull++)
		{
			k = DX12PsoKey{};
			k.kind = DX12_PSO_POLY;
			k.gouraud = g != 0;
			k.cull = cull;
			k.depthBound = true;
			k.depthTest = true;
			k.writeColor = true;
			// opaque: no blend, the shadow stencil
			k.stencil = DX12Stencil::Shadow;
			for (u32 func = 1; func < 8; func++)
				for (u32 w = 0; w < 2; w++)
				{
					k.depthFunc = func;
					k.depthWrite = w != 0;
					Add(k.Pack());
				}
			// punch-through: blended, depth written, greater or equal
			k.blend = true;
			k.depthFunc = 6;
			k.depthWrite = true;
			for (const u8 *b : blends)
			{
				k.src = b[0];
				k.dst = b[1];
				Add(k.Pack());
			}
			// translucent, sorted: no depth write; and unsorted with the depth modes seen most
			k.stencil = DX12Stencil::None;
			for (const u8 *b : blends)
			{
				k.src = b[0];
				k.dst = b[1];
				k.depthFunc = 6;
				k.depthWrite = false;
				Add(k.Pack());
				k.depthFunc = 7;
				Add(k.Pack());
				k.depthWrite = true;
				Add(k.Pack());
				k.depthFunc = 6;
				Add(k.Pack());
			}
			// the shadow passes: where the polygon is (equal), nothing written but the colour
			k.blend = false;
			k.depthWrite = false;
			k.depthFunc = 2;
			k.stencil = DX12Stencil::ShadowLaterWins;
			Add(k.Pack());
			k.stencil = DX12Stencil::ShadowEarlierWins;
			Add(k.Pack());
		}
	// the quads
	k = DX12PsoKey{};
	k.kind = DX12_PSO_QUAD;
	k.writeColor = true;
	for (u32 d = 0; d < 2; d++)
		for (u32 s = 0; s < 3; s++)
		{
			k.depthBound = d != 0;
			k.special = s;
			Add(k.Pack());
		}
	// what earlier runs made
	std::vector<u64> kept(keys.begin(), keys.begin() + fromFile);
	for (u64 key : kept)
		if (DX12PsoKey::Valid(key))
			Add(key);
	started = true;
	NOTICE_LOG(RENDERER, "DX12: %u pipelines made at the start (%u from the library, %u keys from earlier runs)",
			statMade, statFromLibrary, (u32)fromFile);
}

void DX12Pipelines::LoadFile()
{
	const std::string path = get_writable_data_path(PipelineFileName);
	void *file = nullptr;
	int64_t size = 0;
	if (filestream_read_file(path.c_str(), &file, &size) && file != nullptr && (size_t)size >= sizeof(DX12PsoFileHeader))
	{
		DX12PsoFileHeader header;
		memcpy(&header, file, sizeof(header));
		const u64 keyBytes = (u64)header.keyCount * sizeof(u64);
		if (!memcmp(header.magic, PsoFileMagic, sizeof(PsoFileMagic)) && header.version == PsoFileVersion
				&& header.keyCount <= MaxKeys && sizeof(header) + keyBytes <= (u64)size
				&& header.libraryBytes <= (u64)size - sizeof(header) - keyBytes)
		{
			const u64 *k = (const u64 *)((const u8 *)file + sizeof(header));
			for (u32 i = 0; i < header.keyCount; i++)
				keys.push_back(k[i]);
			const u8 *lib = (const u8 *)file + sizeof(header) + keyBytes;
			libraryData.assign(lib, lib + header.libraryBytes);
		}
	}
	free(file);
	if (device1 != nullptr)
	{
		HRESULT hr = E_FAIL;
		if (!libraryData.empty())
			hr = device1->CreatePipelineLibrary(libraryData.data(), libraryData.size(), __uuidof(ID3D12PipelineLibrary), (void **)&library);
		if (FAILED(hr))
		{
			// (a new driver, or none kept: an empty one)
			if (!libraryData.empty())
				INFO_LOG(RENDERER, "DX12: the pipeline library kept is not this driver's (%x): started again", (unsigned)hr);
			library = nullptr;
			libraryData.clear();
			if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, __uuidof(ID3D12PipelineLibrary), (void **)&library)))
				library = nullptr;
		}
	}
	NOTICE_LOG(RENDERER, "DX12: pipeline file %s: %u keys, library of %u KB%s", path.c_str(), (u32)keys.size(),
			(u32)(libraryData.size() / 1024), library == nullptr ? " (no pipeline library on this device)" : "");
}

void DX12Pipelines::SaveFile()
{
	const bool newKeys = statMade != statFromLibrary || storedSinceLoad != 0;
	if (!newKeys && statMadeInFrame == 0)
		return;
	std::sort(keys.begin(), keys.end());
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	size_t libBytes = library != nullptr ? library->GetSerializedSize() : 0;
	DX12PsoFileHeader header;
	memcpy(header.magic, PsoFileMagic, sizeof(PsoFileMagic));
	header.version = PsoFileVersion;
	header.keyCount = (u32)keys.size();
	std::vector<u8> file(sizeof(header) + keys.size() * sizeof(u64) + libBytes);
	u8 *lib = &file[sizeof(header) + keys.size() * sizeof(u64)];
	if (libBytes != 0 && FAILED(library->Serialize(lib, libBytes)))
		libBytes = 0;
	header.libraryBytes = libBytes;
	file.resize(sizeof(header) + keys.size() * sizeof(u64) + libBytes);
	memcpy(&file[0], &header, sizeof(header));
	if (!keys.empty())
		memcpy(&file[sizeof(header)], keys.data(), keys.size() * sizeof(u64));
	// (written beside it, then put in its place: a write cut short leaves the one there was)
	const std::string path = get_writable_data_path(PipelineFileName);
	const std::string tmpPath = path + ".tmp";
	RFILE *f = filestream_open(tmpPath.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == nullptr)
		return;
	bool written = filestream_write(f, &file[0], (int64_t)file.size()) == (int64_t)file.size();
	filestream_close(f);
	if (written)
	{
		filestream_delete(path.c_str());
		written = filestream_rename(tmpPath.c_str(), path.c_str()) == 0;
	}
	if (!written)
		filestream_delete(tmpPath.c_str());
	else
		NOTICE_LOG(RENDERER, "DX12: pipeline file saved: %u keys, library of %u KB", header.keyCount, (u32)(libBytes / 1024));
}

void DX12Pipelines::Term()
{
	if (slots != nullptr)
	{
		NOTICE_LOG(RENDERER, "DX12: %u pipelines made, %u of them in a frame, %u from the library", statMade, statMadeInFrame, statFromLibrary);
		SaveFile();
		for (u32 i = 0; i < SLOTS; i++)
			if (slots[i].pso != nullptr)
				slots[i].pso->Release();
		delete [] slots;
		slots = nullptr;
	}
	if (library != nullptr)
		library->Release();
	library = nullptr;
	libraryData.clear();
	libraryData.shrink_to_fit();
	if (device1 != nullptr)
		device1->Release();
	device1 = nullptr;
	keys.clear();
	used = 0;
	device = nullptr;
	root = nullptr;
	started = false;
}
