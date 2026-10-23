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
/* GPU-side TA processing in Direct3D 11 (rend/ta_gpu.h): a frame's
 * vertices, indices and modifier volume triangles made by compute kernels
 * from the blocks the game sent, and the frame drawn from them by both
 * Direct3D 11 renderers, with nothing read back - the Direct3D 12
 * renderers' (dx12/dx12_ta_decoder.h) in Direct3D 11's terms. NVIDIA only:
 *
 * - Direct3D 11 has no indirect draw whose count the GPU gives, which the
 *   translucent draw groups need (a record for each of their strips, as
 *   many as the decode finds). NVAPI's multi-draw indirect
 *   (NvAPI_D3D11_MultiDrawIndexedInstancedIndirect, dx11_nvapi.h) draws a
 *   count the CPU gives: each group's most, PolyParam::count, of which the
 *   records the decode does not write are all zeros - no indices, no
 *   instance - and draw nothing (the decode starts from zeroed records).
 *   So this is made only on an NVIDIA device (vendor 0x10de) whose driver
 *   has it, with feature level 11.0 and the device's own context; anywhere
 *   else TA processing stays on the CPU, and the log says why.
 * - Direct3D 11 cannot keep a buffer the GPU reads mapped, so the TA
 *   contexts' data are not the GPU's own memory as in Direct3D 12: a frame
 *   decoded is copied, as far as the game wrote it, into a dynamic buffer
 *   (mapped with discard, which the driver gives new memory for while the
 *   GPU still reads the last), and the header pass (ta_gpu_prepare())
 *   writes its tables straight into another. That copy is the CPU's whole
 *   part in it, against the full parse's.
 * - The kernels are DXBC, shader model 5.0, made when the core is built
 *   (rend/dx11/dx11_ta_dxbc.inc, tools/rend/kernels_dx12.sh): the
 *   Direct3D 12 kernels' HLSL with typed buffer views, since Direct3D 11
 *   draws from a buffer, and takes indirect arguments from it, only if it
 *   is not structured. Every buffer (sized for the largest frame the
 *   machine has) and every kernel is made in Setup(), when the renderer is
 *   set up or the option changes: nothing is allocated or compiled while a
 *   game runs.
 * - The outputs are one copy on the GPU: Direct3D 11 runs the decode after
 *   whatever read the last one's, and the draws after the decode, as the
 *   commands are given. The dispatches that go by strips are
 *   DispatchIndirect() of a copy of the frame's words; an opaque or
 *   punch-through draw group is a DrawIndexedInstancedIndirect() of its
 *   command. The last four kernels, which do not depend on one another,
 *   run without waits between them where NVAPI has UAV overlap (Record()).
 *
 * Off (TA processing: CPU), none of this exists, and Ready() says no. */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "comptr.h"

/* What a frame decoded on the GPU is drawn from (rend_context::gpu) */
struct DX11TaDraw
{
	/* the vertices (a Vertex each), the indices (32-bit), the modifier
	 * volume triangles (three positions each) */
	ID3D11Buffer *vertices = nullptr;
	ID3D11Buffer *indices = nullptr;
	ID3D11Buffer *modtrig = nullptr;
	/* The opaque and punch-through draw groups' commands, a draw's
	 * arguments each, at their slot (PolyParam::first); the second ones for
	 * shadows (TA_GPU_SHADOWS) this many bytes on */
	ID3D11Buffer *commands = nullptr;
	u32 shadowCommands = 0;
	/* The translucent draw records: each translucent draw group's at its
	 * place (PolyParam::first): its count, then room for PolyParam::count
	 * draws, those past the count all zeros */
	ID3D11Buffer *trDraws = nullptr;
	/* The translucent polygon parameters' words of the per-pixel renderer,
	 * two a strip (a view of them as pairs) */
	ID3D11ShaderResourceView *trParams = nullptr;
	/* (TA_GPU_INSTANCE) 0, 1, 2...: a word for each translucent strip, its
	 * number, as a vertex attribute of one an instance, which a draw
	 * record's StartInstanceLocation picks */
	ID3D11Buffer *instances = nullptr;
	/* (TA_GPU_SORT) the sorted translucent triangles' indices, written by
	 * Record(), to draw them with */
	ID3D11Buffer *sorted = nullptr;
};

class DX11TaDecoder
{
public:
	DX11TaDecoder();
	~DX11TaDecoder();

	/* Made, or taken down, as the option says (@wanted): when the renderer
	 * is set up and when the options change, never while a frame is drawn.
	 * Off, or where it cannot be made, nothing of it exists. */
	void Setup(bool wanted);
	void Term();
	bool Ready() const { return ready; }
	/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
	 * render context made ready to draw in the full parse's place; whether
	 * it is decoded, and then (@rv) whether it has something to draw */
	bool Prepare(TA_context *ctx, u32 flags, bool *rv);
	/* ...and its decode run, the outputs unbound from the kernels for the
	 * draws to bind */
	void Record(TA_context *ctx);
	const DX11TaDraw& Draw() const { return draw; }
	/* The per-triangle renderer's: room for the translucent triangles it
	 * sorts (TA_GPU_SORT), made with the rest; and whether there is */
	void WantSort(bool on) { sortWanted = on; }
	bool CanSort() const { return sortable; }
	/* The frame's 1/fZ_max copied from the decode's words to @dst at
	 * @offset bytes (a constant buffer's word) */
	void CopyDepthScale(ID3D11Buffer *dst, u32 offset);
	/* A translucent draw group's records drawn: @count of them from the
	 * one at @slot (PolyParam::first + 1) */
	void DrawRecords(u32 slot, u32 count);

	// for measuring
	u64 statFrames = 0;		// decoded
	u64 statSkipped = 0;		// left to the CPU
	u64 statBusy = 0;		// ...of them, because the GPU still had the room for this frame's input
	u64 statWhy[TA_GPU_WHY_COUNT] = {};
	u64 statShadowed = 0;
	u64 statSorted = 0;		// decoded with a sorted pass, sorted on this side
	u64 statDrawFailed = 0;		// multi-draws the driver said no to

private:
	bool Init();
	void Log(const char *when);
	void SetGroups(u32 groups);

	struct Resources;
	Resources *res = nullptr;
	bool ready = false;
	bool failed = false;		// could not be made on this device: not tried again
	bool prepared = false;
	bool sortWanted = false;
	bool sortable = false;
	u32 flags = 0;
	TaGpuFrame frame;
	DX11TaDraw draw;
};
