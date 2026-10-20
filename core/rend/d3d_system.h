/*
	Every Windows and Direct3D header a renderer of this core can use, for
	the first lines of a Direct3D renderer's files: before any of the
	emulator's own headers.

	hw/pvr/pvr_regs.h makes macros of the PowerVR's register names (ID,
	REVISION, TEXT_CONTROL...), and the Windows SDK's headers use some of
	them as names of their own - D3D12_MESSAGE.ID in d3d12sdklayers.h, which
	newer MinGW-w64 headers pull in with d3d12.h. Read after pvr_regs.h such
	a header no longer compiles; read before, it is untouched by them.
*/
#pragma once
#include <windows.h>
#if defined(HAVE_D3D9)
#include <d3d9.h>
#endif
#if defined(HAVE_D3D11)
#include <d3d11.h>
#endif
#if defined(HAVE_D3D12)
#include <d3d12.h>
#include <dxgi1_4.h>
#endif
#include <dxgi.h>
#include <d3dcompiler.h>
#include <libretro.h>
#include <libretro_d3d.h>
