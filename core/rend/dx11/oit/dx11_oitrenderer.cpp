#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
/* The per-pixel renderer: the translucent polygons' pixels are put in
 * lists, sorted by depth and blended at the end, as the Vulkan per-pixel
 * renderer does it (vulkan/oit/oit_drawer.cpp), step for step:
 *
 *  - the opaque and punch-through polygons' depth, and the modifier
 *    volumes into the stencil;
 *  - their colour, where their depth is the one left, darkened where the
 *    stencil says they are in a volume;
 *  - the translucent polygons' pixels into the lists (or, in a list that
 *    is not to be sorted, drawn as they come);
 *  - the translucent modifier volumes, marking the pixels in the lists;
 *  - the lists put together over the colour, into the picture - or, with
 *    a render pass after this one, into a picture the next pass draws on.
 *
 * The rest is the per-triangle renderer's: the textures, renders to
 * textures, the screen's pictures, the last picture. */
#include "../dx11_renderer.h"
#include "dx11_oitshaders.h"
#include "dx11_oitbuffers.h"
#include "hw/pvr/ta.h"
#include <algorithm>

static const D3D11_INPUT_ELEMENT_DESC OITLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    2, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    3, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, spc1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	// the polygon's number, from the numbers at vertex slot 2: the draw's first instance's
	{ "POLYNUMBER", 0, DXGI_FORMAT_R32_UINT,      2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
};

class DX11OITRenderer final : public DX11Renderer
{
public:
	bool Init() override
	{
		DX11Context *context = DX11Context::Instance();
		if (!context->HasPerPixel())
		{
			/* (the per-triangle renderer is set up instead) */
			WARN_LOG(RENDERER, "DX11: the per-pixel renderer needs feature level 11.0 and a context that can be read back from");
			return false;
		}
		if (!DX11Renderer::Init())
			return false;

		DX11Lock lock;
		oitShaders.Init(device, context->GetCompiler());
		bool success = oitShaders.GetVertexShader(true) != nullptr && oitShaders.GetFinalPixelShader() != nullptr;
		ID3DBlob *blob = oitShaders.GetVertexShaderBlob();
		success = success && blob != nullptr
				&& SUCCEEDED(device->CreateInputLayout(OITLayout, ARRAY_SIZE(OITLayout), blob->GetBufferPointer(), blob->GetBufferSize(), oitInputLayout.put()));

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = (sizeof(OITPolyConstants) + 15) & ~15u;
		// (written whole with UpdateSubresource(), see SetConstants())
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		success = success && SUCCEEDED(device->CreateBuffer(&desc, nullptr, oitConstants.put()));

		success = success && buffers.Init(device);
		lastConstantsValid = false;
		if (!success)
		{
			WARN_LOG(RENDERER, "DX11 per-pixel renderer initialization failed");
			Term();
			return false;
		}
		return true;
	}

	void Term() override
	{
		{
			DX11Lock lock;
			trPolyParamsView.reset();
			trPolyParams.reset();
			trPolyParamsSize = 0;
			polyNumbers.reset();
			polyNumbersCount = 0;
			for (auto& picture : pictures)
				picture = Picture();
			picturesWidth = picturesHeight = 0;
			buffers.Term();
			oitConstants.reset();
			oitInputLayout.reset();
			oitShaders.Term();
		}
		DX11Renderer::Term();
	}

protected:
	bool BindsScreen() const override { return false; }
	ID3D11InputLayout *PolyInputLayout() const override { return oitInputLayout; }
	ID3D11Buffer *PolyConstantsBuffer() const override { return oitConstants; }
	void DrawFrame() override;

private:
	struct OITPolyConstants
	{
		float clipTest[4];
		int blend_mode0[4];
		float trilinearAlpha;
		int pp_Number;
		float palette_index;
		int padding0;

		// two volume mode
		int blend_mode1[4];
		int shading_instr0;
		int shading_instr1;
		int fog_control0;
		int fog_control1;
		int use_alpha0;
		int use_alpha1;
		int ignore_tex_alpha0;
		int ignore_tex_alpha1;
	};
	struct Picture
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> target;
		ComPtr<ID3D11ShaderResourceView> view;
	};

	bool MakePictures(u32 width, u32 height);
	bool UploadTrPolyParams();
	void BindOIT(ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
	{
		buffers.Bind(context, target, depth);
	}
	/* the depth buffer's depth or stencil, for the shaders to read */
	void SetT4(ID3D11ShaderResourceView *view)
	{
		context->SetPSResource(4, view);
	}
	void DrawPoly(u32 listType, bool autosort, DX11OITShaders::Pass pass, const PolyParam& poly, u32 first, u32 count);
	void DrawList(u32 listType, bool autosort, DX11OITShaders::Pass pass, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModifierVolumes(bool translucent, int first, int count, const ModifierVolumeParam *params);
	void Resolve(ID3D11RenderTargetView *target, ID3D11ShaderResourceView *opaque);

	DX11OITShaders oitShaders;
	DX11OITBuffers buffers;
	ComPtr<ID3D11InputLayout> oitInputLayout;
	ComPtr<ID3D11Buffer> oitConstants;
	OITPolyConstants lastConstants;
	bool lastConstantsValid = false;
	ComPtr<ID3D11Buffer> trPolyParams;
	ComPtr<ID3D11ShaderResourceView> trPolyParamsView;
	u32 trPolyParamsSize = 0;
	/* The numbers 0, 1, 2... as many as there are translucent polygons:
	 * a polygon's number is read from here as the instance it is drawn as,
	 * and its constants are then the same as the polygon's before more
	 * often than not */
	ComPtr<ID3D11Buffer> polyNumbers;
	u32 polyNumbersCount = 0;
	std::vector<u32> trPolyParamsData;
	/* What the opaque and punch-through polygons are drawn into, two of
	 * them: with several render passes, each draws on what the last one
	 * put together */
	Picture pictures[2];
	u32 picturesWidth = 0;
	u32 picturesHeight = 0;
};

bool DX11OITRenderer::MakePictures(u32 width, u32 height)
{
	if (pixel_buffer_size != buffers.GetSize() && !buffers.Init(device))
		return false;
	if (!buffers.Resize(width, height))
		return false;
	if (pictures[0].texture && width <= picturesWidth && height <= picturesHeight)
		return true;
	width = std::max(width, picturesWidth);
	height = std::max(height, picturesHeight);
	picturesWidth = picturesHeight = 0;
	for (auto& picture : pictures)
	{
		picture = Picture();
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, picture.texture.put()))
				|| FAILED(device->CreateRenderTargetView(picture.texture, nullptr, picture.target.put()))
				|| FAILED(device->CreateShaderResourceView(picture.texture, nullptr, picture.view.put())))
		{
			WARN_LOG(RENDERER, "DX11: per-pixel renderer picture creation failed (%d x %d)", width, height);
			picture = Picture();
			return false;
		}
	}
	picturesWidth = width;
	picturesHeight = height;
	return EnsureDepth(width, height);
}

/* What the shaders that put the lists together need to know of each
 * translucent polygon: its TSP, depth function and mask, whether it takes
 * shadows, and its second volume's TSP */
bool DX11OITRenderer::UploadTrPolyParams()
{
	const int count = pvrrc.global_param_tr.used();
	trPolyParamsData.resize(std::max(count, 1) * 2);
	trPolyParamsData[0] = trPolyParamsData[1] = 0;
	const PolyParam *pp = pvrrc.global_param_tr.head();
	for (int i = 0; i < count; i++, pp++)
	{
		trPolyParamsData[i * 2] = (pp->tsp.full & 0xffff00c0) | ((pp->isp.full >> 16) & 0xe400) | ((pp->pcw.full >> 7) & 1);
		trPolyParamsData[i * 2 + 1] = pp->tsp1.full;
	}
	const u32 size = (u32)(trPolyParamsData.size() * sizeof(u32));
	if (!trPolyParams || trPolyParamsSize < size)
	{
		trPolyParamsView.reset();
		trPolyParams.reset();
		trPolyParamsSize = 0;
		u32 newSize = 4096;
		while (newSize < size)
			newSize *= 2;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = newSize;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(u32) * 2;	// sizeof(struct PolyParam)
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_UNKNOWN;
		view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		view.Buffer.NumElements = newSize / desc.StructureByteStride;
		if (FAILED(device->CreateBuffer(&desc, nullptr, trPolyParams.put()))
				|| FAILED(device->CreateShaderResourceView(trPolyParams, &view, trPolyParamsView.put())))
		{
			WARN_LOG(RENDERER, "DX11: translucent polygon parameters buffer creation failed");
			trPolyParamsView.reset();
			trPolyParams.reset();
			return false;
		}
		trPolyParamsSize = newSize;
	}
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(deviceContext->Map(trPolyParams, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, trPolyParamsData.data(), size);
	deviceContext->Unmap(trPolyParams, 0);
	context->SetPSResource(5, trPolyParamsView);

	if (!polyNumbers || polyNumbersCount < (u32)count)
	{
		polyNumbers.reset();
		polyNumbersCount = 0;
		u32 newCount = 4096;
		while (newCount < (u32)count)
			newCount *= 2;
		std::vector<u32> numbers(newCount);
		for (u32 i = 0; i < newCount; i++)
			numbers[i] = i;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = newCount * sizeof(u32);
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = numbers.data();
		if (FAILED(device->CreateBuffer(&desc, &initial, polyNumbers.put())))
		{
			WARN_LOG(RENDERER, "DX11: polygon number buffer creation failed");
			polyNumbers.reset();
			return false;
		}
		polyNumbersCount = newCount;
	}
	context->SetVertexBuffer(polyNumbers, sizeof(u32), 2);
	return true;
}

void DX11OITRenderer::DrawPoly(u32 listType, bool autosort, DX11OITShaders::Pass pass, const PolyParam& poly, u32 first, u32 count)
{
	D3D11_RECT scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	if (tileClip == TileClipping::Outside)
		SetScissor(scissorRect);
	else
		SetScissor(baseScissor);

	/* The depth pass of a polygon whose pixels are kept whatever their
	 * colour - any but a punch-through one, which is tested on its alpha -
	 * writes the depth and the stencil only: what only the colour is made
	 * from is not needed for it, the texture and the constants but for a
	 * clip test inside the box. (The shader is the one the polygon has,
	 * all the same: the colour pass draws it where its depth is the one
	 * this pass wrote, to the last bit, and another shader is another
	 * compiler's work, which need not come out the same to the last bit.) */
	const bool depthOnly = pass == DX11OITShaders::Depth && listType != ListType_Punch_Through;
	const bool insideClipTest = (poly.tileclip >> 28) == 3;

	float trilinearAlpha = 1.f;
	if (poly.tsp.FilterMode > 1 && poly.pcw.Texture && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1)
	{
		trilinearAlpha = 0.25f * (poly.tsp.MipMapD & 0x3);
		if (poly.tsp.FilterMode == 2)
			// Trilinear pass A
			trilinearAlpha = 1.f - trilinearAlpha;
	}

	const bool twoVolumes = poly.tsp1.full != (u32)-1 || poly.tcw1.full != (u32)-1;
	const bool keepAll = twoVolumes || poly.tsp.SrcSelect;
	const bool palette = BaseTextureCacheData::IsGpuHandledPaletted(poly.tsp, poly.tcw);
	float palette_index = 0.f;
	if (palette)
	{
		if (poly.tcw.PixelFmt == PixelPal4)
			palette_index = (float(poly.tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			palette_index = (float((poly.tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}

	OITPolyConstants constants = {};
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	// The per-pixel pass drops a fragment whose blend would leave the
	// pixel as it is. It cannot tell that when the source is the second
	// accumulation buffer, or when the polygon has a second volume and
	// which blend applies is known only when the lists are put
	// together: those get a blend (8) that none of its cases take.
	constants.blend_mode0[0] = keepAll ? 8 : (int)poly.tsp.SrcInstr;
	constants.blend_mode0[1] = keepAll ? 8 : (int)poly.tsp.DstInstr;
	constants.trilinearAlpha = trilinearAlpha;
	// (pp_Number is not used: the instance's number is the polygon's)
	constants.palette_index = palette_index;
	if (twoVolumes)
	{
		constants.blend_mode1[0] = (int)poly.tsp1.SrcInstr;
		constants.blend_mode1[1] = (int)poly.tsp1.DstInstr;
		constants.shading_instr0 = poly.tsp.ShadInstr;
		constants.shading_instr1 = poly.tsp1.ShadInstr;
		constants.fog_control0 = poly.tsp.FogCtrl;
		constants.fog_control1 = poly.tsp1.FogCtrl;
		constants.use_alpha0 = poly.tsp.UseAlpha;
		constants.use_alpha1 = poly.tsp1.UseAlpha;
		constants.ignore_tex_alpha0 = poly.tsp.IgnoreTexA;
		constants.ignore_tex_alpha1 = poly.tsp1.IgnoreTexA;
	}
	if ((!depthOnly || insideClipTest)
			&& (!lastConstantsValid || memcmp(&constants, &lastConstants, sizeof(constants))))
	{
		SetConstants(oitConstants.get(), constants);
		lastConstants = constants;
		lastConstantsValid = true;
	}

	if (poly.pcw.Texture && !depthOnly)
	{
		if (poly.texid != 0)
			SetTexture(0, ((DX11Texture *)(uintptr_t)poly.texid)->textureView, samplers->GetSampler(poly.tsp));
		if (twoVolumes && poly.texid1 != 0)
			SetTexture(3, ((DX11Texture *)(uintptr_t)poly.texid1)->textureView, samplers->GetSampler(poly.tsp1));
	}

	DX11OITShaders::FragmentShaderParams params = {};
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = poly.tcw.PixelFmt == PixelBumpMap;
	params.clamping = poly.tsp.ColorClamp;
	params.insideClipTest = (poly.tileclip >> 28) == 3;
	params.fog = poly.tsp.FogCtrl;
	params.gouraud = poly.pcw.Gouraud;
	params.ignoreTexAlpha = poly.tsp.IgnoreTexA || poly.tcw.PixelFmt == Pixel565;
	params.offset = poly.pcw.Offset;
	params.shaderInstr = poly.tsp.ShadInstr;
	params.texture = poly.pcw.Texture;
	params.useAlpha = poly.tsp.UseAlpha;
	params.pass = pass;
	params.twoVolume = twoVolumes;
	params.palette = palette;
	SetVS(oitShaders.GetVertexShader(poly.pcw.Gouraud));
	SetPS(oitShaders.GetPixelShader(params));

	// Depth and stencil
	int depthFunc;
	if (pass == DX11OITShaders::Color && !poly.isp.ZWriteDis && listType != ListType_Translucent)
		depthFunc = 2;	// equal
	else if (listType == ListType_Punch_Through || autosort)
		depthFunc = 6;	// greater or equal
	else
		depthFunc = poly.isp.DepthMode;
	/* Depth is written in the depth pass. In the colour pass opaque and
	 * punch-through polygons are drawn where their depth is the one the
	 * depth pass left, and translucent ones that are sorted only into the
	 * lists; the translucent polygons that are not sorted are drawn there,
	 * in order, each tested against the depth the ones before it wrote, so
	 * they still write. */
	const bool colourPassWrites = pass == DX11OITShaders::Color && !autosort && listType == ListType_Translucent;
	bool depthWrite;
	if (pass != DX11OITShaders::Depth && !colourPassWrites)
		depthWrite = false;
	// Z Write Disable seems to be ignored for punch-through.
	// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
	else if (listType == ListType_Punch_Through)
		depthWrite = true;
	else
		depthWrite = !poly.isp.ZWriteDis;

	if (pass == DX11OITShaders::Depth && (listType == ListType_Opaque || listType == ListType_Punch_Through))
		SetDepthStencil(depthStencilStates.Get(true, depthWrite, depthFunc, DX11Stencil::Shadow), poly.pcw.Shadow != 0 ? 0x80 : 0);
	else
		SetDepthStencil(depthStencilStates.Get(true, depthWrite, depthFunc));

	// Apparently punch-through polys support blending, or at least some combinations
	if (listType == ListType_Punch_Through || pass == DX11OITShaders::Color)
		SetBlend(blendStates->Get(true, poly.tsp.SrcInstr, poly.tsp.DstInstr));
	else
		SetBlend(blendStates->Get(false, 1, 0, false));

	SetCullMode(poly.isp.CullMode);
	SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	const UINT polyNumber = listType == ListType_Translucent ? (UINT)(&poly - pvrrc.global_param_tr.head()) : 0;
	deviceContext->DrawIndexedInstanced(count, 1, indexBase + first, vertexBase, polyNumber);
}

void DX11OITRenderer::DrawList(u32 listType, bool autosort, DX11OITShaders::Pass pass, const List<PolyParam>& polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (pp->count > 2)
			DrawPoly(listType, autosort, pass, *pp, pp->first, pp->count);
}

void DX11OITRenderer::DrawModifierVolumes(bool translucent, int first, int count, const ModifierVolumeParam *modVolParams)
{
	if (count == 0 || !haveModVols)
		return;

	// (the modifier volumes' triangles are at vertex slot 1)
	context->SetInputLayout(modVolInputLayout);
	SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	SetVS(oitShaders.GetModVolVertexShader());
	SetBlend(blendStates->Get(false, 1, 0, false));
	if (translucent)
		// (the lists are marked by the shaders, and nothing else is tested or written)
		SetDepthStencil(depthStencilStates.Get(false, false, 7));
	else
		SetPS(oitShaders.GetModVolPixelShader());
	SetScissor(baseScissor);

	const ModifierVolumeParam* params = &modVolParams[first];
	int mod_base = -1;

	for (int cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		{
			D3D11_RECT clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				SetScissor(clipRect);
			else
				SetScissor(baseScissor);
		}

		if (mod_base == -1)
			mod_base = param.first;

		SetCullMode(param.isp.CullMode);
		if (!param.isp.VolumeLast && mv_mode > 0)
		{
			// OR'ing (open volume or quad)
			if (translucent)
				SetPS(oitShaders.GetTrModVolPixelShader(DX11OITShaders::Or));
			else
				SetDepthStencil(depthStencilStates.Get(true, false, 4, DX11Stencil::Or), 2);
		}
		else
		{
			// XOR'ing (closed volume)
			if (translucent)
				SetPS(oitShaders.GetTrModVolPixelShader(DX11OITShaders::Xor));
			else
				SetDepthStencil(depthStencilStates.Get(true, false, 4, DX11Stencil::Xor), 2);
		}
		deviceContext->Draw(param.count * 3, modVolBase + param.first * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			if (translucent)
				SetPS(oitShaders.GetTrModVolPixelShader(mv_mode == 1 ? DX11OITShaders::Inclusion : DX11OITShaders::Exclusion));
			else
				SetDepthStencil(depthStencilStates.Get(false, false, 7, mv_mode == 1 ? DX11Stencil::Inclusion : DX11Stencil::Exclusion), 1);
			deviceContext->Draw((param.first + param.count - mod_base) * 3, modVolBase + mod_base * 3);
			mod_base = -1;
		}
	}
	SetScissor(baseScissor);
	context->SetInputLayout(oitInputLayout);
}

/* The lists put together over the opaque polygons' picture, into @target:
 * the whole picture, not the part the game clips its drawing to - what is
 * outside that has to be written too */
void DX11OITRenderer::Resolve(ID3D11RenderTargetView *target, ID3D11ShaderResourceView *opaque)
{
	BindOIT(target, nullptr);
	SetTexture(0, opaque, samplers->GetPointSampler());
	SetVS(oitShaders.GetFinalVertexShader());
	SetPS(oitShaders.GetFinalPixelShader());
	SetBlend(blendStates->Get(false));
	SetDepthStencil(depthStencilStates.Get(false, false, 7));
	SetCullMode(0);
	SetScissor(D3D11_RECT{ 0, 0, (LONG)targetWidth, (LONG)targetHeight });
	SetTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->SetInputLayout(nullptr);
	deviceContext->Draw(4, 0);
	// (the picture read from is drawn into next)
	context->SetPSResource(0, nullptr);
	context->SetInputLayout(oitInputLayout);
}

void DX11OITRenderer::DrawFrame()
{
	if (!MakePictures(targetWidth, targetHeight) || !UploadTrPolyParams() || !buffers.IsValid())
		return;
	context->SetViewport((float)targetWidth, (float)targetHeight);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	float clear[4] = { 0.f, 0.f, 0.f, 1.f };
	if (pvrrc.clearFramebuffer)
	{
		clear[0] = VO_BORDER_COL.Red / 255.f;
		clear[1] = VO_BORDER_COL.Green / 255.f;
		clear[2] = VO_BORDER_COL.Blue / 255.f;
	}
	int cur = 0;
	deviceContext->ClearRenderTargetView(pictures[cur].target, clear);
	if (pvrrc.isRTT)
		// (what is outside the pixel clip, as the per-triangle renderer leaves it)
		deviceContext->ClearRenderTargetView(targetView, clear);
	deviceContext->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
	SetT4(nullptr);

	RenderPass previous_pass = {};
	const int render_pass_count = pvrrc.render_passes.used();
	for (int render_pass = 0; render_pass < render_pass_count; render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d", render_pass + 1,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.mvo_count - previous_pass.mvo_count,
				current_pass.mvo_tr_count - previous_pass.mvo_tr_count, current_pass.autosort);

		const bool finalPass = render_pass == render_pass_count - 1;
		if (render_pass != 0)
			// (each pass has its own volumes)
			deviceContext->ClearDepthStencilView(depthView, D3D11_CLEAR_STENCIL, 0.f, 0);

		// Depth and stencil. The pixel counter is reset: the lists are
		// empty, and the buffer is filled from its start.
		SetT4(nullptr);
		buffers.ResetCount(deviceContext);
		BindOIT(pictures[cur].target, depthView);
		DrawList(ListType_Opaque, false, DX11OITShaders::Depth, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(ListType_Punch_Through, false, DX11OITShaders::Depth, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);
		DrawModifierVolumes(false, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());

		// Colour, the stencil read by the shaders
		BindOIT(pictures[cur].target, depthViewReadStencil);
		SetT4(stencilSRV);

		/* A render that covers only part of the screen leaves the rest as
		 * it was: the last picture goes in first, under everything */
		if (render_pass == 0 && !pvrrc.isRTT && KeepsLastPicture())
		{
			DrawLastPicture(targetWidth, targetHeight);
			// (the quad was drawn with its own)
			context->SetViewport((float)targetWidth, (float)targetHeight);
			context->SetInputLayout(oitInputLayout);
			BindGeometry();
		}

		// OP + PT
		DrawList(ListType_Opaque, false, DX11OITShaders::Color, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(ListType_Punch_Through, false, DX11OITShaders::Color, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);

		// TR
		/* (In the first frame after the per-pixel lists are made, too: they
		 * start empty, and that frame's translucent polygons are drawn like
		 * any other's.) */
		if (current_pass.autosort)
		{
			// into the lists, tested against the depth by the shaders as well
			SetT4(nullptr);
			BindOIT(pictures[cur].target, depthViewReadOnly);
			SetT4(depthSRV);
			DrawList(ListType_Translucent, true, DX11OITShaders::OIT, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		}
		else
			DrawList(ListType_Translucent, false, DX11OITShaders::Color, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		SetT4(nullptr);

		// The translucent modifier volumes, and the lists put together
		ID3D11RenderTargetView *target = finalPass ? targetView : pictures[cur ^ 1].target.get();
		BindOIT(target, nullptr);
		if (current_pass.mv_op_tr_shared)
			DrawModifierVolumes(true, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());
		else
			DrawModifierVolumes(true, previous_pass.mvo_tr_count, current_pass.mvo_tr_count - previous_pass.mvo_tr_count, pvrrc.global_param_mvo_tr.head());
		Resolve(target, pictures[cur].view);

		if (!finalPass)
		{
			// The translucent polygons' depth, for the passes after this one
			BindOIT(pictures[cur].target, depthView);
			DrawList(ListType_Translucent, current_pass.autosort, DX11OITShaders::Depth, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			// the next pass draws on what this one put together
			cur ^= 1;
		}

		previous_pass = current_pass;
	}
	/* (what is bound stays bound: the picture drawn into is unbound when
	 * it is handed over, and nothing that reads one binds it before
	 * binding another target) */
}

Renderer* rend_OITD3D11()
{
	return new DX11OITRenderer();
}
