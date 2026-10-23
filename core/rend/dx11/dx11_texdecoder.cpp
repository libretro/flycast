/* Textures made on the GPU from their bytes in video memory: see
 * dx11_texdecoder.h */
#include "dx11_texdecoder.h"
#include "dx11_nvapi.h"
#include "dx11context_lr.h"

#include <algorithm>

/* The kernels, by TEXD_KERNEL_D3D, as DXBC made from rend/texdecode.h when
 * the core was built: tools/rend/kernels_dxbc.sh, which checks that they
 * still are its. Nothing is compiled when the core runs. */
#include "rend/texdecode_dxbc.inc"

static bool formatHas(ID3D11Device *device, DXGI_FORMAT format, UINT what)
{
	UINT support = 0;
	return SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & what) == what;
}

void DX11TexDecoder::Init(DX11Context *context)
{
	Term();
	/* Compute shaders and a context that is the device's own: a deferred
	 * one maps a buffer only to discard it, every time */
	if (!context->HasPerPixel())
	{
		INFO_LOG(RENDERER, "DX11: textures are converted on the CPU");
		return;
	}
	ID3D11Device *device = context->GetDevice();
	this->context = context;
	deviceContext = context->GetDeviceContext();

	const UINT uav = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
	const UINT drawn = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
	takes[(int)TextureType::_8888] = formatHas(device, DXGI_FORMAT_R8G8B8A8_UINT, uav);
	takes[(int)TextureType::_8] = formatHas(device, DXGI_FORMAT_R8_UINT, uav);
	takes[(int)TextureType::_565] = context->IsFormatSupported(TextureType::_565) && formatHas(device, DXGI_FORMAT_B5G6R5_UNORM, drawn);
	takes[(int)TextureType::_5551] = context->IsFormatSupported(TextureType::_5551) && formatHas(device, DXGI_FORMAT_B5G5R5A1_UNORM, drawn);
	takes[(int)TextureType::_4444] = context->IsFormatSupported(TextureType::_4444) && formatHas(device, DXGI_FORMAT_B4G4R4A4_UNORM, drawn);

	D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
	noOverwrite = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
			&& options.MapNoOverwriteOnDynamicBufferSRV;

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a
	 * share of the video memory there is, between 4 and 16 MB. (The
	 * largest texture, 1024x1024 with its levels, is under 3 MB.) */
	u64 bytes = (u64)context->GetVramBudget().available_kib() * 1024 / 256;
	bytes = std::max<u64>(bytes, 4 * 1024 * 1024);
	bytes = std::min<u64>(bytes, 16 * 1024 * 1024);
	ringSize = (u32)bytes & ~15u;

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = ringSize;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	bool ok = SUCCEEDED(device->CreateBuffer(&desc, nullptr, ring.put()));
	if (ok)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		view.BufferEx.FirstElement = 0;
		view.BufferEx.NumElements = ringSize / 4;
		view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		ok = SUCCEEDED(device->CreateShaderResourceView(ring, &view, ringView.put()));
	}
	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = sizeof(Job::pc);
	cb.Usage = D3D11_USAGE_DEFAULT;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ok = ok && SUCCEEDED(device->CreateBuffer(&cb, nullptr, constants.put()));
	// every kernel there is, now: none is made while a game runs
	ok = ok && SUCCEEDED(device->CreateComputeShader(texdecode_dxbc[1], texdecode_dxbc_size[1], nullptr, kernel32.put()));
	ok = ok && SUCCEEDED(device->CreateComputeShader(texdecode_dxbc[2], texdecode_dxbc_size[2], nullptr, kernel8.put()));
	ok = ok && SUCCEEDED(device->CreateVertexShader(texdecode_dxbc[3], texdecode_dxbc_size[3], nullptr, vertexShader.put()));
	ok = ok && SUCCEEDED(device->CreatePixelShader(texdecode_dxbc[4], texdecode_dxbc_size[4], nullptr, pixelShader.put()));

	D3D11_RASTERIZER_DESC rast{};
	rast.FillMode = D3D11_FILL_SOLID;
	rast.CullMode = D3D11_CULL_NONE;
	rast.DepthClipEnable = TRUE;
	ok = ok && SUCCEEDED(device->CreateRasterizerState(&rast, rasterizer.put()));
	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = FALSE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	ds.BackFace = ds.FrontFace;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&ds, depthStencil.put()));
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX11: the texture kernels could not be made: textures are converted on the CPU");
		Term();
		return;
	}
	head = 0;
	jobCount = 0;
	ready = true;
	/* An NVIDIA device whose driver has NVAPI's UAV overlap: a frame's
	 * levels are made without the driver waiting for each kernel to be done
	 * before the next starts (Flush()) */
	overlap = context->GetVendor() == 0x10de && dx11_nvapi_uav_overlap_available();
	INFO_LOG(RENDERER, "DX11: textures are made on the GPU (a ring of %u KB%s; 32-bit %d, 8-bit %d, 16-bit drawn %d%d%d; %s)",
			ringSize / 1024, noOverwrite ? "" : ", discarded every frame",
			takes[(int)TextureType::_8888], takes[(int)TextureType::_8],
			takes[(int)TextureType::_565], takes[(int)TextureType::_5551], takes[(int)TextureType::_4444],
			overlap ? "the kernels overlapped through NVAPI" : "each kernel after the one before");
}

void DX11TexDecoder::Term()
{
	if (mapped != nullptr && deviceContext != nullptr)
		deviceContext->Unmap(ring, 0);
	mapped = nullptr;
	ready = false;
	overlap = false;
	jobCount = 0;
	depthStencil.reset();
	rasterizer.reset();
	pixelShader.reset();
	vertexShader.reset();
	kernel8.reset();
	kernel32.reset();
	constants.reset();
	ringView.reset();
	ring.reset();
	for (bool& t : takes)
		t = false;
	context = nullptr;
	deviceContext = nullptr;
}

u8 *DX11TexDecoder::Begin(u32 bytes, u32 levels, u32 *base)
{
	bytes = (bytes + 15) & ~15u;
	if (!ready || bytes > ringSize || levels > MAX_JOBS)
		return nullptr;
	const bool wraps = head + bytes > ringSize;
	if (jobCount + levels > MAX_JOBS || (wraps && mapped != nullptr))
		/* What the ring has now goes to the GPU first: a mapping that
		 * discards would take it away from the kernels yet to read it */
		Flush();
	if (mapped == nullptr)
	{
		/* After what the GPU may still be reading, if the device lets a
		 * buffer it reads through a view be mapped so; from the start of
		 * new memory otherwise, and when the ring is full */
		const D3D11_MAP how = noOverwrite && !wraps && head != 0 ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
		D3D11_MAPPED_SUBRESOURCE map;
		if (FAILED(deviceContext->Map(ring, 0, how, 0, &map)))
			return nullptr;
		mapped = (u8 *)map.pData;
		if (how == D3D11_MAP_WRITE_DISCARD)
			head = 0;
	}
	u8 * const to = mapped + head;
	*base = head / 4;
	head += bytes;
	statBytes += bytes;
	return to;
}

void DX11TexDecoder::Add(ID3D11UnorderedAccessView *uav, ID3D11RenderTargetView *target, const u32 pc[6])
{
	Job& job = jobs[jobCount++];
	job.uav = uav;
	job.target = target;
	memcpy(job.pc, pc, sizeof(u32) * 6);
	job.pc[6] = job.pc[7] = 0;
	statLevels++;
}

void DX11TexDecoder::Cancel(u32 job, u32 count, u32 serial)
{
	if (serial != this->serial)
		// recorded already, and then written over by what came after
		return;
	for (u32 i = job; i < job + count && i < jobCount; i++)
		jobs[i].uav = nullptr, jobs[i].target = nullptr;
}

void DX11TexDecoder::Flush()
{
	if (mapped != nullptr)
	{
		deviceContext->Unmap(ring, 0);
		mapped = nullptr;
	}
	if (jobCount == 0)
		return;
	serial++;
	/* Textures may be bound to be read by the pixel shader. Direct3D
	 * unbinds a texture from there when it is bound to be written, and
	 * what the context knows of what is bound would be wrong: they are
	 * unbound first. */
	context->UnbindPSResources(4);

	bool drawn = false;
	deviceContext->CSSetShaderResources(0, 1, ringView.addr());
	deviceContext->CSSetConstantBuffers(0, 1, constants.addr());
	/* The kernels of a flush do not depend on one another: each reads the
	 * ring and its constants alone, which nothing here writes but the CPU
	 * before it is unmapped, and writes one level of one texture through
	 * its own view - no two of them the same level, since a texture made
	 * again, given back to the pool or uploaded by the CPU takes back what
	 * it asked for before (DX11Texture::CancelDecode(), which nulls those
	 * jobs). With NVAPI's UAV overlap the driver does not wait for each to
	 * be done before the next starts; the run ends before anything that
	 * reads the levels (the views unbound below, then the draws). */
	u32 dispatches = 0;
	if (overlap)
		for (u32 i = 0; i < jobCount; i++)
			if (jobs[i].uav != nullptr)
				dispatches++;
	const bool overlapped = dispatches > 1;
	if (overlapped)
		dx11_nvapi_begin_uav_overlap(deviceContext);
	for (u32 out = TEXD_OUT8; out <= TEXD_OUT32; out += TEXD_OUT32 - TEXD_OUT8)
	{
		bool bound = false;
		for (u32 i = 0; i < jobCount; i++)
		{
			const Job& job = jobs[i];
			if (job.target != nullptr)
				drawn = true;
			if (job.uav == nullptr || ((job.pc[5] >> 6) & 3) != out)
				continue;
			if (!bound)
			{
				deviceContext->CSSetShader(out == TEXD_OUT32 ? kernel32.get() : kernel8.get(), nullptr, 0);
				bound = true;
			}
			deviceContext->UpdateSubresource(constants, 0, nullptr, job.pc, 0, 0);
			deviceContext->CSSetUnorderedAccessViews(0, 1, &job.uav, nullptr);
			deviceContext->Dispatch(((job.pc[3] & 0xFFFF) + 7) / 8, ((job.pc[3] >> 16) + 7) / 8, 1);
		}
	}
	if (overlapped)
		dx11_nvapi_end_uav_overlap(deviceContext);
	// (unbound, to be sampled)
	ID3D11UnorderedAccessView * const noUav = nullptr;
	deviceContext->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);

	if (drawn)
	{
		context->SetInputLayout(nullptr);
		context->SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->SetVS(vertexShader);
		context->SetPS(pixelShader);
		context->SetRasterizer(rasterizer);
		context->SetBlend(nullptr);
		context->SetDepthStencil(depthStencil);
		context->SetPSResource(0, ringView);
		context->SetPSConstants(0, constants);
		for (u32 i = 0; i < jobCount; i++)
		{
			const Job& job = jobs[i];
			if (job.target == nullptr)
				continue;
			deviceContext->UpdateSubresource(constants, 0, nullptr, job.pc, 0, 0);
			context->SetTargets(job.target, nullptr);
			context->SetViewport((float)(job.pc[3] & 0xFFFF), (float)(job.pc[3] >> 16));
			deviceContext->Draw(3, 0);
		}
		context->SetTargets(nullptr, nullptr);
		context->SetPSResource(0, nullptr);
	}
	jobCount = 0;
}
