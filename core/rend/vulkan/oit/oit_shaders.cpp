#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Nov 6, 2019

	Copyright 2019 flyinghead

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
#include "oit_shaders.h"
#include "../compiler.h"
#include "../ssaa.h"
#include "rend/ssaa_pattern.h"

static const char OITVertexShaderSource[] = R"(#version 450

#define pp_Gouraud %d

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 normal_matrix;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;
layout (location = 1) in uvec4        in_base;
layout (location = 2) in uvec4        in_offs;
layout (location = 3) in vec2 in_uv;
layout (location = 4) in uvec4        in_base1;						// New for OIT, only for OP/PT with 2-volume
layout (location = 5) in uvec4        in_offs1;
layout (location = 6) in vec2 in_uv1;

layout (location = 0) INTERPOLATION out lowp vec4 vtx_base;
layout (location = 1) INTERPOLATION out lowp vec4 vtx_offs;
layout (location = 2)               out vec2 vtx_uv;
layout (location = 3) INTERPOLATION out lowp vec4 vtx_base1;		// New for OIT, only for OP/PT with 2-volume
layout (location = 4) INTERPOLATION out lowp vec4 vtx_offs1;
layout (location = 5)               out vec2 vtx_uv1;
// a translucent polygon's number, of a frame decoded on the GPU (TA_GPU_INSTANCE); 0 for the others
layout (location = 6) flat          out uint vtx_pp;

void main()
{
	vtx_pp = uint(gl_InstanceIndex);
	vtx_base = vec4(in_base) / 255.0;
	vtx_offs = vec4(in_offs) / 255.0;
	vtx_uv = in_uv;
	vtx_base1 = vec4(in_base1) / 255.0;								// New for OIT, only for OP/PT with 2-volume
	vtx_offs1 = vec4(in_offs1) / 255.0;
	vtx_uv1 = in_uv1;
	vec4 vpos = uniformBuffer.normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w; 
	gl_Position = vpos;
}
)";

static const char OITShaderHeader[] = R"(#version 450

layout (std140, set = 0, binding = 1) uniform FragmentShaderUniforms
{
	vec4 colorClampMin;
	vec4 colorClampMax;
	vec4 sp_FOG_COL_RAM;
	vec4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
} uniformBuffer;

layout(set = 3, binding = 2, r32ui) uniform coherent restrict uimage2D abufferPointerImg;
struct Pixel {
	uint color;
	float depth;
	uint seq_num;
	uint next;
};
#define EOL 0xFFFFFFFFu
/* The fragments, in up to four storage buffers of one size when the
 * device cannot bind them as one (OITBuffers): fragment i is in the
 * first while i is below its length, and so on. A buffer that is not
 * needed is the first again, and never reached. */
layout (set = 3, binding = 0, std430) coherent restrict buffer PixelBuffer_ {
	Pixel pixels[];
} PixelBuffer[4];
layout(set = 3, binding = 1) buffer PixelCounter_ {
	uint buffer_index;
	uint capacity;	// fragments there is room for
} PixelCounter;

Pixel loadPixel(uint i)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return PixelBuffer[0].pixels[i];
	if (i < 2u * n)
		return PixelBuffer[1].pixels[i - n];
	if (i < 3u * n)
		return PixelBuffer[2].pixels[i - 2u * n];
	return PixelBuffer[3].pixels[i - 3u * n];
}

void storePixel(uint i, Pixel pixel)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		PixelBuffer[0].pixels[i] = pixel;
	else if (i < 2u * n)
		PixelBuffer[1].pixels[i - n] = pixel;
	else if (i < 3u * n)
		PixelBuffer[2].pixels[i - 2u * n] = pixel;
	else
		PixelBuffer[3].pixels[i - 3u * n] = pixel;
}

// A fragment's seq_num, changed in place by the translucent modifier volumes
uint pixelAtomicOr(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicOr(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicOr(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicOr(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicOr(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

uint pixelAtomicXor(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicXor(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicXor(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicXor(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicXor(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

uint pixelAtomicAnd(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicAnd(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicAnd(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicAnd(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicAnd(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

void storeSeqNum(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		PixelBuffer[0].pixels[i].seq_num = v;
	else if (i < 2u * n)
		PixelBuffer[1].pixels[i - n].seq_num = v;
	else if (i < 3u * n)
		PixelBuffer[2].pixels[i - 2u * n].seq_num = v;
	else
		PixelBuffer[3].pixels[i - 3u * n].seq_num = v;
}

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
	// (every fragment is counted, those that find no room too: see OITBuffers::CountPass())
	uint index = atomicAdd(PixelCounter.buffer_index, 1);
	if (index >= PixelCounter.capacity)
		// Buffer overflow
		discard;
	
	return index;
}

void setFragDepth(void)
{
	float w = 100000.0 * gl_FragCoord.w;
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
}
struct PolyParam {
	int tsp_isp_pcw;
	int tsp1;
};
layout (set = 0, binding = 3, std430) readonly buffer TrPolyParamBuffer {
	PolyParam tr_poly_params[];
} TrPolyParam;

#define GET_TSP_FOR_AREA int tsp = area1 ? pp.tsp1 : pp.tsp_isp_pcw;

int getSrcBlendFunc(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 29) & 7;
}

int getDstBlendFunc(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 26) & 7;
}

bool getSrcSelect(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 25) & 1) != 0;
}

bool getDstSelect(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 24) & 1) != 0;
}

int getFogControl(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 22) & 3;
}

bool getUseAlpha(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 20) & 1) != 0;
}

bool getIgnoreTexAlpha(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 19) & 1) != 0;
}

int getShadingInstruction(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 6) & 3;
}

int getDepthFunc(const PolyParam pp)
{
	return (pp.tsp_isp_pcw >> 13) & 7;
}

bool getDepthMask(const PolyParam pp)
{
	return ((pp.tsp_isp_pcw >> 10) & 1) != 1;
}

bool getShadowEnable(const PolyParam pp)
{
	return (pp.tsp_isp_pcw & 1) != 0;
}

/* SSAA: a fragment covers some of its pixel's samples, the eight bits
 * above its polygon's number (which is then under 2^22) */
#ifdef SSAA_SAMPLES
#define POLY_NUMBER_MASK 0x003FFFFFu
#define COVERAGE_SHIFT 22
uint getCoverage(const Pixel pixel)
{
	return (pixel.seq_num >> COVERAGE_SHIFT) & 0xFFu;
}
#else
#define POLY_NUMBER_MASK 0x3FFFFFFFu
#endif

uint getPolyNumber(const Pixel pixel)
{
	return pixel.seq_num & POLY_NUMBER_MASK;
}

#define SHADOW_STENCIL 0x40000000u
#define SHADOW_ACC	   0x80000000u

bool isShadowed(const Pixel pixel)
{
	return (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC;
}

bool isTwoVolumes(const PolyParam pp)
{
	return pp.tsp1 != -1;
}

uint packColors(vec4 v)
{
	return (uint(round(v.r * 255.0)) << 24) | (uint(round(v.g * 255.0)) << 16) | (uint(round(v.b * 255.0)) << 8) | uint(round(v.a * 255.0));
}

vec4 unpackColors(uint u)
{
	return vec4(float((u >> 24) & 255) / 255.0, float((u >> 16) & 255) / 255.0, float((u >> 8) & 255) / 255.0, float(u & 255) / 255.0);
}
)";

static const char OITFragmentShaderSource[] = R"(
#define cp_AlphaTest %d
#define pp_ClipInside %d
#define pp_UseAlpha %d
#define pp_Texture %d
#define pp_IgnoreTexA %d
#define pp_ShadInstr %d
#define pp_Offset %d
#define pp_FogCtrl %d
#define pp_TwoVolumes %d
#define pp_Gouraud %d
#define pp_BumpMap %d
#define ColorClamping %d
#define pp_Palette %d
#define PASS %d
#define pp_SampleRate %d
#define PI 3.1415926

/* SSAA: shaded at each sample, its inputs taken there (1); or once a
 * pixel, its inputs taken at the pixel's centre as without SSAA (2) -
 * where else they are taken when not said is the device's choice */
#if pp_SampleRate == 1
#define AT_SAMPLE sample
#define OWN_SAMPLE gl_SampleID
#else
#define AT_SAMPLE
// (the first sample it covers: a modifier volume's edge is a pixel's, not a sample's, for it)
#define OWN_SAMPLE findLSB(gl_SampleMaskIn[0])
#endif
#if pp_SampleRate == 2
// (or, where it covers only some of the samples, at a place it covers, as without SSAA it would only be drawn where it covers the centre)
#define AT_CENTRE(v) (gl_SampleMaskIn[0] == ((1 << SSAA_SAMPLES) - 1) ? interpolateAtOffset(v, vec2(0.0)) : interpolateAtCentroid(v))
#else
#define AT_CENTRE(v) (v)
#endif
// (a flat input is not interpolated, and is not to be asked for at a place)
#if pp_Gouraud == 1
#define AT_CENTRE_SMOOTH(v) AT_CENTRE(v)
#else
#define AT_CENTRE_SMOOTH(v) (v)
#endif

#define PASS_DEPTH 0
#define PASS_COLOR 1
#define PASS_OIT 2

#if PASS == PASS_DEPTH || PASS == PASS_COLOR
layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor
#endif

#if pp_TwoVolumes == 1
#define IF(x) if (x)
#else
#define IF(x)
#endif

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth AT_SAMPLE
#endif

layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	ivec4 blend_mode0;
	float trilinearAlpha;
	int pp_Number;
	float palette_index;

	// two volume mode
	ivec4 blend_mode1;
	int shading_instr0;
	int shading_instr1;
	int fog_control0;
	int fog_control1;
	int use_alpha0;
	int use_alpha1;
	int ignore_tex_alpha0;
	int ignore_tex_alpha1;
} pushConstants;

#if pp_Texture == 1
layout (set = 1, binding = 0) uniform sampler2D tex0;
#if pp_TwoVolumes == 1
layout (set = 1, binding = 1) uniform sampler2D tex1;
#endif
#endif
#if pp_Palette == 1
layout (set = 0, binding = 6) uniform sampler2D palette;
#endif

#ifdef SSAA_SAMPLES
#if PASS == PASS_COLOR
layout (input_attachment_index = 0, set = 0, binding = 4) uniform usubpassInputMS shadow_stencil;
#define STENCIL_LOAD() subpassLoad(shadow_stencil, OWN_SAMPLE)
#endif
#if PASS == PASS_OIT
layout (input_attachment_index = 0, set = 0, binding = 5) uniform subpassInputMS DepthTex;
#endif
#else
#if PASS == PASS_COLOR
layout (input_attachment_index = 0, set = 0, binding = 4) uniform usubpassInput shadow_stencil;
#define STENCIL_LOAD() subpassLoad(shadow_stencil)
#endif
#if PASS == PASS_OIT
layout (input_attachment_index = 0, set = 0, binding = 5) uniform subpassInput DepthTex;
#endif
#endif

// Vertex input
layout (location = 0) INTERPOLATION in lowp vec4 in_vtx_base;
layout (location = 1) INTERPOLATION in lowp vec4 in_vtx_offs;
layout (location = 2) AT_SAMPLE     in vec2 in_vtx_uv;
layout (location = 3) INTERPOLATION in lowp vec4 in_vtx_base1;			// new for OIT. Only if 2 vol
layout (location = 4) INTERPOLATION in lowp vec4 in_vtx_offs1;
layout (location = 5) AT_SAMPLE     in vec2 in_vtx_uv1;
#define vtx_base AT_CENTRE_SMOOTH(in_vtx_base)
#define vtx_offs AT_CENTRE_SMOOTH(in_vtx_offs)
#define vtx_uv AT_CENTRE(in_vtx_uv)
#define vtx_base1 AT_CENTRE_SMOOTH(in_vtx_base1)
#define vtx_offs1 AT_CENTRE_SMOOTH(in_vtx_offs1)
#define vtx_uv1 AT_CENTRE(in_vtx_uv1)
layout (location = 6) flat          in uint vtx_pp;

#if pp_FogCtrl != 2 || pp_TwoVolumes == 1
layout (set = 0, binding = 2) uniform sampler2D fog_table;

// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
// Splitting the float is done by comparing and halving, which is exact
// and costs less than the log2() and pow() this used to take.
float fog_mode2(float w)
{
	float z = w * uniformBuffer.sp_FOG_DENSITY;
	// Not clamp(): where the depth is infinite and the density zero their
	// product is not a number, and that has to come out as 1 like anything
	// else below 1. The texture lookup this used to end in swallowed it.
	if (!(z >= 1.0))
		z = 1.0;
	if (z > 255.9999)
		z = 255.9999;
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(texture(fog_table, vec2(u, 0.25)).r * 255.0 + 0.5);
	float cur = floor(texture(fog_table, vec2(u, 0.75)).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}
#endif

vec4 colorClamp(vec4 col)
{
// TODO This can change in two-volume mode
#if ColorClamping == 1
	return clamp(col, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
#else
	return col;
#endif
}

#if pp_Palette == 1

vec4 palettePixel(sampler2D tex, vec2 coords)
{
	// palette_index is where the middle of the palette's first colour is in the row of 1024
	vec4 c = vec4(texture(tex, coords).r * (255.0 / 1024.0) + pushConstants.palette_index, 0.5, 0.0, 0.0);
	return texture(palette, c.xy);
}

#endif

void main()
{
	setFragDepth();

	#if PASS == PASS_OIT && defined(SSAA_SAMPLES)
		/* Manual depth testing, at each sample the fragment covers (with its
		 * depth at the pixel's centre: it is shaded once a pixel), which
		 * leaves the samples it is seen at */
		uint coverage = 0u;
		for (int s = 0; s < SSAA_SAMPLES; s++)
			if ((gl_SampleMaskIn[0] & (1 << s)) != 0 && gl_FragDepth >= subpassLoad(DepthTex, s).r)
				coverage |= 1u << s;
		if (coverage == 0u)
			discard;
	#elif PASS == PASS_OIT
		// Manual depth testing
		highp float frontDepth = subpassLoad(DepthTex).r;
		if (gl_FragDepth < frontDepth)
			discard;
	#endif

	// Clip inside the box
	#if pp_ClipInside == 1
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	#endif
	
	highp vec4 color = vtx_base;
	lowp vec4 offset = vtx_offs;
	vec2 uv = vtx_uv;
	bool area1 = false;
	ivec2 cur_blend_mode = pushConstants.blend_mode0.xy;
	
	#if pp_TwoVolumes == 1
		bool cur_use_alpha = pushConstants.use_alpha0 != 0;
		bool cur_ignore_tex_alpha = pushConstants.ignore_tex_alpha0 != 0;
		int cur_shading_instr = pushConstants.shading_instr0;
		int cur_fog_control = pushConstants.fog_control0;
		#if PASS == PASS_COLOR
			uvec4 stencil = STENCIL_LOAD();
			if (stencil.r == 0x81u) {
				color = vtx_base1;
				offset = vtx_offs1;
				uv = vtx_uv1;
				area1 = true;
				cur_blend_mode = pushConstants.blend_mode1.xy;
				cur_use_alpha = pushConstants.use_alpha1 != 0;
				cur_ignore_tex_alpha = pushConstants.ignore_tex_alpha1 != 0;
				cur_shading_instr = pushConstants.shading_instr1;
				cur_fog_control = pushConstants.fog_control1;
			}
		#endif
	#endif

	#if PASS == PASS_COLOR && pp_TwoVolumes == 0
		// Inside a modifier volume the PowerVR2 scales what the polygon is shaded
		// with, its base and offset colours, before the texture is combined with
		// them. Scaling the combined colour instead is the same thing for a
		// modulated texture, but also darkens a decal texture, which the
		// hardware leaves alone.
		uvec4 stencil = STENCIL_LOAD();
		if (stencil.r == 0x81u) {
			color.rgb *= uniformBuffer.shade_scale_factor;
			offset.rgb *= uniformBuffer.shade_scale_factor;
		}
	#endif
	#if pp_UseAlpha == 0 || pp_TwoVolumes == 1
		IF (!cur_use_alpha)
			color.a = 1.0;
	#endif
	#if pp_Texture==1
	{
		highp vec4 texcol;
		#if pp_TwoVolumes == 1
			// only the first volume's texture is ever one of palette indices
			if (area1)
				texcol = texture(tex1, uv);
			else
		#endif
		#if pp_Palette == 0
				texcol = texture(tex0, uv);
		#else
				texcol = palettePixel(tex0, uv);
		#endif
		#if pp_BumpMap == 1
			highp float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			highp float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(vtx_offs.a + vtx_offs.r * sin(s) + vtx_offs.g * cos(s) * cos(r - 2.0 * PI * vtx_offs.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
		#else
			#if pp_IgnoreTexA==1 || pp_TwoVolumes == 1
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
			color.rgb = mix(color.rgb, texcol.rgb, texcol.a);
		}
		#endif
		#if pp_ShadInstr == 3 || pp_TwoVolumes == 1 // MODULATE ALPHA
		IF(cur_shading_instr == 3)
		{
			color *= texcol;
		}
		#endif
		
		#if pp_Offset == 1 && pp_BumpMap == 0
		{
			color.rgb += offset.rgb;
		}
		#endif
	}
	#endif
	
	color = colorClamp(color);
	
	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha. This is a mode
	// of the fog unit, which comes after the texture has been combined in,
	// so it is the combined colour that is replaced: a textured polygon
	// comes out in plain fog colour. It used to be done to the base colour,
	// before texturing, and the texture was then applied on top.
	#if pp_FogCtrl == 3 || pp_TwoVolumes == 1 // LUT Mode 2
		IF (cur_fog_control == 3)
			color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));
	#endif
	#if pp_FogCtrl == 0 || pp_TwoVolumes == 1 // LUT
		IF(cur_fog_control == 0)
		{
			float fog = fog_mode2(gl_FragCoord.w);
			color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_RAM.rgb * fog;
		}
	#endif
	#if pp_Offset==1 && pp_BumpMap == 0 && (pp_FogCtrl == 1 || pp_TwoVolumes == 1)  // Per vertex
		IF(cur_fog_control == 1)
		{
			float fog = offset.a * (255.0 / 256.0);
			color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_VERT.rgb * fog;
		}
	#endif
	
	color *= pushConstants.trilinearAlpha;
	
	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	#if cp_AlphaTest == 1
		if (uniformBuffer.cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	#endif 
	
	//color.rgb=vec3(gl_FragCoord.w * uniformBuffer.sp_FOG_DENSITY / 128.0);
	
	#if PASS == PASS_COLOR 
		FragColor = color;
	#elif PASS == PASS_OIT
		// Discard as many pixels as possible
		switch (cur_blend_mode.y) // DST
		{
		case ONE:
			switch (cur_blend_mode.x) // SRC
			{
				case ZERO:
					discard;
				case ONE:
				case OTHER_COLOR:
				case INVERSE_OTHER_COLOR:
					if (color == vec4(0.0))
						discard;
					break;
				case SRC_ALPHA:
					if (color.a == 0.0 || color.rgb == vec3(0.0))
						discard;
					break;
				case INVERSE_SRC_ALPHA:
					if (color.a == 1.0 || color.rgb == vec3(0.0))
						discard;
					break;
			}
			break;
		// Destination times a white source is not left alone when the
		// fragment turns out to be in a shadow: its colour is scaled then.
		case INVERSE_OTHER_COLOR:
			if (cur_blend_mode.x <= SRC_ALPHA && color == vec4(0.0))
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
					if (color == vec4(0.0))
						discard;
					break;
			}
			break;
		}
		
		ivec2 coords = ivec2(gl_FragCoord.xy);
		uint idx =  getNextPixelIndex();
		
		Pixel pixel;
		pixel.color = packColors(clamp(color, vec4(0.0), vec4(1.0)));
		pixel.depth = gl_FragDepth;
		pixel.seq_num = uint(pushConstants.pp_Number) + vtx_pp;
		#ifdef SSAA_SAMPLES
			pixel.seq_num |= coverage << COVERAGE_SHIFT;
		#endif
		pixel.next = imageAtomicExchange(abufferPointerImg, coords, idx);
		storePixel(idx, pixel);
		
	#endif
}
)";

static const char OITModifierVolumeShader[] = R"(

void main()
{
	setFragDepth();
}
)";

/* A pixel's fragments sorted, at most. With SSAA a translucent fragment
 * is kept once a pixel with the samples it covers, so where a polygon's
 * triangles share an edge that crosses the pixel, each of them has one
 * there: small triangles - a character's - give a pixel two or three for
 * each layer, and the layers kept are three times as many. */
#define MAX_PIXELS_PER_FRAGMENT_1X "32"
#define MAX_PIXELS_PER_FRAGMENT_SSAA "96"

static const char OITFinalShaderSource[] =
"#ifdef SSAA_SAMPLES\n#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT_SSAA "\n#else\n#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT_1X "\n#endif\n"
R"(
#ifdef SSAA_SAMPLES
layout (input_attachment_index = 0, set = 2, binding = 0) uniform subpassInputMS tex;
#else
layout (input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput tex;
#endif

layout (location = 0) out vec4 FragColor;

uint pixel_list[MAX_PIXELS_PER_FRAGMENT];


int fillAndSortFragmentArray(ivec2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int count = 0;
	for (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)
	{
		const Pixel p = loadPixel(idx);
		// Insertion sort. The entry before the first one is not looked at:
		// this used to read pixel_list[-1], and the pixel buffer at whatever
		// index that held, before finding out there was nothing there.
		int j = count - 1;
		while (j >= 0)
		{
			const Pixel jp = loadPixel(pixel_list[j]);
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

/* Blend fragments back-to-front: the @num_frag sorted ones onto @finalColor;
 * with SSAA, those of a sample, @sampleBit */
vec4 blendFragments(int num_frag, vec4 finalColor, uint sampleBit)
{
	vec4 secondaryBuffer = vec4(0.0); // Secondary accumulation buffer
	
	for (int i = 0; i < num_frag; i++)
	{
		const Pixel pixel = loadPixel(pixel_list[i]);
#ifdef SSAA_SAMPLES
		if ((getCoverage(pixel) & sampleBit) == 0u)
			continue;
#endif
		const PolyParam pp = TrPolyParam.tr_poly_params[getPolyNumber(pixel)];
		bool area1 = false;
		bool shadowed = false;
		if (isShadowed(pixel))
		{
			if (isTwoVolumes(pp))
				area1 = true;
			else
				shadowed = true;
		}
		vec4 srcColor;
		if (getSrcSelect(pp, area1))
			srcColor = secondaryBuffer;
		else
		{
			srcColor = unpackColors(pixel.color);
			if (shadowed)
				srcColor.rgb *= uniformBuffer.shade_scale_factor;
		}
		vec4 dstColor = getDstSelect(pp, area1) ? secondaryBuffer : finalColor;
		vec4 srcCoef;
		vec4 dstCoef;
		
		int srcBlend = getSrcBlendFunc(pp, area1);
		switch (srcBlend)
		{
			case ZERO:
				srcCoef = vec4(0.0);
				break;
			case ONE:
				srcCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's
				srcCoef = dstColor;
				break;
			case INVERSE_OTHER_COLOR:
				srcCoef = vec4(1.0) - dstColor;
				break;
			case SRC_ALPHA:
				srcCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				srcCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				srcCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				srcCoef = vec4(1.0 - dstColor.a);
				break;
		}
		int dstBlend = getDstBlendFunc(pp, area1);
		switch (dstBlend)
		{
			case ZERO:
				dstCoef = vec4(0.0);
				break;
			case ONE:
				dstCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				dstCoef = srcColor;
				break;
			case INVERSE_OTHER_COLOR:
				dstCoef = vec4(1.0) - srcColor;
				break;
			case SRC_ALPHA:
				dstCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				dstCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				dstCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				dstCoef = vec4(1.0 - dstColor.a);
				break;
		}
		const vec4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);
		if (getDstSelect(pp, area1))
			secondaryBuffer = result;
		else
			finalColor = result;
	}
	
	return finalColor;
	
}

#if defined(SSAA_SAMPLES) && !defined(SSAA_PER_SAMPLE)
/* SSAA's last subpass: the list sorted once, put together at each sample,
 * and the samples resolved by the shared rule (rend/tile/pvr_resolve.h):
 * each as the unorm8 the attachment holds, (sum + N/2) / N per channel */
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	int num_frag = fillAndSortFragmentArray(coords);
	uint rb = 0u;
	uint ga = 0u;
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		uint c = packUnorm4x8(blendFragments(num_frag, subpassLoad(tex, s), 1u << s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
	FragColor = unpackUnorm4x8(pvr_resolve_join(rb, ga, SSAA_LOG2N));
}
#elif defined(SSAA_SAMPLES)
/* ...and of a render pass that is not the last: each sample put together
 * into the samples the next one draws over */
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	FragColor = blendFragments(fillAndSortFragmentArray(coords), subpassLoad(tex, gl_SampleID), 1u << gl_SampleID);
}
#else
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	// Compute and output final color for the frame buffer
	// Visualize the number of layers in use
	//FragColor = vec4(float(fillAndSortFragmentArray(coords)) / MAX_PIXELS_PER_FRAGMENT * 4, 0, 0, 1);
	FragColor = blendFragments(fillAndSortFragmentArray(coords), subpassLoad(tex), 1u);
}
#endif
)";

static const char OITClearShaderSource[] = R"(
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);

	// Reset pointers
	imageStore(abufferPointerImg, coords, uvec4(EOL));
}
)";

static const char OITTranslucentModvolShaderSource[] =
"#ifdef SSAA_SAMPLES\n#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT_SSAA "\n#else\n#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT_1X "\n#endif\n"
R"(
#define MV_MODE %d

// Must match ModifierVolumeMode enum values
#define MV_XOR		 0
#define MV_OR		 1
#define MV_INCLUSION 2
#define MV_EXCLUSION 3

void main()
{
#ifdef SSAA_SAMPLES
	/* Once a pixel, where the volume covers the pixel's first sample: two
	 * of its triangles that share an edge cover each sample once between
	 * them, but may both have a fragment at a pixel the edge crosses */
	if ((gl_SampleMaskIn[0] & 1) == 0)
		return;
#endif
#if MV_MODE == MV_XOR || MV_MODE == MV_OR
	setFragDepth();
#endif
	ivec2 coords = ivec2(gl_FragCoord.xy);
	
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int list_len = 0;
	while (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)
	{
		const Pixel pixel = loadPixel(idx);
		const PolyParam pp = TrPolyParam.tr_poly_params[getPolyNumber(pixel)];
		if (getShadowEnable(pp))
		{
#if MV_MODE == MV_XOR
			if (gl_FragDepth >= pixel.depth)
				pixelAtomicXor(idx, SHADOW_STENCIL);
#elif MV_MODE == MV_OR
			if (gl_FragDepth >= pixel.depth)
				pixelAtomicOr(idx, SHADOW_STENCIL);
#elif MV_MODE == MV_INCLUSION
			uint prev_val = pixelAtomicAnd(idx, ~(SHADOW_STENCIL));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)
				storeSeqNum(idx, bitfieldInsert(pixel.seq_num, 1u, 31, 1));
#elif MV_MODE == MV_EXCLUSION
			uint prev_val = pixelAtomicAnd(idx, ~(SHADOW_STENCIL|SHADOW_ACC));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)
				storeSeqNum(idx, bitfieldInsert(pixel.seq_num, 1u, 31, 1));
#endif
		}
		idx = pixel.next;
		list_len++;
	}
}
)";

static const char OITFinalVertexShaderSource[] = R"(#version 430

layout (location = 0) in vec3 in_pos;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
}
)";

extern const char ModVolVertexShaderSource[];

vk::UniqueShaderModule OITShaderManager::compileShader(const VertexShaderParams& params)
{
	char buf[sizeof(OITVertexShaderSource) * 2];

	sprintf(buf, OITVertexShaderSource, (int)params.gouraud);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, buf);
}

/* With SSAA, the count of samples defined after the version line (and the
 * resolve's rule for the last subpass); without it, the text as it is */
std::string OITShaderManager::WithSsaa(const std::string& source, bool perSample) const
{
	if (samples <= 1)
		return source;
	const size_t eol = source.find('\n') + 1;
	std::string defines = "#define SSAA_SAMPLES " + std::to_string(samples) + "\n#define SSAA_LOG2N " + std::to_string(SSAA_LOG2(samples)) + "u\n";
	if (perSample)
		defines += "#define SSAA_PER_SAMPLE\n";
	defines += "#define PVR_SHADER_GLSL\n#define PVR_LANE_NONE\n";
	defines += PvrResolveText;
	return source.substr(0, eol) + defines + source.substr(eol);
}

vk::UniqueShaderModule OITShaderManager::compileShader(const FragmentShaderParams& params)
{
	char buf[(sizeof(OITShaderHeader) + sizeof(OITFragmentShaderSource)) * 2];

	strcpy(buf, OITShaderHeader);
	sprintf(buf + strlen(buf), OITFragmentShaderSource, (int)params.alphaTest, (int)params.insideClipTest, (int)params.useAlpha,
			(int)params.texture, (int)params.ignoreTexAlpha, params.shaderInstr, (int)params.offset, params.fog,
			(int)params.twoVolume, (int)params.gouraud, (int)params.bumpmap, (int)params.clamping, (int)params.palette,
			(int)params.pass, (int)params.sampleRate);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, WithSsaa(buf));
}

vk::UniqueShaderModule OITShaderManager::compileFinalShader(bool perSample)
{
	std::string source = OITShaderHeader;
	source += OITFinalShaderSource;
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, WithSsaa(source, perSample));
}
vk::UniqueShaderModule OITShaderManager::compileFinalVertexShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, OITFinalVertexShaderSource);
}
vk::UniqueShaderModule OITShaderManager::compileClearShader()
{
	std::string source = OITShaderHeader;
	source += OITClearShaderSource;
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, WithSsaa(source));
}
vk::UniqueShaderModule OITShaderManager::compileModVolVertexShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, ModVolVertexShaderSource);
}
vk::UniqueShaderModule OITShaderManager::compileModVolFragmentShader()
{
	std::string source = OITShaderHeader;
	source += OITModifierVolumeShader;
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, WithSsaa(source));
}
void OITShaderManager::compileTrModVolFragmentShader(ModVolMode mode)
{
	if (trModVolShaders.empty())
		trModVolShaders.resize((size_t)ModVolMode::Final);
	char buf[(sizeof(OITShaderHeader) + sizeof(OITTranslucentModvolShaderSource)) * 2];

	strcpy(buf, OITShaderHeader);
	sprintf(buf + strlen(buf), OITTranslucentModvolShaderSource, (int)mode);
	trModVolShaders[(size_t)mode] = ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, WithSsaa(buf));
}
