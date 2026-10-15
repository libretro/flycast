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
/* The per-pixel renderer's shaders. What they do is what the Vulkan
 * per-pixel renderer's GLSL does (vulkan/oit/oit_shaders.cpp), written as
 * upstream's Direct3D 11 ones are: a list of the translucent pixels of
 * each screen pixel in a buffer, put together and blended in depth order
 * at the end. */
#include "dx11_oitshaders.h"
#include <cstring>
#include <string>

#define MAX_PIXELS_PER_FRAGMENT "32"

static const char OITVertexShaderSource[] = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif

struct VertexIn
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float4 col1 : COLOR2;
	float4 spec1 : COLOR3;
	float2 uv1 : TEXCOORD2;
	// the translucent polygon's number, for the lists: the draw's instance's
	uint polyNumber : POLYNUMBER;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	noperspective float w : TEXCOORD1;
	INTERPOLATION float4 col1 : COLOR2;		// the second volume's, for opaque and punch-through polygons
	INTERPOLATION float4 spec1 : COLOR3;
	float2 uv1 : TEXCOORD2;
	nointerpolation uint polyNumber : POLYNUMBER;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = transformVertex(vin.pos, vo.w);
	vo.col = vin.col;
	vo.spec = vin.spec;
	vo.uv = vin.uv;
	vo.col1 = vin.col1;
	vo.spec1 = vin.spec1;
	vo.uv1 = vin.uv1;
	vo.polyNumber = vin.polyNumber;
	return vo;
}
)";

static const char OITShaderHeader[] = R"(
cbuffer FragmentConstants : register(b0)
{
	float4 colorClampMin;
	float4 colorClampMax;
	float4 sp_FOG_COL_RAM;
	float4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
};

Texture2D fog_table : register(t2);
SamplerState pointSampler : register(s1);

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

cbuffer PixelBufferConstants : register(b2)
{
	uint pixelBufferCount;	// how many pixels the buffer has room for
};

#define ZERO				0
#define ONE					1
#define OTHER_COLOR			2
#define INVERSE_OTHER_COLOR	3
#define SRC_ALPHA			4
#define INVERSE_SRC_ALPHA	5
#define DST_ALPHA			6
#define INVERSE_DST_ALPHA	7

uint getNextPixelIndex()
{
	uint index;
	PixelCounter.InterlockedAdd(0, 1, index);
	if (index >= pixelBufferCount)
		// Buffer overflow
		discard;
	return index;
}

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
)";

static const char OITPixelShaderSource[] = R"(
#define PI 3.1415926

#define PASS_DEPTH 0
#define PASS_COLOR 1
#define PASS_OIT 2

#if pp_TwoVolumes == 1
#define IF(x) if (x)
#else
#define IF(x)
#endif

#if pp_Gouraud == 0
#define INTERPOLATION nointerpolation
#else
#define INTERPOLATION
#endif

cbuffer PolyConstants : register(b1)
{
	float4 clipTest;
	int4 blend_mode0;
	float trilinearAlpha;
	int pp_Number;
	float palette_index;
	int padding0;

	// two volume mode
	int4 blend_mode1;
	int shading_instr0;
	int shading_instr1;
	int fog_control0;
	int fog_control1;
	int use_alpha0;
	int use_alpha1;
	int ignore_tex_alpha0;
	int ignore_tex_alpha1;
};

Texture2D tex0 : register(t0);
SamplerState sampler0 : register(s0);
Texture2D palette : register(t1);
Texture2D tex1 : register(t3);
SamplerState sampler1 : register(s3);
#if PASS == PASS_COLOR
Texture2D<uint2> shadow_stencil : register(t4);
#endif
#if PASS == PASS_OIT
Texture2D<float> DepthTex : register(t4);
#endif

float4 colorClamp(float4 col)
{
// TODO This can change in two-volume mode
#if ColorClamping == 1
	return clamp(col, colorClampMin, colorClampMax);
#else
	return col;
#endif
}

#if pp_Palette == 1
float4 palettePixel(float2 coords)
{
	// palette_index is where the middle of the palette's first colour is in the row of 1024
	float2 c = float2(tex0.Sample(sampler0, coords).r * (255.0 / 1024.0) + palette_index, 0.5);
	return palette.SampleLevel(pointSampler, c, 0);
}
#endif

struct VertexOut
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	noperspective float w : TEXCOORD1;
	INTERPOLATION float4 col1 : COLOR2;
	INTERPOLATION float4 spec1 : COLOR3;
	float2 uv1 : TEXCOORD2;
	nointerpolation uint polyNumber : POLYNUMBER;
};

struct PSO
{
	float4 col : SV_Target;
	float depth : SV_Depth;
};

PSO main(in VertexOut inpix)
{
	PSO pso;
	pso.depth = fragDepth(inpix.w);

	#if PASS == PASS_OIT
		// Manual depth testing
		float frontDepth = DepthTex[uint2(inpix.pos.xy)];
		if (pso.depth < frontDepth)
			discard;
	#endif

	// Clip inside the box
	#if pp_ClipInside == 1
		if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
				&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
			discard;
	#endif

	float4 color = inpix.col;
	float4 offset = inpix.spec;
	float2 uv = inpix.uv;
	bool area1 = false;
	int2 cur_blend_mode = blend_mode0.xy;

	#if pp_TwoVolumes == 1
		bool cur_use_alpha = use_alpha0 != 0;
		bool cur_ignore_tex_alpha = ignore_tex_alpha0 != 0;
		int cur_shading_instr = shading_instr0;
		int cur_fog_control = fog_control0;
		#if PASS == PASS_COLOR
			uint stencil = shadow_stencil[uint2(inpix.pos.xy)].g;
			if (stencil == 0x81u) {
				color = inpix.col1;
				offset = inpix.spec1;
				uv = inpix.uv1;
				area1 = true;
				cur_blend_mode = blend_mode1.xy;
				cur_use_alpha = use_alpha1 != 0;
				cur_ignore_tex_alpha = ignore_tex_alpha1 != 0;
				cur_shading_instr = shading_instr1;
				cur_fog_control = fog_control1;
			}
		#endif
	#endif

	#if PASS == PASS_COLOR && pp_TwoVolumes == 0
		// Inside a modifier volume the PowerVR2 scales what the polygon is shaded
		// with, its base and offset colours, before the texture is combined with
		// them. Scaling the combined colour instead is the same thing for a
		// modulated texture, but also darkens a decal texture, which the
		// hardware leaves alone.
		uint stencil = shadow_stencil[uint2(inpix.pos.xy)].g;
		if (stencil == 0x81u) {
			color.rgb *= shade_scale_factor;
			offset.rgb *= shade_scale_factor;
		}
	#endif
	#if pp_UseAlpha == 0 || pp_TwoVolumes == 1
		IF (!cur_use_alpha)
			color.a = 1.0;
	#endif
	#if pp_Texture == 1
	{
		float4 texcol;
		#if pp_TwoVolumes == 1
			// only the first volume's texture is ever one of palette indices
			if (area1)
				texcol = tex1.Sample(sampler1, uv);
			else
		#endif
		#if pp_Palette == 0
				texcol = tex0.Sample(sampler0, uv);
		#else
				texcol = palettePixel(uv);
		#endif
		#if pp_BumpMap == 1
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(inpix.spec.a + inpix.spec.r * sin(s) + inpix.spec.g * cos(s) * cos(r - 2.0 * PI * inpix.spec.b), 0.0, 1.0);
			texcol.rgb = float3(1.0, 1.0, 1.0);
		#else
			#if pp_IgnoreTexA == 1 || pp_TwoVolumes == 1
				IF(cur_ignore_tex_alpha)
					texcol.a = 1.0;
			#endif
		#endif
		#if pp_ShadInstr == 0 || pp_TwoVolumes == 1 // DECAL
		IF(cur_shading_instr == 0)
		{
			color = texcol;
		}
		#endif
		#if pp_ShadInstr == 1 || pp_TwoVolumes == 1 // MODULATE
		IF(cur_shading_instr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		#endif
		#if pp_ShadInstr == 2 || pp_TwoVolumes == 1 // DECAL ALPHA
		IF(cur_shading_instr == 2)
		{
			color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		}
		#endif
		#if pp_ShadInstr == 3 || pp_TwoVolumes == 1 // MODULATE ALPHA
		IF(cur_shading_instr == 3)
		{
			color *= texcol;
		}
		#endif

		#if pp_Offset == 1 && pp_BumpMap == 0
			color.rgb += offset.rgb;
		#endif
	}
	#endif

	color = colorClamp(color);

	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha, after the
	// texture has been combined in.
	#if pp_FogCtrl == 3 || pp_TwoVolumes == 1 // LUT Mode 2
		IF (cur_fog_control == 3)
			color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(inpix.w) * (256.0 / 255.0));
	#endif
	#if pp_FogCtrl == 0 || pp_TwoVolumes == 1 // LUT
		IF(cur_fog_control == 0)
		{
			float fog = fog_mode2(inpix.w);
			color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
		}
	#endif
	#if pp_Offset == 1 && pp_BumpMap == 0 && (pp_FogCtrl == 1 || pp_TwoVolumes == 1)  // Per vertex
		IF(cur_fog_control == 1)
		{
			float fog = offset.a * (255.0 / 256.0);
			color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
		}
	#endif

	color *= trilinearAlpha;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	#if cp_AlphaTest == 1
		if (cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	#endif

	#if PASS == PASS_OIT
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
		pixel.depth = pso.depth;
		pixel.seq_num = inpix.polyNumber;
		uint next;
		InterlockedExchange(abufferPointers[coords], idx, next);
		pixel.next = next;
		PixelBuffer[idx] = pixel;
		pso.col = 0.0;
	#else
		pso.col = color;
	#endif
	return pso;
}
)";

static const char OITModVolPixelShaderSource[] = R"(
struct ModVolPixel
{
	float4 pos : SV_POSITION;
	noperspective float w : TEXCOORD1;
};

float main(in ModVolPixel inpix) : SV_Depth
{
	return fragDepth(inpix.w);
}
)";

static const char OITFinalShaderSource[] = R"(
Texture2D<float4> opaqueTex : register(t0);

static uint pixel_list[MAX_PIXELS_PER_FRAGMENT];

int fillAndSortFragmentArray(uint2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = abufferPointers[coords];
	int count = 0;
	for (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)
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

// Blend fragments back-to-front
float4 resolveAlphaBlend(uint2 coords)
{
	// Copy and sort fragments into a local array
	int num_frag = fillAndSortFragmentArray(coords);

	float4 finalColor = opaqueTex.Load(int3(coords, 0));
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
)";

static const char OITFinalVertexShaderSource[] = R"(
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
	// the whole of the viewport, as a strip of two triangles
	return float4((vertexId & 2) != 0 ? 1.0 : -1.0, (vertexId & 1) != 0 ? 1.0 : -1.0, 0.0, 1.0);
}
)";

static const char OITTranslucentModvolShaderSource[] = R"(
// Must match ModVolMode
#define MV_XOR		 0
#define MV_OR		 1
#define MV_INCLUSION 2
#define MV_EXCLUSION 3

struct ModVolPixel
{
	float4 pos : SV_POSITION;
	noperspective float w : TEXCOORD1;
};

void main(in ModVolPixel inpix)
{
#if MV_MODE == MV_XOR || MV_MODE == MV_OR
	float depth = fragDepth(inpix.w);
#endif
	uint2 coords = uint2(inpix.pos.xy);

	uint idx = abufferPointers[coords];
	int list_len = 0;
	while (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)
	{
		const Pixel pixel = PixelBuffer[idx];
		const PolyParam pp = TrPolyParam[getPolyNumber(pixel)];
		if (getShadowEnable(pp))
		{
			uint prev_val;
#if MV_MODE == MV_XOR
			if (depth >= pixel.depth)
				InterlockedXor(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);
#elif MV_MODE == MV_OR
			if (depth >= pixel.depth)
				InterlockedOr(PixelBuffer[idx].seq_num, SHADOW_STENCIL, prev_val);
#elif MV_MODE == MV_INCLUSION
			InterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL), prev_val);
			// (the word as it was read above, with the shadow bit set: as the other per-pixel renderers do)
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)
				PixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;
#elif MV_MODE == MV_EXCLUSION
			InterlockedAnd(PixelBuffer[idx].seq_num, ~(SHADOW_STENCIL|SHADOW_ACC), prev_val);
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)
				PixelBuffer[idx].seq_num = pixel.seq_num | SHADOW_ACC;
#endif
		}
		idx = pixel.next;
		list_len++;
	}
}
)";

static const char * const MacroValues[] = { "0", "1", "2", "3" };

static std::string pixelSource(const char *source)
{
	/* (the fog table and the depth, from the other renderer's) */
	return std::string("#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT "\n") + OITShaderHeader + DX11FogFunction + source;
}

ID3D11VertexShader *DX11OITShaders::GetVertexShader(bool gouraud)
{
	ComPtr<ID3D11VertexShader>& shader = vertexShaders[gouraud];
	if (!shader)
	{
		const D3D_SHADER_MACRO macros[] = {
				{ "pp_Gouraud", MacroValues[gouraud] },
				{ nullptr, nullptr }
		};
		std::string source = std::string(DX11VertexTransform) + OITVertexShaderSource;
		ComPtr<ID3DBlob> blob;
		shader = CompileVS(source.c_str(), "main", "vs_5_0", macros, &blob);
		if (!vertexBlob)
			vertexBlob = blob;
	}
	return shader;
}

ID3D11PixelShader *DX11OITShaders::GetPixelShader(const FragmentShaderParams& params)
{
	const u32 key = params.hash();
	Memo& m = MemoFor(key);
	if (m.shader != nullptr && m.key == key)
		return m.shader;
	ComPtr<ID3D11PixelShader>& shader = pixelShaders[key];
	if (!shader)
	{
		const D3D_SHADER_MACRO macros[] = {
				{ "cp_AlphaTest", MacroValues[params.alphaTest] },
				{ "pp_ClipInside", MacroValues[params.insideClipTest] },
				{ "pp_UseAlpha", MacroValues[params.useAlpha] },
				{ "pp_Texture", MacroValues[params.texture] },
				{ "pp_IgnoreTexA", MacroValues[params.ignoreTexAlpha] },
				{ "pp_ShadInstr", MacroValues[params.shaderInstr & 3] },
				{ "pp_Offset", MacroValues[params.offset] },
				{ "pp_FogCtrl", MacroValues[params.fog & 3] },
				{ "pp_TwoVolumes", MacroValues[params.twoVolume] },
				{ "pp_Gouraud", MacroValues[params.gouraud] },
				{ "pp_BumpMap", MacroValues[params.bumpmap] },
				{ "ColorClamping", MacroValues[params.clamping] },
				{ "pp_Palette", MacroValues[params.palette] },
				{ "PASS", MacroValues[(int)params.pass] },
				{ nullptr, nullptr }
		};
		std::string source = pixelSource(OITPixelShaderSource);
		shader = CompilePS(source.c_str(), "main", "ps_5_0", macros);
	}
	m.key = key;
	m.shader = shader;
	return shader;
}

ID3D11VertexShader *DX11OITShaders::GetModVolVertexShader()
{
	if (!modVolVertexShader)
	{
		std::string source = std::string(DX11VertexTransform) + DX11ModVolVertexShader;
		modVolVertexShader = CompileVS(source.c_str(), "main", "vs_5_0", nullptr, &modVolVertexBlob);
	}
	return modVolVertexShader;
}

ID3D11PixelShader *DX11OITShaders::GetModVolPixelShader()
{
	if (!modVolPixelShader)
	{
		std::string source = pixelSource(OITModVolPixelShaderSource);
		modVolPixelShader = CompilePS(source.c_str(), "main", "ps_5_0", nullptr);
	}
	return modVolPixelShader;
}

ID3D11PixelShader *DX11OITShaders::GetTrModVolPixelShader(ModVolMode mode)
{
	ComPtr<ID3D11PixelShader>& shader = trModVolShaders[mode & 3];
	if (!shader)
	{
		const D3D_SHADER_MACRO macros[] = {
				{ "MV_MODE", MacroValues[mode & 3] },
				{ nullptr, nullptr }
		};
		std::string source = pixelSource(OITTranslucentModvolShaderSource);
		shader = CompilePS(source.c_str(), "main", "ps_5_0", macros);
	}
	return shader;
}

ID3D11VertexShader *DX11OITShaders::GetFinalVertexShader()
{
	if (!finalVertexShader)
		finalVertexShader = CompileVS(OITFinalVertexShaderSource, "main", "vs_5_0", nullptr);
	return finalVertexShader;
}

ID3D11PixelShader *DX11OITShaders::GetFinalPixelShader()
{
	if (!finalPixelShader)
	{
		std::string source = pixelSource(OITFinalShaderSource);
		finalPixelShader = CompilePS(source.c_str(), "main", "ps_5_0", nullptr);
	}
	return finalPixelShader;
}

void DX11OITShaders::Term()
{
	for (auto& shader : vertexShaders)
		shader.reset();
	vertexBlob.reset();
	pixelShaders.clear();
	Forget();
	modVolVertexShader.reset();
	modVolVertexBlob.reset();
	modVolPixelShader.reset();
	for (auto& shader : trModVolShaders)
		shader.reset();
	finalVertexShader.reset();
	finalPixelShader.reset();
	device = nullptr;
	compile = nullptr;
}
