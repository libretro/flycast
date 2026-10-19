/* GPU-side TA processing in OpenGL 4.3 (rend/ta_gpu.h): a frame's vertices,
 * indices and modifier volume triangles made by compute shaders from the
 * blocks the game sent, and the frame drawn from them by both OpenGL
 * renderers - per triangle (gles) and per pixel (gl4) - with nothing read
 * back. Vulkan's is vulkan/ta_decoder.h; this is the same, in OpenGL's terms.
 *
 * - The TA contexts' data are slices of one buffer, persistently mapped,
 *   coherent, in the client's memory (glBufferStorage(), OpenGL 4.4 or
 *   ARB_buffer_storage): the SH4 writes a frame straight into memory the
 *   GPU reads, and reads its control words back from it. Without that
 *   storage, or where reading it is far slower than ordinary memory (the
 *   driver gave write-combined memory), TA processing stays on the CPU.
 *   A slice is written again once the GPU is done with the frame that read
 *   it, which is known by polling that frame's fence (glClientWaitSync()
 *   with no time to wait), never by waiting.
 * - At the start of Process() the header pass (ta_gpu_prepare()) writes the
 *   frame's tables into this frame's piece of a persistently mapped ring,
 *   and makes the render context's lists of draw groups in the full
 *   parse's place; the decode is dispatched then. The draws that follow are
 *   indirect (DrawGroup()): their counts are the GPU's. A frame the header
 *   pass does not do, or one whose piece of the ring the GPU still has, is
 *   parsed and drawn as ever.
 * - The kernels are the SPIR-V for OpenGL made when the core is built
 *   (rend/ta_gpu_gl_spv.inc) where OpenGL takes SPIR-V (4.6 or
 *   ARB_gl_spirv); elsewhere the header's text (rend/ta_gpu_glsl.inc) is
 *   compiled, once, and the programs kept on disk with the renderers' own
 *   (glprogram_cache.h), by the driver and the text, for the next time.
 *   Every buffer (sized for the largest frame the machine has) is made in
 *   Setup(), when the renderer is set up or the option changes: nothing is
 *   allocated or compiled while a game runs.
 * - The outputs are on the GPU, one copy: OpenGL runs the decodes and the
 *   draws that read them in the order they were given.
 *
 * Off (TA processing: CPU), or in an OpenGL before 4.3 (and in OpenGL ES),
 * none of this exists, and Ready() says no. */
#pragma once
#include "types.h"
#include "hw/pvr/ta.h"

#ifdef HAVE_GL4

#include <glsm/glsm.h>
#include <memory>

/* What a frame decoded on the GPU is drawn from (rend_context::gpu): as
 * vulkan/ta_decoder.h's TaGpuDraw has it */
struct GlTaDraw
{
	GLuint vertices = 0, indices = 0, modtrig = 0, trParams = 0;
	/* the frame's words (TAG_O_*): 1/fZ_max at TAG_O_ZSCALE, which the
	 * renderers copy into their depth scale uniform buffer (rend/depth_scale.h) */
	GLuint frame = 0;
	/* (the per-pixel renderer) a word for each translucent strip, its
	 * number: an attribute of one a draw, which a draw record's baseInstance
	 * picks (TA_GPU_INSTANCE) */
	GLuint instances = 0;
	/* (TA_GPU_SORT) this frame's room for the sorted translucent triangles'
	 * indices (ta_gpu_sorted_write()), mapped, in the element buffer
	 * @sorted from @sortedOffset bytes on */
	GLuint sorted = 0;
	GLintptr sortedOffset = 0;
	u32 *sortedMap = nullptr;
};

class GlTaDecoder
{
public:
	GlTaDecoder();
	~GlTaDecoder();

	/* Made, or taken down, as the option says (@wanted): when the renderer
	 * is set up and when the options change, never while a frame is drawn;
	 * with the context bound. Off, or where it cannot be made, nothing of
	 * it exists. */
	void Setup(bool wanted);
	void Term();
	bool Ready() const { return ready; }
	/* Whether the context the renderer has (findGLVersion()) is one this
	 * can be made in: OpenGL 4.3 with persistently mapped buffers. (Whether
	 * it is made is found out in Setup().) */
	static bool ContextCanDo();
	/* At the start of each Process(): the GPU's progress, polled, told to
	 * the TA contexts' pool; nothing is made here */
	void BeginFrame();
	/* The frame's tables, and with TA_GPU_DRAW (in @flags, TA_GPU_*) its
	 * render context made ready to draw in the full parse's place; whether
	 * it is decoded, and then (@rv) whether it has something to draw */
	bool Prepare(TA_context *ctx, u32 flags, bool *rv);
	// ...and its decode dispatched
	void Record(TA_context *ctx);
	/* After the frame's draws (Render()): its fence, by which its slice and
	 * its piece of the ring are known to be free again */
	void EndFrame();

	const GlTaDraw& Draw() const { return draw; }
	/* The per-triangle renderer's: room for the translucent triangles it
	 * sorts (TA_GPU_SORT), made with the rest; and whether there is */
	void WantSort(bool on) { sortWanted = on; }
	bool CanSort() const { return sortable; }

	/* A draw group of a frame decoded on the GPU (PolyParam::first and count:
	 * see TA_GPU_DRAW) drawn as triangle strips from the decode's indices,
	 * which are bound: an opaque or punch-through one by its command - or
	 * with @shadows by its second command, which the GPU wrote for its
	 * second drawing for shadows (TA_GPU_SHADOWS) - a translucent one by its
	 * draw records, with their count (ARB_indirect_parameters, OpenGL 4.6)
	 * or all of its room, whose records past the count draw nothing */
	void DrawGroup(u32 listType, u32 first, u32 count, bool shadows = false);
	/* The decode's vertices bound as GL_ARRAY_BUFFER and its indices as
	 * GL_ELEMENT_ARRAY_BUFFER; its modifier volume triangles as
	 * GL_ARRAY_BUFFER; and (TA_GPU_SORT) this frame's sorted indices written
	 * (ta_gpu_sorted_write(): by Record(), before the prepass's kernel reads
	 * them), and bound as GL_ELEMENT_ARRAY_BUFFER, from the byte offset it
	 * returns */
	void BindGeometry();
	void BindModVols();
	void WriteSorted();
	GLintptr BindSorted();

	// for measuring
	u64 statFrames = 0;		// decoded
	u64 statSkipped = 0;		// left to the CPU
	u64 statBusy = 0;		// ...of them, because the GPU still had this frame's piece of the ring
	u64 statWhy[TA_GPU_WHY_COUNT] = {};
	double statGpuMs = 0;		// GPU time of the decodes timed
	double statGpuMaxMs = 0;
	u64 statGpuTimed = 0;
	u64 statSorted = 0;
	double statSortUs = 0;
	double statSortMaxUs = 0;
	u64 statShadowed = 0;

private:
	bool Init();
	void PollDone();
	void Log(const char *when);
	void BindIndirect(GLuint buffer, bool parameter);

	struct Resources;
	std::unique_ptr<Resources> res;
	bool ready = false;
	bool failed = false;		// could not be made in this context: not tried again
	bool sortWanted = false;
	bool sortable = false;
	bool prepared = false;
	bool recorded = false;
	bool busy = false;		// this frame's piece of the ring is the GPU's still
	int index = 0;
	u32 serial = 0;
	u32 done = 0;
	u32 flags = 0;
	GLuint boundIndirect = 0, boundParameter = 0;
	TaGpuFrame frame;
	GlTaDraw draw;
};

#else

/* (no OpenGL 4: never ready, and a frame is never drawn from the GPU) */
class GlTaDecoder
{
public:
	void Setup(bool wanted) { (void)wanted; }
	void Term() {}
	bool Ready() const { return false; }
	static bool ContextCanDo() { return false; }
	void BeginFrame() {}
	bool Prepare(TA_context *ctx, u32 flags, bool *rv) { (void)ctx; (void)flags; (void)rv; return false; }
	void Record(TA_context *ctx) { (void)ctx; }
	void EndFrame() {}
	void WantSort(bool on) { (void)on; }
	bool CanSort() const { return false; }
	void DrawGroup(u32 listType, u32 first, u32 count, bool shadows = false) { (void)listType; (void)first; (void)count; (void)shadows; }
	void BindGeometry() {}
	void BindModVols() {}
	void WriteSorted() {}
	intptr_t BindSorted() { return 0; }
	// (the frame's words of a decode that never is: no buffer)
	struct { unsigned frame = 0; } drawNone;
	const decltype(drawNone)& Draw() const { return drawNone; }
};

#endif

// the one both OpenGL renderers have (there is one renderer at a time)
extern GlTaDecoder glTaDecoder;
