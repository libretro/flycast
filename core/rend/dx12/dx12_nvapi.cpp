/*
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
#include "dx12_nvapi.h"
#include "rend/d3d_nvapi.h"
#include "types.h"
#include <cstddef>
#include <cstring>

/* From the NVAPI SDK (github.com/NVIDIA/nvapi), which NVIDIA publishes
 * under the MIT licence:
 *   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION &
 *   AFFILIATES. All rights reserved. SPDX-License-Identifier: MIT
 * the interface ids (nvapi_interface.h) and the layout of the two
 * structures below (nvapi.h: NVAPI_D3D12_PSO_EXTENSION_DESC_V1 and
 * NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1, packed to 8 bytes as the
 * header packs them). A structure's version word is its size with the
 * revision in the high half, as MAKE_NVAPI_VERSION makes it. */
static const unsigned NvId_D3D12_CreateGraphicsPipelineState = 0x2fc28856;
static const int NvPsoRasterExtension = 0;		// NV_PSO_RASTER_EXTENSION

#pragma pack(push, 8)
struct NvPsoExtensionDesc
{
	u32 baseVersion;
	int psoExtension;			// NV_PSO_EXTENSION
};

// (the header's derives from the other; the same layout, as its first member)
struct NvPsoRasterizerStateDesc
{
	NvPsoExtensionDesc base;
	u32 version;
	bool ProgrammableSamplePositionsEnable;
	bool InterleavedSamplingEnable;
	u8 SampleCount;
	u8 SamplePositionsX[16];
	u8 SamplePositionsY[16];
	int QuadFillMode;			// NVAPI_QUAD_FILLMODE, 0: disabled
	bool PostZCoverageEnable;
	bool CoverageToColorEnable;
	u8 CoverageToColorRTIndex;
	bool TargetIndepentRasterWithDepth;	// (revision 2)
	u8 ForcedSampleCount;		// (revision 2)
	u8 reserved[62];
};
#pragma pack(pop)

static_assert(sizeof(NvPsoExtensionDesc) == 8, "NVAPI_D3D12_PSO_EXTENSION_DESC_V1");
static_assert(offsetof(NvPsoRasterizerStateDesc, version) == 8, "NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1");
static_assert(offsetof(NvPsoRasterizerStateDesc, SamplePositionsY) == 31, "NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1");
static_assert(offsetof(NvPsoRasterizerStateDesc, QuadFillMode) == 48, "NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1");
static_assert(offsetof(NvPsoRasterizerStateDesc, PostZCoverageEnable) == 52, "NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1");
static_assert(sizeof(NvPsoRasterizerStateDesc) == 120, "NVAPI_D3D12_PSO_RASTERIZER_STATE_DESC_V1");

// NV_PSO_EXTENSION_DESC_VER; NV_RASTERIZER_PSO_EXTENSION_DESC_VER_1, which every driver with the function takes
static const u32 NvPsoExtensionDescVer = (u32)sizeof(NvPsoExtensionDesc) | (1u << 16);
static const u32 NvRasterizerDescVer1 = (u32)sizeof(NvPsoRasterizerStateDesc) | (1u << 16);

typedef int (__cdecl *NvCreateGraphicsPipelineState)(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		u32 extensionCount, const NvPsoExtensionDesc **extensions, ID3D12PipelineState **pso);

static NvCreateGraphicsPipelineState nvCreateGraphicsPipelineState;
static bool probed;

bool dx12_nvapi_post_z_available()
{
	if (!probed)
	{
		probed = true;
		// (rend/d3d_nvapi.h: the library loaded and initialized, once for both renderers)
		nvCreateGraphicsPipelineState = (NvCreateGraphicsPipelineState)d3d_nvapi_query(NvId_D3D12_CreateGraphicsPipelineState);
	}
	return nvCreateGraphicsPipelineState != nullptr;
}

bool dx12_nvapi_create_post_z_pipeline(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		ID3D12PipelineState **out)
{
	*out = nullptr;
	if (!dx12_nvapi_post_z_available())
		return false;
	NvPsoRasterizerStateDesc raster;
	memset(&raster, 0, sizeof(raster));
	raster.base.baseVersion = NvPsoExtensionDescVer;
	raster.base.psoExtension = NvPsoRasterExtension;
	raster.version = NvRasterizerDescVer1;
	// (the render target's: what the header asks for without target-independent rasterization)
	raster.SampleCount = (u8)desc->SampleDesc.Count;
	raster.PostZCoverageEnable = true;
	const NvPsoExtensionDesc *extensions[] = { &raster.base };
	if (nvCreateGraphicsPipelineState(device, desc, 1, extensions, out) != D3D_NVAPI_OK || *out == nullptr)
	{
		*out = nullptr;
		return false;
	}
	return true;
}
