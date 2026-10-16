// The Direct3D 12 per-triangle renderer's shaders: the Direct3D 11
// renderer's (dx11/dx11_shaders.cpp), made once, when the core is built,
// into DXBC (shader model 5.1) by tools/rend/kernels_dx12.sh, with
// rend/dx12/dx12_shader_flags.h and one of these in front:
//   DX12_POLY_VS, DX12_POLY_PS     a polygon (DX12_GOURAUD 0 or 1)
//   DX12_MODVOL_VS, DX12_MODVOL_PS a modifier volume, and the shadows' darkening
//   DX12_QUAD_VS, DX12_QUAD_PS     a picture drawn whole onto another
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
};
#endif

cbuffer VertexConstants : register(b0)
{
	// The normal matrix, which is never more than a scale and then a move:
	// the guest's coordinates to clip space.
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
};

// See DX11VertexTransform: a vertex's z is the guest's 1/w; the clip
// space w is handed down, with the perspective, and one over it at a
// pixel is that 1/w.
float4 transformVertex(float3 pos, out float clipW)
{
	float4 vpos = float4(pos * normalScale.xyz + normalOffset.xyz, 1.0);
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w;
	clipW = vpos.w;
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

// The depth written: the same function of 1/w as the other renderers write
float fragDepth(float w)
{
	w *= 100000.0;
	return log2(1.0 + max(w, -0.999999)) / 34.0;
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
	float4 pos : SV_POSITION;
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
#endif

#ifdef DX12_POLY_PS
struct Pixel
{
	float4 pos : SV_POSITION;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
	float clipW : TEXCOORD1;
};

struct PSO
{
	float4 col : SV_Target;
	float depth : SV_Depth;
};

PSO main(in Pixel inpix)
{
	const float w = 1.0 / inpix.clipW;
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

	PSO pso;
	pso.depth = fragDepth(w);
	pso.col = color;
	return pso;
}
#endif

#ifdef DX12_MODVOL_VS
struct VertexOut
{
	float4 pos : SV_POSITION;
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
#endif

#ifdef DX12_MODVOL_PS
struct Pixel
{
	float4 pos : SV_POSITION;
	float clipW : TEXCOORD1;
};

struct PSO
{
	float4 col : SV_Target;
	float depth : SV_Depth;
};

// The colour is only ever written by the pass that darkens what is left
// in the volumes: by what the shadows take away
PSO main(in Pixel inpix)
{
	PSO pso;
	pso.depth = fragDepth(1.0 / inpix.clipW);
	pso.col = float4(0.0, 0.0, 0.0, 1.0 - shade_scale_factor);
	return pso;
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
