/* Textures made on the GPU from their bytes in video memory.
 *
 * The texture cache converts a texture on the CPU, texel by texel, into
 * the upload ring, and the GPU copies the texels to the image. Here the
 * texture's bytes are copied into a ring of their own as they are - the
 * texels, the codebook of a compressed texture, the part of the palette a
 * paletted one uses - and a compute kernel makes the texels from them,
 * straight into the image: rend/texdecode.h, the same text the CPU side
 * is tested against, so the texels are the converters' to the bit. A
 * planar 16-bit texture in a format the image can have its texels in as
 * they are - 565 always, 1555 as A1R5G5B5, 4444 as B4G4R4A4 with its
 * channels swizzled back by the view - is not converted at all: its rows
 * are copied to the image as they are.
 *
 * What a frame asks for is recorded at the end of Process(), before the
 * frame's draws, in one command buffer of the texture pool's: a barrier for
 * all the images, the copies and the dispatches, a barrier for all of them
 * again. Everything is made when the renderer starts and sized then - the
 * pipelines, the ring (from the video memory budget), the descriptor pools
 * and the lists - and nothing is allocated while the game runs. A frame
 * that asks for more than there is room for has the rest converted on the
 * CPU, as before. The ring's piece and the descriptor pool of a frame are
 * used again when that frame's place comes round, which is when the
 * texture command pool has seen its fence (CommandPool::BeginFrame()). */
#pragma once
#include "vulkan.h"
#include "buffer.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

#include <memory>
#include <vector>

class CommandPool;

class TexDecoder
{
public:
	/* Everything made, for the swap chain the context has; again if that
	 * changed, nothing done otherwise. Nothing is decoded here when the
	 * device cannot. */
	void Init();
	void Term();
	bool Ready() const { return ready; }

	/* Whether an image of @format can be made by the kernel */
	bool Takes(vk::Format format) const;
	/* The format a planar 16-bit texture of @type is copied into as it is,
	 * or eUndefined */
	vk::Format NativeFormat(TextureType type) const;
	/* Whether this frame has @bytes of ring and @dispatches left */
	bool Room(u32 bytes, u32 dispatches) const;

	void BeginFrame(int index);
	/* @bytes of the frame's ring, and where in the ring's buffer they are
	 * (a multiple of 16); null if there is no room */
	u8 *Reserve(u32 bytes, u32 *offset);

	struct Level
	{
		vk::ImageView view;		// the level's storage view
		texd_u32 pc[6];			// base (in words), data, pal, wh, sr, mode
	};
	/* The texture's image made from the ring: @count levels, of an image
	 * of @mipLevels; @fresh if it has never been written. Returns what
	 * Cancel() takes. */
	int AddDecode(vk::Image image, bool fresh, u32 mipLevels, u32 out, const Level *levels, u32 count);
	/* The texture's image copied from the ring: @width by @height texels,
	 * rows of @rowLength, from @offset */
	int AddCopy(vk::Image image, bool fresh, u32 offset, u32 rowLength, u32 width, u32 height);
	/* What was added is not done after all (the texture is made another
	 * way, or goes); returns whether its image was fresh */
	bool Cancel(int job);
	u32 Serial() const { return serial; }

	/* What the frame asked for, recorded into a command buffer of @pool's */
	void Flush(CommandPool& pool);

	// what has been done so far, for measuring
	u64 statBytes = 0;
	u64 statTextures = 0;
	u64 statDispatches = 0;
	u64 statCopies = 0;
	u64 statFull = 0;		// textures that found no room

private:
	enum { MAX_JOBS = 1024, MAX_DISPATCHES = 4096 };

	struct Job
	{
		vk::Image image;
		u32 first, count;		// dispatches; for a copy, the copy's place in copies
		u32 levels;
		u32 out;
		bool fresh;
		bool copy;
		bool cancelled;
	};
	struct Copy
	{
		u32 offset, rowLength, width, height;
	};

	bool ready = false;
	bool flushed = false;	// this frame's are recorded: no more until the next
	int frames = 0;
	int index = 0;
	u32 serial = 0;

	std::unique_ptr<BufferData> ring;
	u8 *ringMapped = nullptr;
	u32 ringFrame = 0;		// bytes for each frame
	u32 ringUsed = 0;

	bool takes32 = false, takes16[3] = {}, takes8 = false;
	vk::Format native[3];		// by TextureType::_5551, _565, _4444

	vk::UniqueDescriptorSetLayout ringLayout, imageLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipelines[3];	// by TEXD_OUT8, 16, 32
	vk::UniqueDescriptorPool ringPool;
	vk::DescriptorSet ringSet;
	std::vector<vk::UniqueDescriptorPool> pools;	// by frame

	// sized once
	std::vector<Job> jobs;
	std::vector<Level> dispatches;
	std::vector<Copy> copies;
	std::vector<vk::DescriptorSetLayout> setLayouts;
	std::vector<vk::DescriptorSet> sets;
	std::vector<vk::DescriptorImageInfo> imageInfos;
	std::vector<vk::WriteDescriptorSet> writes;
	std::vector<vk::ImageMemoryBarrier> barriers;
	u32 jobCount = 0, dispatchCount = 0, copyCount = 0;
};
