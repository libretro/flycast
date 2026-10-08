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
#include "hw/pvr/Renderer_if.h"

#include <algorithm>
#include <memory>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout);

/* Where textures wait on the way to their images: memory that stays mapped,
 * handed out a piece per upload and taken back a frame at a time.
 *
 * Every upload used to make a buffer of its own for this - a Vulkan buffer
 * and a piece of the heap - and retire it afterwards, and the texture was
 * converted into the cache's memory and copied into the buffer. A texture
 * is converted straight into its piece now (Texture::UploadMemory), and the
 * pieces come out of a few large buffers that are made once.
 *
 * A piece is spoken for until the frame it was handed out in has been
 * through the GPU, which is when that frame's place in the chain of frames
 * comes round again (SetCurrentIndex): the same life as the command buffer
 * the copy was recorded in. */
class UploadRing
{
public:
	/* @index is the current frame's from here on: whatever was handed out
	 * under it the last time is free again, and buffers that have gone
	 * unused for a long while are given back */
	void SetCurrentIndex(int index);
	/* @bytes of mapped memory, and where they are: in @buffer at @offset,
	 * a multiple of 16 */
	u8 *Allocate(size_t bytes, vk::Buffer *buffer, vk::DeviceSize *offset);
	void Term() { chunks.clear(); }

private:
	struct Chunk
	{
		std::unique_ptr<BufferData> buffer;
		u8 *mapped;
		size_t size;
		size_t used;
		u32 idle;			// turns of its frame's place it has gone unused
		bool touched;
	};
	enum { IDLE_ROUNDS = 200 };
	std::vector<std::vector<Chunk>> chunks;		// by frame index
	u32 current = 0;
};

class Texture : public BaseTextureCacheData
{
public:
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded = false) override;
	void *UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps) override;
	static UploadRing& Ring();
	u64 GetIntId() { return (u64)reinterpret_cast<uintptr_t>(this); }
	std::string GetId() override { char s[20]; sprintf(s, "%p", this); return s; }
	bool IsNew() const { return !image.get(); }
	vk::ImageView GetImageView() const { return *imageView; }
	vk::Image GetImage() const { return *image; }
	vk::ImageView GetReadOnlyImageView() const { return readOnlyImageView ? readOnlyImageView : *imageView; }
	void SetCommandBuffer(vk::CommandBuffer commandBuffer) { this->commandBuffer = commandBuffer; }
	virtual bool Force32BitTexture(TextureType type) const override { return !VulkanContext::Instance()->IsFormatSupported(type); }

	void SetPhysicalDevice(vk::PhysicalDevice physicalDevice) { this->physicalDevice = physicalDevice; }
	void SetDevice(vk::Device device) { this->device = device; }

private:
	void Init(u32 width, u32 height, vk::Format format ,u32 dataSize, bool mipmapped, bool mipmapsIncluded);
	bool Prepare(int width, int height, bool mipmapped, bool mipmapsIncluded);
	void SetImage(u32 size, void *data, bool isNew, bool genMipmaps);
   void CreateImage(vk::ImageTiling tiling, const vk::ImageUsageFlags& usage, vk::ImageLayout initialLayout,
			const vk::ImageAspectFlags& aspectMask);
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
   void Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, bool readable = true);
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
		if (!texture->image)
			return;
		trashedImages[currentIndex].push_back(std::move(texture->image));
		trashedImageViews[currentIndex].push_back(std::move(texture->imageView));
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
