// The Direct3D 12 per-triangle renderer's shaders: the Direct3D 11
// renderer's (dx11/dx11_shaders.cpp), made once, when the core is built,
// into DXBC (shader model 5.1) by tools/rend/kernels_dx12.sh, with
// rend/dx12/dx12_shader_flags.h and one of these in front:
//   DX12_POLY_VS, DX12_POLY_PS     a polygon (DX12_GOURAUD 0 or 1)
//   DX12_MODVOL_VS, DX12_MODVOL_PS a modifier volume, and the shadows' darkening
//   DX12_QUAD_VS, DX12_QUAD_PS     a picture drawn whole onto another
// and the per-pixel renderer's (oit/dx12_oitrenderer.cpp), which puts the
// translucent polygons' pixels in lists and blends them in depth order
// at the end, as the Direct3D 11 per-pixel renderer's shaders do
// (dx11/oit/dx11_oitshaders.cpp):
//   DX12_OIT_VS                    a polygon with its second volume (DX12_GOURAUD)
//   DX12_OIT_PS                    ...its pixel, in the pass DX12_OIT_PASS
//   DX12_OIT_FINAL_VS, DX12_OIT_FINAL_PS  the lists put together over the picture
//   DX12_OIT_MODVOL_PS             a translucent modifier volume marking the lists
//
// What the Direct3D 11 renderer made a pixel shader of its own for, a
// polygon's bits (#if there), is read here from the root constants, and
// branched on (if): the same operations in the same order, so what a pixel
// comes out as is the same. Only the interpolation, which changes what the
// shaders declare, is a shader of its own (DX12_GOURAUD). So no shader is
// ever compiled while the core runs, and a build of it needs no compiler.
//
// The root signature (DX12Context::MakeRootSignatures()):
//   b1        root constants: the polygon's (PolyConstants) or the quad's
//   b0        the vertex constants (vertex shader), per render
//   b2        the fragment constants (pixel shader), per render
//   t0..      space 1: every texture, by its index in the table
//   s0..      space 1: the polygons' samplers, by index
//   s0, s1    static: nearest and linear, clamped
//   u1, u2, u3, t4, t6  the per-pixel renderer's lists, heads, counter, stencil and depth
//   t5        the per-pixel renderer's translucent polygons' parameters

#if !defined(DX12_GOURAUD) || DX12_GOURAUD == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif
#define PI 3.1415926

Texture2D textures[DX12_SRV_TABLE_SIZE] : register(t0, space1);
SamplerState samplers[DX12_SAMPLER_TABLE_SIZE] : register(s0, space1);
// nearest, clamped: the fog table's and the palette's
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);

#if !defined(DX12_QUAD_VS) && !defined(DX12_QUAD_PS)
cbuffer PolyConstants : register(b1)
{
	float4 clipTest;
	float trilinearAlpha;
	float palette_index;
	float prepassAlpha;
	uint polyFlags;
	uint texIndex;
	uint samplerIndex;
	// the per-pixel renderer's: the second volume's texture and sampler,
	// the translucent polygon's number, and DX12_OF_ bits
	uint texIndex1;
	uint samplerIndex1;
	uint polyNumber;
	uint oitFlags;
};
#endif

cbuffer VertexConstants : register(b0)
{
	// The normal matrix, which is never more than a scale and then a move:
	// the guest's coordinates to clip space. normalScale.w is the depth's
	// scale: one over the frame's largest 1/w (fZ_max).
	float4 normalScale;
	float4 normalOffset;
};

cbuffer FragmentConstants : register(b2)
{
	float4 colorClampMin;
	float4 colorClampMax;
	float4 sp_FOG_COL_RAM;
	float4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
	uint pixelBufferCount;	// how many pixels the per-pixel lists have room for
};

// A vertex's z is the guest's 1/w, which is what the PowerVR compares
// depths by: the clip space position is (x * w, y * w, S, w), so that the
// depth the rasterizer makes of it, z / w, is S / w = S * (1/w) - the
// guest's own depth, scaled by S = 1 / fZ_max into (0, 1], the largest
// nearest: a reversed depth, in a float buffer, tested by the PowerVR's
// modes as they are (mode 6, greater or equal, is GREATER_EQUAL), and no
// depth is written by the pixel shaders. The same function, precise, for
// every pass, so that a pass that tests EQUAL against another's depth
// finds it. One over the clip space w at a pixel (SV_Position.w) is the
// guest's 1/w there, for the fog.
precise float4 transformVertex(float3 pos)
{
	precise float3 p = pos * normalScale.xyz + normalOffset.xyz;
	precise float w = 1.0 / p.z;
	precise float4 vpos = float4(p.x * w, p.y * w, normalScale.w, w);
	return vpos;
}

// The fog coefficient for a depth, worked out the way the PowerVR2 does
// it: see DX11FogFunction
float fog_mode2(float w)
{
	float z = min(max(w * sp_FOG_DENSITY, 1.0), 255.9999);
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(textures[DX12_SRV_FOG].SampleLevel(pointSampler, float2(u, 0.25), 0).r * 255.0 + 0.5);
	float cur = floor(textures[DX12_SRV_FOG].SampleLevel(pointSampler, float2(u, 0.75), 0).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

#ifdef DX12_POLY_VS
struct VertexIn
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	precise float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = transformVertex(vin.pos);
	vo.col = vin.col;
	vo.spec = vin.spec;
	vo.uv = vin.uv;
	return vo;
}
#endif

#ifdef DX12_POLY_PS
struct Pixel
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

float4 main(in Pixel inpix) : SV_Target
{
	// (the guest's 1/w at this pixel)
	const float w = inpix.pos.w;
	const uint shadInstr = (polyFlags >> DX12_PF_SHAD_INSTR_SHIFT) & 3;
	const uint fogCtrl = (polyFlags >> DX12_PF_FOG_SHIFT) & 3;
	const bool bumpmap = (polyFlags & DX12_PF_BUMPMAP) != 0;
	const bool offsetOn = (polyFlags & DX12_PF_OFFSET) != 0;
	// Clip inside the box
	if ((polyFlags & DX12_PF_CLIP_INSIDE) != 0)
	{
		if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
				&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
			discard;
	}

	float4 color = inpix.col;
	if ((polyFlags & DX12_PF_USE_ALPHA) == 0)
		color.a = 1.0;
	float4 offset;
	if ((polyFlags & DX12_PF_SHADOWED) != 0)
	{
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= shade_scale_factor;
		offset = float4(inpix.spec.rgb * shade_scale_factor, inpix.spec.a);
	}
	else
		offset = inpix.spec;
	if ((polyFlags & DX12_PF_TEXTURE) != 0)
	{
		float4 texcol;
		if ((polyFlags & DX12_PF_PALETTE) == 0)
			texcol = textures[texIndex].Sample(samplers[samplerIndex], inpix.uv);
		else
		{
			// palette_index is where the middle of the palette's first colour is in the row of 1024
			float2 c = float2(textures[texIndex].Sample(samplers[samplerIndex], inpix.uv).r * (255.0 / 1024.0) + palette_index, 0.5);
			texcol = textures[DX12_SRV_PALETTE].SampleLevel(pointSampler, c, 0);
		}
		if (bumpmap)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = float3(1.0, 1.0, 1.0);
		}
		else if ((polyFlags & DX12_PF_IGNORE_TEX_A) != 0)
			texcol.a = 1.0;
		if (shadInstr == 0)
			color = texcol;
		else if (shadInstr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		else if (shadInstr == 2)
			color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		else
			color *= texcol;
		if (offsetOn && !bumpmap)
			color.rgb += offset.rgb;
	}

	if ((polyFlags & DX12_PF_CLAMP) != 0)
		color = clamp(color, colorClampMin, colorClampMax);

	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha, after the
	// texture has been combined in.
	if (fogCtrl == 3)
		color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));
	else if (fogCtrl == 0)
	{
		float fog = fog_mode2(w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
	else if (fogCtrl == 1 && offsetOn && !bumpmap)
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}

	if ((polyFlags & DX12_PF_TRILINEAR) != 0)
		color *= trilinearAlpha;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if ((polyFlags & DX12_PF_ALPHA_TEST) != 0)
	{
		if (cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	}
	// The depth prepass of sorted translucent polygons (rend/sorter.h): the
	// pixels that hide what is behind them. (0 for the colours.)
	if (color.a < prepassAlpha)
		discard;
	return color;
}
#endif

#ifdef DX12_MODVOL_VS
// (a position alone: the volumes' triangles, and the polygons' vertices
// read for their position only - the depth of sorted polygons)
precise float4 main(in float3 pos : POSITION) : SV_POSITION
{
	// (behind the eye, or as good as at an infinite distance: nowhere)
	if (pos.z < 0.0 || pos.z > 3.4e37)
		return float4(0.0, 0.0, 1.0, 1.0 / pos.z);
	return transformVertex(pos);
}
#endif

#ifdef DX12_MODVOL_PS
// The colour is only ever written by the pass that darkens what is left
// in the volumes: by what the shadows take away
float4 main(in float4 pos : SV_POSITION) : SV_Target
{
	return float4(0.0, 0.0, 0.0, 1.0 - shade_scale_factor);
}
#endif

#if defined(DX12_QUAD_VS) || defined(DX12_QUAD_PS)
// The quad: no vertex buffer, its rectangle in the root constants (b1):
// x, y, w, h in clip space (y going up), then u0, v0, u1, v1 (v going
// down), then the picture's index and its flags
cbuffer QuadConstants : register(b1)
{
	float4 quadRect;
	float4 quadUV;
	uint quadTex;
	uint quadFlags;
};

struct QuadOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};
#endif

#ifdef DX12_QUAD_VS
// a strip of four: (x, y, u0, v1), (x, y + h, u0, v0), (x + w, y, u1, v1), (x + w, y + h, u1, v0)
QuadOut main(uint id : SV_VertexID)
{
	QuadOut vo;
	const float right = (id & 2) != 0 ? 1.0 : 0.0;
	const float top = (id & 1) != 0 ? 1.0 : 0.0;
	vo.pos = float4(quadRect.x + quadRect.z * right, quadRect.y + quadRect.w * top, 0.0, 1.0);
	vo.uv = float2(right != 0.0 ? quadUV.z : quadUV.x, top != 0.0 ? quadUV.y : quadUV.w);
	return vo;
}
#endif

#ifdef DX12_QUAD_PS
float4 main(in QuadOut vin) : SV_Target
{
	if ((quadFlags & DX12_QF_LINEAR) != 0)
		return textures[quadTex].Sample(linearSampler, vin.uv);
	return textures[quadTex].Sample(pointSampler, vin.uv);
}
#endif

// ---- The per-pixel renderer ----

#if defined(DX12_OIT_VS) || defined(DX12_OIT_PS) || defined(DX12_OIT_FINAL_PS) || defined(DX12_OIT_MODVOL_PS)
struct Pixel
{
	uint color;
	float depth;
	uint seq_num;
	uint next;
};
#define EOL 0xFFFFFFFFu
RWStructuredBuffer<Pixel> PixelBuffer : register(u1);
RWTexture2D<uint> abufferPointers : register(u2);
RWByteAddressBuffer PixelCounter : register(u3);
Texture2D<uint2> shadow_stencil : register(t4);
Texture2D<float> DepthTex : register(t6);

#define ZERO				0
#define ONE					1
#define OTHER_COLOR			2
#define INVERSE_OTHER_COLOR	3
#define SRC_ALPHA			4
#define INVERSE_SRC_ALPHA	5
#define DST_ALPHA			6
#define INVERSE_DST_ALPHA	7

struct PolyParam
{
	int tsp_isp_pcw;
	int tsp1;
};
StructuredBuffer<PolyParam> TrPolyParam : register(t5);

int getTsp(PolyParam pp, bool area1)
{
	return area1 ? pp.tsp1 : pp.tsp_isp_pcw;
}

int getSrcBlendFunc(PolyParam pp, bool area1)
{
	return (getTsp(pp, area1) >> 29) & 7;
}

int getDstBlendFunc(PolyParam pp, bool area1)
{
	return (getTsp(pp, area1) >> 26) & 7;
}

bool getSrcSelect(PolyParam pp, bool area1)
{
	return ((getTsp(pp, area1) >> 25) & 1) != 0;
}

bool getDstSelect(PolyParam pp, bool area1)
{
	return ((getTsp(pp, area1) >> 24) & 1) != 0;
}

bool getShadowEnable(PolyParam pp)
{
	return (pp.tsp_isp_pcw & 1) != 0;
}

uint getPolyNumber(Pixel pixel)
{
	return pixel.seq_num & 0x3FFFFFFFu;
}

#define SHADOW_STENCIL 0x40000000u
#define SHADOW_ACC	   0x80000000u

bool isShadowed(Pixel pixel)
{
	return (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC;
}

bool isTwoVolumes(PolyParam pp)
{
	return pp.tsp1 != -1;
}

uint packColors(float4 v)
{
	return (uint(round(v.r * 255.0)) << 24) | (uint(round(v.g * 255.0)) << 16) | (uint(round(v.b * 255.0)) << 8) | uint(round(v.a * 255.0));
}

float4 unpackColors(uint u)
{
	return float4(float((u >> 24) & 255u) / 255.0, float((u >> 16) & 255u) / 255.0, float((u >> 8) & 255u) / 255.0, float(u & 255u) / 255.0);
}
#endif

#if defined(DX12_OIT_VS) || defined(DX12_OIT_PS)
struct OITVertexOut
{
	precise float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	INTERPOLATION float4 col1 : COLOR2;		// the second volume's, for opaque and punch-through polygons
	INTERPOLATION float4 spec1 : COLOR3;
	float2 uv1 : TEXCOORD2;
};
#endif

#ifdef DX12_OIT_VS
struct OITVertexIn
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float4 col1 : COLOR2;
	float4 spec1 : COLOR3;
	float2 uv1 : TEXCOORD2;
};

OITVertexOut main(in OITVertexIn vin)
{
	OITVertexOut vo;
	vo.pos = transformVertex(vin.pos);
	vo.col = vin.col;
	vo.spec = vin.spec;
	vo.uv = vin.uv;
	vo.col1 = vin.col1;
	vo.spec1 = vin.spec1;
	vo.uv1 = vin.uv1;
	return vo;
}
#endif

#ifdef DX12_OIT_PS
uint getNextPixelIndex()
{
	uint index;
	PixelCounter.InterlockedAdd(0, 1, index);
	if (index >= pixelBufferCount)
		// Buffer overflow
		discard;
	return index;
}

// (the colour is not written in the depth pass or into the lists: the
// pipeline's write mask is off then)
float4 main(in OITVertexOut inpix) : SV_Target
{
	// (the guest's 1/w at this pixel, and the depth the rasterizer made of it)
	const float w = inpix.pos.w;
	const float depth = inpix.pos.z;
	const bool twoVolumes = (oitFlags & DX12_OF_TWO_VOLUMES) != 0;
	const bool bumpmap = (polyFlags & DX12_PF_BUMPMAP) != 0;
	const bool offsetOn = (polyFlags & DX12_PF_OFFSET) != 0;

#if DX12_OIT_PASS == DX12_PASS_OIT
	// Manual depth testing
	float frontDepth = DepthTex[uint2(inpix.pos.xy)];
	if (depth < frontDepth)
		discard;
#endif

	// Clip inside the box
	if ((polyFlags & DX12_PF_CLIP_INSIDE) != 0)
	{
		if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
				&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
			discard;
	}

	float4 color = inpix.col;
	float4 offset = inpix.spec;
	float2 uv = inpix.uv;
	bool area1 = false;
	int2 cur_blend_mode = int2((oitFlags >> DX12_OF_SRC_SHIFT) & 15, (oitFlags >> DX12_OF_DST_SHIFT) & 15);
	bool cur_use_alpha = (polyFlags & DX12_PF_USE_ALPHA) != 0;
	bool cur_ignore_tex_alpha = (polyFlags & DX12_PF_IGNORE_TEX_A) != 0;
	uint cur_shading_instr = (polyFlags >> DX12_PF_SHAD_INSTR_SHIFT) & 3;
	uint cur_fog_control = (polyFlags >> DX12_PF_FOG_SHIFT) & 3;

#if DX12_OIT_PASS == DX12_PASS_COLOR
	const uint stencil = shadow_stencil[uint2(inpix.pos.xy)].g;
	if (twoVolumes)
	{
		if (stencil == 0x81u) {
			color = inpix.col1;
			offset = inpix.spec1;
			uv = inpix.uv1;
			area1 = true;
			cur_blend_mode = int2((oitFlags >> DX12_OF_SRC1_SHIFT) & 15, (oitFlags >> DX12_OF_DST1_SHIFT) & 15);
			cur_use_alpha = (oitFlags & DX12_OF_USE_ALPHA1) != 0;
			cur_ignore_tex_alpha = (oitFlags & DX12_OF_IGNORE_TEX_A1) != 0;
			cur_shading_instr = (oitFlags >> DX12_OF_SHAD_INSTR1_SHIFT) & 3;
			cur_fog_control = (oitFlags >> DX12_OF_FOG1_SHIFT) & 3;
		}
	}
	else if (stencil == 0x81u)
	{
		// Inside a modifier volume the PowerVR2 scales what the polygon is shaded
		// with, its base and offset colours, before the texture is combined with
		// them. Scaling the combined colour instead is the same thing for a
		// modulated texture, but also darkens a decal texture, which the
		// hardware leaves alone.
		color.rgb *= shade_scale_factor;
		offset.rgb *= shade_scale_factor;
	}
#endif
	if (!cur_use_alpha)
		color.a = 1.0;
	if ((polyFlags & DX12_PF_TEXTURE) != 0)
	{
		float4 texcol;
		// only the first volume's texture is ever one of palette indices
		if (area1)
			texcol = textures[texIndex1].Sample(samplers[samplerIndex1], uv);
		else if ((polyFlags & DX12_PF_PALETTE) == 0)
			texcol = textures[texIndex].Sample(samplers[samplerIndex], uv);
		else
		{
			// palette_index is where the middle of the palette's first colour is in the row of 1024
			float2 c = float2(textures[texIndex].Sample(samplers[samplerIndex], uv).r * (255.0 / 1024.0) + palette_index, 0.5);
			texcol = textures[DX12_SRV_PALETTE].SampleLevel(pointSampler, c, 0);
		}
		if (bumpmap)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(inpix.spec.a + inpix.spec.r * sin(s) + inpix.spec.g * cos(s) * cos(r - 2.0 * PI * inpix.spec.b), 0.0, 1.0);
			texcol.rgb = float3(1.0, 1.0, 1.0);
		}
		else if (cur_ignore_tex_alpha)
			texcol.a = 1.0;
		if (cur_shading_instr == 0)
			color = texcol;
		else if (cur_shading_instr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		else if (cur_shading_instr == 2)
			color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		else
			color *= texcol;
		if (offsetOn && !bumpmap)
			color.rgb += offset.rgb;
	}

	if ((polyFlags & DX12_PF_CLAMP) != 0)
		color = clamp(color, colorClampMin, colorClampMax);

	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha, after the
	// texture has been combined in.
	if (cur_fog_control == 3)
		color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));
	else if (cur_fog_control == 0)
	{
		float fog = fog_mode2(w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
	else if (cur_fog_control == 1 && offsetOn && !bumpmap)
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}

	color *= trilinearAlpha;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if ((polyFlags & DX12_PF_ALPHA_TEST) != 0)
	{
		if (cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	}

#if DX12_OIT_PASS == DX12_PASS_OIT
	// Discard as many pixels as possible
	switch (cur_blend_mode.y) // DST
	{
	case ONE:
		switch (cur_blend_mode.x) // SRC
		{
			case ZERO:
				discard;
				break;
			case ONE:
			case OTHER_COLOR:
			case INVERSE_OTHER_COLOR:
				if (all(color == 0.0))
					discard;
				break;
			case SRC_ALPHA:
				if (color.a == 0.0 || all(color.rgb == 0.0))
					discard;
				break;
			case INVERSE_SRC_ALPHA:
				if (color.a == 1.0 || all(color.rgb == 0.0))
					discard;
				break;
		}
		break;
	// Destination times a white source is not left alone when the
	// fragment turns out to be in a shadow: its colour is scaled then.
	case INVERSE_OTHER_COLOR:
		if (cur_blend_mode.x <= SRC_ALPHA && all(color == 0.0))
			discard;
		break;
	case SRC_ALPHA:
		if ((cur_blend_mode.x == ZERO || cur_blend_mode.x == INVERSE_SRC_ALPHA) && color.a == 1.0)
			discard;
		break;
	case INVERSE_SRC_ALPHA:
		switch (cur_blend_mode.x) // SRC
		{
			case ZERO:
			case SRC_ALPHA:
				if (color.a == 0.0)
					discard;
				break;
			case ONE:
			case OTHER_COLOR:
			case INVERSE_OTHER_COLOR:
				if (all(color == 0.0))
					discard;
				break;
		}
		break;
	}

	uint2 coords = uint2(inpix.pos.xy);
	uint idx = getNextPixelIndex();

	Pixel pixel;
	pixel.color = packColors(clamp(color, 0.0, 1.0));
	pixel.depth = depth;
	pixel.seq_num = polyNumber;
	uint next;
	InterlockedExchange(abufferPointers[coords], idx, next);
	pixel.next = next;
	PixelBuffer[idx] = pixel;
	return 0.0;
#else
	return color;
#endif
}
#endif

#ifdef DX12_OIT_FINAL_VS
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
	// the whole of the viewport, as a strip of two triangles
	return float4((vertexId & 2) != 0 ? 1.0 : -1.0, (vertexId & 1) != 0 ? 1.0 : -1.0, 0.0, 1.0);
}
#endif

#ifdef DX12_OIT_FINAL_PS
static uint pixel_list[DX12_OIT_MAX_PIXELS];

int fillAndSortFragmentArray(uint2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = abufferPointers[coords];
	int count = 0;
	for (; idx != EOL && count < DX12_OIT_MAX_PIXELS; count++)
	{
		const Pixel p = PixelBuffer[idx];
		// Insertion sort. The entry before the first one is not looked at.
		int j = count - 1;
		while (j >= 0)
		{
			const Pixel jp = PixelBuffer[pixel_list[j]];
			if (!(jp.depth > p.depth
					|| (jp.depth == p.depth && getPolyNumber(jp) > getPolyNumber(p))))
				break;
			pixel_list[j + 1] = pixel_list[j];
			j--;
		}
		pixel_list[j + 1] = idx;
		idx = p.next;
	}
	return count;
}

// Blend fragments back-to-front, onto the opaque polygons' picture (at texIndex of the table)
float4 resolveAlphaBlend(uint2 coords)
{
	// Copy and sort fragments into a local array
	int num_frag = fillAndSortFragmentArray(coords);

	float4 finalColor = textures[texIndex].Load(int3(coords, 0));
	float4 secondaryBuffer = 0.0; // Secondary accumulation buffer

	for (int i = 0; i < num_frag; i++)
	{
		const Pixel pixel = PixelBuffer[pixel_list[i]];
		const PolyParam pp = TrPolyParam[getPolyNumber(pixel)];
		bool area1 = false;
		bool shadowed = false;
		if (isShadowed(pixel))
		{
			if (isTwoVolumes(pp))
				area1 = true;
			else
				shadowed = true;
		}
		float4 srcColor;
		if (getSrcSelect(pp, area1))
			srcColor = secondaryBuffer;
		else
		{
			srcColor = unpackColors(pixel.color);
			if (shadowed)
				srcColor.rgb *= shade_scale_factor;
		}
		float4 dstColor = getDstSelect(pp, area1) ? secondaryBuffer : finalColor;
		float4 srcCoef = 0.0;
		float4 dstCoef = 0.0;

		int srcBlend = getSrcBlendFunc(pp, area1);
		switch (srcBlend)
		{
			case ZERO:
				srcCoef = 0.0;
				break;
			case ONE:
				srcCoef = 1.0;
				break;
			case OTHER_COLOR:
				// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's
				srcCoef = dstColor;
				break;
			case INVERSE_OTHER_COLOR:
				srcCoef = 1.0 - dstColor;
				break;
			case SRC_ALPHA:
				srcCoef = srcColor.a;
				break;
			case INVERSE_SRC_ALPHA:
				srcCoef = 1.0 - srcColor.a;
				break;
			case DST_ALPHA:
				srcCoef = dstColor.a;
				break;
			case INVERSE_DST_ALPHA:
				srcCoef = 1.0 - dstColor.a;
				break;
		}
		int dstBlend = getDstBlendFunc(pp, area1);
		switch (dstBlend)
		{
			case ZERO:
				dstCoef = 0.0;
				break;
			case ONE:
				dstCoef = 1.0;
				break;
			case OTHER_COLOR:
				dstCoef = srcColor;
				break;
			case INVERSE_OTHER_COLOR:
				dstCoef = 1.0 - srcColor;
				break;
			case SRC_ALPHA:
				dstCoef = srcColor.a;
				break;
			case INVERSE_SRC_ALPHA:
				dstCoef = 1.0 - srcColor.a;
				break;
			case DST_ALPHA:
				dstCoef = dstColor.a;
				break;
			case INVERSE_DST_ALPHA:
				dstCoef = 1.0 - dstColor.a;
				break;
		}
		const float4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);
		if (getDstSelect(pp, area1))
			secondaryBuffer = result;
		else
			finalColor = result;
	}

	return finalColor;
}

float4 main(float4 pos : SV_Position) : SV_Target
{
	uint2 coords = uint2(pos.xy);
	// Compute and output final color for the frame buffer
	float4 color = resolveAlphaBlend(coords);
	// The lists start empty for the next pass, or the next frame
	abufferPointers[coords] = EOL;
	return color;
}
#endif

#ifdef DX12_OIT_MODVOL_PS
// A translucent modifier volume: the pixels of the lists it covers marked,
// by its mode (oitFlags, DX12_OF_MV_). Nothing is written to the target.
void main(in float4 pos : SV_POSITION)
{
	const uint mode = oitFlags & 3;
	const float depth = pos.z;
	uint2 coords = uint2(pos.xy);

	uint idx = abufferPointers[coords];
	int list_len = 0;
	while (idx != EOL && list_len < DX12_OIT_MAX_PIXELS)
	{
		const Pixel pixel = PixelBuffer[idx];
		const PolyParam pp = TrPolyParam[getPolyNumber(pixel)];
		if (getShadowEnable(pp))
		{
			uint prev_val;
			if (mode == DX12_OF_MV_XOR)
			{
				if (depth >= pixel.depth)
					InterlockedXor(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);
			}
			else if (mode == DX12_OF_MV_OR)
			{
				if (depth >= pixel.depth)
					InterlockedOr(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);
			}
			else if (mode == DX12_OF_MV_INCLUSION)
			{
				InterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL), prev_val);
				// (the word as it was read above, with the shadow bit set: as the other per-pixel renderers do)
				if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)
					PixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;
			}
			else
			{
				InterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL|SHADOW_ACC), prev_val);
				if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)
					PixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;
			}
		}
		idx = pixel.next;
		list_len++;
	}
}
#endif
