#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
 *  Created on: Oct 3, 2019

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
#include "vulkan.h"
#include "shaders.h"
#include "compiler.h"

static const char VertexShaderSource[] = R"(#version 450

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

layout (location = 0) INTERPOLATION out lowp vec4 vtx_base;
layout (location = 1) INTERPOLATION out lowp vec4 vtx_offs;
layout (location = 2)               out vec2 vtx_uv;

void main()
{
	vtx_base = vec4(in_base) / 255.0;
	vtx_offs = vec4(in_offs) / 255.0;
	vtx_uv = in_uv;
	vec4 vpos = uniformBuffer.normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w; 
	gl_Position = vpos;
}
)";

static const char FragmentShaderSource[] = R"(#version 450

#define cp_AlphaTest %d
#define pp_ClipInside %d
#define pp_UseAlpha %d
#define pp_Texture %d
#define pp_IgnoreTexA %d
#define pp_ShadInstr %d
#define pp_Offset %d
#define pp_FogCtrl %d
#define pp_Gouraud %d
#define pp_BumpMap %d
#define ColorClamping %d
#define pp_TriLinear %d
#define pp_Palette %d
#define pp_Shadowed %d
#define PI 3.1415926

layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif

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

layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	float trilinearAlpha;
	float palette_index;
} pushConstants;

#if pp_Texture == 1
layout (set = 1, binding = 0) uniform sampler2D tex;
#endif
#if pp_Palette == 1
layout (set = 0, binding = 3) uniform sampler2D palette;
#endif

// Vertex input
layout (location = 0) INTERPOLATION in lowp vec4 vtx_base;
layout (location = 1) INTERPOLATION in lowp vec4 vtx_offs;
layout (location = 2)               in vec2 vtx_uv;

#if pp_FogCtrl != 2
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
#if ColorClamping == 1
	return clamp(col, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
#else
	return col;
#endif
}

#if pp_Palette == 1

vec4 palettePixel(sampler2D tex, vec2 coords)
{
	vec4 c = vec4(texture(tex, coords).r * 255.0 / 1023.0 + pushConstants.palette_index, 0.5, 0.0, 0.0);
	return texture(palette, c.xy);
}

#endif

void main()
{
	// Clip inside the box
	#if pp_ClipInside == 1
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	#endif
	
	vec4 color = vtx_base;
	#if pp_UseAlpha == 0
		color.a = 1.0;
	#endif
	#if pp_Shadowed == 1
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= uniformBuffer.shade_scale_factor;
		vec4 offset = vec4(vtx_offs.rgb * uniformBuffer.shade_scale_factor, vtx_offs.a);
	#else
		#define offset vtx_offs
	#endif
	#if pp_Texture == 1
	{
		#if pp_Palette == 0
			vec4 texcol = texture(tex, vtx_uv);
		#else
			vec4 texcol = palettePixel(tex, vtx_uv);
		#endif
		
		#if pp_BumpMap == 1
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
		#else
			#if pp_IgnoreTexA == 1
				texcol.a = 1.0;
			#endif
			
			#if cp_AlphaTest == 1
				if (uniformBuffer.cp_AlphaTestValue > texcol.a)
					discard;
			#endif 
		#endif
		#if pp_ShadInstr == 0
		{
			color = texcol;
		}
		#endif
		#if pp_ShadInstr == 1
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		#endif
		#if pp_ShadInstr == 2
		{
			color.rgb = mix(color.rgb, texcol.rgb, texcol.a);
		}
		#endif
		#if  pp_ShadInstr == 3
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
	#if pp_FogCtrl == 3
		color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));
	#endif
	#if pp_FogCtrl == 0
	{
		float fog = fog_mode2(gl_FragCoord.w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_RAM.rgb * fog;
	}
	#endif
	#if pp_FogCtrl == 1 && pp_Offset==1 && pp_BumpMap == 0
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + uniformBuffer.sp_FOG_COL_VERT.rgb * fog;
	}
	#endif
	
	#if pp_TriLinear == 1
	color *= pushConstants.trilinearAlpha;
	#endif
	
	#if cp_AlphaTest == 1
		color.a = 1.0;
	#endif 
	//color.rgb = vec3(gl_FragCoord.w * uniformBuffer.sp_FOG_DENSITY / 128.0);

	float w = gl_FragCoord.w * 100000.0;
	gl_FragDepth = log2(1.0 + w) / 34.0;

	gl_FragColor = color;
}
)";

extern const char ModVolVertexShaderSource[] = R"(#version 450

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 normal_matrix;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;

void main()
{
	vec4 vpos = in_pos;
	if (vpos.z < 0.0 || vpos.z > 3.4e37)
	{
		gl_Position = vec4(0.0, 0.0, 1.0, 1.0 / vpos.z);
		return;
	}

	vpos = uniformBuffer.normal_matrix * vpos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w; 
	gl_Position = vpos;
}
)";

static const char ModVolFragmentShaderSource[] = R"(#version 450

layout (location = 0) out vec4 FragColor;

layout (push_constant) uniform pushBlock
{
	float sp_ShaderColor;
} pushConstants;

void main()
{
	float w = gl_FragCoord.w * 100000.0;
	gl_FragDepth = log2(1.0 + w) / 34.0;
	FragColor = vec4(0.0, 0.0, 0.0, pushConstants.sp_ShaderColor);
}
)";

static const char QuadVertexShaderSource[] = R"(#version 450

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;

layout (location = 0) out vec2 outUV;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	outUV = in_uv;
}
)";

static const char QuadFragmentShaderSource[] = R"(#version 450

layout (binding = 0) uniform sampler2D tex;
layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 FragColor;

void main() 
{
	FragColor = texture(tex, inUV);
}
)";

vk::UniqueShaderModule ShaderManager::compileShader(const VertexShaderParams& params)
{
	char buf[sizeof(VertexShaderSource) * 2];

	sprintf(buf, VertexShaderSource, (int)params.gouraud);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, buf);
}

vk::UniqueShaderModule ShaderManager::compileShader(const FragmentShaderParams& params)
{
	char buf[sizeof(FragmentShaderSource) * 2];

	sprintf(buf, FragmentShaderSource, (int)params.alphaTest, (int)params.insideClipTest, (int)params.useAlpha, (int)params.texture,
			(int)params.ignoreTexAlpha, params.shaderInstr, (int)params.offset, params.fog, (int)params.gouraud,
			(int)params.bumpmap, (int)params.clamping, (int)params.trilinear, (int)params.palette,
			(int)params.shadowed);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, buf);
}

vk::UniqueShaderModule ShaderManager::compileModVolVertexShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, ModVolVertexShaderSource);
}

vk::UniqueShaderModule ShaderManager::compileModVolFragmentShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, ModVolFragmentShaderSource);
}

vk::UniqueShaderModule ShaderManager::compileQuadVertexShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, QuadVertexShaderSource);
}

vk::UniqueShaderModule ShaderManager::compileQuadFragmentShader()
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, QuadFragmentShaderSource);
}
