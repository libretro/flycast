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
#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include "dx12_texture.h"
#include "dx12context_lr.h"
#include "dx12_texdecoder.h"
#include "dx12_quad.h"
#include "rend/rtt_watch.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include <algorithm>
#include <vector>

DX12TexDecoder *DX12Texture::decoder;
DX12TexturePool *DX12Texture::pool;

/* ---- The texture pool ---- */

// by TextureType: 565, 5551, 4444, 8888, 8; and the pictures rendered to
static const DXGI_FORMAT poolFormats[DX12TexturePool::FORMATS] = {
	DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM,
	DXGI_FORMAT_R8G8B8A8_UNORM
};
static const char * const poolFormatNames[DX12TexturePool::FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "8888 rendered to" };
static const u32 poolTexel[DX12TexturePool::FORMATS] = { 2, 2, 2, 4, 1, 4 };

/* Rows of @bytes each, @rows of them, into the upload ring at Direct3D's
 * pitch (a multiple of 256), for CopyTextureRegion. False if the ring
 * cannot hold them. */
static bool uploadRows(DX12Context *context, const u8 *src, u32 srcPitch, u32 bytes, u32 rows, DX12Upload& out, u32& pitch)
{
	pitch = (bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	if (!context->Upload(pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, out))
		return false;
	if (src != nullptr)
	{
		if (pitch == bytes && srcPitch == bytes)
			memcpy(out.cpu, src, (size_t)bytes * rows);
		else
			for (u32 y = 0; y < rows; y++)
				memcpy(out.cpu + (size_t)y * pitch, src + (size_t)y * srcPitch, bytes);
	}
	return true;
}

/* The copy of what uploadRows() put in the ring into @level of @texture */
static void copyToLevel(DX12Context *context, DX12PooledTexture *texture, u32 level, u32 width, u32 height, DXGI_FORMAT format,
		const DX12Upload& upload, u32 pitch)
{
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = texture->resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = level;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = upload.buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = upload.offset;
	src.PlacedFootprint.Footprint.Format = format;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	context->Cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void DX12TexturePool::Init(const VramBudget& budget, const DX12TexDecoder *decoder)
{
	if (ready)
		return;
	book.Reset(1);
	// how much games use them, in eighths of what they use 565
	static const u32 use[FORMATS] = { 8, 6, 6, 4, 2, 0 };
	static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, TexPoolBook::TARGETS };
	u32 have[FORMATS];
	for (int f = 0; f < FORMATS; f++)
	{
		usable[f] = f == TARGET || DX12Context::Instance()->IsFormatSupported((TextureType)f);
		writable[f] = f != TARGET && usable[f] && decoder != nullptr && decoder->Takes((TextureType)f);
		have[f] = usable[f] ? poolTexel[f] : 0;
	}
	u32 counts[TexPoolBook::CLASSES];
	const u64 bytes = TexPoolBook::Plan(budget, have, use, kind, FORMATS, counts);
	ready = true;
	for (int c = 0; c < TexPoolBook::CLASSES; c++)
		for (u32 i = 0; i < counts[c]; i++)
		{
			DX12PooledTexture *texture = Make(c, false);
			if (texture == nullptr)
				break;
			book.Give(texture);
		}
	NOTICE_LOG(RENDERER, "DX12: texture pool of %u textures, %u MB (planned %u MB)", book.made, (u32)(book.bytesMade >> 20), (u32)(bytes >> 20));
}

void DX12TexturePool::Term()
{
	if (!ready)
		return;
	book.Summary("DX12");
	DX12Context *context = DX12Context::Instance();
	while (all != nullptr)
	{
		DX12PooledTexture *next = all->all;
		if (all->srv != ~0u)
			context->Srv().Free(all->srv);
		if (all->rtv != ~0u)
			context->Rtv().Free(all->rtv);
		delete all;
		all = next;
	}
	book.Reset(1);
	ready = false;
}

bool DX12TexturePool::MakeResource(DX12PooledTexture *t, u32 f, u32 width, u32 height, u32 levels)
{
	DX12Context *context = DX12Context::Instance();
	const bool wide = f == (u32)TextureType::_8888 || f == (u32)TextureType::_8;
	t->width = width;
	t->height = height;
	t->levels = levels;
	t->format = poolFormats[f];
	// (typeless, for the unsigned integer views the kernels write through)
	DXGI_FORMAT format = t->writable && wide ? (f == (u32)TextureType::_8 ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS) : poolFormats[f];
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	if (f == TARGET || (t->writable && !wide))
		flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	else if (t->writable)
		flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (f == TARGET && targetStorage)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	t->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	if (!dx12_make_texture(context->GetDevice(), width, height, levels, format, flags, t->state, t->resource))
		return false;
	t->srv = context->Srv().Alloc();
	if (t->srv == ~0u)
	{
		WARN_LOG(RENDERER, "DX12: the bindless table is full");
		t->resource.reset();
		return false;
	}
	context->MakeSrv(t->resource, poolFormats[f], levels, t->srv);
	if (f == TARGET)
	{
		t->rtv = context->Rtv().Alloc();
		if (t->rtv == ~0u)
		{
			context->Srv().Free(t->srv);
			t->srv = ~0u;
			t->resource.reset();
			return false;
		}
		context->GetDevice()->CreateRenderTargetView(t->resource, nullptr, context->Rtv().Cpu(t->rtv));
	}
	return true;
}

DX12PooledTexture *DX12TexturePool::Make(int cls, bool late)
{
	const u32 f = TexPoolBook::Format(cls);
	const u32 width = TexPoolBook::Width(cls), height = TexPoolBook::Height(cls);
	DX12PooledTexture *t = new DX12PooledTexture();
	t->writable = writable[f];
	if (!MakeResource(t, f, width, height, TexPoolBook::Mips(cls) ? 1 + bitscanrev(std::max(width, height)) : 1))
	{
		WARN_LOG(RENDERER, "DX12: texture pool: a %ux%u %s texture could not be made", width, height, poolFormatNames[f]);
		delete t;
		return nullptr;
	}
	book.Made(t, cls, TexPoolBook::Bytes(cls, poolTexel[f]), late, poolFormatNames[f]);
	t->all = all;
	all = t;
	return t;
}

DX12PooledTexture *DX12TexturePool::MakeOwn(u32 f, u32 width, u32 height, u32 levels)
{
	if (!ready || f >= FORMATS || !usable[f] || width == 0 || height == 0)
		return nullptr;
	DX12PooledTexture *t = new DX12PooledTexture();
	t->writable = false;
	t->own = true;
	t->cls = ~0u;
	if (!MakeResource(t, f, width, height, levels))
	{
		WARN_LOG(RENDERER, "DX12: a %ux%u %s texture could not be made", width, height, poolFormatNames[f]);
		delete t;
		return nullptr;
	}
	t->bytes = 0;
	return t;
}

void DX12TexturePool::Give(DX12PooledTexture *texture)
{
	if (texture == nullptr)
		return;
	if (!texture->own)
	{
		book.Give(texture);
		return;
	}
	// one made for one texture: goes once the GPU is done with it
	DX12Context *context = DX12Context::Instance();
	if (texture->resource)
	{
		texture->resource->AddRef();
		context->Release(texture->resource.get());
		texture->resource.reset();
	}
	context->ReleaseSrv(texture->srv);
	if (texture->rtv != ~0u)
		context->Rtv().Free(texture->rtv);
	delete texture;
}

DX12PooledTexture *DX12TexturePool::TakeTarget(u32 width, u32 height)
{
	if (!ready)
		return nullptr;
	const int cls = TexPoolBook::Class(TARGET, width, height, false);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<DX12PooledTexture *>(e) : Make(cls, true);
}

DX12PooledTexture *DX12TexturePool::Take(TextureType type, u32 width, u32 height, bool mips)
{
	const u32 f = (u32)type;
	if (!ready || f >= 5 || !usable[f])
		return nullptr;
	const int cls = TexPoolBook::Class(f, width, height, mips);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<DX12PooledTexture *>(e) : Make(cls, true);
}

/* ---- The cache's textures ---- */

bool DX12Texture::TakeSpare(u32 width, u32 height)
{
	DX12PooledTexture *t = pool != nullptr ? pool->TakeTarget(width, height) : nullptr;
	if (t == nullptr)
		return false;
	ReleaseSpare();
	spare = t;
	return true;
}

void DX12Texture::ReleaseSpare()
{
	if (spare == nullptr)
		return;
	if (pool != nullptr)
		pool->Give(spare);
	spare = nullptr;
}

bool DX12Texture::TakePooled(u32 width, u32 height, bool mips)
{
	if (pool == nullptr)
		return false;
	if (pooled != nullptr && !pooled->own && (int)pooled->cls == TexPoolBook::Class((u32)tex_type, width, height, mips))
		return true;
	DX12PooledTexture *t = pool->Take(tex_type, width, height, mips);
	if (t == nullptr)
		return false;
	ReleasePooled();
	pooled = t;
	return true;
}

void DX12Texture::ReleasePooled()
{
	CancelDecode();
	uploadCpu = nullptr;
	if (pooled == nullptr)
		return;
	if (pool != nullptr)
		pool->Give(pooled);
	pooled = nullptr;
}

void DX12Texture::CancelDecode()
{
	if (decodeCount != 0 && decoder != nullptr)
		decoder->Cancel(decodeJob, decodeCount, decodeSerial);
	decodeCount = 0;
}

/* Made on the GPU (dx12_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
bool DX12Texture::GpuDecode(const texd_plan& plan)
{
	DX12Context *context = DX12Context::Instance();
	if (context->GetDevice() == nullptr || pool == nullptr)
		return false;
	if (plan.native)
	{
		/* A planar 16-bit texture: the PowerVR's texels are Direct3D's
		 * as they are, and its rows go from video memory to the texture */
		if (tex_type == TextureType::_8888 || tex_type == TextureType::_8 || !context->IsFormatSupported(tex_type)
				|| !TakePooled(plan.width, plan.height, false))
			return false;
		CancelDecode();
		DX12Upload upload;
		u32 pitch;
		if (!uploadRows(context, &vram[plan.src + plan.level[0].data], (plan.level[0].sr & 0xFFFF) * 2, plan.width * 2, plan.height, upload, pitch))
			return false;
		context->Transition(pooled->resource, pooled->state, D3D12_RESOURCE_STATE_COPY_DEST);
		copyToLevel(context, pooled, 0, plan.width, plan.height, pooled->format, upload, pitch);
		context->Transition(pooled->resource, pooled->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return true;
	}
	if (decoder == nullptr || !decoder->Takes(tex_type)
			|| !TakePooled(plan.width, plan.height, plan.mipmapped != 0) || !pooled->writable || pooled->levels < plan.levels)
		return false;
	CancelDecode();
	u32 base;
	u8 * const to = decoder->Begin(plan.pal + plan.pal_entries * 4, plan.levels, &base);
	if (to == nullptr)
		return false;
	memcpy(to, &vram[plan.src], plan.bytes);
	if (plan.pal_entries != 0)
		memcpy(to + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);

	const u32 d3d16 = plan.out != TEXD_OUT16 ? 0
			: TEXD_D3D16(tex_type == TextureType::_5551 ? TEXD_D3D_5551 : tex_type == TextureType::_4444 ? TEXD_D3D_4444 : TEXD_D3D_565);
	decodeJob = decoder->Next();
	decodeSerial = decoder->Serial();
	for (u32 i = 0; i < plan.levels; i++)
	{
		const texd_level& lv = plan.level[i];
		const u32 pc[6] = { base, lv.data, plan.pal, lv.wh, lv.sr, lv.mode | d3d16 };
		decoder->Add(pooled, lv.mip, pc);
	}
	decodeCount = plan.levels;
	return true;
}

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there. */
static std::vector<u16> turned;

static const u16 *turnBits(u8 *data, size_t texels, unsigned by, bool inPlace)
{
	u16 *src = (u16 *)data;
	u16 *dst = src;
	if (!inPlace)
	{
		if (turned.size() < texels)
			turned.resize(texels);
		dst = turned.data();
	}
	for (size_t i = 0; i < texels; i++)
		dst[i] = (u16)((src[i] >> by) | (src[i] << (16 - by)));
	return dst;
}

/* The cache converts straight into the upload ring where the texture's
 * rows are Direct3D's pitch as they are (a multiple of 256 bytes), and
 * nothing is turned after: the texture types whose bits are in Direct3D's
 * order (upload memory is write-combined: not to be read back). */
void *DX12Texture::UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	uploadCpu = nullptr;
	DX12Context *context = DX12Context::Instance();
	if (context->GetDevice() == nullptr || pool == nullptr || mipmaps || (width * pixel) % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0
			|| bytes != (size_t)width * height * pixel || tex_type == TextureType::_5551 || tex_type == TextureType::_4444
			|| TexPoolBook::Class((u32)tex_type, width, height, false) < 0)
		return nullptr;
	if (!context->Upload((u32)bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, uploadPiece))
		return nullptr;
	uploadCpu = uploadPiece.cpu;
	uploadPitch = width * pixel;
	return uploadPiece.cpu;
}

void DX12Texture::UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	DX12Context *context = DX12Context::Instance();
	if (context->GetDevice() == nullptr || pool == nullptr)
		return;
	// what the decoder was to make of it this frame is this now
	CancelDecode();
	u8 * const inRing = uploadCpu;
	uploadCpu = nullptr;

	u32 bpp = 2;
	unsigned turn = 0;
	switch (tex_type)
	{
	case TextureType::_5551:
		turn = 1;
		break;
	case TextureType::_565:
		break;
	case TextureType::_4444:
		turn = 4;
		break;
	case TextureType::_8888:
		bpp = 4;
		break;
	case TextureType::_8:
		bpp = 1;
		break;
	default:
		WARN_LOG(RENDERER, "Unsupported texture type %d", (int)tex_type);
		return;
	}
	u32 levels = 1;
	if (mipmapsIncluded)
		for (int dim = width / 2; dim != 0; dim /= 2)
			levels++;
	/* (a mipmapped texture whose levels are not given - a custom one - is
	 * sampled from its largest level: Direct3D 12 makes no levels itself) */

	/* One of the pool's, made before the game; one of its own for a shape
	 * the pool has not (a custom texture) */
	if (!TakePooled(width, height, mipmapsIncluded))
	{
		if (pooled == nullptr || pooled->own == false || pooled->width != (u32)width || pooled->height != (u32)height
				|| pooled->levels != levels || pooled->format != poolFormats[(int)tex_type])
		{
			ReleasePooled();
			pooled = pool->MakeOwn((u32)tex_type, width, height, levels);
			if (pooled == nullptr)
				return;
		}
	}
	if (turn != 0 && data != inRing)
	{
		size_t texels = (size_t)width * height;
		if (mipmapsIncluded)
			for (int dim = width / 2; dim != 0; dim /= 2)
				texels += (size_t)dim * dim;
		/* (in the texture cache's own buffer, which nothing reads after
		 * this but a texture dump, which wants it as it is) */
		data = (u8 *)turnBits(data, texels, turn, !settings.rend.DumpTextures);
	}

	context->Transition(pooled->resource, pooled->state, D3D12_RESOURCE_STATE_COPY_DEST);
	if (data == inRing && inRing != nullptr)
	{
		// converted into the ring already (UploadMemory())
		copyToLevel(context, pooled, 0, width, height, pooled->format, uploadPiece, uploadPitch);
	}
	else if (mipmapsIncluded)
	{
		// from the smallest level, 1x1, to the largest
		for (u32 i = 0; i < levels; i++)
		{
			const u32 dim = 1 << i;
			DX12Upload upload;
			u32 pitch;
			if (uploadRows(context, data, dim * bpp, dim * bpp, dim, upload, pitch))
				copyToLevel(context, pooled, levels - i - 1, dim, dim, pooled->format, upload, pitch);
			data += (size_t)dim * dim * bpp;
		}
	}
	else
	{
		DX12Upload upload;
		u32 pitch;
		if (uploadRows(context, data, width * bpp, width * bpp, height, upload, pitch))
			copyToLevel(context, pooled, 0, width, height, pooled->format, upload, pitch);
	}
	context->Transition(pooled->resource, pooled->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool DX12Texture::Force32BitTexture(TextureType type) const
{
	return !DX12Context::Instance()->IsFormatSupported(type);
}

bool DX12Texture::Delete()
{
	if (!BaseTextureCacheData::Delete())
		return false;
	ReleasePooled();
	ReleaseSpare();
	return true;
}

/* ---- The samplers ---- */

void dx12_make_samplers()
{
	DX12Context *context = DX12Context::Instance();
	ID3D12Device *device = context->GetDevice();
	if (device == nullptr)
		return;
	for (u32 i = 0; i < DX12_SAMPLER_TABLE_SIZE; i++)
	{
		// the bits back from the index: MipMapD, then FilterMode, ClampV, ClampU, FlipV, FlipU
		TSP tsp = {};
		tsp.full = ((i & 0xF) << 8) | (((i >> 4) & 0x3F) << 13);
		D3D12_SAMPLER_DESC desc{};
		if (tsp.FilterMode == 0)
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		else if (settings.rend.AnisotropicFiltering > 1)
			desc.Filter = D3D12_FILTER_ANISOTROPIC;
		else
			// (the mipmap level nearest, as the Vulkan renderer's: trilinear filtering is two passes)
			desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		desc.AddressU = tsp.ClampU ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : tsp.FlipU ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.AddressV = tsp.ClampV ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : tsp.FlipV ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		desc.MipLODBias = D_Adjust_LoD_Bias[tsp.MipMapD];
		desc.MaxAnisotropy = std::min(16, std::max(1, settings.rend.AnisotropicFiltering));
		desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		desc.MinLOD = 0.f;
		desc.MaxLOD = D3D12_FLOAT32_MAX;
		device->CreateSampler(&desc, context->Samplers().Cpu(i));
	}
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* @width by @height from (@x, @y) of @texture (in @state) into @rgba,
 * through the context's readback buffer: what is recorded is submitted,
 * and waited for */
static bool readBack(ID3D12Resource *texture, D3D12_RESOURCE_STATES& state, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	DX12Context *context = DX12Context::Instance();
	const u32 pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	ID3D12Resource *buffer = context->ReadbackBuffer((u64)pitch * height);
	if (buffer == nullptr || texture == nullptr)
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	const D3D12_RESOURCE_STATES was = state;
	context->Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = buffer;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	dst.PlacedFootprint.Footprint.Width = width;
	dst.PlacedFootprint.Footprint.Height = height;
	dst.PlacedFootprint.Footprint.Depth = 1;
	dst.PlacedFootprint.Footprint.RowPitch = pitch;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = texture;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box = { x, y, 0, x + width, y + height, 1 };
	context->Cmd()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
	context->Transition(texture, state, was);
	const u64 done = context->Submit();
	context->WaitFor(done);
	const D3D12_RANGE range = { 0, (SIZE_T)pitch * height };
	void *p = nullptr;
	if (FAILED(buffer->Map(0, &range, &p)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	for (u32 l = 0; l < height; l++)
		memcpy(rgba + (size_t)l * width * 4, (const u8 *)p + (size_t)l * pitch, (size_t)width * 4);
	const D3D12_RANGE none = { 0, 0 };
	buffer->Unmap(0, &none);
	return true;
}

bool dx12_read_picture(ID3D12Resource *texture, D3D12_RESOURCE_STATES& state, u32 width, u32 height, u8 *rgba)
{
	return readBack(texture, state, 0, 0, width, height, rgba);
}

/* The picture as the console would have it: w by h, R G B A. */
static void dx12_rtt_read(const RttWatch *watch, u8 *rgba)
{
	DX12Context *context = DX12Context::Instance();
	DX12Texture *texture = (DX12Texture *)watch->tex;
	DX12PooledTexture *image = texture->Image();
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;

	if (context->GetDevice() == nullptr || image == nullptr || !image->resource)
	{
		memset(rgba, 0, (size_t)w * h * 4);
		return;
	}
	if (scale == 1)
	{
		readBack(image->resource, image->state, x0, y0, w, h, rgba);
		return;
	}
	/* Made the console's size on the card, filtered, and read at that: the
	 * clip's part of the texture drawn onto one of the pool's pictures. */
	u32 pw = 8, ph = 8;
	while (pw < w)
		pw *= 2;
	while (ph < h)
		ph *= 2;
	DX12TexturePool *pool = DX12Texture::Pool();
	DX12PooledTexture *small = pool != nullptr ? pool->TakeTarget(pw, ph) : nullptr;
	if (small == nullptr)
	{
		WARN_LOG(RENDERER, "Render to texture read back: no %ux%u picture", pw, ph);
		memset(rgba, 0, (size_t)w * h * 4);
		return;
	}
	context->Transition(small->resource, small->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	context->SetTargets(small->rtv, ~0u);
	context->SetViewport((float)w, (float)h);
	const D3D12_RECT scissor = { 0, 0, (LONG)w, (LONG)h };
	context->SetScissor(scissor);
	DX12Quad::Draw(image->srv, true, -1.f, -1.f, 2.f, 2.f,
			(float)x0 / image->width, (float)y0 / image->height,
			(float)(x0 + w * scale) / image->width, (float)(y0 + h * scale) / image->height);
	context->SetTargets(~0u, ~0u);
	readBack(small->resource, small->state, 0, 0, w, h, rgba);
	pool->Give(small);
}

/* The texture cache's, never this module's to delete. */
static void dx12_rtt_release(uintptr_t)
{
}

static const RttWatchBackend dx12_rtt_backend = { dx12_rtt_read, dx12_rtt_release };

/* What the render covers of video memory: the clip's pixels, from (x0, y0)
 * of the frame at addr, which moves to the first of them. (As the Vulkan
 * renderers' rtt_read.cpp has it.) */
static void covered(u32& addr, u32 x0, u32 y0, u32& width, u32 height, u32& stride, u32& bytes)
{
	stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = (x0 + width) * 2;
	else if ((x0 + width) * 2 > stride)
		// Happens for Virtua Tennis
		width = x0 * 2 >= stride ? 0 : stride / 2 - x0;
	addr += y0 * stride + x0 * 2;
	bytes = width == 0 || height == 0 ? 0 : stride * (height - 1) + width * 2;
}

void dx12_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(addr, x0, y0, width, height, stride, bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx12_rtt_watch(DX12Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	RttWatch watch = {};

	covered(addr, x0, y0, width, height, watch.stride, watch.bytes);
	watch.addr = addr;
	watch.w = width;
	watch.h = height;
	watch.x0 = x0;
	watch.y0 = y0;
	watch.packmode = FB_W_CTRL.fb_packmode;
	watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
	watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
	watch.dither = FB_W_CTRL.fb_dither;
	watch.tex = (uintptr_t)texture;
	watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
	if (watch.bytes != 0)
		rtt_watch_add(&watch, &dx12_rtt_backend);
}

void dx12_rtt_term(void)
{
	rtt_watch_term();
	turned.clear();
	turned.shrink_to_fit();
}
