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
/* The compute renderer on Direct3D 12 (Rendering Method: Compute): a frame
 * drawn into the per-pixel renderer's picture of the screen by the kernels
 * of rend/tile/pvr_compute.h, tile by tile, as the Vulkan per-pixel
 * renderer has it (vulkan/compute_drawer.h), in Direct3D 12's terms:
 *
 * - Its input is the CPU's parse of the frame (ta_parse_vdrc()), packed by
 *   the same rules (rend/tile/compute_frame.h) into this frame's piece of
 *   a ring in write-combined memory of a custom heap, which the kernels
 *   read in place; or, for a frame the GPU decoded (TA processing: GPU,
 *   dx12_ta_decoder.h), the decode's own buffers, read in place: only the
 *   polygon parameters and the render passes go up, and PVRC_K_GPUSRC
 *   numbers the triangles on the GPU.
 * - The kernels are DXIL made when the source is (shader model 6.0, for
 *   their 64-bit integers: dx12_compute_dxil.inc, tools/rend/kernels_dx12.sh),
 *   the walk's for the screen's samples and for one; every pipeline, the
 *   ring, the primitives' setup and the bins are made in Init(), when the
 *   renderer starts or changes size: nothing is allocated or compiled while
 *   a game runs.
 * - The buffers are a table of unordered access views for each piece of
 *   the ring (so that a read past a buffer's end is 0, as Vulkan's robust
 *   buffers have it); the textures are the renderers' bindless table and
 *   samplers, a word for each of the frame's texture slots in its piece of
 *   the ring saying which; the picture is a view in that table.
 * - The dispatches whose sizes the GPU made (the clear's, and a decoded
 *   frame's setup, scatter and sort) take them from the bins' counts,
 *   copied to an argument buffer first; nothing is read back and nothing
 *   waits. A few counts for the log are copied to a readback buffer, read
 *   when the fence says that frame is done.
 * - A piece of the ring is taken again once the fence says the frame that
 *   had it is done; a frame whose piece the GPU still has is left to the
 *   per-pixel renderer (never waited for).
 * - A render to a texture is drawn the same way, at one sample a pixel as
 *   the per-pixel renderer draws it, into the very texture that renderer
 *   would have drawn it in (DX12Renderer::BeginRTT(), whose pool's
 *   pictures are made for unordered access then): the size of the frame
 *   from its first pixel to the pixel clip's last, in powers of two, times
 *   Render To Texture Upscaling, cleared outside the clip. Its bins are made
 *   for the largest such texture.
 * - A frame it cannot draw is left to the per-pixel renderer (CanDraw(),
 *   Prepare()): more textures than it has slots for, more render passes
 *   than the kernels take, a picture to start from that cannot be read
 *   where it is (the frontend's, or the one being drawn into). */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "rend/tile/compute_frame.h"
#include "rend/dx11/comptr.h"
#include "dx12_ta_decoder.h"

class DX12ComputeDrawer
{
public:
	DX12ComputeDrawer();
	~DX12ComputeDrawer();
	/* Made for a picture of @width x @height and @samples a pixel; false
	 * when the device cannot have it (logged) */
	bool Init(u32 width, u32 height, u32 samples);
	void Term();
	/* Whether the device has what the kernels need (the reason it has not,
	 * or null) */
	static const char *DeviceLacks();
	bool Ready() const { return ready; }
	// what a frame decoded on the GPU is read from (pvrrc.gpu)
	void SetGpuDraw(DX12TaDecoder *decoder) { this->decoder = decoder; }
	u32 GetSamples() const { return samples; }
	// the screen's size it is made for
	u32 Width() const { return width; }
	u32 Height() const { return height; }

	// whether the frame in pvrrc is one this draws (before anything is recorded)
	bool CanDraw() const;
	/* The frame's words and textures, by @frame's rules, @last the place in
	 * the bindless table of the picture to start from (~0u: none; @scaled,
	 * one of another size); a render to a texture, into one of @rttWidth x
	 * @rttHeight, at one sample a pixel. False: it does not fit, or its
	 * piece of the ring is still the GPU's, and the per-pixel renderer draws
	 * it; nothing is recorded either way. */
	bool Prepare(ComputeFrame& frame, u32 last, bool scaled, u32 rttWidth = 0, u32 rttHeight = 0);
	/* ...then drawn into @picture (R8G8B8A8, made for unordered access),
	 * which is in UNORDERED_ACCESS: all of it, the pixels outside the
	 * frame's clip the clear colour; recorded into the context's list,
	 * which the next Submit() executes */
	void Record(ID3D12Resource *picture);

	/* The picture before copied into one of the drawer's own (made with
	 * it, of the screen's size), for a frame to start from when it cannot be
	 * read where it is - the picture the frame is drawn into, or the
	 * frontend's (version 2): @from, of the screen's size, in COPY_SOURCE;
	 * recorded after Prepare(), which is given its place in the bindless
	 * table (StartSrv()) */
	void CopyStart(ID3D12Resource *from);
	u32 StartSrv() const;

	// a frame left to the per-pixel renderer, and why (ComputeFrame::Why_*)
	void Fallback(u32 why) { statFallback++; statWhy[why < ComputeFrame::Why_Count ? why : 0]++; }
	u64 statFrames = 0;
	u64 statGpuFrames = 0;
	u64 statRttFrames = 0;
	u64 statFallback = 0;
	u64 statBusy = 0;
	u64 statWhy[ComputeFrame::Why_Count] = {};

private:
	struct Resources;
	Resources *res = nullptr;
	bool ready = false;
	bool failed = false;
	u32 samples = 1;
	u32 width = 0, height = 0;
	DX12TaDecoder *decoder = nullptr;
};
