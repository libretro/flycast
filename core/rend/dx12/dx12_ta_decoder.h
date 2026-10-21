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
/* GPU-side TA processing in Direct3D 12 (rend/ta_gpu.h): a frame's
 * vertices, indices and modifier volume triangles made by compute kernels
 * from the blocks the game sent, and the frame drawn from them by both
 * Direct3D 12 renderers, with nothing read back. The Vulkan renderers'
 * (vulkan/ta_decoder.h) in Direct3D 12's terms:
 *
 * - The TA contexts' data are slices of one buffer in a custom heap of
 *   the host's memory, cached (write back), mapped for good
 *   (tactx_gpu_pool_attach()): the SH4 writes a frame straight into memory
 *   the GPU reads, and reads its control words back from it at the speed
 *   of ordinary memory. A slice is written again once the GPU is done with
 *   the frame that read it, which is known by polling the context's fence,
 *   never by waiting.
 * - At the start of Process() the header pass (ta_gpu_prepare()) writes the
 *   frame's tables into this frame's piece of a mapped buffer in the upload
 *   heap, and makes the render context's lists of draw groups in the full
 *   parse's place; the decode is recorded into the context's command list
 *   then, before the frame's draws, which are indirect (ExecuteIndirect of
 *   a draw's arguments alone): their counts are the GPU's. A frame the
 *   header pass does not do, or one whose piece of the tables the GPU still
 *   reads, is parsed and drawn as ever.
 * - The kernels are DXBC made when the core is built
 *   (rend/dx12/dx12_ta_dxbc.inc, tools/rend/kernels_dx12.sh); every buffer
 *   (sized for the largest frame the machine has) and the kernels'
 *   pipelines are made in Setup(), when the renderer is set up or the
 *   option changes: nothing is allocated or compiled while a game runs.
 * - The outputs are on the GPU, one copy: the context's one queue runs a
 *   frame's decode after whatever read the last one's, the buffers'
 *   transitions between the kernels' writes and the draws' reads keeping
 *   them in order.
 *
 * Off (TA processing: CPU), none of this exists, and Ready() says no. */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/dx11/comptr.h"

/* What a frame decoded on the GPU is drawn from (rend_context::gpu), as
 * vulkan/ta_decoder.h's TaGpuDraw has it */
struct DX12TaDraw
{
	/* the vertices (a Vertex each), the indices, the modifier volume
	 * triangles (three positions each) */
	D3D12_VERTEX_BUFFER_VIEW vertices{};
	D3D12_INDEX_BUFFER_VIEW indices{};
	D3D12_VERTEX_BUFFER_VIEW modtrig{};
	/* The opaque and punch-through draw groups' commands, a draw's
	 * arguments each, at their slot (PolyParam::first); the second ones for
	 * shadows (TA_GPU_SHADOWS) this many bytes on */
	ID3D12Resource *commands = nullptr;
	u64 shadowCommands = 0;
	/* The translucent draw records: each translucent draw group's at its
	 * place (PolyParam::first): its count, then at most PolyParam::count
	 * draws (ExecuteIndirect with that count) */
	ID3D12Resource *trDraws = nullptr;
	/* The translucent polygon parameters' words of the per-pixel renderer */
	D3D12_GPU_VIRTUAL_ADDRESS trParams = 0;
	/* (TA_GPU_INSTANCE) a word for each translucent strip, its number, as
	 * a vertex attribute of one an instance, which a draw record's
	 * StartInstanceLocation picks */
	D3D12_VERTEX_BUFFER_VIEW instances{};
	/* the frame's words (TAG_O_*): 1/fZ_max at TAG_O_ZSCALE, which the
	 * renderers copy into their vertex constants (rend/depth_scale.h) */
	ID3D12Resource *frame = nullptr;
	/* ExecuteIndirect of a draw's arguments alone */
	ID3D12CommandSignature *drawIndexed = nullptr;
	/* (TA_GPU_SORT) this frame's sorted translucent triangles' indices
	 * (ta_gpu_sorted_write(), written by Record()), to draw them with */
	D3D12_INDEX_BUFFER_VIEW sorted{};
};

class DX12TaDecoder
{
public:
	DX12TaDecoder();
	~DX12TaDecoder();

	/* Made, or taken down, as the option says (@wanted): when the renderer
	 * is set up and when the options change, never while a frame is drawn.
	 * Off, or where it cannot be made, nothing of it exists. */
	void Setup(bool wanted);
	void Term();
	bool Ready() const { return ready; }
	/* At the start of each Process(): the GPU's progress, polled, told to
	 * the TA contexts' pool; nothing is made here */
	void BeginFrame();
	/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
	 * render context made ready to draw in the full parse's place; whether
	 * it is decoded, and then (@rv) whether it has something to draw */
	bool Prepare(TA_context *ctx, u32 flags, bool *rv);
	/* ...and its decode recorded into the context's command list, the
	 * outputs left in the states the draws read them in */
	void Record(TA_context *ctx);
	const DX12TaDraw& Draw() const { return draw; }
	/* The per-triangle renderer's: room for the translucent triangles it
	 * sorts (TA_GPU_SORT), made with the rest; and whether there is */
	void WantSort(bool on) { sortWanted = on; }
	bool CanSort() const { return sortable; }
	/* The frame's 1/fZ_max copied from the decode's words to @dst at
	 * @offset bytes, which is in COPY_DEST (a constant buffer's word) */
	void CopyDepthScale(ID3D12Resource *dst, u64 offset);

	// for measuring
	u64 statFrames = 0;		// decoded
	u64 statSkipped = 0;		// left to the CPU
	u64 statBusy = 0;		// ...of them, because the GPU still had this frame's piece of the tables
	u64 statWhy[TA_GPU_WHY_COUNT] = {};
	u64 statShadowed = 0;
	u64 statSorted = 0;		// decoded with a sorted pass, sorted on this side

private:
	bool Init();
	void Log(const char *when);
	void States(bool decoding);

	struct Resources;
	Resources *res = nullptr;
	bool ready = false;
	bool failed = false;		// could not be made on this device: not tried again
	bool prepared = false;
	bool sortWanted = false;
	bool sortable = false;
	bool decoding = true;		// the outputs are in the kernels' state (unordered access)
	u32 index = 0;			// this frame's piece of the tables
	u32 flags = 0;
	TaGpuFrame frame;
	DX12TaDraw draw;
};
