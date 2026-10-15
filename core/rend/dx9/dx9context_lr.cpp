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
#include "dx9context_lr.h"
#include <algorithm>

DX9Context DX9Context::instance;

D3DFORMAT DX9Context::TextureFormat(TextureType type)
{
	switch (type)
	{
	case TextureType::_565:
		return D3DFMT_R5G6B5;
	case TextureType::_5551:
		return D3DFMT_A1R5G5B5;
	case TextureType::_4444:
		return D3DFMT_A4R4G4B4;
	case TextureType::_8888:
		// (R G B A bytes, which the card reads as B G R A: the shaders swap them)
		return D3DFMT_A8R8G8B8;
	case TextureType::_8:
		return D3DFMT_A8;
	default:
		return D3DFMT_UNKNOWN;
	}
}

bool DX9Context::Init(const struct retro_hw_render_interface_d3d9 *iface)
{
	Term();
	/* (any version from the first on: a later one only adds to it) */
	if (iface == nullptr || iface->device == nullptr || iface->interface_version < 1
			|| iface->lock_context == nullptr || iface->unlock_context == nullptr || iface->set_texture == nullptr
			|| iface->get_sync_index == nullptr || iface->get_sync_index_mask == nullptr || iface->wait_sync_index == nullptr)
		return false;

	handle = iface->handle;
	lock_context = iface->lock_context;
	unlock_context = iface->unlock_context;
	set_texture = iface->set_texture;
	get_sync_index = iface->get_sync_index;
	get_sync_index_mask = iface->get_sync_index_mask;
	wait_sync_index = iface->wait_sync_index;

	compile = (pD3DCompile)(void *)iface->D3DCompile;
	if (compile == nullptr)
	{
		/* (a frontend that found none: the system's) */
		static const char * const names[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
		for (const char *name : names)
		{
			compilerModule = LoadLibraryA(name);
			if (compilerModule == nullptr)
				continue;
			compile = (pD3DCompile)(void *)GetProcAddress(compilerModule, "D3DCompile");
			if (compile != nullptr)
				break;
			FreeLibrary(compilerModule);
			compilerModule = nullptr;
		}
	}

	bool ok;
	{
		DX9Lock lock;
		ok = InitLocked(iface);
	}
	if (!ok)
		Term();
	return ok;
}

bool DX9Context::InitLocked(const struct retro_hw_render_interface_d3d9 *iface)
{
	iface->device->AddRef();
	device.reset(iface->device);
	IDirect3DDevice9Ex *deviceEx = nullptr;
	ex = SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&deviceEx));
	if (deviceEx != nullptr)
		deviceEx->Release();

	D3DCAPS9 caps;
	if (FAILED(device->GetDeviceCaps(&caps)))
	{
		ERROR_LOG(RENDERER, "DX9: no device capabilities");
		return false;
	}
	extraTargets = caps.NumSimultaneousRTs > 1 ? std::min<DWORD>(caps.NumSimultaneousRTs, 4) - 1 : 0;
	dynamicTextures = (caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES) != 0;
	separateAlpha = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) != 0;
	maxAnisotropy = std::max<DWORD>(caps.MaxAnisotropy, 1);
	if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0)
			|| caps.MaxVertexIndex <= 0xffff
			|| ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL))
			|| (ex && !dynamicTextures))
	{
		ERROR_LOG(RENDERER, "DX9: shader model 3, 32-bit indexes and textures of any size needed (shaders %x/%x, max index %x)",
				(unsigned)caps.VertexShaderVersion, (unsigned)caps.PixelShaderVersion, (unsigned)caps.MaxVertexIndex);
		return false;
	}

	/* The formats, as the adapter has them for the mode it shows */
	{
		IDirect3D9 *d3d = nullptr;
		D3DDEVICE_CREATION_PARAMETERS params;
		D3DDISPLAYMODE mode;
		bool ok = SUCCEEDED(device->GetDirect3D(&d3d)) && d3d != nullptr
				&& SUCCEEDED(device->GetCreationParameters(&params))
				&& SUCCEEDED(d3d->GetAdapterDisplayMode(params.AdapterOrdinal, &mode));
		if (ok)
		{
			auto check = [&](DWORD usage, D3DRESOURCETYPE type, D3DFORMAT format) {
				// (D3DOK_NOAUTOGEN is a success that is not one)
				return d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format, usage, type, format) == D3D_OK;
			};
			for (int i = 0; i < 5; i++)
			{
				const D3DFORMAT format = TextureFormat((TextureType)i);
				formatSupported[i] = check(WriteUsage(), D3DRTYPE_TEXTURE, format);
				mipsSupported[i] = formatSupported[i] && check(WriteUsage() | D3DUSAGE_AUTOGENMIPMAP, D3DRTYPE_TEXTURE, format);
			}
			ok = check(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
			/* (a float for the depth where there is one, as the other renderers have it) */
			depthFormat = D3DFMT_UNKNOWN;
			for (D3DFORMAT format : { D3DFMT_D24FS8, D3DFMT_D24S8 })
				if (check(D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, format)
						&& SUCCEEDED(d3d->CheckDepthStencilMatch(params.AdapterOrdinal, params.DeviceType, mode.Format,
								D3DFMT_A8R8G8B8, format)))
				{
					depthFormat = format;
					break;
				}
		}
		if (d3d != nullptr)
			d3d->Release();
		if (!ok || depthFormat == D3DFMT_UNKNOWN || !formatSupported[(int)TextureType::_8888]
				|| !formatSupported[(int)TextureType::_8])
		{
			ERROR_LOG(RENDERER, "DX9: the formats needed are not there");
			return false;
		}
	}

	vramBudget = VramBudget();
	vramBudget.budget_kib = vram_kib(device->GetAvailableTextureMem());
	NOTICE_LOG(RENDERER, "DX9: %s device, interface version %u, %s textures, depth %s, 16-bit formats %d%d%d",
			ex ? "Direct3D9Ex" : "Direct3D 9", iface->interface_version, dynamicTextures ? "dynamic" : "managed",
			depthFormat == D3DFMT_D24FS8 ? "D24FS8" : "D24S8",
			formatSupported[0], formatSupported[1], formatSupported[2]);
	vram_budget_log("DX9", vramBudget);

	if (!shaders.Init(device, compile))
	{
		ERROR_LOG(RENDERER, "DX9: no shaders (no compiler, or it failed)");
		return false;
	}

	static const D3DVERTEXELEMENT9 quadElements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		D3DDECL_END()
	};
	if (FAILED(device->CreateVertexDeclaration(quadElements, quadDecl.put()))
			|| FAILED(device->CreateVertexBuffer(QuadCount * 4 * 4 * sizeof(float), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
					0, D3DPOOL_DEFAULT, quadBuffer.put(), nullptr)))
	{
		ERROR_LOG(RENDERER, "DX9: initialization failed");
		return false;
	}
	quadNext = QuadCount;

	/* The base state, recorded */
	if (SUCCEEDED(device->BeginStateBlock()))
	{
		SetBase(true);
		IDirect3DStateBlock9 *block = nullptr;
		if (FAILED(device->EndStateBlock(&block)))
			block = nullptr;
		baseState.reset(block);
	}
	Forget();
	return true;
}

/* What the renderer relies on and never changes */
void DX9Context::SetBase(bool record)
{
	static const DWORD states[][2] = {
		{ D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_LIGHTING, FALSE },
		{ D3DRS_FOGENABLE, FALSE },
		{ D3DRS_SPECULARENABLE, FALSE },
		{ D3DRS_CLIPPING, TRUE },
		{ D3DRS_CLIPPLANEENABLE, 0 },
		{ D3DRS_SRGBWRITEENABLE, FALSE },
		{ D3DRS_MULTISAMPLEANTIALIAS, FALSE },
		{ D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_BLENDOP, D3DBLENDOP_ADD },
		{ D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE },
		{ D3DRS_SCISSORTESTENABLE, TRUE },
		{ D3DRS_DEPTHBIAS, 0 },
		{ D3DRS_SLOPESCALEDEPTHBIAS, 0 },
		{ D3DRS_WRAP0, 0 },
		{ D3DRS_DITHERENABLE, FALSE },
		{ D3DRS_POINTSPRITEENABLE, FALSE },
		{ D3DRS_ANTIALIASEDLINEENABLE, FALSE },
	};
	for (const auto& state : states)
	{
		if (record)
			device->SetRenderState((D3DRENDERSTATETYPE)state[0], state[1]);
		renderState[state[0]] = state[1];
	}
	if (record)
	{
		device->SetStreamSourceFreq(0, 1);
		for (DWORD stage = 0; stage < 3; stage++)
		{
			device->SetTexture(stage, nullptr);
			device->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
			device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, 0);
			device->SetSamplerState(stage, D3DSAMP_ELEMENTINDEX, 0);
			device->SetSamplerState(stage, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
			// the palette's and the fog table's: nearest, clamped
			if (stage != DX9S_Texture)
			{
				device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
				device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				device->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
				device->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, 1);
			}
		}
	}
	for (int stage = 0; stage < 3; stage++)
		bound.texture[stage] = nullptr;
	bound.sampler[DX9S_Palette] = bound.sampler[DX9S_Fog] = PointSampler();
}

void DX9Context::Forget()
{
	epoch++;
	for (DWORD& state : renderState)
		state = Unknown;
	memset(&bound, 0, sizeof(bound));
	/* (what is known not to be anything this sets) */
	bound.vs = reinterpret_cast<IDirect3DVertexShader9 *>(1);
	bound.ps = reinterpret_cast<IDirect3DPixelShader9 *>(1);
	bound.decl = reinterpret_cast<IDirect3DVertexDeclaration9 *>(1);
	bound.stream = reinterpret_cast<IDirect3DVertexBuffer9 *>(1);
	bound.indices = reinterpret_cast<IDirect3DIndexBuffer9 *>(1);
	bound.target = reinterpret_cast<IDirect3DSurface9 *>(1);
	bound.depth = reinterpret_cast<IDirect3DSurface9 *>(1);
	bound.bools = ~0u;
	bound.depthKey = ~0u;
	bound.blendKey = ~0u;
	for (int stage = 0; stage < 3; stage++)
		bound.texture[stage] = reinterpret_cast<IDirect3DBaseTexture9 *>(1);
	if (!device)
		return;
	if (baseState)
	{
		baseState->Apply();
		SetBase(false);
	}
	else
		SetBase(true);
	/* (no more than the one target the renderer draws into) */
	for (DWORD i = 1; i <= extraTargets; i++)
		device->SetRenderTarget(i, nullptr);
}

void DX9Context::Term()
{
	frameTexture.reset();
	handedOver = false;
	if (device)
	{
		DX9Lock lock;
		/* Nothing of this core's left bound to the device, which keeps
		 * what is bound: a device is not reset while any of its default
		 * pool resources is there. (The frontend drops the frame it has
		 * of this core's.) */
		set_texture(handle, nullptr);
		EndScene();
		IDirect3DSurface9 *back = nullptr;
		if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back != nullptr)
		{
			device->SetRenderTarget(0, back);
			back->Release();
		}
		device->SetDepthStencilSurface(nullptr);
		for (DWORD stage = 0; stage < 3; stage++)
			device->SetTexture(stage, nullptr);
		device->SetStreamSource(0, nullptr, 0, 0);
		device->SetIndices(nullptr);
		device->SetVertexShader(nullptr);
		device->SetPixelShader(nullptr);
		device->SetVertexDeclaration(nullptr);
		Forget();
	}
	spares.clear();
	spareBytes = 0;
	quadBuffer.reset();
	quadDecl.reset();
	baseState.reset();
	shaders.Term();
	device.reset();
	if (compilerModule != nullptr)
		FreeLibrary(compilerModule);
	compilerModule = nullptr;
	compile = nullptr;
	ex = false;
	handle = nullptr;
	lock_context = nullptr;
	unlock_context = nullptr;
	set_texture = nullptr;
	get_sync_index = nullptr;
	get_sync_index_mask = nullptr;
	wait_sync_index = nullptr;
	extraTargets = 0;
	inScene = false;
}

void DX9Context::Lock()
{
	if (lockDepth++ != 0 || lock_context == nullptr)
		return;
	if (lock_context(handle))
		/* the frontend has drawn with it since: nothing is as it was left */
		Forget();
}

void DX9Context::Unlock()
{
	if (lockDepth <= 0 || --lockDepth != 0)
		return;
	EndScene();
	if (unlock_context != nullptr)
		unlock_context(handle);
}

bool DX9Context::BeginScene()
{
	if (inScene)
		return true;
	inScene = device && SUCCEEDED(device->BeginScene());
	return inScene;
}

void DX9Context::EndScene()
{
	if (!inScene)
		return;
	device->EndScene();
	inScene = false;
}

void DX9Context::SetSampler(DWORD stage, u32 key)
{
	const u32 old = bound.sampler[stage];
	if (key == old)
		return;
	const bool known = (old & 0x10000) != 0;
	auto field = [](u32 k, int shift, u32 mask) { return (k >> shift) & mask; };
	const bool linear = key & 1;
	const u32 anisotropy = std::max<u32>(1, std::min<u32>(field(key, 1, 31), maxAnisotropy));
	if (!known || field(key, 0, 63) != field(old, 0, 63))
	{
		device->SetSamplerState(stage, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		device->SetSamplerState(stage, D3DSAMP_MINFILTER, !linear ? D3DTEXF_POINT : anisotropy > 1 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
		device->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, anisotropy);
		if (!known)
			// (the level nearest, as the other renderers: trilinear filtering is two passes)
			device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}
	if (!known || field(key, 6, 7) != field(old, 6, 7))
		device->SetSamplerState(stage, D3DSAMP_ADDRESSU, field(key, 6, 7));
	if (!known || field(key, 9, 7) != field(old, 9, 7))
		device->SetSamplerState(stage, D3DSAMP_ADDRESSV, field(key, 9, 7));
	if (!known || field(key, 12, 15) != field(old, 12, 15))
	{
		const float bias = D_Adjust_LoD_Bias[field(key, 12, 15)];
		DWORD bits;
		memcpy(&bits, &bias, sizeof(bits));
		device->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, bits);
	}
	bound.sampler[stage] = key;
}

void DX9Context::SetTarget(IDirect3DSurface9 *target, IDirect3DSurface9 *depth, u32 width, u32 height)
{
	if (target != bound.target)
	{
		device->SetRenderTarget(0, target);
		bound.target = target;
		// (which sets the viewport and the scissor rectangle to all of it)
		bound.viewport[0] = bound.viewport[1] = 0;
		bound.scissorSet = false;
	}
	if (depth != bound.depth)
	{
		device->SetDepthStencilSurface(depth);
		bound.depth = depth;
	}
	if (width != bound.viewport[0] || height != bound.viewport[1])
	{
		const D3DVIEWPORT9 viewport = { 0, 0, width, height, 0.f, 1.f };
		device->SetViewport(&viewport);
		const float halfPixel[4] = { -1.f / width, 1.f / height, 0.f, 0.f };
		device->SetVertexShaderConstantF(DX9VC_HalfPixel, halfPixel, 1);
		bound.viewport[0] = width;
		bound.viewport[1] = height;
	}
}

void DX9Context::Clear(DWORD flags, D3DCOLOR color)
{
	// (the viewport's, within the scissor rectangle)
	const RECT all = { 0, 0, (LONG)bound.viewport[0], (LONG)bound.viewport[1] };
	SetScissor(all);
	device->Clear(0, nullptr, flags, color, 0.f, 0);
}

static const DWORD depthFuncs[] =
{
	D3DCMP_NEVER,				//0	Never
	D3DCMP_LESS,				//1	Less
	D3DCMP_EQUAL,				//2	Equal
	D3DCMP_LESSEQUAL,			//3	Less Or Equal
	D3DCMP_GREATER,				//4	Greater
	D3DCMP_NOTEQUAL,			//5	Not Equal
	D3DCMP_GREATEREQUAL,		//6	Greater Or Equal
	D3DCMP_ALWAYS,				//7	Always
};

void DX9Context::SetDepthStencil(bool depthTest, bool depthWrite, int depthFunc, int stencil, DWORD ref)
{
	const u32 key = (u32)depthTest | ((u32)depthWrite << 1) | ((u32)(depthFunc & 7) << 2) | ((u32)stencil << 5);
	if (key != bound.depthKey)
	{
		SetRenderState(D3DRS_ZENABLE, depthTest ? D3DZB_TRUE : D3DZB_FALSE);
		SetRenderState(D3DRS_ZWRITEENABLE, depthWrite);
		SetRenderState(D3DRS_ZFUNC, depthFuncs[depthFunc & 7]);
		SetRenderState(D3DRS_STENCILENABLE, stencil != DX9Stencil_None);
		if (stencil != DX9Stencil_None)
		{
			DWORD fail = D3DSTENCILOP_KEEP, zfail = D3DSTENCILOP_KEEP, pass = D3DSTENCILOP_KEEP;
			DWORD func = D3DCMP_ALWAYS, readMask = 0xff, writeMask = 0xff;
			switch (stencil)
			{
			case DX9Stencil_Shadow:
				pass = D3DSTENCILOP_REPLACE;
				readMask = 0;
				writeMask = 0x80;
				break;
			case DX9Stencil_ShadowLaterWins:
				pass = D3DSTENCILOP_REPLACE;
				func = D3DCMP_EQUAL;
				readMask = 0x81;
				writeMask = 0x40;
				break;
			case DX9Stencil_ShadowEarlierWins:
				pass = D3DSTENCILOP_INVERT;
				func = D3DCMP_EQUAL;
				readMask = 0xC1;
				writeMask = 0x40;
				break;
			case DX9Stencil_Xor:
				pass = D3DSTENCILOP_INVERT;
				readMask = 0;
				writeMask = 2;
				break;
			case DX9Stencil_Or:
				pass = D3DSTENCILOP_REPLACE;
				readMask = 2;
				writeMask = 2;
				break;
			case DX9Stencil_Inclusion:
				fail = zfail = D3DSTENCILOP_ZERO;
				pass = D3DSTENCILOP_REPLACE;
				func = D3DCMP_LESSEQUAL;
				readMask = writeMask = 3;
				break;
			case DX9Stencil_Exclusion:
				fail = zfail = D3DSTENCILOP_ZERO;
				func = D3DCMP_EQUAL;
				readMask = writeMask = 3;
				break;
			case DX9Stencil_Final:
				// takes shadows, in a volume, and not drawn again for it (bit 6);
				// clears the volume bits and that mark
				fail = zfail = pass = D3DSTENCILOP_ZERO;
				func = D3DCMP_EQUAL;
				readMask = 0xC1;
				writeMask = 0x43;
				break;
			}
			SetRenderState(D3DRS_STENCILFAIL, fail);
			SetRenderState(D3DRS_STENCILZFAIL, zfail);
			SetRenderState(D3DRS_STENCILPASS, pass);
			SetRenderState(D3DRS_STENCILFUNC, func);
			SetRenderState(D3DRS_STENCILMASK, readMask);
			SetRenderState(D3DRS_STENCILWRITEMASK, writeMask);
		}
		bound.depthKey = key;
	}
	if (stencil != DX9Stencil_None)
		SetRenderState(D3DRS_STENCILREF, ref);
}

/* The TSP's instructions as Direct3D's factors. Those for the alpha
 * cannot be colours: other colour's alpha is the other's alpha. */
static const DWORD srcBlend[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_DESTCOLOR,
	D3DBLEND_INVDESTCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
};
static const DWORD dstBlend[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCCOLOR,
	D3DBLEND_INVSRCCOLOR,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
};
static const DWORD srcBlendAlpha[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
};
static const DWORD dstBlendAlpha[] =
{
	D3DBLEND_ZERO,
	D3DBLEND_ONE,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_SRCALPHA,
	D3DBLEND_INVSRCALPHA,
	D3DBLEND_DESTALPHA,
	D3DBLEND_INVDESTALPHA,
};

void DX9Context::SetBlend(bool enable, u32 src, u32 dst, bool writeColor)
{
	src &= 7;
	dst &= 7;
	if (!enable)
		src = dst = 0;
	const u32 key = (u32)enable | (src << 1) | (dst << 4) | ((u32)writeColor << 7);
	if (key == bound.blendKey)
		return;
	SetRenderState(D3DRS_ALPHABLENDENABLE, enable);
	if (enable)
	{
		SetRenderState(D3DRS_SRCBLEND, srcBlend[src]);
		SetRenderState(D3DRS_DESTBLEND, dstBlend[dst]);
		SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, separateAlpha);
		if (separateAlpha)
		{
			SetRenderState(D3DRS_SRCBLENDALPHA, srcBlendAlpha[src]);
			SetRenderState(D3DRS_DESTBLENDALPHA, dstBlendAlpha[dst]);
		}
	}
	SetRenderState(D3DRS_COLORWRITEENABLE, writeColor ? 0xf : 0);
	bound.blendKey = key;
}

void DX9Context::SetAlphaBlend()
{
	const u32 key = 0x100;
	if (key == bound.blendKey)
		return;
	SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
	bound.blendKey = key;
}

ComPtr<IDirect3DTexture9> DX9Context::MakeTexture(u32 width, u32 height, u32 levels, DWORD usage, D3DFORMAT format)
{
	ComPtr<IDirect3DTexture9> texture;
	for (size_t i = 0; i < spares.size(); i++)
	{
		Spare& spare = spares[i];
		if (spare.width == width && spare.height == height && spare.levels == levels
				&& spare.usage == usage && spare.format == format)
		{
			texture = std::move(spare.texture);
			spareBytes -= spare.bytes;
			spares.erase(spares.begin() + i);
			return texture;
		}
	}
	if (!device)
		return texture;
	/* (the managed pool where the device has one and the processor writes
	 * the texture without dynamic ones) */
	const D3DPOOL pool = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) != 0 || ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;
	HRESULT hr = device->CreateTexture(width, height, levels, usage, format, pool, texture.put(), nullptr);
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX9: texture creation failed (%x): %d x %d, %d levels, format %d", (unsigned)hr,
				width, height, levels, (int)format);
		texture.reset();
	}
	return texture;
}

void DX9Context::Recycle(ComPtr<IDirect3DTexture9>& texture)
{
	if (!texture)
		return;
	D3DSURFACE_DESC desc;
	if (SUCCEEDED(texture->GetLevelDesc(0, &desc)) && (desc.Usage & D3DUSAGE_AUTOGENMIPMAP) == 0)
	{
		Spare spare;
		spare.width = desc.Width;
		spare.height = desc.Height;
		spare.levels = texture->GetLevelCount();
		spare.usage = desc.Usage;
		spare.format = desc.Format;
		const u32 pixel = desc.Format == D3DFMT_A8R8G8B8 ? 4 : desc.Format == D3DFMT_A8 ? 1 : 2;
		spare.bytes = desc.Width * desc.Height * pixel * (spare.levels > 1 ? 4 : 3) / 3;
		/* (the oldest go first, when there are too many to keep) */
		while (!spares.empty() && (spares.size() >= MaxSpares || spareBytes + spare.bytes > MaxSpareBytes))
		{
			spareBytes -= spares.front().bytes;
			spares.erase(spares.begin());
		}
		if (spare.bytes <= MaxSpareBytes)
		{
			spare.texture = std::move(texture);
			spareBytes += spare.bytes;
			spares.push_back(std::move(spare));
		}
	}
	texture.reset();
}

void DX9Context::DrawQuad(IDirect3DTexture9 *texture, bool linear, bool swapRB,
		float x, float y, float w, float h, float u0, float v0, float u1, float v1, bool blend, float alpha)
{
	if (!quadBuffer || texture == nullptr || !BeginScene())
		return;
	const float vertices[4][4] = {
		{ x,     y,     u0, v1 },
		{ x,     y + h, u0, v0 },
		{ x + w, y,     u1, v1 },
		{ x + w, y + h, u1, v0 },
	};
	DWORD flags = D3DLOCK_NOOVERWRITE;
	if (quadNext >= QuadCount)
	{
		flags = D3DLOCK_DISCARD;
		quadNext = 0;
	}
	void *data;
	if (FAILED(quadBuffer->Lock(quadNext * sizeof(vertices), sizeof(vertices), &data, flags)))
		return;
	memcpy(data, vertices, sizeof(vertices));
	quadBuffer->Unlock();

	SetDecl(quadDecl);
	SetStream(quadBuffer, 4 * sizeof(float));
	SetVS(shaders.GetVS(DX9VS_Quad));
	SetPS(shaders.GetPS(DX9PS_Quad));
	SetDepthStencil(false, false, 7, DX9Stencil_None, 0);
	if (blend)
		SetAlphaBlend();
	else
		SetBlend(false);
	SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	const RECT all = { 0, 0, (LONG)bound.viewport[0], (LONG)bound.viewport[1] };
	SetScissor(all);
	SetTexture(0, texture);
	SetSampler(0, linear ? LinearSampler() : PointSampler());
	SetBools(swapRB ? (bound.bools | (1u << DX9B_SwapRB)) : (bound.bools & ~(1u << DX9B_SwapRB)));
	const float color[4] = { 1.f, 1.f, 1.f, alpha };
	device->SetPixelShaderConstantF(DX9PC_Quad, color, 1);
	device->DrawPrimitive(D3DPT_TRIANGLESTRIP, quadNext * 4, 2);
	quadNext++;
}

void DX9Context::SetFrame(IDirect3DTexture9 *texture)
{
	if (texture != nullptr)
		texture->AddRef();
	frameTexture.reset(texture);
	handedOver = texture != nullptr;
	/* (with the device taken, by the render that drew it) */
	if (handedOver)
	{
		EndScene();
		set_texture(handle, texture);
	}
}

bool DX9Context::Present()
{
	/* from here on the frontend has what it was handed: the next render
	 * waits for it to be done with it (WaitPresentIndex()) */
	const bool frame = handedOver;
	handedOver = false;
	return frame;
}

unsigned DX9Context::GetPresentCount() const
{
	if (get_sync_index_mask == nullptr)
		return 1;
	const unsigned mask = get_sync_index_mask(handle);
	unsigned count = 1;
	while (count < 32 && (mask >> count) != 0)
		count++;
	return count;
}

unsigned DX9Context::GetPresentIndex() const
{
	if (get_sync_index == nullptr)
		return 0;
	const unsigned index = get_sync_index(handle);
	return index < 32 ? index : 0;
}

void DX9Context::WaitPresentIndex()
{
	if (wait_sync_index != nullptr && lockDepth == 0)
		wait_sync_index(handle);
}
