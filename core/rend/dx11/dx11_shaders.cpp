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
/* The per-triangle renderer's shaders: the Vulkan renderer's GLSL
 * (vulkan/shaders.cpp) in HLSL, in the shape upstream's Direct3D 11
 * shaders have. What a pixel comes out as is what the Vulkan and OpenGL
 * renderers make of it, bit for bit where the two languages allow. */
#include "dx11_shaders.h"
#include <cstring>
#include <string>

const char * const DX11VertexTransform = R"(
cbuffer VertexConstants : register(b0)
{
	// The normal matrix, which is never more than a scale and then a move:
	// the guest's coordinates to clip space.
	float4 normalScale;
	float4 normalOffset;
	// 1/fZ_max, and the inverse of what the matrix scales z by (rend/depth_scale.h)
	float4 depthScale;
};

// A vertex's z is the guest's 1/w. It is made the clip space w's inverse,
// and x and y are multiplied by w, so that the division by it leaves them
// where they are and everything in between is interpolated with the
// perspective. What the pixel shaders take for the depth (gl_FragCoord.w in
// the other renderers) is 1/w, which goes across the screen in a straight
// line. It is not handed down as it is, without the perspective: where a
// triangle is clipped (a vertex far off the screen, as a floor's or a
// train's near the camera has), what the rasterizer then makes of such a
// value is not always right: the polygon got next to no depth and was
// hidden behind the background.
// The clip space w is handed down instead, with the perspective, which
// clipping keeps right; one over it at a pixel is that 1/w.
// The depth is the position's (rend/depth_scale.h): z goes out as S, so
// that the depth the rasterizer interpolates is S*z, the guest's 1/w up to
// 1 at fZ_max, linear across the screen as the chip has it; no pixel
// shader writes a depth. This one function places the vertices of every
// shader, and the position is precise, so a pass that tests EQUAL against
// another pass's depth is exact.
float4 transformVertex(float3 pos, out float clipW)
{
	precise float4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);
	vpos.w = 1.0 / vpos.z;
	vpos.xy *= vpos.w;
	// (a z beyond fZ_max is at depth 1, not clipped)
	vpos.z = min(depthScale.x * depthScale.y, vpos.w);
	clipW = vpos.w;
	return vpos;
}
)";

static const char VertexShaderSource[] = R"(
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
};

struct VertexOut
{
	precise float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float clipW : TEXCOORD1;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = transformVertex(vin.pos, vo.clipW);
	vo.col = vin.col;
	vo.spec = vin.spec;
	vo.uv = vin.uv;
	return vo;
}
)";

const char * const DX11ModVolVertexShader = R"(
struct VertexOut
{
	precise float4 pos : SV_POSITION;
	float clipW : TEXCOORD1;
};

VertexOut main(in float3 pos : POSITION)
{
	VertexOut vo;
	// (behind the eye, or as good as at an infinite distance: nowhere)
	if (pos.z < 0.0 || pos.z > 3.4e37)
	{
		vo.pos = float4(0.0, 0.0, 1.0, 1.0 / pos.z);
		vo.clipW = vo.pos.w;
		return vo;
	}
	vo.pos = transformVertex(pos, vo.clipW);
	return vo;
}
)";

const char * const DX11FogFunction = R"(
// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
float fog_mode2(float w)
{
	// (max() and min() hand back the other operand when one is not a
	// number - an infinite depth times a density of zero - and that comes
	// out as 1 like anything else below 1)
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
	float next = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.25), 0).r * 255.0 + 0.5);
	float cur = floor(fog_table.SampleLevel(pointSampler, float2(u, 0.75), 0).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

)";

static const char PixelShaderHeader[] = R"(
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
// nearest, clamped: the fog table's and the palette's
SamplerState pointSampler : register(s1);
)";

static const char PixelShaderSource[] = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif
#define PI 3.1415926

cbuffer PolyConstants : register(b1)
{
	float4 clipTest;
	float trilinearAlpha;
	float palette_index;
	float prepassAlpha;
};

Texture2D tex : register(t0);
SamplerState texSampler : register(s0);
Texture2D palette : register(t1);

float4 colorClamp(float4 col)
{
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
	float2 c = float2(tex.Sample(texSampler, coords).r * (255.0 / 1024.0) + palette_index, 0.5);
	return palette.SampleLevel(pointSampler, c, 0);
}
#endif

struct Pixel
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float clipW : TEXCOORD1;
};

// (the depth is the position's, which the depth test took: no SV_Depth)
struct PSO
{
	float4 col : SV_Target;
};

PSO main(in Pixel inpix)
{
	const float w = 1.0 / inpix.clipW;
	// Clip inside the box
#if pp_ClipInside == 1
	if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
			&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
		discard;
#endif

	float4 color = inpix.col;
#if pp_UseAlpha == 0
	color.a = 1.0;
#endif
#if pp_Shadowed == 1
	// A pixel inside a modifier volume: what the polygon is shaded with,
	// its base and offset colours, is scaled before the texture is
	// combined with them. The texture itself is not.
	color.rgb *= shade_scale_factor;
	float4 offset = float4(inpix.spec.rgb * shade_scale_factor, inpix.spec.a);
#else
	float4 offset = inpix.spec;
#endif
#if pp_Texture == 1
	{
	#if pp_Palette == 0
		float4 texcol = tex.Sample(texSampler, inpix.uv);
	#else
		float4 texcol = palettePixel(inpix.uv);
	#endif
	#if pp_BumpMap == 1
		float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
		float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
		texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
		texcol.rgb = float3(1.0, 1.0, 1.0);
	#else
		#if pp_IgnoreTexA == 1
		texcol.a = 1.0;
		#endif
	#endif
	#if pp_ShadInstr == 0
		color = texcol;
	#endif
	#if pp_ShadInstr == 1
		color.rgb *= texcol.rgb;
		color.a = texcol.a;
	#endif
	#if pp_ShadInstr == 2
		color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
	#endif
	#if pp_ShadInstr == 3
		color *= texcol;
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
#if pp_FogCtrl == 3
	color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));
#endif
#if pp_FogCtrl == 0
	{
		float fog = fog_mode2(w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
#endif
#if pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}
#endif

#if pp_TriLinear == 1
	color *= trilinearAlpha;
#endif

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
#if cp_AlphaTest == 1
	if (cp_AlphaTestValue > color.a)
		discard;
	color.a = 1.0;
#endif
	// The depth prepass of sorted translucent polygons (rend/sorter.h): the
	// pixels that hide what is behind them. (0 for the colours.)
	if (color.a < prepassAlpha)
		discard;

	PSO pso;
	pso.col = color;
	return pso;
}
)";

static const char ModVolPixelShaderSource[] = R"(
struct Pixel
{
	float4 pos : SV_POSITION;
	float clipW : TEXCOORD1;
};

// The colour is only ever written by the pass that darkens what is left
// in the volumes: by what the shadows take away (the depth is the position's)
float4 main(in Pixel inpix) : SV_Target
{
	return float4(0.0, 0.0, 0.0, 1.0 - shade_scale_factor);
}
)";

static const char QuadVertexShaderSource[] = R"(
struct VertexIn
{
	float2 pos : POSITION;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = float4(vin.pos, 0.0, 1.0);
	vo.uv = vin.uv;
	return vo;
}
)";

static const char QuadPixelShaderSource[] = R"(
struct VertexOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

Texture2D tex : register(t0);
SamplerState texSampler : register(s0);

float4 main(in VertexOut vin) : SV_Target
{
	return tex.Sample(texSampler, vin.uv);
}
)";

ComPtr<ID3DBlob> DX11ShaderCompiler::Compile(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
		ID3DInclude *include)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;

	if (compile == nullptr)
		return blob;
	HRESULT hr = compile(source, strlen(source), nullptr, macros, include, entry, profile, 0, 0, blob.put(), errors.put());
	if (FAILED(hr))
	{
		ERROR_LOG(RENDERER, "Shader compilation failed (%x): %s", (unsigned)hr,
				errors ? (const char *)errors->GetBufferPointer() : "");
		blob.reset();
	}
	return blob;
}

ComPtr<ID3D11VertexShader> DX11ShaderCompiler::CompileVS(const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ComPtr<ID3DBlob> *blobOut, ID3DInclude *include)
{
	ComPtr<ID3D11VertexShader> shader;
	ComPtr<ID3DBlob> blob = Compile(source, entry, profile, macros, include);
	if (blob && FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put())))
		ERROR_LOG(RENDERER, "Vertex shader creation failed");
	if (blobOut != nullptr)
		*blobOut = blob;
	return shader;
}

ComPtr<ID3D11PixelShader> DX11ShaderCompiler::CompilePS(const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ID3DInclude *include)
{
	ComPtr<ID3D11PixelShader> shader;
	ComPtr<ID3DBlob> blob = Compile(source, entry, profile, macros, include);
	if (blob && FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.put())))
		ERROR_LOG(RENDERER, "Pixel shader creation failed");
	return shader;
}

static const char * const MacroValues[] = { "0", "1", "2", "3" };

ID3D11VertexShader *DX11Shaders::GetVertexShader(bool gouraud)
{
	ComPtr<ID3D11VertexShader>& shader = vertexShaders[gouraud];
	if (!shader)
	{
		const D3D_SHADER_MACRO macros[] = {
				{ "pp_Gouraud", MacroValues[gouraud] },
				{ nullptr, nullptr }
		};
		std::string source = std::string(DX11VertexTransform) + VertexShaderSource;
		ComPtr<ID3DBlob> blob;
		shader = CompileVS(source.c_str(), "main", "vs_4_0", macros, &blob);
		if (!vertexBlob)
			vertexBlob = blob;
	}
	return shader;
}

ID3D11PixelShader *DX11Shaders::GetPixelShader(const DX11FragmentShaderParams& params)
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
				{ "pp_Gouraud", MacroValues[params.gouraud] },
				{ "pp_BumpMap", MacroValues[params.bumpmap] },
				{ "ColorClamping", MacroValues[params.clamping] },
				{ "pp_TriLinear", MacroValues[params.trilinear] },
				{ "pp_Palette", MacroValues[params.palette] },
				{ "pp_Shadowed", MacroValues[params.shadowed] },
				{ nullptr, nullptr }
		};
		std::string source = std::string(PixelShaderHeader) + DX11FogFunction + PixelShaderSource;
		shader = CompilePS(source.c_str(), "main", "ps_4_0", macros);
	}
	m.key = key;
	m.shader = shader;
	return shader;
}

ID3D11VertexShader *DX11Shaders::GetModVolVertexShader()
{
	if (!modVolVertexShader)
	{
		std::string source = std::string(DX11VertexTransform) + DX11ModVolVertexShader;
		modVolVertexShader = CompileVS(source.c_str(), "main", "vs_4_0", nullptr, &modVolVertexBlob);
	}
	return modVolVertexShader;
}

ID3D11PixelShader *DX11Shaders::GetModVolPixelShader()
{
	if (!modVolPixelShader)
	{
		std::string source = std::string(PixelShaderHeader) + DX11FogFunction + ModVolPixelShaderSource;
		modVolPixelShader = CompilePS(source.c_str(), "main", "ps_4_0", nullptr);
	}
	return modVolPixelShader;
}

ID3D11VertexShader *DX11Shaders::GetQuadVertexShader()
{
	if (!quadVertexShader)
		quadVertexShader = CompileVS(QuadVertexShaderSource, "main", "vs_4_0", nullptr, &quadVertexBlob);
	return quadVertexShader;
}

ID3D11PixelShader *DX11Shaders::GetQuadPixelShader()
{
	if (!quadPixelShader)
		quadPixelShader = CompilePS(QuadPixelShaderSource, "main", "ps_4_0", nullptr);
	return quadPixelShader;
}

void DX11Shaders::Term()
{
	for (auto& shader : vertexShaders)
		shader.reset();
	vertexBlob.reset();
	pixelShaders.clear();
	Forget();
	modVolVertexShader.reset();
	modVolVertexBlob.reset();
	modVolPixelShader.reset();
	quadVertexShader.reset();
	quadVertexBlob.reset();
	quadPixelShader.reset();
	device = nullptr;
	compile = nullptr;
}
