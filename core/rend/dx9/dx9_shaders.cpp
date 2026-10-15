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
/* The Direct3D 11 renderer's shaders (dx11/dx11_shaders.cpp) in shader
 * model 3, which is what the Vulkan and OpenGL renderers do with a pixel,
 * with these differences of Direct3D 9's:
 *
 * - There is no interpolation without the perspective. What the pixel
 *   shaders take for the depth, 1/w, goes across the screen in a straight
 *   line: the vertex shader hands over its inverse, which the perspective
 *   interpolation brings to the inverse of the straight line's value.
 * - A pixel's centre is where its coordinates are whole numbers, and not
 *   half a pixel further on: the vertices are moved back by half a pixel.
 * - No depth clipping can be turned off: the clip space's z is 0, inside
 *   the clip volume, and the depth is the pixel shader's own anyway.
 * - Vertex colours are read as D3DCOLOR, blue in the first byte: the
 *   vertices have red there, and the vertex shader puts it back.
 * - Flat shading is the device's (D3DRS_SHADEMODE), as is culling. */
#include "dx9_shaders.h"
#include <cstring>
#include <string>

static const char VertexTransform[] = R"(
float4 normalScale : register(c0);
float4 normalOffset : register(c1);
float4 halfPixel : register(c2);

// A vertex's z is the guest's 1/w. It is made the clip space w's inverse,
// and x and y are multiplied by w, so that the division by it leaves them
// where they are and everything in between is interpolated with the
// perspective.
float4 transformVertex(float3 pos, out float invW)
{
	float4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);
	vpos.w = 1.0 / vpos.z;
	invW = vpos.w;
	vpos.xy = (vpos.xy + halfPixel.xy) * vpos.w;
	vpos.z = 0.0;
	return vpos;
}
)";

static const char VertexShaderSource[] = R"(
struct VertexIn
{
	float3 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float invW : TEXCOORD1;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = transformVertex(vin.pos, vo.invW);
	vo.col = vin.col.bgra;
	vo.spec = vin.spec.bgra;
	vo.uv = vin.uv;
	return vo;
}
)";

static const char ModVolVertexShaderSource[] = R"(
struct VertexOut
{
	float4 pos : POSITION;
	float invW : TEXCOORD1;
};

VertexOut main(in float3 pos : POSITION)
{
	VertexOut vo;
	// (behind the eye, or as good as at an infinite distance: nowhere)
	if (pos.z < 0.0 || pos.z > 3.4e37)
	{
		vo.pos = float4(0.0, 0.0, 0.0, 1.0 / pos.z);
		vo.invW = vo.pos.w;
		return vo;
	}
	vo.pos = transformVertex(pos, vo.invW);
	return vo;
}
)";

static const char QuadVertexShaderSource[] = R"(
float4 halfPixel : register(c2);

struct VertexIn
{
	float2 pos : POSITION;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = float4(vin.pos + halfPixel.xy, 0.0, 1.0);
	vo.uv = vin.uv;
	return vo;
}
)";

static const char PixelShaderHeader[] = R"(
float4 colorClampMin : register(c0);
float4 colorClampMax : register(c1);
float4 sp_FOG_COL_RAM : register(c2);
float4 sp_FOG_COL_VERT : register(c3);
// the alpha test's value, the fog density, the shadows' scale
float4 frameParams : register(c4);

sampler2D fog_table : register(s2);

// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
float fog_mode2(float w)
{
	float z = min(max(w * frameParams.y, 1.0), 255.9999);
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(tex2Dlod(fog_table, float4(u, 0.25, 0.0, 0.0)).a * 255.0 + 0.5);
	float cur = floor(tex2Dlod(fog_table, float4(u, 0.75, 0.0, 0.0)).a * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

// The depth written: the same function of 1/w as the other renderers write
float fragDepth(float w)
{
	w *= 100000.0;
	return log2(1.0 + max(w, -0.999999)) / 34.0;
}

struct PSO
{
	float4 col : COLOR0;
	float depth : DEPTH;
};
)";

static const char PixelShaderSource[] = R"(
#define PI 3.1415926

float4 clipTest : register(c5);
// the trilinear pass's alpha, where in the palette's row the first colour
// is, the depth prepass's least alpha (0 for the colour: rend/sorter.h)
float4 polyParams : register(c6);

// What the other renderers make a shader for: flags, 0 or 1, in c8 to c11
// (as constants of their own: not every compiler places booleans where
// they are asked to go). Every pixel of a polygon branches the same way.
float4 flags[4] : register(c8);
#define pp_UseAlpha (flags[0].x > 0.5)
#define pp_IgnoreTexA (flags[0].y > 0.5)
#define pp_Offset (flags[0].z > 0.5)
#define pp_BumpMap (flags[0].w > 0.5)
#define pp_ColorClamp (flags[1].x > 0.5)
#define pp_TriLinear (flags[1].y > 0.5)
#define pp_ClipInside (flags[1].z > 0.5)
#define cp_AlphaTest (flags[1].w > 0.5)
#define pp_Shadowed (flags[2].x > 0.5)
#define pp_ShadInstr0 (flags[2].y > 0.5)
#define pp_ShadInstr1 (flags[2].z > 0.5)
#define pp_Fog0 (flags[2].w > 0.5)
#define pp_Fog1 (flags[3].x > 0.5)
#define pp_SwapRB (flags[3].y > 0.5)

sampler2D tex : register(s0);
sampler2D palette : register(s1);

#if pp_Palette == 1
float4 palettePixel(float2 coords)
{
	// polyParams.y is where the middle of the palette's first colour is in the row of 1024
	float2 c = float2(tex2D(tex, coords).a * (255.0 / 1024.0) + polyParams.y, 0.5);
	// (B G R A in the texture, the palette's R G B A bytes)
	return tex2Dlod(palette, float4(c, 0.0, 0.0)).bgra;
}
#endif

struct Pixel
{
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float invW : TEXCOORD1;
	float2 vpos : VPOS;
};

PSO main(in Pixel inpix)
{
	float w = 1.0 / inpix.invW;

	// Clip inside the box (a pixel's centre, as the other renderers have it)
	if (pp_ClipInside)
	{
		float2 p = inpix.vpos + 0.5;
		if (p.x >= clipTest.x && p.x <= clipTest.z && p.y >= clipTest.y && p.y <= clipTest.w)
			discard;
	}

	float4 color = inpix.col;
	if (!pp_UseAlpha)
		color.a = 1.0;
	float4 offset = inpix.spec;
	// A pixel inside a modifier volume: what the polygon is shaded with,
	// its base and offset colours, is scaled before the texture is
	// combined with them. The texture itself is not.
	if (pp_Shadowed)
	{
		color.rgb *= frameParams.z;
		offset.rgb *= frameParams.z;
	}
#if pp_Texture == 1
	{
	#if pp_Palette == 0
		float4 texcol = tex2D(tex, inpix.uv);
		if (pp_SwapRB)
			texcol = texcol.bgra;
	#else
		float4 texcol = palettePixel(inpix.uv);
	#endif
		if (pp_BumpMap)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = float3(1.0, 1.0, 1.0);
		}
		else
		{
			if (pp_IgnoreTexA)
				texcol.a = 1.0;
		}
		if (pp_ShadInstr1)
		{
			if (pp_ShadInstr0)
				color *= texcol;
			else
				color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		}
		else
		{
			if (pp_ShadInstr0)
			{
				color.rgb *= texcol.rgb;
				color.a = texcol.a;
			}
			else
				color = texcol;
		}
		if (pp_Offset)
		{
			if (!pp_BumpMap)
				color.rgb += offset.rgb;
		}
	}
#endif

	if (pp_ColorClamp)
		color = clamp(color, colorClampMin, colorClampMax);

	if (pp_Fog1)
	{
		// Look-up table mode 2: the fog colour takes the place of the
		// pixel's colour and the fog coefficient the place of its alpha,
		// after the texture has been combined in.
		if (pp_Fog0)
			color = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));
	}
	else
	{
		if (pp_Fog0)
		{
			if (pp_Offset)
			{
				if (!pp_BumpMap)
				{
					float fog = offset.a * (255.0 / 256.0);
					color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
				}
			}
		}
		else
		{
			float fog = fog_mode2(w);
			color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
		}
	}

	if (pp_TriLinear)
		color *= polyParams.x;

	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if (cp_AlphaTest)
	{
		if (frameParams.x > color.a)
			discard;
		color.a = 1.0;
	}
	// The depth prepass of sorted translucent polygons: the pixels that
	// hide what is behind them
	if (color.a < polyParams.z)
		discard;

	PSO pso;
	pso.depth = fragDepth(w);
	pso.col = color;
	return pso;
}
)";

static const char ModVolPixelShaderSource[] = R"(
// The colour is only ever written by the pass that darkens what is left
// in the volumes: by what the shadows take away
PSO main(in float invW : TEXCOORD1)
{
	PSO pso;
	pso.depth = fragDepth(1.0 / invW);
	pso.col = float4(0.0, 0.0, 0.0, 1.0 - frameParams.z);
	return pso;
}
)";

static const char QuadPixelShaderSource[] = R"(
sampler2D tex : register(s0);
float4 quadColor : register(c7);
// (the polygons' flag pp_SwapRB)
float4 flags3 : register(c11);

float4 main(in float2 uv : TEXCOORD0) : COLOR0
{
	float4 c = tex2D(tex, uv);
	if (flags3.y > 0.5)
		c = c.bgra;
	return c * quadColor;
}
)";

static bool compileOne(pD3DCompile compile, const std::string& source, const char *profile,
		const D3D_SHADER_MACRO *macros, std::vector<DWORD>& code)
{
	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> errors;
	HRESULT hr = compile(source.c_str(), source.size(), nullptr, macros, nullptr, "main", profile,
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
	if (FAILED(hr) || !blob)
	{
		ERROR_LOG(RENDERER, "DX9: shader compilation failed (%x): %s", (unsigned)hr,
				errors ? (const char *)errors->GetBufferPointer() : "");
		return false;
	}
	const size_t size = blob->GetBufferSize();
	code.resize((size + 3) / 4);
	memcpy(code.data(), blob->GetBufferPointer(), size);
	return true;
}

bool DX9Shaders::Compile(pD3DCompile compile)
{
	if (compiled)
		return true;
	if (compile == nullptr)
		return false;
	const std::string vs = VertexTransform;
	const std::string ps = PixelShaderHeader;
	const D3D_SHADER_MACRO plain[] = { { "pp_Texture", "0" }, { "pp_Palette", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO texture[] = { { "pp_Texture", "1" }, { "pp_Palette", "0" }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO paletted[] = { { "pp_Texture", "1" }, { "pp_Palette", "1" }, { nullptr, nullptr } };

	bool ok = compileOne(compile, vs + VertexShaderSource, "vs_3_0", nullptr, vsCode[DX9VS_Main])
			&& compileOne(compile, vs + ModVolVertexShaderSource, "vs_3_0", nullptr, vsCode[DX9VS_ModVol])
			&& compileOne(compile, QuadVertexShaderSource, "vs_3_0", nullptr, vsCode[DX9VS_Quad])
			&& compileOne(compile, ps + PixelShaderSource, "ps_3_0", plain, psCode[DX9PS_Plain])
			&& compileOne(compile, ps + PixelShaderSource, "ps_3_0", texture, psCode[DX9PS_Texture])
			&& compileOne(compile, ps + PixelShaderSource, "ps_3_0", paletted, psCode[DX9PS_Palette])
			&& compileOne(compile, ps + ModVolPixelShaderSource, "ps_3_0", nullptr, psCode[DX9PS_ModVol])
			&& compileOne(compile, QuadPixelShaderSource, "ps_3_0", nullptr, psCode[DX9PS_Quad]);
	if (!ok)
	{
		for (auto& code : vsCode)
			code.clear();
		for (auto& code : psCode)
			code.clear();
		return false;
	}
	compiled = true;
	return true;
}

bool DX9Shaders::Init(IDirect3DDevice9 *device, pD3DCompile compile)
{
	Term();
	if (!Compile(compile))
		return false;
	for (int i = 0; i < DX9VS_Count; i++)
		if (FAILED(device->CreateVertexShader(vsCode[i].data(), vs[i].put())))
		{
			ERROR_LOG(RENDERER, "DX9: vertex shader creation failed");
			Term();
			return false;
		}
	for (int i = 0; i < DX9PS_Count; i++)
		if (FAILED(device->CreatePixelShader(psCode[i].data(), ps[i].put())))
		{
			ERROR_LOG(RENDERER, "DX9: pixel shader creation failed");
			Term();
			return false;
		}
	return true;
}

void DX9Shaders::Term()
{
	for (auto& shader : vs)
		shader.reset();
	for (auto& shader : ps)
		shader.reset();
}
