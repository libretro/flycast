/* GPU-side TA processing on Vulkan: a frame's vertices, indices and
 * modifier volume triangles made on the GPU from the blocks the game sent
 * (rend/ta_gpu.h), with nothing read back.
 *
 * - The TA contexts' data are slices of one persistently mapped buffer of
 *   host-cached memory (tactx_gpu_pool_attach()): the SH4 writes a frame
 *   straight into memory the GPU reads, and reads its control words back
 *   from it at the speed of ordinary memory. A slice is written again once
 *   the work of the frame that read it last is done, which is known by
 *   polling the texture command pool's fences, never by waiting.
 * - At the start of Process() the header pass (ta_gpu_prepare()) writes
 *   the frame's tables into this frame's piece of a ring; the decode is
 *   recorded into a command buffer of the texture pool's, submitted with
 *   the frame's textures, before any draw.
 * - The kernels, every buffer (sized for the largest frame the machine
 *   has), the descriptor sets and the timestamp queries are made in
 *   Init(); nothing is allocated or compiled while a game runs.
 * - The outputs are on the device, one copy: frames are decoded one after
 *   the other on the one queue, and each decode waits for whatever the one
 *   before is read by.
 *
 * Until the raster renderers draw from these buffers (the next stage), the
 * frame is also parsed on the CPU as ever and drawn from that; the decode
 * only runs, for measuring it. Off (TA processing: CPU), none of this
 * exists. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "hw/pvr/ta.h"

#include <memory>
#include <vector>

class CommandPool;

class TaDecoder
{
public:
	TaDecoder();
	~TaDecoder();

	/* Made, or taken down, as the option says; at the start of each
	 * Process(), after the texture pool's BeginFrame(). The GPU's progress
	 * is told to the TA contexts' pool here. */
	void BeginFrame(CommandPool& pool, bool wanted);
	void Term();
	bool Ready() const { return ready; }

	/* The frame's tables, before the full parse; whether it is decoded */
	bool Prepare(TA_context *ctx);
	/* ...and its decode recorded, if the full parse had something to draw */
	void Record(TA_context *ctx, CommandPool& pool);

	// for measuring
	u64 statFrames = 0;		// decoded
	u64 statSkipped = 0;		// left to the CPU
	double statGpuMs = 0;		// GPU time of the decodes timed
	double statGpuMaxMs = 0;
	u64 statGpuTimed = 0;

private:
	bool Init(CommandPool& pool);
	void PollDone(CommandPool& pool);

	struct Resources;
	std::unique_ptr<Resources> res;
	bool ready = false;
	bool failed = false;		// could not be made on this device: not tried again
	int frames = 0;
	int index = 0;
	u32 serial = 0;			// this frame's
	u32 done = 0;			// every frame up to this one is done
	std::vector<u32> slotSerial;	// the frame each slot of the pool last had
	std::vector<bool> slotTimed;	// ...and whether it has a decode timed
	bool prepared = false;
	TaGpuFrame frame;
};
