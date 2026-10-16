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
/* Textures made on the GPU from their bytes in video memory, for the
 * Direct3D 12 renderers: what dx11/dx11_texdecoder.h is for the Direct3D
 * 11 ones, with the same kernels - the Direct3D 11 renderers' DXBC
 * (rend/texdecode_dxbc.inc, shader model 5.0), which Direct3D 12 takes as
 * it is, so the texels are the CPU's converters' to the bit.
 *  - The texture's bytes - the texels, the codebook of a compressed
 *    texture, the part of the palette a paletted one uses - are copied as
 *    they are into the context's upload ring, which the kernels read as a
 *    raw buffer through a root descriptor (t0).
 *  - 32-bit texels and palette indices are written by a compute kernel
 *    through an unsigned integer view of the texture's level, made in a
 *    ring of places after the bindless table (u0, a descriptor table).
 *  - 16-bit texels are drawn into the level by a pixel shader, through a
 *    render target view made in the context's ring of those: few devices
 *    write the 16-bit formats through unordered access views.
 *  - A planar 16-bit texture needs no kernel: the PowerVR's texels are
 *    Direct3D's B5G6R5, B5G5R5A1 and B4G4R4A4 as they are, and its rows go
 *    from video memory to the texture through the ring, with a copy.
 * What a frame asks for is recorded at the end of Process(), before the
 * frame's draws, into the same command list: nothing waits for the GPU,
 * and nothing is allocated or compiled while a game runs - the pipelines
 * are made when the renderer starts. */
#include <windows.h>
#include <d3d12.h>
#include "rend/dx11/comptr.h"
#include "types.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

class DX12Context;
struct DX12PooledTexture;

class DX12TexDecoder
{
public:
	/* Everything made; nothing is done here when the device cannot */
	void Init(DX12Context *context);
	void Term();
	bool Ready() const { return ready; }

	/* Whether a texture of @type can be made by the kernels */
	bool Takes(TextureType type) const { return ready && (unsigned)type < 5 && takes[(int)type]; }

	/* A texture of @levels levels to be made from @bytes: where they go in
	 * the ring (a multiple of 16 of them), and where that is in words.
	 * Null if a texture that large never fits. */
	u8 *Begin(u32 bytes, u32 levels, u32 *base);
	/* A level of @texture, made with the six words of rend/texdecode.h */
	void Add(DX12PooledTexture *texture, u32 level, const u32 pc[6]);
	/* Where the next level added goes, and the serial of what is not
	 * recorded yet: for Cancel() */
	u32 Next() const { return jobCount; }
	u32 Serial() const { return serial; }
	/* The @count levels from @job on, added under @serial, are not made
	 * after all: the texture is made another way, or goes */
	void Cancel(u32 job, u32 count, u32 serial);

	/* What was asked for, recorded: the kernels run, the textures left to
	 * be sampled */
	void Flush();

	// what has been done so far, for measuring
	u64 statBytes = 0;
	u64 statLevels = 0;

private:
	enum { MAX_JOBS = 2048 };
	struct Job
	{
		DX12PooledTexture *texture;
		u32 level;
		u32 pc[8];		// (eight root constants)
	};
	/* The unordered access views' places: a ring, a place used again only
	 * once the GPU is done with the list it was made for */
	u32 TakeUav();

	bool ready = false;
	bool takes[5] = {};
	DX12Context *context = nullptr;
	ComPtr<ID3D12PipelineState> kernel32, kernel8;
	ComPtr<ID3D12PipelineState> drawn[3];	// 565, 5551, 4444

	Job jobs[MAX_JOBS];
	u32 jobCount = 0;
	u32 serial = 0;

	u32 uavHead = 0, uavTail = 0;			// positions, modulo UavCount for the place
	enum { UAV_MARKS = 16 };
	struct UavMark { u64 fence; u32 head; };
	UavMark uavMarks[UAV_MARKS];
	u32 uavMarkFirst = 0, uavMarkCount = 0;
};
