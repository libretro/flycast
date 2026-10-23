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
#pragma once
/* NVIDIA's NVAPI, for the one thing the Direct3D 12 renderer takes from it:
 * a pipeline whose pixel shader's SV_Coverage has the samples the depth
 * test passed (post-depth coverage), which Direct3D 12 itself has no way
 * to ask for. SSAA's per-pixel lists use it (DX12Pipelines::Build()).
 *
 * NVAPI is the driver's own nvapi64.dll (nvapi.dll for a 32-bit build),
 * loaded when it is first asked for and kept, never linked: a machine
 * without it, or with another vendor's GPU, loses nothing and the build
 * needs no SDK. Its functions are found by their published interface ids
 * through nvapi_QueryInterface, as RetroArch's Direct3D drivers find
 * theirs; the structures are the NVAPI SDK's (nvapi.h, MIT licence), only
 * what is used, declared in dx12_nvapi.cpp. Only the thread that renders
 * calls these. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>

/* Whether NVAPI is there, with the function that makes such pipelines */
bool dx12_nvapi_post_z_available();
/* @desc made into *@out with post-depth coverage on: false, and nothing
 * made, where NVAPI does not make it (not there, not this GPU, an error) */
bool dx12_nvapi_create_post_z_pipeline(ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
		ID3D12PipelineState **out);
