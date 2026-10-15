/*
    Created on: Oct 27, 2019

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
#include <map>
#include "../vulkan.h"
#include "../utils.h"

enum class Pass { Depth, Color, OIT };

class OITShaderManager
{
public:
	struct VertexShaderParams
	{
		bool gouraud;

		u32 hash() { return (u32)gouraud; }
	};

	// alpha test, clip test, use alpha, texture, ignore alpha, shader instr, offset, fog, gouraud, bump, clamp
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
		bool twoVolume;
		bool palette;
		Pass pass;
		int sampleRate;	// SSAA: shaded at each sample (1), or once a pixel at its centre (2); 0 without SSAA

		u32 hash()
		{
			return ((u32)alphaTest) | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
				| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | (shaderInstr << 5)
				| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)gouraud << 10)
				| ((u32)bumpmap << 11) | ((u32)clamping << 12) | ((u32)twoVolume << 13)
				| ((u32)palette << 14) | ((int)pass << 15) | ((u32)sampleRate << 17);
		}
	};

	vk::ShaderModule GetVertexShader(const VertexShaderParams& params) { return getShader(vertexShaders, params); }
	vk::ShaderModule GetFragmentShader(const FragmentShaderParams& params) { return getShader(fragmentShaders, params); }
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
	vk::ShaderModule GetTrModVolShader(ModVolMode mode)
	{
		if (trModVolShaders.empty() || !trModVolShaders[(size_t)mode])
			compileTrModVolFragmentShader(mode);
		return *trModVolShaders[(size_t)mode];
	}

	/* @perSample: SSAA's, of a render pass that is not the last, which puts
	 * each sample together; else the last one's, which resolves them too */
	vk::ShaderModule GetFinalShader(bool perSample = false)
	{
		vk::UniqueShaderModule& shader = perSample ? finalPerSampleShader : finalAutosortShader;
		if (!shader)
			shader = compileFinalShader(perSample);
		return *shader;
	}
	/* SSAA's samples a pixel (1: off): the shaders made for another count
	 * are dropped */
	void SetSamples(u32 samples)
	{
		if (samples != this->samples)
		{
			Term();
			this->samples = samples;
		}
	}
	vk::ShaderModule GetFinalVertexShader()
	{
		if (!finalVertexShader)
			finalVertexShader = compileFinalVertexShader();
		return *finalVertexShader;
	}
	vk::ShaderModule GetClearShader()
	{
		if (!clearShader)
			clearShader = compileClearShader();
		return *clearShader;
	}
	void Term()
	{
		vertexShaders.clear();
		fragmentShaders.clear();
		modVolVertexShader.reset();
		modVolShader.reset();
		trModVolShaders.clear();
		finalVertexShader.reset();
		finalAutosortShader.reset();
		finalSortedShader.reset();
		finalPerSampleShader.reset();
		clearShader.reset();
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
	vk::UniqueShaderModule compileModVolVertexShader();
	vk::UniqueShaderModule compileModVolFragmentShader();
	void compileTrModVolFragmentShader(ModVolMode mode);
	vk::UniqueShaderModule compileFinalShader(bool perSample);
	std::string WithSsaa(const std::string& source, bool perSample = false) const;
	vk::UniqueShaderModule compileFinalVertexShader();
	vk::UniqueShaderModule compileClearShader();

	std::map<u32, vk::UniqueShaderModule> vertexShaders;
	std::map<u32, vk::UniqueShaderModule> fragmentShaders;
	vk::UniqueShaderModule modVolVertexShader;
	vk::UniqueShaderModule modVolShader;
	std::vector<vk::UniqueShaderModule> trModVolShaders;

	vk::UniqueShaderModule finalVertexShader;
	vk::UniqueShaderModule finalAutosortShader;
	vk::UniqueShaderModule finalSortedShader;
	vk::UniqueShaderModule finalPerSampleShader;
	vk::UniqueShaderModule clearShader;
	u32 samples = 1;
};

