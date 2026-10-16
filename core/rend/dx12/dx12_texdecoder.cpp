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
#include "dx12_texdecoder.h"
#include "dx12context_lr.h"
#include "dx12_texture.h"
#include "dx12_shaders.h"
#include <algorithm>

// the 16-bit formats the kernel draws into, by TEXD_D3D_565, 5551, 4444
static const DXGI_FORMAT drawnFormats[3] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM };

void DX12TexDecoder::Init(DX12Context *context)
{
	Term();
	ID3D12Device *device = context->GetDevice();
	if (device == nullptr)
		return;
	this->context = context;

	takes[(int)TextureType::_8888] = context->CanWriteUAV(DXGI_FORMAT_R8G8B8A8_UINT);
	takes[(int)TextureType::_8] = context->CanWriteUAV(DXGI_FORMAT_R8_UINT);
	takes[(int)TextureType::_565] = context->IsFormatSupported(TextureType::_565) && context->CanRenderTo(DXGI_FORMAT_B5G6R5_UNORM);
	takes[(int)TextureType::_5551] = context->IsFormatSupported(TextureType::_5551) && context->CanRenderTo(DXGI_FORMAT_B5G5R5A1_UNORM);
	takes[(int)TextureType::_4444] = context->IsFormatSupported(TextureType::_4444) && context->CanRenderTo(DXGI_FORMAT_B4G4R4A4_UNORM);

	// every pipeline there is, now: none is made while a game runs
	bool ok = true;
	D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
	cs.pRootSignature = context->DecodeRootSignature();
	cs.CS = dx12_texdecode_kernel(1);
	ok = ok && SUCCEEDED(device->CreateComputePipelineState(&cs, __uuidof(ID3D12PipelineState), (void **)kernel32.put()));
	cs.CS = dx12_texdecode_kernel(2);
	ok = ok && SUCCEEDED(device->CreateComputePipelineState(&cs, __uuidof(ID3D12PipelineState), (void **)kernel8.put()));
	for (int f = 0; ok && f < 3; f++)
	{
		if (!takes[f])
			continue;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = context->DecodeRootSignature();
		desc.VS = dx12_texdecode_kernel(3);
		desc.PS = dx12_texdecode_kernel(4);
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = drawnFormats[f];
		desc.SampleDesc.Count = 1;
		ok = SUCCEEDED(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void **)drawn[f].put()));
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: the texture kernels could not be made: textures are converted on the CPU");
		Term();
		return;
	}
	jobCount = 0;
	uavHead = uavTail = 0;
	uavMarkFirst = uavMarkCount = 0;
	ready = true;
	INFO_LOG(RENDERER, "DX12: textures are made on the GPU (32-bit %d, 8-bit %d, 16-bit drawn %d%d%d)",
			takes[(int)TextureType::_8888], takes[(int)TextureType::_8],
			takes[(int)TextureType::_565], takes[(int)TextureType::_5551], takes[(int)TextureType::_4444]);
}

void DX12TexDecoder::Term()
{
	ready = false;
	jobCount = 0;
	for (auto& d : drawn)
		d.reset();
	kernel8.reset();
	kernel32.reset();
	for (bool& t : takes)
		t = false;
	context = nullptr;
}

u8 *DX12TexDecoder::Begin(u32 bytes, u32 levels, u32 *base)
{
	bytes = (bytes + 15) & ~15u;
	if (!ready || bytes > context->UploadRingSize() || levels > MAX_JOBS)
		return nullptr;
	if (jobCount + levels > MAX_JOBS)
		Flush();
	DX12Upload upload;
	if (!context->Upload(bytes, 16, upload))
		return nullptr;
	*base = (u32)(upload.offset / 4);
	statBytes += bytes;
	return upload.cpu;
}

void DX12TexDecoder::Add(DX12PooledTexture *texture, u32 level, const u32 pc[6])
{
	Job& job = jobs[jobCount++];
	job.texture = texture;
	job.level = level;
	memcpy(job.pc, pc, sizeof(u32) * 6);
	job.pc[6] = job.pc[7] = 0;
	statLevels++;
}

void DX12TexDecoder::Cancel(u32 job, u32 count, u32 serial)
{
	if (serial != this->serial)
		// recorded already, and then written over by what came after
		return;
	for (u32 i = job; i < job + count && i < jobCount; i++)
		jobs[i].texture = nullptr;
}

u32 DX12TexDecoder::TakeUav()
{
	// what the lists done have used is free again
	const u64 done = context->Completed();
	while (uavMarkCount != 0 && uavMarks[uavMarkFirst].fence <= done)
	{
		uavTail = uavMarks[uavMarkFirst].head;
		uavMarkFirst = (uavMarkFirst + 1) % UAV_MARKS;
		uavMarkCount--;
	}
	while (uavHead - uavTail >= DX12Context::UavCount)
	{
		/* every place is a list's the GPU has not done: as many lists
		 * behind as there are marks */
		if (uavMarkCount == 0)
			break;
		context->WaitFor(uavMarks[uavMarkFirst].fence);
		uavTail = uavMarks[uavMarkFirst].head;
		uavMarkFirst = (uavMarkFirst + 1) % UAV_MARKS;
		uavMarkCount--;
		context->statStalls++;
	}
	return DX12Context::UavFirst + (uavHead++ % DX12Context::UavCount);
}

void DX12TexDecoder::Flush()
{
	if (jobCount == 0)
		return;
	serial++;
	ID3D12Device *device = context->GetDevice();
	ID3D12GraphicsCommandList *cmd = context->Cmd();

	// each texture written: into the state its levels are made in
	for (u32 i = 0; i < jobCount; i++)
	{
		DX12PooledTexture *t = jobs[i].texture;
		if (t == nullptr)
			continue;
		const bool wide = ((jobs[i].pc[5] >> 6) & 3) != TEXD_OUT16;
		context->Transition(t->resource, t->state, wide ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// the compute kernels: the 32-bit levels, then the 8-bit ones
	bool drawnAny = false, computeBound = false;
	for (u32 out = TEXD_OUT32;; out = TEXD_OUT8)
	{
		bool bound = false;
		for (u32 i = 0; i < jobCount; i++)
		{
			const Job& job = jobs[i];
			if (job.texture == nullptr)
				continue;
			const u32 kind = (job.pc[5] >> 6) & 3;
			if (kind == TEXD_OUT16)
			{
				drawnAny = true;
				continue;
			}
			if (kind != out)
				continue;
			if (!computeBound)
			{
				cmd->SetComputeRootSignature(context->DecodeRootSignature());
				cmd->SetComputeRootShaderResourceView(1, context->UploadRingGpu());
				computeBound = true;
			}
			if (!bound)
			{
				cmd->SetPipelineState(out == TEXD_OUT32 ? kernel32.get() : kernel8.get());
				bound = true;
			}
			const u32 uav = TakeUav();
			D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
			view.Format = out == TEXD_OUT32 ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8_UINT;
			view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = job.level;
			device->CreateUnorderedAccessView(job.texture->resource, nullptr, &view, context->Srv().Cpu(uav));
			cmd->SetComputeRoot32BitConstants(0, 8, job.pc, 0);
			cmd->SetComputeRootDescriptorTable(2, context->Srv().Gpu(uav));
			cmd->Dispatch(((job.pc[3] & 0xFFFF) + 7) / 8, ((job.pc[3] >> 16) + 7) / 8, 1);
		}
		if (out == TEXD_OUT8)
			break;
	}
	if (computeBound)
	{
		// (the pipeline bound is not one the context knows)
		context->ForgetPipeline();
		// the places used are the list's until it is done
		if (uavMarkCount == UAV_MARKS)
		{
			// (the oldest mark merged into the next: its places freed later, not sooner)
			uavMarkFirst = (uavMarkFirst + 1) % UAV_MARKS;
			uavMarkCount--;
		}
		uavMarks[(uavMarkFirst + uavMarkCount) % UAV_MARKS] = { context->LastSubmitted() + 1, uavHead };
		uavMarkCount++;
	}

	// the 16-bit levels, drawn
	if (drawnAny)
	{
		cmd->SetGraphicsRootSignature(context->DecodeRootSignature());
		cmd->SetGraphicsRootShaderResourceView(1, context->UploadRingGpu());
		context->ForgetRaster();
		context->SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (u32 i = 0; i < jobCount; i++)
		{
			const Job& job = jobs[i];
			if (job.texture == nullptr || ((job.pc[5] >> 6) & 3) != TEXD_OUT16)
				continue;
			const u32 f = (job.pc[5] >> 20) & 3;
			if (f > 2 || !drawn[f])
				continue;
			context->SetPipeline(drawn[f]);
			const u32 rtv = context->TransientRtv();
			D3D12_RENDER_TARGET_VIEW_DESC view{};
			view.Format = drawnFormats[f];
			view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = job.level;
			device->CreateRenderTargetView(job.texture->resource, &view, context->Rtv().Cpu(rtv));
			context->SetTargets(rtv, ~0u);
			const float w = (float)(job.pc[3] & 0xFFFF), h = (float)(job.pc[3] >> 16);
			context->SetViewport(w, h);
			const D3D12_RECT scissor = { 0, 0, (LONG)w, (LONG)h };
			context->SetScissor(scissor);
			cmd->SetGraphicsRoot32BitConstants(0, 8, job.pc, 0);
			cmd->DrawInstanced(3, 1, 0, 0);
		}
		context->SetTargets(~0u, ~0u);
		context->ForgetRootConstants();
	}

	// to be sampled
	for (u32 i = 0; i < jobCount; i++)
	{
		DX12PooledTexture *t = jobs[i].texture;
		if (t != nullptr)
			context->Transition(t->resource, t->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	jobCount = 0;
}
