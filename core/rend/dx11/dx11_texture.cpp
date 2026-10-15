/*
	Copyright 2021 flyinghead

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
#include "dx11_texture.h"
#include "dx11context_lr.h"
#include "dx11_texdecoder.h"
#include "rend/rtt_watch.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include <algorithm>
#include <vector>

DX11TexDecoder *DX11Texture::decoder;
DX11TexturePool *DX11Texture::pool;

/* ---- The texture pool ---- */

// by TextureType: 565, 5551, 4444, 8888, 8; and the pictures rendered to
static const DXGI_FORMAT poolFormats[DX11TexturePool::FORMATS] = {
	DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM,
	DXGI_FORMAT_R8G8B8A8_UNORM
};
static const char * const poolFormatNames[DX11TexturePool::FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "8888 rendered to" };
static const u32 poolTexel[DX11TexturePool::FORMATS] = { 2, 2, 2, 4, 1, 4 };

void DX11TexturePool::Init(ID3D11Device *device, const VramBudget& budget, const DX11TexDecoder *decoder)
{
	if (ready)
		return;
	this->device = device;
	book.Reset(1);
	// how much games use them, in eighths of what they use 565
	static const u32 use[FORMATS] = { 8, 4, 4, 4, 2, 0 };
	static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, TexPoolBook::TARGETS };
	u32 have[FORMATS];
	for (int f = 0; f < FORMATS; f++)
	{
		usable[f] = f == TARGET || DX11Context::Instance()->IsFormatSupported((TextureType)f);
		writable[f] = f != TARGET && usable[f] && decoder != nullptr && decoder->Takes((TextureType)f);
		have[f] = usable[f] ? poolTexel[f] : 0;
	}
	u32 counts[TexPoolBook::CLASSES];
	const u64 bytes = TexPoolBook::Plan(budget, have, use, kind, FORMATS, counts);
	ready = true;
	for (int c = 0; c < TexPoolBook::CLASSES; c++)
		for (u32 i = 0; i < counts[c]; i++)
		{
			DX11PooledTexture *texture = Make(c, false);
			if (texture == nullptr)
				break;
			book.Give(texture);
		}
	NOTICE_LOG(RENDERER, "DX11: texture pool of %u textures, %u MB (planned %u MB)", book.made, (u32)(book.bytesMade >> 20), (u32)(bytes >> 20));
}

void DX11TexturePool::Term()
{
	if (!ready)
		return;
	book.Summary("DX11");
	while (all != nullptr)
	{
		DX11PooledTexture *next = all->all;
		delete all;
		all = next;
	}
	book.Reset(1);
	ready = false;
	device = nullptr;
}

DX11PooledTexture *DX11TexturePool::Make(int cls, bool late)
{
	const u32 f = TexPoolBook::Format(cls);
	const u32 width = TexPoolBook::Width(cls), height = TexPoolBook::Height(cls);
	const bool wide = f == (u32)TextureType::_8888 || f == (u32)TextureType::_8;
	DX11PooledTexture *t = new DX11PooledTexture();
	t->levels = TexPoolBook::Mips(cls) ? 1 + bitscanrev(std::max(width, height)) : 1;
	t->writable = writable[f];

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = t->levels;
	desc.ArraySize = 1;
	// (typeless, for the unsigned integer views the kernels write through)
	desc.Format = t->writable && wide ? (f == (u32)TextureType::_8 ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS) : poolFormats[f];
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t->writable ? (wide ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET) : 0);
	if (f == TARGET)
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, t->texture.put()));
	if (ok)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = poolFormats[f];
		view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		view.Texture2D.MostDetailedMip = 0;
		view.Texture2D.MipLevels = t->levels;
		ok = SUCCEEDED(device->CreateShaderResourceView(t->texture, &view, t->view.put()));
	}
	if (ok && f == TARGET)
		ok = SUCCEEDED(device->CreateRenderTargetView(t->texture, nullptr, t->targets[0].put()));
	for (u32 level = 0; ok && t->writable && level < t->levels; level++)
		if (wide)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = f == (u32)TextureType::_8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = level;
			ok = SUCCEEDED(device->CreateUnorderedAccessView(t->texture, &uav, t->uavs[level].put()));
		}
		else
		{
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = poolFormats[f];
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			rtv.Texture2D.MipSlice = level;
			ok = SUCCEEDED(device->CreateRenderTargetView(t->texture, &rtv, t->targets[level].put()));
		}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX11: texture pool: a %ux%u %s texture could not be made", width, height, poolFormatNames[f]);
		delete t;
		return nullptr;
	}
	book.Made(t, cls, TexPoolBook::Bytes(cls, poolTexel[f]), late, poolFormatNames[f]);
	t->all = all;
	all = t;
	return t;
}

DX11PooledTexture *DX11TexturePool::TakeTarget(u32 width, u32 height)
{
	if (!ready)
		return nullptr;
	const int cls = TexPoolBook::Class(TARGET, width, height, false);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<DX11PooledTexture *>(e) : Make(cls, true);
}

bool DX11Texture::TakeSpare(u32 width, u32 height)
{
	DX11PooledTexture *t = pool != nullptr ? pool->TakeTarget(width, height) : nullptr;
	if (t == nullptr)
		return false;
	ReleaseSpare();
	sparePooled = t;
	spare = t->texture;
	spareView = t->view;
	spareTarget = t->targets[0];
	return true;
}

void DX11Texture::ReleaseSpare()
{
	if (sparePooled == nullptr)
		return;
	spareTarget.reset();
	spareView.reset();
	spare.reset();
	if (pool != nullptr)
		pool->Give(sparePooled);
	sparePooled = nullptr;
}

DX11PooledTexture *DX11TexturePool::Take(TextureType type, u32 width, u32 height, bool mips)
{
	const u32 f = (u32)type;
	if (!ready || f >= 5 || !usable[f])
		return nullptr;
	const int cls = TexPoolBook::Class(f, width, height, mips);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<DX11PooledTexture *>(e) : Make(cls, true);
}

bool DX11Texture::TakePooled(u32 width, u32 height, bool mips)
{
	if (pool == nullptr)
		return false;
	if (pooled != nullptr && (int)pooled->cls == TexPoolBook::Class((u32)tex_type, width, height, mips))
		return true;
	DX11PooledTexture *t = pool->Take(tex_type, width, height, mips);
	if (t == nullptr)
		return false;
	ReleasePooled();
	pooled = t;
	texture = t->texture;
	textureView = t->view;
	return true;
}

void DX11Texture::ReleasePooled()
{
	CancelDecode();
	if (pooled == nullptr)
		return;
	textureView.reset();
	texture.reset();
	if (pool != nullptr)
		pool->Give(pooled);
	pooled = nullptr;
}

void DX11Texture::CancelDecode()
{
	if (decodeCount != 0 && decoder != nullptr)
		decoder->Cancel(decodeJob, decodeCount, decodeSerial);
	decodeCount = 0;
}

/* Made on the GPU (dx11_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
bool DX11Texture::GpuDecode(const texd_plan& plan)
{
	DX11Context *context = DX11Context::Instance();
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
		context->GetDeviceContext()->UpdateSubresource(texture, 0, nullptr, &vram[plan.src + plan.level[0].data],
				(plan.level[0].sr & 0xFFFF) * 2, 0);
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
		if (plan.out == TEXD_OUT16)
			decoder->Add(nullptr, pooled->targets[lv.mip], pc);
		else
			decoder->Add(pooled->uavs[lv.mip], nullptr, pc);
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

void DX11Texture::UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	DX11Context *context = DX11Context::Instance();
	ID3D11Device *device = context->GetDevice();
	ID3D11DeviceContext *deviceContext = context->GetDeviceContext();
	if (device == nullptr)
		return;
	// what the decoder was to make of it this frame is this now
	CancelDecode();

	DXGI_FORMAT format;
	u32 bpp = 2;
	unsigned turn = 0;
	switch (tex_type)
	{
	case TextureType::_5551:
		format = DXGI_FORMAT_B5G5R5A1_UNORM;
		turn = 1;
		break;
	case TextureType::_565:
		format = DXGI_FORMAT_B5G6R5_UNORM;
		break;
	case TextureType::_4444:
		format = DXGI_FORMAT_B4G4R4A4_UNORM;
		turn = 4;
		break;
	case TextureType::_8888:
		format = DXGI_FORMAT_R8G8B8A8_UNORM;
		bpp = 4;
		break;
	case TextureType::_8:
		format = DXGI_FORMAT_R8_UNORM;
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
	// smaller levels made on the card, where it can make them
	const bool generate = mipmapped && !mipmapsIncluded && context->CanGenerateMips(tex_type);
	if (generate)
		for (int dim = std::max(width, height) / 2; dim != 0; dim /= 2)
			levels++;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (generate ? D3D11_BIND_RENDER_TARGET : 0);
	desc.MiscFlags = generate ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

	/* One of the pool's, made before the game (but one whose smaller levels
	 * the card makes) */
	const bool fromPool = !generate && TakePooled(width, height, mipmapsIncluded);
	if (!fromPool)
		ReleasePooled();
	if (texture && !fromPool)
	{
		/* Kept if it is what the new one would be. (A render to this
		 * texture leaves one that can be rendered to, which is not.) */
		D3D11_TEXTURE2D_DESC cur;
		texture->GetDesc(&cur);
		if (cur.Width != desc.Width || cur.Height != desc.Height || cur.Format != desc.Format
				|| cur.MipLevels != desc.MipLevels || cur.BindFlags != desc.BindFlags || cur.MiscFlags != desc.MiscFlags)
		{
			textureView.reset();
			texture.reset();
		}
	}
	if (turn != 0)
	{
		size_t texels = (size_t)width * height;
		if (mipmapsIncluded)
			for (int dim = width / 2; dim != 0; dim /= 2)
				texels += (size_t)dim * dim;
		/* (in the texture cache's own buffer, which nothing reads after
		 * this but a texture dump, which wants it as it is) */
		data = (u8 *)turnBits(data, texels, turn, !settings.rend.DumpTextures);
	}
	/* A texture made anew is made with its pixels, which saves copying
	 * them again after (one whose smaller levels the card makes is made,
	 * and then filled) */
	D3D11_SUBRESOURCE_DATA initial[16];
	const bool withData = !generate && levels <= 16;
	if (withData)
	{
		const u8 *p = data;
		for (u32 i = 0; i < levels; i++)
		{
			// (the levels included are from the smallest, 1x1, to the largest)
			const u32 level = mipmapsIncluded ? levels - i - 1 : 0;
			const u32 w = mipmapsIncluded ? 1u << i : (u32)width;
			const u32 h = mipmapsIncluded ? 1u << i : (u32)height;
			initial[level].pSysMem = p;
			initial[level].SysMemPitch = w * bpp;
			initial[level].SysMemSlicePitch = w * h * bpp;
			p += (size_t)w * h * bpp;
		}
	}
	if (!texture)
	{
		HRESULT hr = device->CreateTexture2D(&desc, withData ? initial : nullptr, texture.put());
		if (FAILED(hr))
		{
			ERROR_LOG(RENDERER, "Texture creation failed (%x): type %d, %d x %d, %d levels", (unsigned)hr, (int)tex_type, width, height, levels);
			texture.reset();
			return;
		}
		hr = device->CreateShaderResourceView(texture, nullptr, textureView.put());
		if (FAILED(hr))
			ERROR_LOG(RENDERER, "Texture view creation failed (%x)", (unsigned)hr);
		if (withData)
			return;
	}

	if (mipmapsIncluded)
	{
		// from the smallest level, 1x1, to the largest
		for (u32 i = 0; i < levels; i++)
		{
			const u32 dim = 1 << i;
			deviceContext->UpdateSubresource(texture, levels - i - 1, nullptr, data, dim * bpp, dim * dim * bpp);
			data += (size_t)dim * dim * bpp;
		}
	}
	else
	{
		deviceContext->UpdateSubresource(texture, 0, nullptr, data, width * bpp, width * height * bpp);
		if (generate && textureView)
			deviceContext->GenerateMips(textureView);
	}
}

bool DX11Texture::Force32BitTexture(TextureType type) const
{
	return !DX11Context::Instance()->IsFormatSupported(type);
}

bool DX11Texture::Delete()
{
	if (!BaseTextureCacheData::Delete())
		return false;
	ReleasePooled();
	ReleaseSpare();
	textureView.reset();
	texture.reset();
	spareTarget.reset();
	spareView.reset();
	spare.reset();
	return true;
}

ID3D11SamplerState *DX11Samplers::MakeSampler(TSP tsp)
{
	const u32 key = tsp.full & TSP_Mask;

	D3D11_SAMPLER_DESC desc{};
	if (tsp.FilterMode == 0)
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	else if (settings.rend.AnisotropicFiltering > 1)
		desc.Filter = D3D11_FILTER_ANISOTROPIC;
	else
		// (the mipmap level nearest, as the Vulkan renderer's: trilinear filtering is two passes)
		desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	desc.AddressU = tsp.ClampU ? D3D11_TEXTURE_ADDRESS_CLAMP : tsp.FlipU ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressV = tsp.ClampV ? D3D11_TEXTURE_ADDRESS_CLAMP : tsp.FlipV ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
	desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MipLODBias = D_Adjust_LoD_Bias[tsp.MipMapD];
	desc.MaxAnisotropy = std::min(16, std::max(1, settings.rend.AnisotropicFiltering));
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MinLOD = 0.f;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	ComPtr<ID3D11SamplerState>& sampler = samplers[key >> 8];
	ID3D11Device *device = DX11Context::Instance()->GetDevice();
	if (device == nullptr || FAILED(device->CreateSamplerState(&desc, sampler.put())))
		WARN_LOG(RENDERER, "Sampler creation failed");
	return sampler;
}

ID3D11SamplerState *DX11Samplers::GetPointSampler()
{
	TSP tsp = {};
	tsp.FilterMode = 0;
	tsp.ClampU = 1;
	tsp.ClampV = 1;
	return GetSampler(tsp);
}

ID3D11SamplerState *DX11Samplers::GetLinearSampler()
{
	if (!linear)
	{
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxAnisotropy = 1;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		ID3D11Device *device = DX11Context::Instance()->GetDevice();
		if (device == nullptr || FAILED(device->CreateSamplerState(&desc, linear.put())))
			WARN_LOG(RENDERER, "Sampler creation failed");
	}
	return linear;
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* A picture that can be read from this side, kept from one read to the
 * next: a read can come at any time, the middle of a frame included, and
 * what is made for it is used again. And the target a picture made finer
 * than the console's is brought down to the console's size in. */
static ComPtr<ID3D11Texture2D> staging;
static ComPtr<ID3D11Texture2D> small;
static ComPtr<ID3D11RenderTargetView> smallTarget;

static bool makeStaging(ID3D11Device *device, u32 width, u32 height)
{
	if (staging)
	{
		D3D11_TEXTURE2D_DESC desc;
		staging->GetDesc(&desc);
		if (desc.Width >= width && desc.Height >= height)
			return true;
		width = std::max(width, desc.Width);
		height = std::max(height, desc.Height);
		staging.reset();
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())))
	{
		WARN_LOG(RENDERER, "Staging texture creation failed (%d x %d)", width, height);
		staging.reset();
		return false;
	}
	return true;
}

/* @width by @height from (@x, @y) of @texture into @rgba, through the
 * staging picture */
static bool readBack(ID3D11Texture2D *texture, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	DX11Context *context = DX11Context::Instance();
	ID3D11DeviceContext *deviceContext = context->GetDeviceContext();

	if (!context->CanReadBack() || !makeStaging(context->GetDevice(), width, height))
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	const D3D11_BOX box = { x, y, 0, x + width, y + height, 1 };
	deviceContext->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, &box);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(deviceContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	for (u32 l = 0; l < height; l++)
		memcpy(rgba + (size_t)l * width * 4, (const u8 *)mapped.pData + (size_t)l * mapped.RowPitch, (size_t)width * 4);
	deviceContext->Unmap(staging, 0);
	return true;
}

bool dx11_read_picture(ID3D11Texture2D *texture, u32 width, u32 height, u8 *rgba)
{
	DX11Lock lock;
	return readBack(texture, 0, 0, width, height, rgba);
}

/* The picture as the console would have it: w by h, R G B A. */
static void dx11_rtt_read(const RttWatch *watch, u8 *rgba)
{
	DX11Lock lock;
	DX11Context *context = DX11Context::Instance();
	ID3D11Device *device = context->GetDevice();
	ID3D11DeviceContext *deviceContext = context->GetDeviceContext();
	DX11Texture *texture = (DX11Texture *)watch->tex;
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;

	if (device == nullptr || !texture->texture)
	{
		memset(rgba, 0, (size_t)w * h * 4);
		return;
	}
	if (scale == 1)
	{
		readBack(texture->texture, x0, y0, w, h, rgba);
		return;
	}
	/* Made the console's size on the card, filtered, and read at that: the
	 * clip's part of the texture drawn onto a picture of w by h. */
	if (small)
	{
		D3D11_TEXTURE2D_DESC desc;
		small->GetDesc(&desc);
		if (desc.Width < w || desc.Height < h)
		{
			smallTarget.reset();
			small.reset();
		}
	}
	if (!small)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = std::max(w, 640u);
		desc.Height = std::max(h, 480u);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, small.put()))
				|| FAILED(device->CreateRenderTargetView(small, nullptr, smallTarget.put())))
		{
			WARN_LOG(RENDERER, "Render to texture read back: target creation failed");
			small.reset();
			memset(rgba, 0, (size_t)w * h * 4);
			return;
		}
	}
	D3D11_TEXTURE2D_DESC src;
	texture->texture->GetDesc(&src);
	context->SetTargets(smallTarget, nullptr);
	context->SetViewport((float)w, (float)h);
	context->GetQuad().Draw(texture->textureView, context->GetSamplers().GetLinearSampler(),
			-1.f, -1.f, 2.f, 2.f,
			(float)x0 / src.Width, (float)y0 / src.Height,
			(float)(x0 + w * scale) / src.Width, (float)(y0 + h * scale) / src.Height);
	readBack(small, 0, 0, w, h, rgba);
}

/* The texture cache's, never this module's to delete. */
static void dx11_rtt_release(uintptr_t)
{
}

static const RttWatchBackend dx11_rtt_backend = { dx11_rtt_read, dx11_rtt_release };

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

void dx11_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(addr, x0, y0, width, height, stride, bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx11_rtt_watch(DX11Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
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
		rtt_watch_add(&watch, &dx11_rtt_backend);
}

void dx11_rtt_term(void)
{
	rtt_watch_term();
	smallTarget.reset();
	small.reset();
	staging.reset();
	turned.clear();
	turned.shrink_to_fit();
}
