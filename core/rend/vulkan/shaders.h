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
#pragma once

#include "vulkan.h"
#include "SPIRV/GlslangToSpv.h"

struct VertexShaderParams
{
	bool gouraud;

	u32 hash() { return (u32)gouraud; }
};

// alpha test, clip test, use alpha, texture, ignore alpha, shader instr, offset, fog, gouraud, bump, clamp, trilinear
struct FragmentShaderParams
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool trilinear;
	bool palette;
	bool shadowed;	// base and offset colours scaled: a pixel in a modifier volume
	int sampleRate;	// SSAA: shaded at each sample (1), or once a pixel at its centre (2); 0 without SSAA
	int samples;	// SSAA's (with sampleRate 2), 1 without

	u32 hash()
	{
		return ((u32)alphaTest) | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
			| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | (shaderInstr << 5)
			| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)gouraud << 10)
			| ((u32)bumpmap << 11) | ((u32)clamping << 12) | ((u32)trilinear << 13)
			| ((u32)palette << 14) | ((u32)shadowed << 15) | ((u32)sampleRate << 16) | ((u32)samples << 18);
	}
};

/* The depth prepass's shader (rend/sorter.h): what it is made for, the
 * rest being the push constants' flags (PREPASS_*) */
struct PrepassShaderParams
{
	bool texture;
	bool gouraud;
	int sampleRate;	// as FragmentShaderParams
	int samples;
	u32 hash()
	{
		return (u32)texture | ((u32)gouraud << 1) | ((u32)sampleRate << 2) | ((u32)samples << 4);
	}
};
// The flags, in the push constant where the colour pass has its trilinear alpha
#define PREPASS_CLIP_INSIDE   1u
#define PREPASS_USE_ALPHA     2u
#define PREPASS_PALETTE       4u
#define PREPASS_BUMP_MAP      8u
#define PREPASS_IGNORE_TEX_A  16u
#define PREPASS_SHAD_INSTR    32u	// two bits: the shading instruction
#define PREPASS_CLAMP         128u
#define PREPASS_FOG_ALPHA     256u	// fog mode 3: the fog in the alpha
#define PREPASS_ALWAYS        512u	// every pixel hides what is behind it

// std140 alignment required
struct VertexShaderUniforms
{
	float normal_matrix[16];
};

// std140 alignment required
struct FragmentShaderUniforms
{
	float colorClampMin[4];
	float colorClampMax[4];
	float sp_FOG_COL_RAM[4];	// Only using 3 elements but easier for std140
	float sp_FOG_COL_VERT[4];	// same comment
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
};

class ShaderManager
{
public:
	vk::ShaderModule GetVertexShader(const VertexShaderParams& params) { return getShader(vertexShaders, params); }
	vk::ShaderModule GetFragmentShader(const FragmentShaderParams& params) { return getShader(fragmentShaders, params); }
	vk::ShaderModule GetPrepassShader(const PrepassShaderParams& params) { return getShader(prepassShaders, params); }
	vk::ShaderModule GetModVolVertexShader()
	{
		if (!modVolVertexShader)
			modVolVertexShader = compileModVolVertexShader();
		return *modVolVertexShader;
	}
	vk::ShaderModule GetModVolShader()
	{
		if (!modVolShader)
			modVolShader = compileModVolFragmentShader();
		return *modVolShader;
	}
	vk::ShaderModule GetQuadVertexShader()
	{
		if (!quadVertexShader)
			quadVertexShader = compileQuadVertexShader();
		return *quadVertexShader;
	}
	vk::ShaderModule GetQuadFragmentShader()
	{
		if (!quadFragmentShader)
			quadFragmentShader = compileQuadFragmentShader();
		return *quadFragmentShader;
	}
	void Term()
	{
		vertexShaders.clear();
		fragmentShaders.clear();
		prepassShaders.clear();
		modVolVertexShader.reset();
		modVolShader.reset();
		quadVertexShader.reset();
		quadFragmentShader.reset();
	}

private:
	template<typename T>
	vk::ShaderModule getShader(std::map<u32, vk::UniqueShaderModule>& map, T params)
	{
		auto it = map.find(params.hash());
		if (it != map.end())
			return it->second.get();
		map[params.hash()] = compileShader(params);
		return map[params.hash()].get();
	}
	vk::UniqueShaderModule compileShader(const VertexShaderParams& params);
	vk::UniqueShaderModule compileShader(const FragmentShaderParams& params);
	vk::UniqueShaderModule compileShader(const PrepassShaderParams& params);
	vk::UniqueShaderModule compileModVolVertexShader();
	vk::UniqueShaderModule compileModVolFragmentShader();
	vk::UniqueShaderModule compileQuadVertexShader();
	vk::UniqueShaderModule compileQuadFragmentShader();

	std::map<u32, vk::UniqueShaderModule> vertexShaders;
	std::map<u32, vk::UniqueShaderModule> fragmentShaders;
	std::map<u32, vk::UniqueShaderModule> prepassShaders;
	vk::UniqueShaderModule modVolVertexShader;
	vk::UniqueShaderModule modVolShader;
	vk::UniqueShaderModule quadVertexShader;
	vk::UniqueShaderModule quadFragmentShader;
};
