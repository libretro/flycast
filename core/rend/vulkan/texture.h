/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

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
#include "vulkan_context.h"
#include "buffer.h"
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "rend/tex_pool.h"
#include "hw/pvr/Renderer_if.h"

#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout);

class CommandPool;
class TexDecoder;

/* Where textures wait on the way to their images: memory that stays mapped,
 * handed out a piece per upload and taken back a frame at a time.
 *
 * Every upload used to make a buffer of its own for this - a Vulkan buffer
 * and a piece of the heap - and retire it afterwards, and the texture was
 * converted into the cache's memory and copied into the buffer. A texture
 * is converted straight into its piece now (Texture::UploadMemory), and the
 * pieces come out of one buffer, made when the renderer starts (Init()), a
 * share of the video memory budget, and never grown: a ring, the frames'
 * pieces one after the other.
 *
 * A piece is spoken for until the frame it was handed out in has been
 * through the GPU: when that frame's place in the chain of frames comes
 * round again (SetCurrentIndex(), after the texture command pool waited for
 * its fence), or before, when the ring is full and that frame's fence,
 * polled, says it is done. Nothing waits for it: when the frames still on
 * the GPU hold the whole ring, there is no piece (Allocate() is null), and
 * the texture is made with the next frame instead. */
class CommandPool;
class UploadRing
{
public:
	/* The ring, @bytes of it, for the frames of @pool */
	void Init(size_t bytes, CommandPool *pool);
	/* @index is the current frame's from here on: whatever was handed out
	 * under it the last time is free again */
	void SetCurrentIndex(int index);
	/* @bytes of mapped memory, and where they are: in @buffer at @offset,
	 * a multiple of 16; null if the ring has no room */
	u8 *Allocate(size_t bytes, vk::Buffer *buffer, vk::DeviceSize *offset);
	void Term();
	bool Ready() const { return mapped != nullptr; }

	// what has been done so far, for measuring
	u64 statFull = 0;		// pieces there was no room for
	u64 statPolled = 0;		// frames found done before their turn

private:
	enum { MAX_FRAMES = 8 };
	std::unique_ptr<BufferData> buffer;
	u8 *mapped = nullptr;
	u64 size = 0;
	u64 head = 0, tail = 0;			// bytes handed out and given back, ever
	u64 frameEnd[MAX_FRAMES] = {};	// where each place's last frame ended,
	bool frameLive[MAX_FRAMES] = {};	// while it may be on the GPU
	u32 current = 0;
	CommandPool *pool = nullptr;
};

/* An image of the texture pool's: optimal, sampled, copied to and from,
 * and written by the decoding kernel too where the decoder takes its
 * format, with a storage view of each level for that */
struct PooledImage : TexPoolEntry
{
	vk::UniqueImage image;
	Allocation allocation;
	vk::UniqueImageView view;
	vk::UniqueImageView storageViews[TEXD_MAX_LEVELS];
	vk::ImageUsageFlags usage;
	vk::Format format = vk::Format::eUndefined;
	u32 levels = 1;
	bool storage = false;
	PooledImage *all = nullptr;		// every image there is, for Term()
};

/* The cache's textures' images (rend/tex_pool.h): made when the renderer
 * starts, in the device memory's large blocks, and handed out to the
 * textures that need one of their shape. And the pictures rendered to
 * (TakeTarget()), RGBA8, of their own kind of image. */
class TexturePool
{
public:
	/* (again with the renderer's next start, or a new size: nothing is done
	 * if it is made already) */
	void Init(const TexDecoder *decoder);
	void Term();
	bool Ready() const { return ready; }
	/* An image for a texture of @format, @width by @height, with its
	 * levels or not: a free one of that shape, or one made now if there is
	 * none. Null if the shape is not one of the pool's. */
	PooledImage *Take(vk::Format format, u32 width, u32 height, bool mips);
	/* An image to render to, @width by @height, with @usage */
	PooledImage *TakeTarget(u32 width, u32 height, const vk::ImageUsageFlags& usage);
	/* Free again now; or when the frames that may still use it are done */
	void Give(PooledImage *image) { book.Give(image); }
	void Retire(PooledImage *image) { book.Retire(image); }
	// (the texture command pool's frame index)
	void Frame(u32 index) { if (ready) book.Frame(index); }
	const TexPoolBook& Book() const { return book; }

private:
	enum { FORMATS = 8, TARGET = 7 };
	static int FormatIndex(vk::Format format);
	PooledImage *Make(int cls, bool late);

	TexPoolBook book;
	bool ready = false;
	const TexDecoder *decoder = nullptr;
	vk::Format formats[FORMATS];
	bool storable[FORMATS] = {};
	bool usable[FORMATS] = {};
	PooledImage *all = nullptr;
};

class Texture : public BaseTextureCacheData
{
public:
	~Texture();
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	bool GpuDecode(const texd_plan& plan) override;
	static UploadRing& Ring();
	/* The renderer's, while it has one */
	static void SetDecoder(TexDecoder *decoder) { Texture::decoder = decoder; }
	static void SetPool(TexturePool *pool) { Texture::pool = pool; }
	static TexturePool *Pool() { return pool; }
	/* A texture of the cache's: its images come from the pool. (Not the
	 * renderer's own pictures, which go when it says.) */
	void SetCached() { cached = true; }
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	std::string GetId() override { char s[20]; sprintf(s, "%p", this); return s; }
	bool IsNew() const { return !image.get() && pooled == nullptr; }
	vk::ImageView GetImageView() const { return pooled != nullptr ? *pooled->view : *imageView; }
	vk::Image GetImage() const { return pooled != nullptr ? *pooled->image : *image; }
	vk::ImageView GetReadOnlyImageView() const { return readOnlyImageView ? readOnlyImageView : GetImageView(); }
	void SetCommandBuffer(vk::CommandBuffer commandBuffer) { this->commandBuffer = commandBuffer; }
	/* Where a command buffer comes from when an upload needs one: a
	 * texture made on the GPU records nothing of its own */
	void SetCommandPool(CommandPool *commandPool) { this->commandPool = commandPool; }
	virtual bool Force32BitTexture(TextureType type) const override { return !VulkanContext::Instance()->IsFormatSupported(type); }

	void SetPhysicalDevice(vk::PhysicalDevice physicalDevice) { this->physicalDevice = physicalDevice; }
	void SetDevice(vk::Device device) { this->device = device; }

private:
	/* What an image is made for: written by the CPU as before; by the
	 * decoding kernel; by a copy of the texture's texels as they are */
	enum Maker { CPU, KERNEL, COPY };
	void Init(u32 width, u32 height, vk::Format format ,u32 dataSize, bool mipmapped, bool mipmapsIncluded, Maker maker = CPU);
	bool Prepare(int width, int height, bool mipmapped, bool mipmapsIncluded, Maker maker = CPU, vk::Format native = vk::Format::eUndefined);
	void SetImage(u32 size, void *data, bool isNew, bool genMipmaps);
   void CreateImage(vk::ImageTiling tiling, const vk::ImageUsageFlags& usage, vk::ImageLayout initialLayout,
			const vk::ImageAspectFlags& aspectMask, vk::ImageCreateFlags flags = vk::ImageCreateFlags());
	/* The job this texture has with the decoder this frame is not done
	 * after all: it is made another way, or goes */
	void CancelDecode();
	/* The image from the pool, for one of this shape; false if the pool
	 * has no such shape */
	bool TakePooled(u32 width, u32 height, bool mipmapped);
	/* The pool's image given back: when the frames that may use it are
	 * done, or now */
	void ReleasePooled(bool now);
	vk::ImageView StorageView(u32 level) const { return pooled != nullptr ? *pooled->storageViews[level] : *storageViews[level]; }
	/* The image rendered to, extent by extent, with @usage: the pool's, or
	 * one made (CreateImage()) where the pool has none of that shape */
	void CreateRenderTarget(const vk::ImageUsageFlags& usage);
	/* No room in the upload ring: the texture is made with the next frame,
	 * and until then its image is left as it is */
	void Defer(bool isNew);
	/* what the image was made to be used for */
	vk::ImageUsageFlags imageUsage;
	void GenerateMipmaps();

	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent;
	u32 mipmapLevels = 1;
	bool needsStaging = false;
	/* the piece of the upload ring the texture was converted into, between
	 * UploadMemory() and the UploadToGPU() that follows; and whether the
	 * image was made anew for it */
	u8 *slot = nullptr;
	vk::Buffer slotBuffer;
	vk::DeviceSize slotOffset = 0;
	bool prepared = false;
	bool preparedNew = false;
	vk::CommandBuffer commandBuffer;
	CommandPool *commandPool = nullptr;
	/* the image has never been written: it is made from nothing */
	bool fresh = false;
	/* made for the kernel to write, with a view for each level */
	bool storage = false;
	vk::UniqueImageView storageViews[TEXD_MAX_LEVELS];
	int decodeJob = -1;
	u32 decodeSerial = 0;
	static TexDecoder *decoder;
	static TexturePool *pool;
	PooledImage *pooled = nullptr;
	bool cached = false;

	Allocation allocation;
	vk::UniqueImage image;
	vk::UniqueImageView imageView;
	vk::ImageView readOnlyImageView;

	vk::PhysicalDevice physicalDevice;
	vk::Device device;

	friend class TextureDrawer;
	friend class OITTextureDrawer;
	friend class TextureCache;
};

class SamplerManager
{
public:
	vk::Sampler GetSampler(TSP tsp)
	{
		u32 samplerHash = tsp.full & TSP_Mask;	// MipMapD, FilterMode, ClampU, ClampV, FlipU, FlipV
		const auto& it = samplers.find(samplerHash);
		vk::Sampler sampler;
		if (it != samplers.end())
			return it->second.get();
		vk::Filter filter = tsp.FilterMode == 0 ? vk::Filter::eNearest : vk::Filter::eLinear;
		vk::SamplerAddressMode uRepeat = tsp.ClampU ? vk::SamplerAddressMode::eClampToEdge
				: tsp.FlipU ? vk::SamplerAddressMode::eMirroredRepeat : vk::SamplerAddressMode::eRepeat;
		vk::SamplerAddressMode vRepeat = tsp.ClampV ? vk::SamplerAddressMode::eClampToEdge
				: tsp.FlipV ? vk::SamplerAddressMode::eMirroredRepeat : vk::SamplerAddressMode::eRepeat;

		bool anisotropicFiltering = settings.rend.AnisotropicFiltering > 1 && VulkanContext::Instance()->SupportsSamplerAnisotropy()
				&& filter == vk::Filter::eLinear;
		return samplers.emplace(
					std::make_pair(samplerHash, VulkanContext::Instance()->GetDevice().createSamplerUnique(
						vk::SamplerCreateInfo(vk::SamplerCreateFlags(), filter, filter,
							vk::SamplerMipmapMode::eNearest, uRepeat, vRepeat, vk::SamplerAddressMode::eClampToEdge, D_Adjust_LoD_Bias[tsp.MipMapD],
							anisotropicFiltering, std::min((float)settings.rend.AnisotropicFiltering, VulkanContext::Instance()->GetMaxSamplerAnisotropy()),
							false, vk::CompareOp::eNever,
							0.0f, 256.0f, vk::BorderColor::eFloatOpaqueBlack)))).first->second.get();
	}
	void Term()
	{
		samplers.clear();
	}
	static const u32 TSP_Mask = 0x7ef00;

private:
	std::map<u32, vk::UniqueSampler> samplers;
};

class FramebufferAttachment
{
public:
	FramebufferAttachment(vk::PhysicalDevice physicalDevice, vk::Device device)
		: format(vk::Format::eUndefined), physicalDevice(physicalDevice), device(device)
		{}
   /* @readable: with memory this side can read, to copy the image into (it is as large as the image) */
   void Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, bool readable = true,
		   vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1);
	void Reset() { image.reset(); imageView.reset(); }

	vk::ImageView GetImageView() const { return *imageView; }
	vk::Image GetImage() const { return *image; }
	const BufferData* GetBufferData() const { return stagingBufferData.get(); }
	vk::ImageView GetStencilView() const { return *stencilView; }
	vk::Extent2D getExtent() const { return extent; }

private:
	vk::Format format;
	vk::Extent2D extent;

	std::unique_ptr<BufferData> stagingBufferData;
	Allocation allocation;
	vk::UniqueImage image;
	vk::UniqueImageView imageView;
	vk::UniqueImageView stencilView;

	vk::PhysicalDevice physicalDevice;
	vk::Device device;
};

class TextureCache : public BaseTextureCache<Texture>
{
public:
	void SetCurrentIndex(int index) {
		if (currentIndex < inFlightTextures.size())
			std::for_each(inFlightTextures[currentIndex].begin(), inFlightTextures[currentIndex].end(),
				[](Texture *texture) { texture->readOnlyImageView = vk::ImageView(); });
		currentIndex = index;
		Texture::Ring().SetCurrentIndex(index);
		if (Texture::Pool() != nullptr)
			Texture::Pool()->Frame(index);
		EmptyTrash(inFlightTextures);
		EmptyTrash(trashedImageViews);
		EmptyTrash(trashedImages);
		EmptyTrash(trashedMem);
	}

	bool IsInFlight(Texture *texture)
	{
		for (u32 i = 0; i < inFlightTextures.size(); i++)
			if (i != currentIndex && inFlightTextures[i].find(texture) != inFlightTextures[i].end())
				return true;
		return false;
	}

	void SetInFlight(Texture *texture)
	{
		inFlightTextures[currentIndex].insert(texture);
	}

	void DestroyLater(Texture *texture)
	{
		if (texture->pooled != nullptr)
		{
			texture->ReleasePooled(false);
			texture->format = vk::Format::eUndefined;
			return;
		}
		if (!texture->image)
			return;
		texture->CancelDecode();
		trashedImages[currentIndex].push_back(std::move(texture->image));
		trashedImageViews[currentIndex].push_back(std::move(texture->imageView));
		if (texture->storage)
			for (auto& view : texture->storageViews)
				if (view)
					trashedImageViews[currentIndex].push_back(std::move(view));
		texture->storage = false;
		trashedMem[currentIndex].push_back(std::move(texture->allocation));
		texture->format = vk::Format::eUndefined;
	}

	void Cleanup();

	/* Empties the cache between two frames. The frames before this one can
	 * still be on their way through the GPU, drawing with these textures, so
	 * the images are not destroyed here: they are retired, as a texture that
	 * is replaced is, and destroyed once those frames are done. */
	void ClearLater()
	{
		for (auto& pair : cache)
			DestroyLater(&pair.second);
		BaseTextureCache::Clear();
		for (auto& set : inFlightTextures)
			set.clear();
	}

	/* Empties the cache and destroys everything now, what was retired too.
	 * For when the renderer is going away and nothing is drawing any more. */
	void Clear()
	{
		BaseTextureCache::Clear();
		for (auto& set : inFlightTextures)
			set.clear();
      for (auto& v : trashedImageViews)
			v.clear();
		for (auto& v : trashedImages)
			v.clear();
		for (auto& v : trashedMem)
			v.clear();
		Texture::Ring().Term();
	}

private:
	bool clearTexture(Texture *tex)
	{
		for (auto& set : inFlightTextures)
			set.erase(tex);

		return tex->Delete();
	}
	template<typename T>
	void EmptyTrash(T& v)
	{
		if (v.size() < currentIndex + 1)
			v.resize(currentIndex + 1);
		v[currentIndex].clear();
	}
	std::vector<std::unordered_set<Texture *>> inFlightTextures;
	std::vector<std::vector<vk::UniqueImageView>> trashedImageViews;
	std::vector<std::vector<vk::UniqueImage>> trashedImages;
	std::vector<std::vector<Allocation>> trashedMem;
	u32 currentIndex = 0;
};
