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
// SSAA (DX12Renderer's notes in dx12_renderer.h): the frame drawn into
// pictures of 2, 4 or 8 samples a pixel and resolved by the shared rule
// (rend/tile/pvr_resolve.h). Its shaders are these with DX12_SSAA defined,
// and DX12_SAMPLE_RATE for a polygon's: 1, shaded at each sample, its
// inputs taken there; 2, once a pixel, its inputs at the pixel's centre
// as without SSAA (or, where it covers only some of the samples, at their
// centroid: without SSAA it would only be drawn where it covers the
// centre), and its textures filtered with the derivatives at the centres.
// Without DX12_SSAA they are what they were, to the byte. Its own:
//   DX12_SSAA_RESOLVE_PS           the samples resolved into the picture
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

#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 1
#define AT_SAMPLE sample
#else
#define AT_SAMPLE
#endif
#if !defined(DX12_GOURAUD) || DX12_GOURAUD == 1
#define INTERPOLATION AT_SAMPLE
#else
#define INTERPOLATION nointerpolation
#endif
#define PI 3.1415926

#ifdef DX12_MS_TABLE
// (the table's pictures of samples, SSAA's: read by the shaders that resolve them, which read nothing else of it)
Texture2DMS<float4> msTextures[DX12_SRV_TABLE_SIZE] : register(t0, space1);
#else
Texture2D textures[DX12_SRV_TABLE_SIZE] : register(t0, space1);
#endif
SamplerState samplers[DX12_SAMPLER_TABLE_SIZE] : register(s0, space1);
// nearest, clamped: the fog table's and the palette's
SamplerState pointSampler : register(s0);
SamplerState linearSampler : register(s1);

#if !defined(DX12_QUAD_VS) && !defined(DX12_QUAD_PS) && !defined(DX12_SSAA_RESOLVE_PS)
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
#ifdef DX12_SSAA
	uint ssaaMask;			// SSAA: every sample of a pixel, a bit each
	uint ssaaLog2;			// ...log2 of how many
#endif
};

#if defined(DX12_SSAA) && (defined(DX12_SSAA_RESOLVE_PS) || defined(DX12_OIT_FINAL_PS))
// (a unorm8 colour's bytes, red lowest, as its picture holds them, and back: exact)
uint packUnorm8(float4 v)
{
	const uint4 b = uint4(round(saturate(v) * 255.0));
	return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
float4 unpackUnorm8(uint c)
{
	return float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0;
}
#endif

#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
// (inpix's: the polygon's input there, at the pixel's centre where it covers all of its samples, else at the centroid of those it covers)
#define AT_CENTRE(v) (inpix.coverage == ssaaMask ? EvaluateAttributeSnapped(v, int2(0, 0)) : EvaluateAttributeCentroid(v))
/* ...and a texture filtered with the derivatives of its coordinates at the
 * pixels' centres, as without SSAA: where the polygon covers only some of
 * a pixel's samples its coordinates are taken at their centroid, so the
 * differences across a 2x2 block of pixels with such a pixel in it (along
 * every edge between two triangles) are not the texture's footprint, and
 * anisotropic filtering would take its texels along that wrong line - a
 * dotted line of speckles on each edge */
static float4 uvGrad;
static float4 uv1Grad;
#define TEX(t, s, coords, g) textures[t].SampleGrad(samplers[s], coords, (g).xy, (g).zw)
#else
#define AT_CENTRE(v) (v)
#define TEX(t, s, coords, g) textures[t].Sample(samplers[s], coords)
#endif
// (a flat input is not interpolated, and is not to be asked for at a place)
#if !defined(DX12_GOURAUD) || DX12_GOURAUD == 1
#define AT_CENTRE_SMOOTH(v) AT_CENTRE(v)
#else
#define AT_CENTRE_SMOOTH(v) (v)
#endif

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
#ifndef DX12_MS_TABLE
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
#endif

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
	AT_SAMPLE float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	AT_SAMPLE float2 uv : TEXCOORD0;
#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
	uint coverage : SV_Coverage;
#endif
};

float4 main(in Pixel inpix) : SV_Target
{
#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
	// (first, while nothing is discarded: derivatives are defined only then)
	if ((polyFlags & DX12_PF_TEXTURE) != 0)
	{
		const float2 c = EvaluateAttributeSnapped(inpix.uv, int2(0, 0));
		uvGrad = float4(ddx(c), ddy(c));
	}
#endif
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

	float4 color = AT_CENTRE_SMOOTH(inpix.col);
	if ((polyFlags & DX12_PF_USE_ALPHA) == 0)
		color.a = 1.0;
	float4 offset;
	if ((polyFlags & DX12_PF_SHADOWED) != 0)
	{
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= shade_scale_factor;
		offset = float4(AT_CENTRE_SMOOTH(inpix.spec).rgb * shade_scale_factor, AT_CENTRE_SMOOTH(inpix.spec).a);
	}
	else
		offset = AT_CENTRE_SMOOTH(inpix.spec);
	if ((polyFlags & DX12_PF_TEXTURE) != 0)
	{
		float4 texcol;
		if ((polyFlags & DX12_PF_PALETTE) == 0)
			texcol = TEX(texIndex, samplerIndex, AT_CENTRE(inpix.uv), uvGrad);
		else
		{
			// palette_index is where the middle of the palette's first colour is in the row of 1024
			float2 c = float2(TEX(texIndex, samplerIndex, AT_CENTRE(inpix.uv), uvGrad).r * (255.0 / 1024.0) + palette_index, 0.5);
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
#ifdef DX12_SSAA
// (SSAA: the samples' - the per-pixel renderer's table of views is the one of them then)
Texture2DMS<uint2> shadow_stencil : register(t4);
Texture2DMS<float> DepthTex : register(t6);
#else
Texture2D<uint2> shadow_stencil : register(t4);
Texture2D<float> DepthTex : register(t6);
#endif

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

#ifdef DX12_SSAA
/* SSAA: a translucent fragment is kept once a pixel, with the samples it
 * covers in the eight bits above its polygon's number (which is then under
 * 2^22) */
#define COVERAGE_SHIFT 22
uint getCoverage(Pixel pixel)
{
	return (pixel.seq_num >> COVERAGE_SHIFT) & 0xFFu;
}
uint getPolyNumber(Pixel pixel)
{
	return pixel.seq_num & 0x003FFFFFu;
}
#else
uint getPolyNumber(Pixel pixel)
{
	return pixel.seq_num & 0x3FFFFFFFu;
}
#endif

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
	precise AT_SAMPLE float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	AT_SAMPLE float2 uv : TEXCOORD0;
	INTERPOLATION float4 col1 : COLOR2;		// the second volume's, for opaque and punch-through polygons
	INTERPOLATION float4 spec1 : COLOR3;
	AT_SAMPLE float2 uv1 : TEXCOORD2;
	/* a translucent polygon's number in its list: the root constant's, and
	 * its strip's for a frame whose translucent strips are drawn by the
	 * GPU's records (the per-instance attribute) */
	nointerpolation uint polyNumber : POLYNUMBER;
#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
	uint coverage : SV_Coverage;
#elif defined(DX12_SAMPLE_RATE) && DX12_OIT_PASS == DX12_PASS_COLOR
	uint sampleIndex : SV_SampleIndex;
#endif
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
	// (an instance's: 0 but for the GPU's translucent draw records, whose StartInstanceLocation picks it)
	uint instNumber : POLYNUMBER;
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
	vo.polyNumber = polyNumber + vin.instNumber;
	return vo;
}
#endif

#ifdef DX12_OIT_PS
/* (SSAA: a pixel's stencil at the sample being shaded, or the first one
 * the polygon covers when it is shaded once a pixel: a modifier volume's
 * edge is then a pixel's, not a sample's, for it) */
#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
#define STENCIL_AT(c) shadow_stencil.Load(c, firstbitlow(inpix.coverage))
#elif defined(DX12_SAMPLE_RATE)
#define STENCIL_AT(c) shadow_stencil.Load(c, inpix.sampleIndex)
#else
#define STENCIL_AT(c) shadow_stencil[c]
#endif
#if DX12_OIT_PASS == DX12_PASS_OIT && defined(DX12_SSAA)
// ---- the lists' depth test under SSAA (tools/rend/oit_depth_test_d3d12.sh runs this text, to the next such line) ----
/* The samples of @mask at which the fragment's depth - @z at the pixel's
 * centre, @dz its differences to the next pixels in x and y (the depth is
 * a plane across the screen, so they give it at any place) - is at or in
 * front of the opaque polygons' depth (larger is nearer) at pixel @pix;
 * in @osum and @on the sum of those samples' places and their number.
 *
 * Direct3D has no post-depth coverage: SV_Coverage is the polygon's,
 * before the depth test, [earlydepthstencil] or not (the D3D11.3
 * functional spec, 16.3.2, which Direct3D 12 keeps), so the test is done
 * here, and is not quite exact: the device interpolates the depth at a
 * sample its own way, and the plane taken from the centre's and its
 * differences comes out a few units in the last place off it. Where the
 * polygon is the opaque one's plane - a translucent decal on it, drawn
 * again - a fifth of the samples would then fail at random. A depth within
 * 16 units of 2^-23 of the opaque one counts as equal, as Vulkan's
 * per-pixel renderer has it where its device has no
 * VK_EXT_post_depth_coverage (vulkan/shaders/oit_sample_depth.glsl): far
 * below anything a game puts between two polygons it means apart. */
#define OIT_DEPTH_NEAR (1.0 - 16.0 / 8388608.0)
uint oitSampleDepthTest(uint mask, int2 pix, float z, float2 dz, out float2 osum, out float on)
{
	uint seen = 0u;
	osum = float2(0.0, 0.0);
	on = 0.0;
	for (uint s = 0u; s < (1u << ssaaLog2); s++)
	{
		const float2 o = GetRenderTargetSamplePosition(s);
		if ((mask & (1u << s)) != 0u && z + dz.x * o.x + dz.y * o.y >= DepthTex.Load(pix, s) * OIT_DEPTH_NEAR)
		{
			seen |= 1u << s;
			osum += o;
			on += 1.0;
		}
	}
	return seen;
}

/* The depth the fragment is kept and sorted with: at the centroid of the
 * samples it is seen at (@seen, the sum of their places @osum and their
 * number @on), which is in the triangle; where it is seen at all of them,
 * the centre's, which that centroid is (the standard patterns are centred) */
float oitKeptDepth(uint seen, float z, float2 dz, float2 osum, float on)
{
	return seen == ssaaMask ? z : z + dot(osum / on, dz);
}
// ----
#endif

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
#if defined(DX12_SAMPLE_RATE) && DX12_SAMPLE_RATE == 2
#if DX12_OIT_PASS == DX12_PASS_OIT
	// (the depth's differences across the pixel: for its depth at each sample)
	const float dzdx = ddx_fine(inpix.pos.z);
	const float dzdy = ddy_fine(inpix.pos.z);
#endif
	// (first, while nothing is discarded: derivatives are defined only then)
	if ((polyFlags & DX12_PF_TEXTURE) != 0)
	{
		const float2 c = EvaluateAttributeSnapped(inpix.uv, int2(0, 0));
		uvGrad = float4(ddx(c), ddy(c));
		if (twoVolumes)
		{
			const float2 c1 = EvaluateAttributeSnapped(inpix.uv1, int2(0, 0));
			uv1Grad = float4(ddx(c1), ddy(c1));
		}
	}
#endif

#if DX12_OIT_PASS == DX12_PASS_OIT && defined(DX12_SSAA)
	/* Manual depth testing, at each sample the fragment covers, with its
	 * depth there, which leaves the samples it is seen at. It is kept, and
	 * sorted, with its depth at the centroid of those: the centre's,
	 * extrapolated past the triangle where the triangle only clips the
	 * pixel, would sort it in front of or behind its neighbours wrongly - a
	 * dotted line of wrong colours along the edges between a model's
	 * polygons. */
	float2 osum;
	float on;
	const uint coverage = oitSampleDepthTest(inpix.coverage, int2(inpix.pos.xy), depth, float2(dzdx, dzdy), osum, on);
	if (coverage == 0u)
		discard;
	const float fragDepth = oitKeptDepth(coverage, depth, float2(dzdx, dzdy), osum, on);
#elif DX12_OIT_PASS == DX12_PASS_OIT
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

	float4 color = AT_CENTRE_SMOOTH(inpix.col);
	float4 offset = AT_CENTRE_SMOOTH(inpix.spec);
	float2 uv = AT_CENTRE(inpix.uv);
	bool area1 = false;
	int2 cur_blend_mode = int2((oitFlags >> DX12_OF_SRC_SHIFT) & 15, (oitFlags >> DX12_OF_DST_SHIFT) & 15);
	bool cur_use_alpha = (polyFlags & DX12_PF_USE_ALPHA) != 0;
	bool cur_ignore_tex_alpha = (polyFlags & DX12_PF_IGNORE_TEX_A) != 0;
	uint cur_shading_instr = (polyFlags >> DX12_PF_SHAD_INSTR_SHIFT) & 3;
	uint cur_fog_control = (polyFlags >> DX12_PF_FOG_SHIFT) & 3;

#if DX12_OIT_PASS == DX12_PASS_COLOR
	const uint stencil = STENCIL_AT(uint2(inpix.pos.xy)).g;
	if (twoVolumes)
	{
		if (stencil == 0x81u) {
			color = AT_CENTRE_SMOOTH(inpix.col1);
			offset = AT_CENTRE_SMOOTH(inpix.spec1);
			uv = AT_CENTRE(inpix.uv1);
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
			texcol = TEX(texIndex1, samplerIndex1, uv, uv1Grad);
		else if ((polyFlags & DX12_PF_PALETTE) == 0)
			texcol = TEX(texIndex, samplerIndex, uv, uvGrad);
		else
		{
			// palette_index is where the middle of the palette's first colour is in the row of 1024
			float2 c = float2(TEX(texIndex, samplerIndex, uv, uvGrad).r * (255.0 / 1024.0) + palette_index, 0.5);
			texcol = textures[DX12_SRV_PALETTE].SampleLevel(pointSampler, c, 0);
		}
		if (bumpmap)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(AT_CENTRE_SMOOTH(inpix.spec).a + AT_CENTRE_SMOOTH(inpix.spec).r * sin(s) + AT_CENTRE_SMOOTH(inpix.spec).g * cos(s) * cos(r - 2.0 * PI * AT_CENTRE_SMOOTH(inpix.spec).b), 0.0, 1.0);
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
#ifdef DX12_SSAA
	pixel.depth = fragDepth;
#else
	pixel.depth = depth;
#endif
	pixel.seq_num = inpix.polyNumber;
#ifdef DX12_SSAA
	pixel.seq_num |= coverage << COVERAGE_SHIFT;
#endif
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

#if defined(DX12_OIT_FINAL_PS) || defined(DX12_OIT_MODVOL_PS)
/* A pixel's fragments sorted, at most. With SSAA a translucent fragment
 * is kept once a pixel with the samples it covers, so where a polygon's
 * triangles share an edge that crosses the pixel, each of them has one
 * there: small triangles - a character's - give a pixel two or three for
 * each layer, and the layers kept are three times as many. */
#ifdef DX12_SSAA
#define OIT_MAX_PIXELS (3 * DX12_OIT_MAX_PIXELS)
#else
#define OIT_MAX_PIXELS DX12_OIT_MAX_PIXELS
#endif
#endif

#ifdef DX12_OIT_FINAL_PS
static uint pixel_list[OIT_MAX_PIXELS];

int fillAndSortFragmentArray(uint2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = abufferPointers[coords];
	int count = 0;
	for (; idx != EOL && count < OIT_MAX_PIXELS; count++)
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

#ifdef DX12_SSAA
/* Blend the @num_frag sorted fragments back-to-front onto @finalColor:
 * those that cover sample @sampleBit */
float4 blendFragments(int num_frag, float4 finalColor, uint sampleBit)
#else
// Blend fragments back-to-front, onto the opaque polygons' picture (at texIndex of the table)
float4 resolveAlphaBlend(uint2 coords)
#endif
{
#ifndef DX12_SSAA
	// Copy and sort fragments into a local array
	int num_frag = fillAndSortFragmentArray(coords);

	float4 finalColor = textures[texIndex].Load(int3(coords, 0));
#endif
	float4 secondaryBuffer = 0.0; // Secondary accumulation buffer

	for (int i = 0; i < num_frag; i++)
	{
		const Pixel pixel = PixelBuffer[pixel_list[i]];
#ifdef DX12_SSAA
		if ((getCoverage(pixel) & sampleBit) == 0u)
			continue;
#endif
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

#if defined(DX12_SSAA) && !defined(DX12_OIT_PER_SAMPLE)
/* SSAA, into the picture: the list sorted once, put together at each
 * sample onto that sample's opaque colour (at texIndex of the table), and
 * the samples resolved by the shared rule (rend/tile/pvr_resolve.h): each
 * as the unorm8 its picture holds, (sum + N/2) / N per channel */
float4 main(float4 pos : SV_Position) : SV_Target
{
	uint2 coords = uint2(pos.xy);
	int num_frag = fillAndSortFragmentArray(coords);
	uint rb = 0u;
	uint ga = 0u;
	for (uint s = 0u; s < (1u << ssaaLog2); s++)
	{
		uint c = packUnorm8(blendFragments(num_frag, msTextures[texIndex].Load(int2(coords), s), 1u << s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
	// The lists start empty for the next pass, or the next frame
	abufferPointers[coords] = EOL;
	return unpackUnorm8(pvr_resolve_join(rb, ga, ssaaLog2));
}
#elif defined(DX12_SSAA)
/* ...and, of a render pass that is not the last, each sample put together
 * into the samples the next one draws over. (The lists are emptied after
 * it, for all the samples of a pixel read them: DX12OITRenderer::Resolve().) */
float4 main(float4 pos : SV_Position, uint sampleIndex : SV_SampleIndex) : SV_Target
{
	uint2 coords = uint2(pos.xy);
	return blendFragments(fillAndSortFragmentArray(coords), msTextures[texIndex].Load(int2(coords), sampleIndex), 1u << sampleIndex);
}
#else
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
#endif

#ifdef DX12_OIT_MODVOL_PS
// A translucent modifier volume: the pixels of the lists it covers marked,
// by its mode (oitFlags, DX12_OF_MV_). Nothing is written to the target.
#ifdef DX12_SSAA
void main(in float4 pos : SV_POSITION, in uint coverage : SV_Coverage)
#else
void main(in float4 pos : SV_POSITION)
#endif
{
#ifdef DX12_SSAA
	/* Once a pixel, where the volume covers the pixel's first sample: two
	 * of its triangles that share an edge cover each sample once between
	 * them, but may both have a fragment at a pixel the edge crosses */
	if ((coverage & 1u) == 0u)
		return;
#endif
	const uint mode = oitFlags & 3;
	const float depth = pos.z;
	uint2 coords = uint2(pos.xy);

	uint idx = abufferPointers[coords];
	int list_len = 0;
	while (idx != EOL && list_len < OIT_MAX_PIXELS)
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

#ifdef DX12_SSAA_RESOLVE_PS
/* SSAA's resolve: the picture of samples at resolveTex of the table into
 * the picture bound, a pixel each, by the shared rule
 * (rend/tile/pvr_resolve.h): each sample as the unorm8 its picture holds,
 * (sum + N/2) / N per channel. Drawn with DX12_OIT_FINAL_VS. */
cbuffer ResolveConstants : register(b1)
{
	uint resolveTex;
	uint resolveLog2;
};

float4 main(float4 pos : SV_Position) : SV_Target
{
	const int2 coords = int2(pos.xy);
	uint rb = 0u;
	uint ga = 0u;
	for (uint s = 0u; s < (1u << resolveLog2); s++)
	{
		uint c = packUnorm8(msTextures[resolveTex].Load(coords, s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
	return unpackUnorm8(pvr_resolve_join(rb, ga, resolveLog2));
}
#endif
