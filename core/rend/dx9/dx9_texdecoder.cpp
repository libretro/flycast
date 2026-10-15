/* Textures made on the GPU from their bytes in video memory: see
 * dx9_texdecoder.h */
#include "dx9_texdecoder.h"
#include "dx9context_lr.h"
#define TEXD_SM3_KERNEL 0
#include "rend/texdecode_sm3.h"

#include <algorithm>

/* The shaders, by TEXD_SM3_KERNEL, as bytecode made from rend/texdecode_sm3.h
 * when the core was built: tools/rend/kernels_dxbc.sh, which checks that
 * they still are its */
#include "rend/texdecode_dx9.inc"

void DX9TexDecoder::Init(DX9Context *context)
{
	Term();
	if (!settings.rend.GpuTextureDecodeD3D9)
		return;
	device = context->GetDevice();
	if (device == nullptr)
		return;
	this->context = context;

	// what can be drawn into
	IDirect3D9 *d3d = nullptr;
	D3DDEVICE_CREATION_PARAMETERS params;
	D3DDISPLAYMODE mode;
	D3DCAPS9 caps;
	bool ok = SUCCEEDED(device->GetDirect3D(&d3d)) && d3d != nullptr && SUCCEEDED(device->GetCreationParameters(&params))
			&& SUCCEEDED(d3d->GetAdapterDisplayMode(params.AdapterOrdinal, &mode)) && SUCCEEDED(device->GetDeviceCaps(&caps));
	for (int t = 0; ok && t < 5; t++)
		takes[t] = context->IsFormatSupported((TextureType)t)
				&& d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE,
						DX9Context::TextureFormat((TextureType)t)) == D3D_OK;
	if (d3d != nullptr)
		d3d->Release();

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a share
	 * of the video memory there is, between 4 and 16 MB, rows of 4 KB */
	u64 bytes = (u64)context->GetVramBudget().available_kib() * 1024 / 256;
	bytes = std::max<u64>(bytes, 4 << 20);
	bytes = std::min<u64>(bytes, 16 << 20);
	ringRows = (u32)(bytes / (TEXD_SM3_RING_WIDTH * 4));
	if (ok)
		ringRows = std::min<u32>(ringRows, caps.MaxTextureHeight);
	// (a power of two: the shader's row is a multiplication by its inverse)
	while (ringRows & (ringRows - 1))
		ringRows &= ringRows - 1;
	static const D3DVERTEXELEMENT9 elements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		D3DDECL_END()
	};
	ok = ok && context->HasDynamicTextures() && ringRows >= 256 && caps.MaxTextureWidth >= TEXD_SM3_RING_WIDTH
			&& SUCCEEDED(device->CreateTexture(TEXD_SM3_RING_WIDTH, ringRows, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, ring.put(), nullptr))
			&& SUCCEEDED(device->CreateVertexShader((const DWORD *)texdecode_dx9[1], vertexShader.put()))
			&& SUCCEEDED(device->CreatePixelShader((const DWORD *)texdecode_dx9[2], pixelShader.put()))
			&& SUCCEEDED(device->CreateVertexDeclaration(elements, decl.put()));
	if (ok)
	{
		// (the rows are where the shader reads them only if they are 4 KB apart)
		D3DLOCKED_RECT rect;
		ok = SUCCEEDED(ring->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD));
		if (ok)
		{
			ok = rect.Pitch == TEXD_SM3_RING_WIDTH * 4;
			ring->UnlockRect(0);
		}
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX9: the texture shaders could not be set up: textures are converted on the CPU");
		Term();
		return;
	}
	head = 0;
	jobCount = 0;
	ready = true;
	NOTICE_LOG(RENDERER, "DX9: textures are drawn on the GPU (a ring of %u KB; drawn into 565 %d, 5551 %d, 4444 %d, 8888 %d, 8-bit %d)",
			ringRows * TEXD_SM3_RING_WIDTH * 4 / 1024, takes[0], takes[1], takes[2], takes[3], takes[4]);
}

void DX9TexDecoder::Term()
{
	if (ready)
		NOTICE_LOG(RENDERER, "DX9: textures drawn on the GPU: %u levels, %u MB of bytes; %u not, for want of room",
				(u32)statLevels, (u32)(statBytes >> 20), (u32)statFull);
	if (mapped != nullptr && ring)
		ring->UnlockRect(0);
	mapped = nullptr;
	ready = false;
	jobCount = 0;
	decl.reset();
	pixelShader.reset();
	vertexShader.reset();
	ring.reset();
	for (bool& t : takes)
		t = false;
	context = nullptr;
	device = nullptr;
}

u8 *DX9TexDecoder::Begin(u32 bytes, u32 levels, u32 *base)
{
	bytes = (bytes + 15) & ~15u;
	const u32 size = ringRows * TEXD_SM3_RING_WIDTH * 4;
	if (!ready || head + bytes > size || jobCount + levels > MAX_JOBS)
	{
		/* (the ring is the frame's: what does not fit is converted on the
		 * CPU, as before) */
		statFull++;
		return nullptr;
	}
	if (mapped == nullptr)
	{
		D3DLOCKED_RECT rect;
		if (FAILED(ring->LockRect(0, &rect, nullptr, D3DLOCK_DISCARD)))
			return nullptr;
		mapped = (u8 *)rect.pBits;
		head = 0;
	}
	u8 * const to = mapped + head;
	*base = head / 4;
	head += bytes;
	statBytes += bytes;
	return to;
}

void DX9TexDecoder::Add(IDirect3DSurface9 *surface, TextureType type, const u32 pc[6])
{
	Job& job = jobs[jobCount++];
	const u32 m = pc[5];
	job.surface = surface;
	job.width = pc[3] & 0xFFFF;
	job.height = pc[3] >> 16;
	const float k[16] = {
		(float)pc[0], (float)pc[1], (float)pc[2], 1.f / ringRows,
		(float)job.width, (float)job.height, (float)(pc[4] & 0xFFFF), (float)(pc[4] >> 16),
		(float)(m & 15), (float)((m >> 4) & 3), (float)((m >> 6) & 3), (float)((m >> 8) & 1),
		(float)((m >> 12) & 15), (float)((m >> 16) & 15), (float)(int)type, 0.f,
	};
	memcpy(job.k, k, sizeof(k));
	statLevels++;
}

void DX9TexDecoder::Cancel(u32 job, u32 count, u32 serial)
{
	if (serial != this->serial)
		return;
	for (u32 i = job; i < job + count && i < jobCount; i++)
		jobs[i].surface = nullptr;
}

void DX9TexDecoder::Flush()
{
	if (mapped != nullptr)
	{
		ring->UnlockRect(0);
		mapped = nullptr;
	}
	head = 0;
	if (jobCount == 0)
		return;
	serial++;
	u32 live = 0;
	for (u32 i = 0; i < jobCount; i++)
		live += jobs[i].surface != nullptr;
	if (live == 0 || !context->BeginScene())
	{
		jobCount = 0;
		return;
	}
	/* Every state through the context, which knows what the device has:
	 * nothing blended, tested or culled, the ring read texel for texel */
	context->SetBlend(false);
	context->SetDepthStencil(false, false, 7, DX9Stencil_None, 0);
	context->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	context->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	context->SetDecl(decl);
	context->SetVS(vertexShader);
	context->SetPS(pixelShader);
	context->SetTexture(0, ring);
	context->SetSampler(0, DX9Context::PointSampler());
	static const float triangle[3][4] = { { -1.f, -1.f, 0.f, 1.f }, { -1.f, 3.f, 0.f, 1.f }, { 3.f, -1.f, 0.f, 1.f } };
	for (u32 i = 0; i < jobCount; i++)
	{
		const Job& job = jobs[i];
		if (job.surface == nullptr)
			continue;
		context->SetTarget(job.surface, nullptr, job.width, job.height);
		device->SetPixelShaderConstantF(TEXD_SM3_CONSTANT, job.k, 4);
		device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(triangle[0]));
	}
	/* (the texture slot is the polygons': the ring is not left there. The
	 * last level drawn into stays the target until the frame's own is set,
	 * which is before anything is drawn with it) */
	context->SetTexture(0, nullptr);
	jobCount = 0;
}
