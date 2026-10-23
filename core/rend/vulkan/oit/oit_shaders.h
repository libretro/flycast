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
#include "../shaders.h"
#include "../vulkan_context.h"

enum class Pass { Depth, Color, OIT };

/* The per-pixel renderer's shaders, made in Init(), before any pipeline
 * is: SPIR-V of vulkan/shaders/ (raster_spv.inc) */
class OITShaderManager
{
public:
	struct VertexShaderParams
	{
		bool gouraud;
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

		// what the shader is specialized for (pso_key.h): all but gouraud, pass and sampleRate
		s32 key() const
		{
			return ((s32)alphaTest << OIT_SC_ALPHA_TEST) | ((s32)insideClipTest << OIT_SC_CLIP_INSIDE)
				| ((s32)useAlpha << OIT_SC_USE_ALPHA) | ((s32)texture << OIT_SC_TEXTURE)
				| ((s32)ignoreTexAlpha << OIT_SC_IGNORE_TEX_A) | (shaderInstr << OIT_SC_SHAD_INSTR)
				| ((s32)offset << OIT_SC_OFFSET) | (fog << OIT_SC_FOG_CTRL) | ((s32)twoVolume << OIT_SC_TWO_VOLUMES)
				| ((s32)bumpmap << OIT_SC_BUMP_MAP) | ((s32)clamping << OIT_SC_CLAMPING) | ((s32)palette << OIT_SC_PALETTE);
		}
	};

	void Init()
	{
		modules.Make(RSPV_MODVOL_VERT, RSPV_MODVOL_VERT);
		modules.Make(RSPV_OIT_VERT_G0, RSPV_OIT_FRAG_G1_P2_R2);
		// (SSAA's lists taking the samples the depth test passed: only where the device has that)
		postDepthCoverage = VulkanContext::Instance()->postDepthCoverage;
		if (postDepthCoverage)
			modules.Make(RSPV_OIT_FRAG_G0_P2_R2_PDC, RSPV_OIT_FRAG_G1_P2_R2_PDC);
		modules.Make(RSPV_OIT_MODVOL_FRAG, RSPV_OIT_FINAL_VERT);
	}
	vk::ShaderModule GetVertexShader(const VertexShaderParams& params) const
	{
		return modules.Get(params.gouraud ? RSPV_OIT_VERT_G1 : RSPV_OIT_VERT_G0);
	}
	// the module of @params' interpolation, pass and sample rate: the rest is its key()
	vk::ShaderModule GetFragmentShader(const FragmentShaderParams& params) const
	{
		// G0_P0_R0 ... G0_P2_R2, without the lists' at each sample: eight a gouraud
		static const int byPassRate[3][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 6, -1, 7 } };
		if (params.pass == Pass::OIT && params.sampleRate == 2 && postDepthCoverage)
			return modules.Get(params.gouraud ? RSPV_OIT_FRAG_G1_P2_R2_PDC : RSPV_OIT_FRAG_G0_P2_R2_PDC);
		return modules.Get((RasterSpv)(RSPV_OIT_FRAG_G0_P0_R0 + (params.gouraud ? 8 : 0) + byPassRate[(int)params.pass][params.sampleRate]));
	}
	vk::ShaderModule GetModVolVertexShader() const { return modules.Get(RSPV_MODVOL_VERT); }
	vk::ShaderModule GetModVolShader() const { return modules.Get(RSPV_OIT_MODVOL_FRAG); }
	vk::ShaderModule GetTrModVolShader(ModVolMode mode) const
	{
		return modules.Get((RasterSpv)(RSPV_OIT_TRMODVOL_FRAG_M0_S0 + 2 * (int)mode + (samples > 1 ? 1 : 0)));
	}

	/* @perSample: SSAA's, of a render pass that is not the last, which puts
	 * each sample together; else the last one's, which resolves them too */
	vk::ShaderModule GetFinalShader(bool perSample = false) const
	{
		return modules.Get(samples <= 1 ? RSPV_OIT_FINAL_FRAG_S0 : perSample ? RSPV_OIT_FINAL_FRAG_S2 : RSPV_OIT_FINAL_FRAG_S1);
	}
	/* SSAA's samples a pixel (1: off): what the pipelines are specialized
	 * for, and which modules they take */
	void SetSamples(u32 samples)
	{
		this->samples = samples;
	}
	u32 GetSamples() const { return samples; }
	vk::ShaderModule GetFinalVertexShader() const { return modules.Get(RSPV_OIT_FINAL_VERT); }
	vk::ShaderModule GetClearShader() const { return modules.Get(RSPV_OIT_CLEAR_FRAG); }
	void Term()
	{
		modules.Term();
	}

private:
	RasterModules modules;
	u32 samples = 1;
	bool postDepthCoverage = false;	// the device's (VulkanContext::postDepthCoverage), as Init() found it
};
