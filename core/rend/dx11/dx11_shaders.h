/*
	Copyright 2021 flyinghead

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
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include "types.h"
#include "comptr.h"

/* The shaders, in HLSL, compiled the first time each one is wanted with the
 * compiler the frontend hands over (or d3dcompiler_47.dll's), and kept. */
class DX11ShaderCompiler
{
public:
	void Init(ID3D11Device *device, pD3DCompile compile) { this->device = device; this->compile = compile; }
	/* (an array of macros ending with a null one, or null) */
	ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ID3DInclude *include = nullptr);
	ComPtr<ID3D11VertexShader> CompileVS(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ComPtr<ID3DBlob> *blob = nullptr, ID3DInclude *include = nullptr);
	ComPtr<ID3D11PixelShader> CompilePS(const char *source, const char *entry, const char *profile, const D3D_SHADER_MACRO *macros,
			ID3DInclude *include = nullptr);

protected:
	ID3D11Device *device = nullptr;
	pD3DCompile compile = nullptr;
};

/* What a polygon's pixel shader is made for: as the Vulkan renderer's */
struct DX11FragmentShaderParams
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
	bool trilinear;
	bool palette;
	bool shadowed;	// drawn a second time where it is in a modifier volume: rend/shadows.h

	u32 hash() const
	{
		return (u32)alphaTest | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
			| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | ((u32)shaderInstr << 5)
			| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)gouraud << 10)
			| ((u32)bumpmap << 11) | ((u32)clamping << 12) | ((u32)trilinear << 13)
			| ((u32)palette << 14) | ((u32)shadowed << 15);
	}
};

class DX11Shaders : public DX11ShaderCompiler
{
public:
	void Term();

	ID3D11VertexShader *GetVertexShader(bool gouraud);
	ID3D11PixelShader *GetPixelShader(const DX11FragmentShaderParams& params);
	ID3D11VertexShader *GetModVolVertexShader();
	ID3D11PixelShader *GetModVolPixelShader();
	ID3D11VertexShader *GetQuadVertexShader();
	ID3D11PixelShader *GetQuadPixelShader();

	/* For the input layouts: what the vertex shaders take */
	ID3DBlob *GetVertexShaderBlob() { GetVertexShader(true); return vertexBlob; }
	ID3DBlob *GetModVolVertexShaderBlob() { GetModVolVertexShader(); return modVolVertexBlob; }
	ID3DBlob *GetQuadVertexShaderBlob() { GetQuadVertexShader(); return quadVertexBlob; }

private:
	ComPtr<ID3D11VertexShader> vertexShaders[2];
	ComPtr<ID3DBlob> vertexBlob;
	std::unordered_map<u32, ComPtr<ID3D11PixelShader>> pixelShaders;
	ComPtr<ID3D11VertexShader> modVolVertexShader;
	ComPtr<ID3DBlob> modVolVertexBlob;
	ComPtr<ID3D11PixelShader> modVolPixelShader;
	ComPtr<ID3D11VertexShader> quadVertexShader;
	ComPtr<ID3DBlob> quadVertexBlob;
	ComPtr<ID3D11PixelShader> quadPixelShader;
};

/* The HLSL the per-pixel renderer shares with the other: the vertex
 * transform and the fog table */
extern const char * const DX11VertexTransform;
extern const char * const DX11FogFunction;
extern const char * const DX11ModVolVertexShader;
