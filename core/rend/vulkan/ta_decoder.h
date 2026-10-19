/* GPU-side TA processing on Vulkan: a frame's vertices, indices and
 * modifier volume triangles made on the GPU from the blocks the game sent
 * (rend/ta_gpu.h), and the frame drawn from them, with nothing read back.
 *
 * - The TA contexts' data are slices of one persistently mapped buffer of
 *   host-cached memory (tactx_gpu_pool_attach()): the SH4 writes a frame
 *   straight into memory the GPU reads, and reads its control words back
 *   from it at the speed of ordinary memory. A slice is written again once
 *   the work of the frame that read it last is done, which is known by
 *   polling the texture command pool's fences, never by waiting.
 * - At the start of Process() the header pass (ta_gpu_prepare()) writes
 *   the frame's tables into this frame's piece of a ring, and makes the
 *   render context's lists of draw groups in the full parse's place; the
 *   decode is recorded into a command buffer of the texture pool's,
 *   submitted with the frame before its draws, which are indirect: their
 *   counts are the GPU's (TaGpuDraw). A frame the header pass does not do
 *   (ta_gpu_prepare()'s TA_GPU_WHY_*) is parsed and drawn as ever.
 * - The kernels (SPIR-V made when the core is built: rend/ta_gpu_spv.inc),
 *   every buffer (sized for the largest frame the machine has), the
 *   descriptor sets and the timestamp queries are made in Setup(), when
 *   the renderer is set up or the option changes, and the pipelines go
 *   through the context's pipeline cache; nothing is allocated or
 *   compiled while a game runs.
 * - The outputs are on the device, one copy: frames are decoded one after
 *   the other on the one queue, and each decode waits for whatever the one
 *   before is read by.
 *
 * Off (TA processing: CPU), none of this exists. */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "hw/pvr/ta.h"

#include <memory>
#include <vector>

class CommandPool;

/* What a frame decoded on the GPU is drawn from (rend_context::gpu): the
 * vertex, index and modifier volume triangle buffers; the opaque and
 * punch-through draw groups' commands, at their slot (PolyParam::first);
 * the translucent draw records, each translucent draw group's at its
 * place (first: a count, then at most PolyParam::count draws); and the
 * translucent polygon parameters' words of the per-pixel renderer.
 * drawCount: vkCmdDrawIndexedIndirectCountKHR(), or null, when a group's
 * whole room is drawn (its records past the count draw nothing). */
struct TaGpuDraw
{
	vk::Buffer vertices, indices, modtrig, commands, trDraws, trParams;
	vk::DeviceSize trParamsSize = 0;
	/* the frame's words (TAG_O_*): 1/fZ_max at TAG_O_ZSCALE, which the
	 * drawers copy into their vertex uniforms (rend/depth_scale.h) */
	vk::Buffer frame;
	PFN_vkCmdDrawIndexedIndirectCountKHR drawCount = nullptr;
	/* The second commands of the opaque and punch-through draw groups for
	 * shadows (TA_GPU_SHADOWS, TAG_K_SHADOW): a group's at this many bytes
	 * on from its own */
	vk::DeviceSize shadowCommands = 0;
	/* (TA_GPU_SORT) this frame's room for the sorted translucent triangles'
	 * indices (ta_gpu_sorted_write()), mapped */
	vk::Buffer sorted;
	vk::DeviceSize sortedOffset = 0;
	u32 *sortedMap = nullptr;
};

class TaDecoder
{
public:
	TaDecoder();
	~TaDecoder();

	/* Made, or taken down, as the option says (@wanted), for the texture
	 * pool's size: when the renderer is set up (its context made, or its
	 * size changed) and when the options change, never while a frame is
	 * drawn. Off, or where it cannot be made, nothing of it exists. */
	void Setup(CommandPool& pool, bool wanted);
	/* At the start of each Process(), after the texture pool's
	 * BeginFrame(); nothing is made here. The GPU's progress is told to
	 * the TA contexts' pool here. */
	void BeginFrame(CommandPool& pool);
	void Term();
	bool Ready() const { return ready; }

	/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
	 * render context made ready to draw in the full parse's place; whether
	 * it is decoded, and then (@rv) whether it has something to draw */
	bool Prepare(TA_context *ctx, u32 flags, bool *rv);
	/* ...and its decode recorded */
	void Record(TA_context *ctx, CommandPool& pool);
	// what the drawers draw a decoded frame from
	const TaGpuDraw *Draw() const { return &draw; }
	// whether the translucent draws can number their strips (firstInstance)
	bool HasInstance() const { return instance; }
	/* The per-triangle renderer's: room for the translucent triangles it
	 * sorts (TA_GPU_SORT), made with the rest; and whether there is */
	void WantSort(bool on) { sortWanted = on; }
	bool CanSort() const { return sortable; }

	// for measuring
	u64 statFrames = 0;		// decoded
	u64 statSkipped = 0;		// left to the CPU
	u64 statWhy[TA_GPU_WHY_COUNT] = {};
	double statGpuMs = 0;		// GPU time of the decodes timed
	double statGpuMaxMs = 0;
	u64 statGpuTimed = 0;
	u64 statSorted = 0;		// decoded with a sorted pass, sorted on this side
	double statSortUs = 0;		// ...what the sorts took
	double statSortMaxUs = 0;
	u64 statShadowed = 0;		// with draw groups the GPU says take shadows or not

private:
	bool Init(CommandPool& pool);
	void PollDone(CommandPool& pool);
	void Log(const char *when);

	struct Resources;
	std::unique_ptr<Resources> res;
	bool ready = false;
	bool failed = false;		// could not be made on this device: not tried again
	bool instance = false;
	bool sortWanted = false;
	bool sortable = false;
	int frames = 0;
	int index = 0;
	u32 serial = 0;			// this frame's
	u32 done = 0;			// every frame up to this one is done
	std::vector<u32> slotSerial;	// the frame each slot of the pool last had
	std::vector<bool> slotTimed;	// ...and whether it has a decode timed
	bool prepared = false;
	u32 flags = 0;
	TaGpuFrame frame;
	TaGpuDraw draw;
};
