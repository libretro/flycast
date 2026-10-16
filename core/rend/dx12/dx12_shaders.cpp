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
#include "dx12_shaders.h"
#include <cstddef>
#include "dx12_dxbc.inc"
#include "rend/texdecode_dxbc.inc"

D3D12_SHADER_BYTECODE dx12_shader(DX12Shader shader)
{
	const int n = (int)shader;
	D3D12_SHADER_BYTECODE code = { dx12_dxbc[n], dx12_dxbc_size[n] };
	return code;
}

D3D12_SHADER_BYTECODE dx12_texdecode_kernel(int kernel)
{
	D3D12_SHADER_BYTECODE code = { texdecode_dxbc[kernel], texdecode_dxbc_size[kernel] };
	return code;
}
