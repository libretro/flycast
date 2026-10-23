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
/* What the Direct3D 11 renderers take from NVAPI: see dx11_nvapi.h */
#include "dx11_nvapi.h"
#include "rend/d3d_nvapi.h"

/* From the NVAPI SDK (github.com/NVIDIA/nvapi), which NVIDIA publishes
 * under the MIT licence:
 *   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION &
 *   AFFILIATES. All rights reserved. SPDX-License-Identifier: MIT
 * the interface ids (nvapi_interface.h) of these functions, and their
 * parameters (nvapi.h): NvAPI_D3D11_BeginUAVOverlap(IUnknown
 * *pDeviceOrContext), NvAPI_D3D11_EndUAVOverlap(IUnknown *),
 * NvAPI_D3D11_MultiDrawIndexedInstancedIndirect(ID3D11DeviceContext *,
 * NvU32 drawCount, ID3D11Buffer *, NvU32 alignedByteOffsetForArgs, NvU32
 * alignedByteStrideForArgs), each returning an NvAPI_Status. */
static const unsigned NvId_D3D11_BeginUAVOverlap = 0x65b93ca8;
static const unsigned NvId_D3D11_EndUAVOverlap = 0x2216a357;
static const unsigned NvId_D3D11_MultiDrawIndexedInstancedIndirect = 0x59e890f9;

typedef int (__cdecl *NvUavOverlap)(IUnknown *deviceOrContext);
typedef int (__cdecl *NvMultiDrawIndexed)(ID3D11DeviceContext *context, u32 drawCount, ID3D11Buffer *buffer,
		u32 alignedByteOffsetForArgs, u32 alignedByteStrideForArgs);

static NvUavOverlap nvBeginUavOverlap, nvEndUavOverlap;
static NvMultiDrawIndexed nvMultiDrawIndexed;
static bool probed;

static void probe()
{
	if (probed)
		return;
	probed = true;
	nvBeginUavOverlap = (NvUavOverlap)d3d_nvapi_query(NvId_D3D11_BeginUAVOverlap);
	nvEndUavOverlap = (NvUavOverlap)d3d_nvapi_query(NvId_D3D11_EndUAVOverlap);
	// (both or neither: a run begun is always ended)
	if (nvBeginUavOverlap == nullptr || nvEndUavOverlap == nullptr)
		nvBeginUavOverlap = nvEndUavOverlap = nullptr;
	nvMultiDrawIndexed = (NvMultiDrawIndexed)d3d_nvapi_query(NvId_D3D11_MultiDrawIndexedInstancedIndirect);
}

bool dx11_nvapi_uav_overlap_available()
{
	probe();
	return nvBeginUavOverlap != nullptr;
}

void dx11_nvapi_begin_uav_overlap(ID3D11DeviceContext *context)
{
	nvBeginUavOverlap(context);
}

void dx11_nvapi_end_uav_overlap(ID3D11DeviceContext *context)
{
	nvEndUavOverlap(context);
}

bool dx11_nvapi_multi_draw_available()
{
	probe();
	return nvMultiDrawIndexed != nullptr;
}

bool dx11_nvapi_multi_draw_indexed(ID3D11DeviceContext *context, u32 count, ID3D11Buffer *args, u32 offset, u32 stride)
{
	return nvMultiDrawIndexed(context, count, args, offset, stride) == D3D_NVAPI_OK;
}
