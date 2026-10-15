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
#include "dx9_texture.h"
#include "dx9context_lr.h"
#include "dx9_texdecoder.h"
#include "rend/rtt_watch.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include <algorithm>

DX9TexDecoder *DX9Texture::decoder;
DX9TexturePool *DX9Texture::pool;

/* ---- The texture pool ---- */

static const char * const poolNames[DX9TexturePool::FORMATS] = {
	"565", "5551", "4444", "8888", "8-bit", "565 drawn", "5551 drawn", "4444 drawn", "8888 drawn", "8-bit drawn", "8888 rendered to"
};

static TextureType poolType(u32 f)
{
	return (TextureType)(f >= DX9TexturePool::TARGET ? (u32)TextureType::_8888 : f >= DX9TexturePool::DRAWN ? f - DX9TexturePool::DRAWN : f);
}

static u32 poolTexel(u32 f)
{
	const TextureType type = poolType(f);
	return type == TextureType::_8888 ? 4 : type == TextureType::_8 ? 1 : 2;
}

void DX9TexturePool::Init(DX9Context *context, const DX9TexDecoder *decoder)
{
	const bool drawn = decoder != nullptr && decoder->Ready();
	if (ready)
		return;
	this->context = context;
	book.Reset(1);
	/* how much games use them, in eighths of what they use 565: the ones
	 * the processor writes fewer when the pixel shader draws most */
	static const u32 written[FORMATS] = { 8, 4, 4, 4, 2, 0, 0, 0, 0, 0, 0 };
	static const u32 shared[FORMATS] = { 4, 2, 2, 2, 1, 8, 4, 4, 4, 2, 0 };
	static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TexPoolBook::TARGETS };
	u32 have[FORMATS];
	for (u32 f = 0; f < FORMATS; f++)
	{
		const TextureType type = poolType(f);
		usable[f] = f == TARGET || (context->IsFormatSupported(type) && (f < DRAWN || (drawn && decoder->Takes(type))));
		have[f] = usable[f] ? poolTexel(f) : 0;
	}
	u32 counts[TexPoolBook::CLASSES];
	const u64 bytes = TexPoolBook::Plan(context->GetVramBudget(), have, drawn ? shared : written, kind, FORMATS, counts);
	ready = true;
	for (int c = 0; c < TexPoolBook::CLASSES; c++)
		for (u32 i = 0; i < counts[c]; i++)
		{
			DX9PooledTexture *t = Make(c, false);
			if (t == nullptr)
				break;
			book.Give(t);
		}
	NOTICE_LOG(RENDERER, "DX9: texture pool of %u textures, %u MB (planned %u MB)", book.made, (u32)(book.bytesMade >> 20), (u32)(bytes >> 20));
}

void DX9TexturePool::Term()
{
	if (!ready)
		return;
	book.Summary("DX9");
	while (all != nullptr)
	{
		DX9PooledTexture *next = all->all;
		delete all;
		all = next;
	}
	book.Reset(1);
	ready = false;
	context = nullptr;
}

DX9PooledTexture *DX9TexturePool::Make(int cls, bool late)
{
	IDirect3DDevice9 *device = context->GetDevice();
	const u32 f = TexPoolBook::Format(cls);
	const u32 width = TexPoolBook::Width(cls), height = TexPoolBook::Height(cls);
	if (device == nullptr)
		return nullptr;
	DX9PooledTexture *t = new DX9PooledTexture();
	t->levels = TexPoolBook::Mips(cls) ? 1 + bitscanrev(std::max(width, height)) : 1;
	const DWORD usage = f >= DRAWN ? D3DUSAGE_RENDERTARGET : context->WriteUsage();
	// (the managed pool where the device has one and the processor writes the texture without dynamic ones)
	const D3DPOOL where = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) != 0 || context->IsEx() ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
	bool ok = SUCCEEDED(device->CreateTexture(width, height, t->levels, usage, DX9Context::TextureFormat(poolType(f)), where, t->texture.put(), nullptr));
	for (u32 level = 0; ok && f >= DRAWN && level < t->levels; level++)
		ok = SUCCEEDED(t->texture->GetSurfaceLevel(level, t->surfaces[level].put()));
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX9: texture pool: a %ux%u %s texture could not be made", width, height, poolNames[f]);
		delete t;
		return nullptr;
	}
	book.Made(t, cls, TexPoolBook::Bytes(cls, poolTexel(f)), late, poolNames[f]);
	t->all = all;
	all = t;
	return t;
}

DX9PooledTexture *DX9TexturePool::Take(u32 f, u32 width, u32 height, bool mips)
{
	if (!ready || f >= FORMATS || !usable[f])
		return nullptr;
	const int cls = TexPoolBook::Class(f, width, height, mips);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<DX9PooledTexture *>(e) : Make(cls, true);
}

bool DX9Texture::TakePooled(u32 f, u32 width, u32 height, bool mips)
{
	if (pool == nullptr)
		return false;
	if (pooled != nullptr && (int)pooled->cls == TexPoolBook::Class(f, width, height, mips))
		return true;
	DX9PooledTexture *t = pool->Take(f, width, height, mips);
	if (t == nullptr)
		return false;
	ReleasePooled();
	if (texture)
		DX9Context::Instance()->Recycle(texture);
	pooled = t;
	texture = t->texture;
	return true;
}

void DX9Texture::ReleasePooled()
{
	CancelDecode();
	if (pooled == nullptr)
		return;
	if (lockedBits != nullptr && texture)
		texture->UnlockRect(0);
	lockedBits = nullptr;
	texture.reset();
	if (pool != nullptr)
		pool->Give(pooled);
	pooled = nullptr;
}

void DX9Texture::CancelDecode()
{
	if (decodeCount != 0 && decoder != nullptr)
		decoder->Cancel(decodeJob, decodeCount, decodeSerial);
	decodeCount = 0;
}

bool DX9Texture::TakeSpare(u32 width, u32 height)
{
	DX9PooledTexture *t = pool != nullptr ? pool->Take(DX9TexturePool::TARGET, width, height, false) : nullptr;
	if (t == nullptr)
		return false;
	ReleaseSpare();
	sparePooled = t;
	spare = t->texture;
	if (FAILED(spare->GetSurfaceLevel(0, spareSurface.put())))
	{
		ReleaseSpare();
		return false;
	}
	return true;
}

void DX9Texture::ReleaseSpare()
{
	if (sparePooled == nullptr)
		return;
	spareSurface.reset();
	spare.reset();
	if (pool != nullptr)
		pool->Give(sparePooled);
	sparePooled = nullptr;
}

/* A planar 16-bit texture copied as it is: R5G6B5, A1R5G5B5 and A4R4G4B4
 * have the PowerVR's bits in its order, and its rows go from video memory
 * to the texture where it is locked, with no conversion. Or made by the
 * pixel shader (dx9_texdecoder.h), with the core option for it. */
bool DX9Texture::GpuDecode(const texd_plan& plan)
{
	DX9Context *context = DX9Context::Instance();
	if (context->GetDevice() == nullptr)
		return false;
	if (plan.native)
	{
		if ((unsigned)tex_type >= 3 || !context->IsFormatSupported(tex_type))
			return false;
		CancelDecode();
		if (lockedBits != nullptr && texture)
			texture->UnlockRect(0);
		lockedBits = nullptr;
		if (!Prepare(plan.width, plan.height, 1, context->WriteUsage(), DX9Context::TextureFormat(tex_type)))
			return false;
		D3DLOCKED_RECT rect;
		if (FAILED(texture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
			return false;
		const u8 *from = &vram[plan.src + plan.level[0].data];
		const u32 stride = (plan.level[0].sr & 0xFFFF) * 2, row = plan.width * 2;
		if (stride == row && (u32)rect.Pitch == row)
			memcpy(rect.pBits, from, (size_t)row * plan.height);
		else
			for (u32 y = 0; y < plan.height; y++)
				memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, from + (size_t)y * stride, row);
		texture->UnlockRect(0);
		swapRB = false;
		return true;
	}
	if (!settings.rend.GpuTextureDecodeD3D9 || decoder == nullptr || !decoder->Takes(tex_type)
			|| !TakePooled(DX9TexturePool::DRAWN + (u32)tex_type, plan.width, plan.height, plan.mipmapped != 0) || pooled->levels < plan.levels)
		return false;
	CancelDecode();
	u32 base;
	u8 * const to = decoder->Begin(plan.pal + plan.pal_entries * 4, plan.levels, &base);
	if (to == nullptr)
		return false;
	memcpy(to, &vram[plan.src], plan.bytes);
	if (plan.pal_entries != 0)
		memcpy(to + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);
	decodeJob = decoder->Next();
	decodeSerial = decoder->Serial();
	for (u32 i = 0; i < plan.levels; i++)
	{
		const texd_level& lv = plan.level[i];
		const u32 pc[6] = { base, lv.data, plan.pal, lv.wh, lv.sr, lv.mode };
		decoder->Add(pooled->surfaces[lv.mip], tex_type, pc);
	}
	decodeCount = plan.levels;
	// (drawn with its channels where they are sampled from)
	swapRB = false;
	return true;
}

/* Whether the texture is the one wanted, and made if not (the one it had
 * goes back to the context, to be used again). The pool's, if it has one
 * of this shape. */
bool DX9Texture::Prepare(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format)
{
	DX9Context *context = DX9Context::Instance();
	if ((usage & D3DUSAGE_AUTOGENMIPMAP) == 0 && (unsigned)tex_type < 5 && format == DX9Context::TextureFormat(tex_type)
			&& TakePooled((u32)tex_type, width, height, levels > 1))
		return true;
	ReleasePooled();
	if (texture)
	{
		/* (A render to this texture leaves one that can be rendered to,
		 * which is not one of these) */
		D3DSURFACE_DESC desc;
		if (SUCCEEDED(texture->GetLevelDesc(0, &desc)) && desc.Width == width && desc.Height == height
				&& desc.Format == format && desc.Usage == usage
				&& ((usage & D3DUSAGE_AUTOGENMIPMAP) != 0 || texture->GetLevelCount() == levels))
			return true;
		context->Recycle(texture);
	}
	texture = context->MakeTexture(width, height, levels, usage, format);
	return (bool)texture;
}

void *DX9Texture::UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	DX9Context *context = DX9Context::Instance();
	lockedBits = nullptr;
	/* Into the texture itself: one level, at a pitch that is the texture's
	 * width (a converter writes the rows one after another), and not a
	 * 16-bit format whose bits are in another order here (which they are
	 * turned to on the way in) */
	if (mipmaps || context->GetDevice() == nullptr || bytes != (size_t)width * height * pixel
			|| tex_type == TextureType::_5551 || tex_type == TextureType::_4444)
		return nullptr;
	const D3DFORMAT format = DX9Context::TextureFormat(tex_type);
	if (format == D3DFMT_UNKNOWN || !Prepare(width, height, 1, context->WriteUsage(), format))
		return nullptr;
	D3DLOCKED_RECT rect;
	if (FAILED(texture->LockRect(0, &rect, nullptr, context->WriteLockFlags())))
		return nullptr;
	if ((u32)rect.Pitch != width * pixel)
	{
		texture->UnlockRect(0);
		return nullptr;
	}
	lockedBits = rect.pBits;
	return lockedBits;
}

/* The 16-bit textures come out of the texture cache as OpenGL has them,
 * red in the top bits and alpha in the bottom one or four. Direct3D's
 * formats with alpha have it in the top bits, and the rest in the same
 * order: a turn of the bits to the right puts them there, as they are
 * written. */
static void copyRow(u8 *dst, const u8 *src, u32 bytes, unsigned turn)
{
	if (turn == 0)
	{
		memcpy(dst, src, bytes);
		return;
	}
	const u16 *s = (const u16 *)src;
	u16 *d = (u16 *)dst;
	for (u32 i = 0; i < bytes / 2; i++)
		d[i] = (u16)((s[i] >> turn) | (s[i] << (16 - turn)));
}

void DX9Texture::UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	DX9Context *context = DX9Context::Instance();
	// what the decoder was to make of it this frame is this now
	CancelDecode();
	if (lockedBits != nullptr)
	{
		const bool inPlace = data == lockedBits;
		texture->UnlockRect(0);
		lockedBits = nullptr;
		if (inPlace)
		{
			swapRB = tex_type == TextureType::_8888;
			return;
		}
	}
	if (context->GetDevice() == nullptr)
		return;

	const D3DFORMAT format = DX9Context::TextureFormat(tex_type);
	u32 bpp = 2;
	unsigned turn = 0;
	switch (tex_type)
	{
	case TextureType::_5551:
		turn = 1;
		break;
	case TextureType::_4444:
		turn = 4;
		break;
	case TextureType::_565:
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
	// smaller levels made on the card, where it can make them
	const bool generate = mipmapped && !mipmapsIncluded && context->CanGenerateMips(tex_type);
	const DWORD usage = context->WriteUsage() | (generate ? D3DUSAGE_AUTOGENMIPMAP : 0);
	if (!Prepare(width, height, generate ? 0 : levels, usage, format))
	{
		ERROR_LOG(RENDERER, "Texture creation failed: type %d, %d x %d, %d levels", (int)tex_type, width, height, levels);
		return;
	}
	swapRB = tex_type == TextureType::_8888;

	// (the levels included are from the smallest, 1x1, to the largest)
	for (u32 i = 0; i < levels; i++)
	{
		const u32 level = levels - i - 1;
		const u32 w = mipmapsIncluded ? 1u << i : (u32)width;
		const u32 h = mipmapsIncluded ? 1u << i : (u32)height;
		D3DLOCKED_RECT rect;
		if (FAILED(texture->LockRect(level, &rect, nullptr, context->WriteLockFlags())))
		{
			WARN_LOG(RENDERER, "Texture lock failed: %d x %d, level %d", w, h, level);
			return;
		}
		const u32 row = w * bpp;
		if ((u32)rect.Pitch == row)
			copyRow((u8 *)rect.pBits, data, row * h, turn);
		else
			for (u32 y = 0; y < h; y++)
				copyRow((u8 *)rect.pBits + (size_t)y * rect.Pitch, data + (size_t)y * row, row, turn);
		texture->UnlockRect(level);
		data += (size_t)row * h;
	}
}

bool DX9Texture::Force32BitTexture(TextureType type) const
{
	return !DX9Context::Instance()->IsFormatSupported(type);
}

bool DX9Texture::Delete()
{
	if (!BaseTextureCacheData::Delete())
		return false;
	if (lockedBits != nullptr && texture)
		texture->UnlockRect(0);
	lockedBits = nullptr;
	ReleasePooled();
	ReleaseSpare();
	DX9Context *context = DX9Context::Instance();
	context->Recycle(texture);
	spareSurface.reset();
	context->Recycle(spare);
	return true;
}

/* ---- Renders to textures, and video memory (rend/rtt_watch.h) ---- */

/* Pictures in system memory a render target is read back into, kept from
 * one read to the next by size: a read can come at any time, the middle
 * of a frame included, and what is made for it is used again. And the
 * target a picture made finer than the console's is brought down to the
 * console's size in. */
struct Staging
{
	u32 width, height;
	ComPtr<IDirect3DSurface9> surface;
};
static Staging staging[4];
static u32 stagingNext;
static ComPtr<IDirect3DTexture9> small;
static ComPtr<IDirect3DSurface9> smallSurface;

static IDirect3DSurface9 *stagingFor(IDirect3DDevice9 *device, u32 width, u32 height)
{
	for (Staging& s : staging)
		if (s.surface && s.width == width && s.height == height)
			return s.surface;
	Staging& s = staging[stagingNext++ % ARRAY_SIZE(staging)];
	s.surface.reset();
	if (FAILED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, s.surface.put(), nullptr)))
	{
		WARN_LOG(RENDERER, "Read back surface creation failed (%d x %d)", width, height);
		s.surface.reset();
		return nullptr;
	}
	s.width = width;
	s.height = height;
	return s.surface;
}

/* @width by @height from (@x, @y) of a render target into @rgba */
static bool readBack(IDirect3DSurface9 *surface, u32 x, u32 y, u32 width, u32 height, u8 *rgba)
{
	DX9Context *context = DX9Context::Instance();
	IDirect3DDevice9 *device = context->GetDevice();
	D3DSURFACE_DESC desc;
	IDirect3DSurface9 *sysmem = nullptr;
	if (device == nullptr || surface == nullptr || FAILED(surface->GetDesc(&desc))
			|| x + width > desc.Width || y + height > desc.Height
			|| (sysmem = stagingFor(device, desc.Width, desc.Height)) == nullptr)
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	context->EndScene();
	D3DLOCKED_RECT rect;
	const RECT area = { (LONG)x, (LONG)y, (LONG)(x + width), (LONG)(y + height) };
	if (FAILED(device->GetRenderTargetData(surface, sysmem)) || FAILED(sysmem->LockRect(&rect, &area, D3DLOCK_READONLY)))
	{
		WARN_LOG(RENDERER, "A picture could not be read back from the card");
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	// B G R A to R G B A
	for (u32 l = 0; l < height; l++)
	{
		const u8 *src = (const u8 *)rect.pBits + (size_t)l * rect.Pitch;
		u8 *dst = rgba + (size_t)l * width * 4;
		for (u32 i = 0; i < width; i++, src += 4, dst += 4)
		{
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			dst[3] = src[3];
		}
	}
	sysmem->UnlockRect();
	return true;
}

bool dx9_read_picture(IDirect3DTexture9 *texture, u32 width, u32 height, u8 *rgba)
{
	DX9Lock lock;
	ComPtr<IDirect3DSurface9> surface;
	if (texture == nullptr || FAILED(texture->GetSurfaceLevel(0, surface.put())))
	{
		memset(rgba, 0, (size_t)width * height * 4);
		return false;
	}
	return readBack(surface, 0, 0, width, height, rgba);
}

/* The picture as the console would have it: w by h, R G B A. */
static void dx9_rtt_read(const RttWatch *watch, u8 *rgba)
{
	DX9Lock lock;
	DX9Context *context = DX9Context::Instance();
	IDirect3DDevice9 *device = context->GetDevice();
	DX9Texture *texture = (DX9Texture *)watch->tex;
	const u32 w = watch->w, h = watch->h, scale = watch->scale > 1 ? watch->scale : 1;
	const u32 x0 = watch->x0 * scale, y0 = watch->y0 * scale;

	ComPtr<IDirect3DSurface9> surface;
	if (device == nullptr || !texture->texture || FAILED(texture->texture->GetSurfaceLevel(0, surface.put())))
	{
		memset(rgba, 0, (size_t)w * h * 4);
		return;
	}
	if (scale == 1)
	{
		readBack(surface, x0, y0, w, h, rgba);
		return;
	}
	/* Made the console's size on the card, filtered, and read at that: the
	 * clip's part of the texture drawn onto a picture of w by h. */
	if (small)
	{
		D3DSURFACE_DESC desc;
		small->GetLevelDesc(0, &desc);
		if (desc.Width < w || desc.Height < h)
		{
			smallSurface.reset();
			small.reset();
		}
	}
	if (!small)
	{
		small = context->MakeTexture(std::max(w, 640u), std::max(h, 480u), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8);
		if (!small || FAILED(small->GetSurfaceLevel(0, smallSurface.put())))
		{
			WARN_LOG(RENDERER, "Render to texture read back: target creation failed");
			smallSurface.reset();
			small.reset();
			memset(rgba, 0, (size_t)w * h * 4);
			return;
		}
	}
	D3DSURFACE_DESC src;
	surface->GetDesc(&src);
	context->SetTarget(smallSurface, nullptr, w, h);
	context->DrawQuad(texture->texture, true, false, -1.f, -1.f, 2.f, 2.f,
			(float)x0 / src.Width, (float)y0 / src.Height,
			(float)(x0 + w * scale) / src.Width, (float)(y0 + h * scale) / src.Height);
	readBack(smallSurface, 0, 0, w, h, rgba);
}

/* The texture cache's, never this module's to delete. */
static void dx9_rtt_release(uintptr_t)
{
}

static const RttWatchBackend dx9_rtt_backend = { dx9_rtt_read, dx9_rtt_release };

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

void dx9_rtt_supersede(u32 addr, u32 x0, u32 y0, u32 width, u32 height)
{
	u32 stride, bytes;

	covered(addr, x0, y0, width, height, stride, bytes);
	if (bytes != 0)
		rtt_watch_supersede(addr, bytes);
}

void dx9_rtt_watch(DX9Texture *texture, u32 addr, u32 x0, u32 y0, u32 width, u32 height)
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
		rtt_watch_add(&watch, &dx9_rtt_backend);
}

void dx9_rtt_term(void)
{
	rtt_watch_term();
	smallSurface.reset();
	small.reset();
	for (Staging& s : staging)
		s.surface.reset();
}
