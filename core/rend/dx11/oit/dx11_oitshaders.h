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
#include "../dx11_shaders.h"

/* The per-pixel renderer's shaders: the Vulkan one's (vulkan/oit/oit_shaders.cpp)
 * in HLSL. They need shader model 5 (feature level 11.0). */
class DX11OITShaders : public DX11ShaderCompiler
{
public:
	enum Pass { Depth, Color, OIT };
	// Must match the order of the modifier volume modes in the shader
	enum ModVolMode { Xor, Or, Inclusion, Exclusion };

	struct FragmentShaderParams
	{
		bool alphaTest;
		bool insideClipTest;
		bool useAlpha;
		bool texture;
		bool ignoreTexAlpha;
		int shaderInstr;
		bool offset;
		int fog;
		bool twoVolume;
		bool gouraud;
		bool bumpmap;
		bool clamping;
		bool palette;
		Pass pass;

		u32 hash() const
		{
			return (u32)alphaTest | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
				| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | ((u32)shaderInstr << 5)
				| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)twoVolume << 10) | ((u32)gouraud << 11)
				| ((u32)bumpmap << 12) | ((u32)clamping << 13) | ((u32)palette << 14) | ((u32)pass << 15);
		}
	};

	void Term();
	ID3D11VertexShader *GetVertexShader(bool gouraud);
	ID3D11PixelShader *GetPixelShader(const FragmentShaderParams& params);
	ID3D11VertexShader *GetModVolVertexShader();
	ID3D11PixelShader *GetModVolPixelShader();
	ID3D11PixelShader *GetTrModVolPixelShader(ModVolMode mode);
	ID3D11VertexShader *GetFinalVertexShader();
	ID3D11PixelShader *GetFinalPixelShader();

	ID3DBlob *GetVertexShaderBlob() { GetVertexShader(true); return vertexBlob; }
	ID3DBlob *GetModVolVertexShaderBlob() { GetModVolVertexShader(); return modVolVertexBlob; }

private:
	ComPtr<ID3D11VertexShader> vertexShaders[2];
	ComPtr<ID3DBlob> vertexBlob;
	std::unordered_map<u32, ComPtr<ID3D11PixelShader>> pixelShaders;
	ComPtr<ID3D11VertexShader> modVolVertexShader;
	ComPtr<ID3DBlob> modVolVertexBlob;
	ComPtr<ID3D11PixelShader> modVolPixelShader;
	ComPtr<ID3D11PixelShader> trModVolShaders[4];
	ComPtr<ID3D11VertexShader> finalVertexShader;
	ComPtr<ID3D11PixelShader> finalPixelShader;
};
