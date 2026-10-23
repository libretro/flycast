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
/* NVAPI's loader, for both Direct3D renderers: see d3d_nvapi.h */
#include "d3d_nvapi.h"
#ifdef _WIN32
#include <windows.h>

/* From the NVAPI SDK (github.com/NVIDIA/nvapi), which NVIDIA publishes
 * under the MIT licence:
 *   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION &
 *   AFFILIATES. All rights reserved. SPDX-License-Identifier: MIT
 * NvAPI_Initialize's interface id (nvapi_interface.h) */
static const unsigned NvId_Initialize = 0x0150e828;

typedef void *(__cdecl *NvQueryInterface)(unsigned id);
typedef int (__cdecl *NvInitialize)();

static NvQueryInterface query;
static bool probed;

void *d3d_nvapi_query(unsigned id)
{
	if (!probed)
	{
		probed = true;
#ifdef _WIN64
		HMODULE dll = LoadLibraryA("nvapi64.dll");
#else
		HMODULE dll = LoadLibraryA("nvapi.dll");
#endif
		if (dll == nullptr)
			return nullptr;
		NvQueryInterface q = (NvQueryInterface)(void *)GetProcAddress(dll, "nvapi_QueryInterface");
		NvInitialize initialize = q != nullptr ? (NvInitialize)q(NvId_Initialize) : nullptr;
		if (initialize == nullptr || initialize() != D3D_NVAPI_OK)
		{
			FreeLibrary(dll);
			return nullptr;
		}
		// (the library is kept from here on: NVAPI is not to be unloaded once initialized)
		query = q;
	}
	return query != nullptr ? query(id) : nullptr;
}

#else

void *d3d_nvapi_query(unsigned id)
{
	(void)id;
	return nullptr;
}

#endif
