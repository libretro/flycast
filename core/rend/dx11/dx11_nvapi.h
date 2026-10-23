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
/* What the Direct3D 11 renderers take from NVIDIA's NVAPI, loaded when it
 * is run by the loader both Direct3D renderers share (rend/d3d_nvapi.h),
 * never linked. Each is for a device of NVIDIA's (DX11Context::GetVendor()
 * 0x10de) alone, which the caller sees to: a machine with NVAPI can have
 * another vendor's GPU drawing. Without NVAPI none of it is there, and the
 * renderers do what they do without it. Only the thread that renders calls
 * these. */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "types.h"

/* UAV overlap: between NvAPI_D3D11_BeginUAVOverlap() and EndUAVOverlap()
 * the driver does not wait for one draw or dispatch that writes an
 * unordered access view to be done before the next that has the same
 * view starts, as Direct3D 11 otherwise has it. Only for work that does
 * not read what another in the run writes, nor writes what another reads
 * or writes: the caller's to know. */
bool dx11_nvapi_uav_overlap_available();
void dx11_nvapi_begin_uav_overlap(ID3D11DeviceContext *context);
void dx11_nvapi_end_uav_overlap(ID3D11DeviceContext *context);

/* NvAPI_D3D11_MultiDrawIndexedInstancedIndirect(): @count draws, each
 * DrawIndexedInstancedIndirect()'s arguments, from @offset bytes into
 * @args, @stride bytes apart. Its count is the CPU's: an entry of the
 * GPU's that is not to draw has no indices or no instances. */
bool dx11_nvapi_multi_draw_available();
bool dx11_nvapi_multi_draw_indexed(ID3D11DeviceContext *context, u32 count, ID3D11Buffer *args, u32 offset, u32 stride);
