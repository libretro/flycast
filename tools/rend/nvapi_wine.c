/* nvapi64.dll for tests under wine, where there is no NVIDIA driver: what
 * the Direct3D 11 renderers take from NVAPI (rend/dx11/dx11_nvapi.h),
 * done the way NVIDIA's documentation says it is, with Direct3D 11 alone,
 * so that the renderers' NVAPI paths can be run on a machine without an
 * NVIDIA GPU (wine's wined3d says its device is an NVIDIA one):
 *
 *   NvAPI_Initialize                         succeeds
 *   NvAPI_D3D11_BeginUAVOverlap / End...     do nothing: the driver's waits
 *                                            between dispatches are kept,
 *                                            which is what an overlap that
 *                                            happens to find no work to run
 *                                            together gives
 *   NvAPI_D3D11_MultiDrawIndexedInstancedIndirect(ctx, n, buf, off, stride)
 *                                            "loop[s] over that draw count
 *                                            and perform[s] the
 *                                            DrawIndexedInstancedIndirect
 *                                            operation each time,
 *                                            incrementing the buffer offset
 *                                            by the supplied stride"
 *                                            (nvapi.h): so it is here
 *
 * Anything else it is asked for is not there. What it was asked to do is
 * written to stderr when it is unloaded. This tests the renderers' use of
 * these functions - which, with what arguments - not NVIDIA's driver.
 *
 *   x86_64-w64-mingw32-gcc -O2 -shared -o nvapi64.dll tools/rend/nvapi_wine.c -ld3d11
 *
 * and put beside the program that loads the core. The interface ids are
 * from the NVAPI SDK (github.com/NVIDIA/nvapi, nvapi_interface.h), which
 * NVIDIA publishes under the MIT licence:
 *   SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION &
 *   AFFILIATES. All rights reserved. SPDX-License-Identifier: MIT */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

static unsigned long multi_draws, draws, overlaps_begun, overlaps_ended, refused;

static int __cdecl nv_initialize(void)
{
   return 0;
}

static int __cdecl nv_begin_uav_overlap(IUnknown *device_or_context)
{
   (void)device_or_context;
   overlaps_begun++;
   return 0;
}

static int __cdecl nv_end_uav_overlap(IUnknown *device_or_context)
{
   (void)device_or_context;
   overlaps_ended++;
   return 0;
}

static int __cdecl nv_multi_draw_indexed(ID3D11DeviceContext *context, unsigned count, ID3D11Buffer *buffer,
      unsigned offset, unsigned stride)
{
   unsigned i;
   /* (nvapi.h: the stride must be at least the five words of a draw's arguments) */
   if (context == NULL || buffer == NULL || stride < 5 * 4 || (offset & 3) != 0 || (stride & 3) != 0)
   {
      refused++;
      return -5;	/* NVAPI_INVALID_ARGUMENT */
   }
   multi_draws++;
   for (i = 0; i < count; i++)
      ID3D11DeviceContext_DrawIndexedInstancedIndirect(context, buffer, offset + i * stride);
   draws += count;
   return 0;
}

__declspec(dllexport) void *__cdecl nvapi_QueryInterface(unsigned id)
{
   switch (id)
   {
   case 0x0150e828: return (void *)nv_initialize;
   case 0x65b93ca8: return (void *)nv_begin_uav_overlap;
   case 0x2216a357: return (void *)nv_end_uav_overlap;
   case 0x59e890f9: return (void *)nv_multi_draw_indexed;
   default: return NULL;
   }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
   (void)instance;
   (void)reserved;
   if (reason == DLL_PROCESS_DETACH)
      fprintf(stderr, "nvapi_wine: %lu multi-draws (%lu draws), %lu refused; UAV overlap begun %lu times, ended %lu\n",
            multi_draws, draws, refused, overlaps_begun, overlaps_ended);
   return TRUE;
}
