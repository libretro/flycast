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
#include "rend/rtt_watch.h"
#include "hw/pvr/pvr_regs.h"
#include <algorithm>
#include <vector>

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there. */
static std::vector<u16> turned;

static const u16 *turnBits(const u8 *data, size_t texels, unsigned by)
{
	if (turned.size() < texels)
		turned.resize(texels);
	const u16 *src = (const u16 *)data;
	u16 *dst = turned.data();
	for (size_t i = 0; i < texels; i++)
		dst[i] = (u16)((src[i] >> by) | (src[i] << (16 - by)));
	return turned.data();
}

void DX11Texture::UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	DX11Context *context = DX11Context::Instance();
	ID3D11Device *device = context->GetDevice();
	ID3D11DeviceContext *deviceContext = context->GetDeviceContext();
	if (device == nullptr)
		return;

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

	if (texture)
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
	if (!texture)
	{
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture.put());
		if (FAILED(hr))
		{
			ERROR_LOG(RENDERER, "Texture creation failed (%x): type %d, %d x %d, %d levels", (unsigned)hr, (int)tex_type, width, height, levels);
			texture.reset();
			return;
		}
		hr = device->CreateShaderResourceView(texture, nullptr, textureView.put());
		if (FAILED(hr))
			ERROR_LOG(RENDERER, "Texture view creation failed (%x)", (unsigned)hr);
	}

	if (turn != 0)
	{
		size_t texels = (size_t)width * height;
		if (mipmapsIncluded)
			for (int dim = width / 2; dim != 0; dim /= 2)
				texels += (size_t)dim * dim;
		data = (u8 *)turnBits(data, texels, turn);
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
	textureView.reset();
	texture.reset();
	spareTarget.reset();
	spareView.reset();
	spare.reset();
	return true;
}

ID3D11SamplerState *DX11Samplers::GetSampler(TSP tsp)
{
	const u32 key = tsp.full & TSP_Mask;
	auto it = samplers.find(key);
	if (it != samplers.end())
		return it->second;

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

	ComPtr<ID3D11SamplerState>& sampler = samplers[key];
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
	deviceContext->OMSetRenderTargets(1, smallTarget.addr(), nullptr);
	const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
	deviceContext->RSSetViewports(1, &vp);
	context->GetQuad().Draw(texture->textureView, context->GetSamplers().GetLinearSampler(),
			-1.f, -1.f, 2.f, 2.f,
			(float)x0 / src.Width, (float)y0 / src.Height,
			(float)(x0 + w * scale) / src.Width, (float)(y0 + h * scale) / src.Height);
	ID3D11RenderTargetView *none = nullptr;
	deviceContext->OMSetRenderTargets(1, &none, nullptr);
	readBack(small, 0, 0, w, h, rgba);
	/* (the renderer sets what it draws with when it next draws: this is
	 * never in the middle of that) */
	context->StateChanged();
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
