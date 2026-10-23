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
/* NVIDIA's NVAPI, for the Direct3D renderers (dx11/dx11_nvapi.h,
 * dx12/dx12_nvapi.h, which have what each takes from it): the driver's own
 * nvapi64.dll (nvapi.dll for a 32-bit build), loaded the first time it is
 * asked for and kept, never linked - a machine without it, or with another
 * vendor's GPU, loses nothing, and the build needs no SDK. Its functions are
 * found by their published interface ids through nvapi_QueryInterface, as
 * RetroArch's Direct3D drivers find theirs; the ids and the structures are
 * the NVAPI SDK's (nvapi_interface.h and nvapi.h, MIT licence), only what is
 * used, declared where it is used. One loader for both renderers: it is
 * loaded and initialized once, whichever asks first. Only the thread that
 * renders calls these. */

/* NvAPI_Status's success */
#define D3D_NVAPI_OK 0

/* NVAPI's function with interface id @id, initialized; null where there
 * is no NVAPI (no library, NvAPI_Initialize() failed) or the driver has no
 * such function */
void *d3d_nvapi_query(unsigned id);
