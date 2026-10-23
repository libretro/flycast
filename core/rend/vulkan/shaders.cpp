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
#include "vulkan_context.h"
#include "shaders.h"
#include "rend/sorter.h"
#define RASTER_SPV_WORDS
#include "raster_spv.inc"

// (both cast: on 32-bit x86, whose x87 keeps floats wider, the literal is
// otherwise compared at the wider precision)
static_assert((float)PREPASS_ALPHA_MIN == (float)PREPASS_ALPHA_MIN_GLSL, "the prepass's alpha, in shaders/prepass.frag (pso_key.h)");

vk::UniqueShaderModule MakeRasterModule(RasterSpv id)
{
	return VulkanContext::Instance()->GetDevice().createShaderModuleUnique(
			vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(), raster_spv_size[id], raster_spv[id]));
}
