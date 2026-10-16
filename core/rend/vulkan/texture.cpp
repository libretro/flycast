#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
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
#include <cmath>
#include "texture.h"
#include "utils.h"

#include <algorithm>
#include <memory>

void setImageLayout(vk::CommandBuffer const& commandBuffer, vk::Image image, vk::Format format, u32 mipmapLevels, vk::ImageLayout oldImageLayout, vk::ImageLayout newImageLayout)
{
	vk::AccessFlags sourceAccessMask;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eTransferDstOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::ePreinitialized:
		sourceAccessMask = vk::AccessFlagBits::eHostWrite;
		break;
	case vk::ImageLayout::eGeneral:     // sourceAccessMask is empty
	case vk::ImageLayout::eUndefined:
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags sourceStage;
	switch (oldImageLayout)
	{
	case vk::ImageLayout::eGeneral:
	case vk::ImageLayout::ePreinitialized:
		sourceStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		sourceStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eUndefined:
		sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	default:
		verify(false);
		break;
	}

	vk::AccessFlags destinationAccessMask;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		break;
	case vk::ImageLayout::eGeneral:   // empty destinationAccessMask
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eShaderRead;
		break;
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferRead;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
		destinationAccessMask = vk::AccessFlagBits::eTransferWrite;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead;
		break;
	default:
		verify(false);
		break;
	}

	vk::PipelineStageFlags destinationStage;
	switch (newImageLayout)
	{
	case vk::ImageLayout::eColorAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
		break;
	case vk::ImageLayout::eDepthStencilAttachmentOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
		break;
	case vk::ImageLayout::eGeneral:
		destinationStage = vk::PipelineStageFlagBits::eHost;
		break;
	case vk::ImageLayout::eShaderReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
		break;
	case vk::ImageLayout::eTransferDstOptimal:
	case vk::ImageLayout::eTransferSrcOptimal:
		destinationStage = vk::PipelineStageFlagBits::eTransfer;
		break;
	case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
		destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
		break;
	default:
		verify(false);
		break;
	}

	vk::ImageAspectFlags aspectMask;
	if (newImageLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal || newImageLayout == vk::ImageLayout::eDepthStencilReadOnlyOptimal)
	{
		aspectMask = vk::ImageAspectFlagBits::eDepth;
		if (format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint)
		{
			aspectMask |= vk::ImageAspectFlagBits::eStencil;
		}
	}
	else
	{
		aspectMask = vk::ImageAspectFlagBits::eColor;
	}

	vk::ImageSubresourceRange imageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1);
	vk::ImageMemoryBarrier imageMemoryBarrier(sourceAccessMask, destinationAccessMask, oldImageLayout, newImageLayout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, imageSubresourceRange);
	commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, nullptr, nullptr, imageMemoryBarrier);
}

#include "rend/upload_layout.h"
#include "tex_decoder.h"
#include "commandpool.h"
#include "hw/pvr/pvr_mem.h"

TexDecoder *Texture::decoder;
TexturePool *Texture::pool;

Texture::~Texture()
{
	CancelDecode();
	/* (a cache's texture goes when nothing has drawn with it for long, or
	 * when the renderer does: its image is free at once) */
	ReleasePooled(true);
}

/* ---- The texture pool ---- */

/* The formats the pool has images of, by its numbers for them: the
 * converters' five, the two the decoder copies planar textures into, and
 * the pictures rendered to */
static const vk::Format poolFormats[] = {
	vk::Format::eR5G5B5A1UnormPack16, vk::Format::eR5G6B5UnormPack16, vk::Format::eR4G4B4A4UnormPack16,
	vk::Format::eR8G8B8A8Unorm, vk::Format::eR8Unorm,
	vk::Format::eA1R5G5B5UnormPack16, vk::Format::eB4G4R4A4UnormPack16,
	vk::Format::eR8G8B8A8Unorm,
};
static const char * const poolFormatNames[] = { "5551", "565", "4444", "8888", "8-bit", "1555", "4444 (planar)", "8888 rendered to" };
// what the pictures rendered to are used for (the drawers' renderTargetUsage, and to be written as a texture after)
static const vk::ImageUsageFlags targetUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;

int TexturePool::FormatIndex(vk::Format format)
{
	for (int i = 0; i < FORMATS; i++)
		if (poolFormats[i] == format)
			return i;
	return -1;
}

void TexturePool::Init(const TexDecoder *decoder)
{
	if (ready)
		return;
	VulkanContext *context = VulkanContext::Instance();
	const vk::PhysicalDevice physicalDevice = context->GetPhysicalDevice();
	this->decoder = decoder;
	book.Reset(TexPoolBook::MAX_FRAMES);

	static const u32 texel[FORMATS] = { 2, 2, 2, 4, 1, 2, 2, 4 };
	// how much games use them, in eighths of what they use 565
	static const u32 use[FORMATS] = { 6, 8, 6, 4, 2, 1, 1, 0 };
	static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, 0, 0, TexPoolBook::TARGETS };
	u32 have[FORMATS];
	for (int f = 0; f < FORMATS; f++)
	{
		formats[f] = poolFormats[f];
		const vk::FormatProperties props = physicalDevice.getFormatProperties(formats[f]);
		const vk::FormatFeatureFlags needed = vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst
				| vk::FormatFeatureFlagBits::eTransferSrc;
		usable[f] = (props.optimalTilingFeatures & needed) == needed;
		if (f == TARGET)
			usable[f] = usable[f] && (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eColorAttachment);
		// the 16-bit ones the device cannot sample are converted to 32 bits (Force32BitTexture())
		if (f < 3 && !context->IsFormatSupported((TextureType)(f == 0 ? TextureType::_5551 : f == 1 ? TextureType::_565 : TextureType::_4444)))
			usable[f] = false;
		if (f >= 5)
			usable[f] = usable[f] && decoder != nullptr && decoder->NativeFormat(f == 5 ? TextureType::_5551 : TextureType::_4444) == formats[f];
		storable[f] = f < 5 && f != TARGET && decoder != nullptr && decoder->Ready() && decoder->Takes(formats[f]);
		have[f] = usable[f] ? texel[f] : 0;
	}
	u32 counts[TexPoolBook::CLASSES];
	const u64 bytes = TexPoolBook::Plan(context->GetVramBudget(), have, use, kind, FORMATS, counts);
	ready = true;
	for (int c = 0; c < TexPoolBook::CLASSES; c++)
		for (u32 i = 0; i < counts[c]; i++)
		{
			PooledImage *image = Make(c, false);
			if (image == nullptr)
				break;
			book.Give(image);
		}
	NOTICE_LOG(RENDERER, "Vulkan: texture pool of %u images, %u MB (planned %u MB)", book.made, (u32)(book.bytesMade >> 20), (u32)(bytes >> 20));
}

void TexturePool::Term()
{
	if (!ready)
		return;
	book.Summary("Vulkan");
	while (all != nullptr)
	{
		PooledImage *next = all->all;
		delete all;
		all = next;
	}
	book.Reset(TexPoolBook::MAX_FRAMES);
	ready = false;
	decoder = nullptr;
}

PooledImage *TexturePool::Make(int cls, bool late)
{
	const vk::Device device = VulkanContext::Instance()->GetDevice();
	const u32 f = TexPoolBook::Format(cls);
	const u32 width = TexPoolBook::Width(cls), height = TexPoolBook::Height(cls);
	const vk::Format format = formats[f];
	PooledImage *pi = new PooledImage();

	pi->format = format;
	pi->levels = TexPoolBook::Mips(cls) ? 1 + bitscanrev(std::max(width, height)) : 1;
	pi->storage = storable[f];
	pi->usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
	if (f == TARGET)
		pi->usage = targetUsage;
	vk::ImageCreateFlags flags;
	if (pi->storage)
	{
		/* written through views of a format of integers of the same size:
		 * the 16- and 8-bit formats cannot be storage images themselves */
		pi->usage |= vk::ImageUsageFlagBits::eStorage;
		flags = vk::ImageCreateFlagBits::eMutableFormat;
		if (format != vk::Format::eR8G8B8A8Unorm)
			flags |= vk::ImageCreateFlagBits::eExtendedUsage;
	}
	pi->image = device.createImageUnique(vk::ImageCreateInfo(flags, vk::ImageType::e2D, format, vk::Extent3D(width, height, 1), pi->levels, 1,
			vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, pi->usage, vk::SharingMode::eExclusive, 0, nullptr, vk::ImageLayout::eUndefined));
	pi->allocation = VulkanContext::Instance()->GetHeap().AllocateForImage(*pi->image, false, false);

	/* A 4444 texture copied as it is: the PowerVR has alpha, red, green
	 * and blue from the top, which this format calls blue, green, red and
	 * alpha */
	const vk::ComponentMapping components = format == vk::Format::eB4G4R4A4UnormPack16
			? vk::ComponentMapping(vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eA, vk::ComponentSwizzle::eB)
			: vk::ComponentMapping();
	vk::ImageViewCreateInfo viewInfo(vk::ImageViewCreateFlags(), *pi->image, vk::ImageViewType::e2D, format, components,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, pi->levels, 0, 1));
	// (sampled only: the image's other uses are through views of other formats)
	const vk::ImageViewUsageCreateInfo viewUsage(pi->usage & ~vk::ImageUsageFlagBits::eStorage);
	if (flags & vk::ImageCreateFlagBits::eExtendedUsage)
		viewInfo.pNext = &viewUsage;
	pi->view = device.createImageViewUnique(viewInfo);
	if (pi->storage)
	{
		const vk::Format viewFormat = format == vk::Format::eR8G8B8A8Unorm ? vk::Format::eR32Uint
				: format == vk::Format::eR8Unorm ? vk::Format::eR8Uint : vk::Format::eR16Uint;
		const vk::ImageViewUsageCreateInfo storageUsage(vk::ImageUsageFlagBits::eStorage);
		for (u32 level = 0; level < pi->levels; level++)
		{
			vk::ImageViewCreateInfo info(vk::ImageViewCreateFlags(), *pi->image, vk::ImageViewType::e2D, viewFormat, vk::ComponentMapping(),
					vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, level, 1, 0, 1));
			info.pNext = &storageUsage;
			pi->storageViews[level] = device.createImageViewUnique(info);
		}
	}
	book.Made(pi, cls, TexPoolBook::Bytes(cls, f == 3 || f == TARGET ? 4 : f == 4 ? 1 : 2), late, poolFormatNames[f]);
	pi->all = all;
	all = pi;
	return pi;
}

PooledImage *TexturePool::Take(vk::Format format, u32 width, u32 height, bool mips)
{
	const int f = FormatIndex(format);
	if (!ready || f < 0 || !usable[f])
		return nullptr;
	const int cls = TexPoolBook::Class((u32)f, width, height, mips);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<PooledImage *>(e) : Make(cls, true);
}

PooledImage *TexturePool::TakeTarget(u32 width, u32 height, const vk::ImageUsageFlags& usage)
{
	if (!ready || !usable[TARGET] || (usage & targetUsage) != usage)
		return nullptr;
	const int cls = TexPoolBook::Class(TARGET, width, height, false);
	if (cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<PooledImage *>(e) : Make(cls, true);
}

void Texture::CreateRenderTarget(const vk::ImageUsageFlags& usage)
{
	PooledImage *pi = pool != nullptr ? pool->TakeTarget(extent.width, extent.height, usage) : nullptr;
	if (pi == nullptr)
	{
		CreateImage(vk::ImageTiling::eOptimal, usage, vk::ImageLayout::eUndefined, vk::ImageAspectFlagBits::eColor);
		return;
	}
	// what it had goes, as CreateImage() lets it go
	CancelDecode();
	ReleasePooled(false);
	for (auto& view : storageViews)
		view.reset();
	imageView.reset();
	image.reset();
	allocation = Allocation();
	pooled = pi;
	// (a texture of the cache's, whose image is the pool's to take back)
	cached = true;
	mipmapLevels = 1;
	imageUsage = pi->usage;
	storage = false;
	fresh = true;
	debugPath = DBG_TARGET;
	debugLevels = 0;
}

bool Texture::TakePooled(u32 width, u32 height, bool mipmapped)
{
	if (pool == nullptr || !cached)
		return false;
	PooledImage *pi = pool->Take(format, width, height, mipmapped);
	if (pi == nullptr)
		return false;
	// what it had goes, as CreateImage() lets it go
	CancelDecode();
	ReleasePooled(false);
	for (auto& view : storageViews)
		view.reset();
	imageView.reset();
	image.reset();
	allocation = Allocation();
	pooled = pi;
	mipmapLevels = pi->levels;
	imageUsage = pi->usage;
	storage = pi->storage;
	needsStaging = true;
	// nothing in it is kept: whatever was drawn into it is another texture's
	fresh = true;
	return true;
}

void Texture::ReleasePooled(bool now)
{
	if (pooled == nullptr)
		return;
	CancelDecode();
	if (pool != nullptr)
	{
		if (now)
			pool->Give(pooled);
		else
			pool->Retire(pooled);
	}
	pooled = nullptr;
	storage = false;
}

/* A texel of this texture's, in bytes */
static u32 texel_bytes(TextureType type)
{
	return type == TextureType::_8888 ? 4 : type == TextureType::_8 ? 1 : 2;
}

UploadRing& Texture::Ring()
{
	static UploadRing ring;
	return ring;
}

void UploadRing::Init(size_t bytes, CommandPool *pool)
{
	this->pool = pool;
	if (mapped != nullptr && size == (u64)bytes)
		return;
	Term();
	this->pool = pool;
	buffer = std::unique_ptr<BufferData>(new BufferData(bytes, vk::BufferUsageFlagBits::eTransferSrc));
	mapped = (u8 *)buffer->MapMemory();
	size = bytes;
	head = tail = 0;
	current = 0;
	for (u32 i = 0; i < MAX_FRAMES; i++)
		frameLive[i] = false;
	INFO_LOG(RENDERER, "Vulkan: upload ring of %u KB", (u32)(bytes >> 10));
}

void UploadRing::Term()
{
	if (mapped != nullptr)
		NOTICE_LOG(RENDERER, "Vulkan: upload ring: %u MB handed out, %u frames found done before their turn, %u textures put off for want of room",
				(u32)(head >> 20), (u32)statPolled, (u32)statFull);
	mapped = nullptr;
	buffer.reset();
	size = head = tail = 0;
	for (u32 i = 0; i < MAX_FRAMES; i++)
		frameLive[i] = false;
}

void UploadRing::SetCurrentIndex(int index)
{
	if (mapped == nullptr)
		return;
	// where the frame that ends here ends; then this place's last frame is done
	frameEnd[current % MAX_FRAMES] = head;
	frameLive[current % MAX_FRAMES] = true;
	current = (u32)index;
	if (frameLive[current % MAX_FRAMES])
	{
		tail = std::max(tail, frameEnd[current % MAX_FRAMES]);
		frameLive[current % MAX_FRAMES] = false;
	}
}

u8 *UploadRing::Allocate(size_t bytes, vk::Buffer *buffer, vk::DeviceSize *offset)
{
	if (mapped == nullptr)
		return nullptr;
	bytes = (bytes + 15) & ~(size_t)15;
	if (bytes > size)
	{
		statFull++;
		return nullptr;
	}
	for (;;)
	{
		const u64 at = head % size;
		// (a piece is never split: from the start again if it would be)
		const u64 skip = at + bytes > size ? size - at : 0;
		if (head + skip + bytes - tail <= size)
		{
			head += skip;
			u8 *memory = mapped + head % size;
			*buffer = this->buffer->buffer.get();
			*offset = head % size;
			head += bytes;
			return memory;
		}
		/* Full: the oldest frame still out, if its fence says it is done
		 * (the frames are done in the order they went) */
		const int frames = pool != nullptr ? std::min(pool->GetSize(), (int)MAX_FRAMES) : 0;
		bool freed = false;
		for (int k = 1; k < frames && !freed; k++)
		{
			const u32 slot = (current + (u32)k) % (u32)frames;
			if (!frameLive[slot])
				continue;
			if (!pool->IsDone((int)slot))
				break;
			tail = std::max(tail, frameEnd[slot]);
			frameLive[slot] = false;
			statPolled++;
			freed = true;
		}
		if (!freed)
		{
			statFull++;
			return nullptr;
		}
	}
}

/* Makes the image what a texture of this size and kind needs, if it is not
 * already, for @maker to write; in format @native, if one is given.
 * Returns whether it was made anew. */
bool Texture::Prepare(int width, int height, bool mipmapped, bool mipmapsIncluded, Maker maker, vk::Format native)
{
	vk::Format format = vk::Format::eUndefined;
	switch (tex_type)
	{
	case TextureType::_5551:
		format = vk::Format::eR5G5B5A1UnormPack16;
		break;
	case TextureType::_565:
		format = vk::Format::eR5G6B5UnormPack16;
		break;
	case TextureType::_4444:
		format = vk::Format::eR4G4B4A4UnormPack16;
		break;
	case TextureType::_8888:
		format = vk::Format::eR8G8B8A8Unorm;
		break;
	case TextureType::_8:
		format = vk::Format::eR8Unorm;
		break;
	}
	if (native != vk::Format::eUndefined)
		format = native;
	if (width == (int)extent.width && height == (int)extent.height && format == this->format
			// an image made for the CPU to write where it is, or not for the kernel
			&& (maker == CPU || needsStaging) && (maker != KERNEL || storage)
			&& (imageUsage & vk::ImageUsageFlagBits::eTransferDst || !needsStaging))
		return false;
	Init(width, height, format, width * height * texel_bytes(tex_type), mipmapped, mipmapsIncluded, maker);
	return true;
}

void Texture::CancelDecode()
{
	if (decodeJob >= 0 && decoder != nullptr && decodeSerial == decoder->Serial() && decoder->Cancel(decodeJob))
		// what was to make it from nothing did not: the next upload does
		fresh = true;
	decodeJob = -1;
}

/* The kernel's, or a copy of the texels as they are: see tex_decoder.h.
 * Everything that can say no does before anything is changed. */
bool Texture::GpuDecode(const texd_plan& plan)
{
	if (decoder == nullptr || !decoder->Ready())
		return false;
	const vk::Format nativeFormat = plan.native ? decoder->NativeFormat(tex_type) : vk::Format::eUndefined;
	const bool copy = nativeFormat != vk::Format::eUndefined;
	if (!copy)
	{
		vk::Format format;
		switch (tex_type)
		{
		case TextureType::_5551: format = vk::Format::eR5G5B5A1UnormPack16; break;
		case TextureType::_565: format = vk::Format::eR5G6B5UnormPack16; break;
		case TextureType::_4444: format = vk::Format::eR4G4B4A4UnormPack16; break;
		case TextureType::_8888: format = vk::Format::eR8G8B8A8Unorm; break;
		default: format = vk::Format::eR8Unorm; break;
		}
		if (!decoder->Takes(format))
			return false;
	}
	const u32 bytes = plan.pal + plan.pal_entries * 4;
	if (!decoder->Room(bytes, copy ? 0 : plan.levels))
		return false;

	CancelDecode();
	prepared = false;
	Prepare(plan.width, plan.height, IsMipmapped(), plan.mipmapped != 0, copy ? COPY : KERNEL, nativeFormat);
	u32 offset;
	u8 * const to = decoder->Reserve(bytes, &offset);
	memcpy(to, &vram[plan.src], plan.bytes);
	if (plan.pal_entries != 0)
		memcpy(to + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);

	if (copy)
		decodeJob = decoder->AddCopy(GetImage(), fresh, offset, plan.level[0].sr & 0xFFFF, plan.width, plan.height);
	else
	{
		TexDecoder::Level levels[TEXD_MAX_LEVELS];
		for (u32 i = 0; i < plan.levels; i++)
		{
			const texd_level& lv = plan.level[i];
			levels[i].view = StorageView(lv.mip);
			levels[i].pc[0] = offset / 4;
			levels[i].pc[1] = lv.data;
			levels[i].pc[2] = plan.pal;
			levels[i].pc[3] = lv.wh;
			levels[i].pc[4] = lv.sr;
			levels[i].pc[5] = lv.mode;
		}
		decodeJob = decoder->AddDecode(GetImage(), fresh, mipmapLevels, plan.out, levels, plan.levels);
	}
	decodeSerial = decoder->Serial();
	fresh = false;
	debugPath = copy ? DBG_COPY : DBG_KERNEL;
	debugLevels = 0;
	return true;
}

void *Texture::UploadMemory(u32 width, u32 height, size_t bytes, u32 pixel, bool mipmaps)
{
	CancelDecode();
	prepared = true;
	preparedNew = Prepare(width, height, IsMipmapped(), mipmaps);
	if (!needsStaging)
		// a small image the texture is written into where it is, at the pitch it has
		return nullptr;
	const u32 pad = mipmaps && mipmapLevels > 1 ? upload_levels_pad(pixel) : 0;
	slot = Ring().Allocate(pad + bytes, &slotBuffer, &slotOffset);
	// (no room: converted into the cache's memory, and SetImage() finds none either)
	return slot != nullptr ? slot + pad : nullptr;
}

void Texture::UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	const u32 pixel = texel_bytes(tex_type);
	// the texels as they are handed over: the levels after one another, or the one image
	u32 dataSize = width * height * pixel;
	if (mipmapsIncluded)
		for (int w = width / 2; w != 0; w /= 2)
			dataSize += w * w * pixel;
	if (!prepared)
	{
		CancelDecode();
		preparedNew = Prepare(width, height, mipmapped, mipmapsIncluded);
	}
	prepared = false;
	SetImage(dataSize, data, preparedNew, mipmapped && !mipmapsIncluded);
	slot = nullptr;
}

void Texture::Init(u32 width, u32 height, vk::Format format, u32 dataSize, bool mipmapped, bool mipmapsIncluded, Maker maker)
{
	this->extent = vk::Extent2D(width, height);
	this->format = format;
	mipmapLevels = 1;
	if (mipmapped)
		mipmapLevels += floor(log2(std::max(width, height)));
	// an image of the pool's, made before the game: optimal, written through a staging copy or by the GPU
	if (TakePooled(width, height, mipmapped))
		return;

	if (maker != CPU)
	{
		/* Optimal, written by the GPU, and copied to as well: a texture
		 * made here once can be converted on the CPU the next time */
		needsStaging = true;
		vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
		vk::ImageCreateFlags flags;
		if (maker == KERNEL)
		{
			/* written through views of a format of integers of the same
			 * size: the 16- and 8-bit formats cannot be storage images
			 * themselves */
			usage |= vk::ImageUsageFlagBits::eStorage;
			flags = vk::ImageCreateFlagBits::eMutableFormat;
			if (format != vk::Format::eR8G8B8A8Unorm)
				flags |= vk::ImageCreateFlagBits::eExtendedUsage;
		}
		CreateImage(vk::ImageTiling::eOptimal, usage, vk::ImageLayout::eUndefined, vk::ImageAspectFlagBits::eColor, flags);
		if (maker == KERNEL)
		{
			const vk::Format viewFormat = format == vk::Format::eR8G8B8A8Unorm ? vk::Format::eR32Uint
					: format == vk::Format::eR8Unorm ? vk::Format::eR8Uint : vk::Format::eR16Uint;
			const vk::ImageViewUsageCreateInfo viewUsage(vk::ImageUsageFlagBits::eStorage);
			for (u32 level = 0; level < mipmapLevels; level++)
			{
				vk::ImageViewCreateInfo viewInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D, viewFormat, vk::ComponentMapping(),
						vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, level, 1, 0, 1));
				viewInfo.pNext = &viewUsage;
				storageViews[level] = device.createImageViewUnique(viewInfo);
			}
			storage = true;
		}
		return;
	}
	vk::FormatProperties formatProperties = physicalDevice.getFormatProperties(format);

	vk::ImageTiling imageTiling = (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)
			== vk::FormatFeatureFlagBits::eSampledImage
			? vk::ImageTiling::eOptimal
			: vk::ImageTiling::eLinear;
	if (height <= 32
			&& dataSize / height <= 64
			&& !mipmapped
			&& (formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage)
		imageTiling = vk::ImageTiling::eLinear;
	needsStaging = imageTiling != vk::ImageTiling::eLinear;
	vk::ImageLayout initialLayout;
	vk::ImageUsageFlags usageFlags = vk::ImageUsageFlagBits::eSampled;
	if (needsStaging)
	{
		usageFlags |= vk::ImageUsageFlagBits::eTransferDst;
		initialLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		verify((formatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) == vk::FormatFeatureFlagBits::eSampledImage);
		initialLayout = vk::ImageLayout::ePreinitialized;
	}
	if (mipmapped && !mipmapsIncluded)
		usageFlags |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	CreateImage(imageTiling, usageFlags, initialLayout, vk::ImageAspectFlagBits::eColor);
}

void Texture::CreateImage(vk::ImageTiling tiling, const vk::ImageUsageFlags& usage, vk::ImageLayout initialLayout,
		const vk::ImageAspectFlags& aspectMask, vk::ImageCreateFlags flags)
{
	// the old image's storage views go with it
	CancelDecode();
	ReleasePooled(false);
	for (auto& view : storageViews)
		view.reset();
	storage = false;
	fresh = true;
	imageUsage = usage;
	vk::ImageCreateInfo imageCreateInfo(flags, vk::ImageType::e2D, format, vk::Extent3D(extent, 1), mipmapLevels, 1,
										vk::SampleCountFlagBits::e1, tiling, usage,
										vk::SharingMode::eExclusive, 0, nullptr, initialLayout);
	image = device.createImageUnique(imageCreateInfo);

	allocation = VulkanContext::Instance()->GetHeap().AllocateForImage(*image,
			tiling == vk::ImageTiling::eLinear, !needsStaging);

	/* A 4444 texture copied as it is: the PowerVR has alpha, red, green
	 * and blue from the top, which this format calls blue, green, red and
	 * alpha */
	const vk::ComponentMapping components = format == vk::Format::eB4G4R4A4UnormPack16
			? vk::ComponentMapping(vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eA, vk::ComponentSwizzle::eB)
			: vk::ComponentMapping();
	vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D, format, components,
			vk::ImageSubresourceRange(aspectMask, 0, mipmapLevels, 0, 1));
	// (sampled only: the image's other uses are through views of other formats)
	const vk::ImageViewUsageCreateInfo viewUsage(usage & ~vk::ImageUsageFlagBits::eStorage);
	if (flags & vk::ImageCreateFlagBits::eExtendedUsage)
		imageViewCreateInfo.pNext = &viewUsage;
	imageView = device.createImageViewUnique(imageViewCreateInfo);
}

void Texture::Defer(bool isNew)
{
	if (isNew)
	{
		/* (an image never written is sampled all the same: it is made one
		 * that can be, whatever is in it) */
		commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		setImageLayout(commandBuffer, GetImage(), format, mipmapLevels, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal);
		commandBuffer.end();
	}
	fresh = false;
	if (cached)
		dirty = FrameCount != 0 ? FrameCount : 1;
}

/* The texture debug log's checksum of @size bytes (FNV-1a) */
static u32 DebugSum(const u8 *p, u32 size)
{
	u32 h = 2166136261u;
	for (u32 i = 0; i < size; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

void Texture::SetImage(u32 srcSize, void *srcData, bool isNew, bool genMipmaps)
{
	if (!commandBuffer && commandPool != nullptr)
		commandBuffer = commandPool->Allocate();
	verify((bool)commandBuffer);
	isNew = isNew || fresh;
	debugLevels = 0;
	if (needsStaging)
	{
		const u32 pixel = texel_bytes(tex_type);
		const u32 pad = mipmapLevels > 1 && !genMipmaps ? upload_levels_pad(pixel) : 0;
		/* Converted into its piece of the ring already - or, for what was
		 * not (a custom texture, one scaled up, the renderer's own small
		 * textures, one there was no room for), a piece for it now, and the
		 * one copy. None, and it waits for the next frame. */
		if (slot == nullptr || srcData != slot + pad)
		{
			slot = Ring().Allocate(pad + srcSize, &slotBuffer, &slotOffset);
			if (slot == nullptr)
			{
				Defer(isNew);
				return;
			}
			memcpy(slot + pad, srcData, srcSize);
		}
	}
	fresh = false;
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	if (!isNew && !needsStaging)
		setImageLayout(commandBuffer, GetImage(), format, mipmapLevels, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eGeneral);

	if (needsStaging)
	{
		const u32 pixel = texel_bytes(tex_type);
		const bool levels = mipmapLevels > 1 && !genMipmaps;
		const u32 pad = levels ? upload_levels_pad(pixel) : 0;
		if (pad != 0)
			// the 1x1 level, to where a copy can start: see upload_layout.h
			memcpy(slot, slot + pad, pixel);

		// Since we're going to blit to the texture image, set its layout to eTransferDstOptimal
		setImageLayout(commandBuffer, GetImage(), format, mipmapLevels, isNew ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageLayout::eTransferDstOptimal);

		if (levels)
		{
			for (u32 i = 0; i < mipmapLevels; i++)
			{
				vk::BufferImageCopy copyRegion(slotOffset + upload_level_at(pixel, i), 1 << i, 1 << i,
						vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, mipmapLevels - i - 1, 0, 1),
						vk::Offset3D(0, 0, 0), vk::Extent3D(1 << i, 1 << i, 1));
				commandBuffer.copyBufferToImage(slotBuffer, GetImage(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
				if (settings.rend.TextureDebug)
					debugSum[mipmapLevels - i - 1] = DebugSum(slot + upload_level_at(pixel, i), (1u << i) * (1u << i) * pixel);
			}
			debugLevels = settings.rend.TextureDebug ? mipmapLevels : 0;
		}
		else
		{
			if (settings.rend.TextureDebug)
			{
				debugSum[0] = DebugSum(slot, extent.width * extent.height * pixel);
				debugLevels = 1;
			}
			vk::BufferImageCopy copyRegion(slotOffset, extent.width, extent.height, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
					vk::Offset3D(0, 0, 0), vk::Extent3D(extent, 1));
			commandBuffer.copyBufferToImage(slotBuffer, GetImage(), vk::ImageLayout::eTransferDstOptimal, copyRegion);
			if (mipmapLevels > 1)
				GenerateMipmaps();
		}
		// Set the layout for the texture image from eTransferDstOptimal to SHADER_READ_ONLY
		setImageLayout(commandBuffer, GetImage(), format, mipmapLevels, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
		debugPath = DBG_STAGED;
	}
	else
	{
		void *data = allocation.MapMemory();
		verify(data != nullptr);
		vk::SubresourceLayout layout = device.getImageSubresourceLayout(GetImage(), vk::ImageSubresource(vk::ImageAspectFlagBits::eColor, 0, 0));
		if (layout.size != srcSize)
		{
			u8 *src = (u8 *)srcData;
			u8 *dst = (u8 *)data;
			u32 srcSz = extent.width * 2;
			if (tex_type == TextureType::_8888)
				srcSz *= 2;
			else if (tex_type == TextureType::_8)
				srcSz /= 2;
			u8 * const srcEnd = src + srcSz * extent.height;
			for (; src < srcEnd; src += srcSz, dst += layout.rowPitch)
				memcpy(dst, src, srcSz);
		}
		else
			memcpy(data, srcData, srcSize);
		debugPath = DBG_LINEAR;
		debugLevels = 0;
		if (mipmapLevels > 1)
			GenerateMipmaps();
		else
			// If we can use the linear tiled image as a texture, just do it
			setImageLayout(commandBuffer, GetImage(), format, mipmapLevels, isNew ? vk::ImageLayout::ePreinitialized : vk::ImageLayout::eGeneral,
					vk::ImageLayout::eShaderReadOnlyOptimal);
	}
	commandBuffer.end();
}

void Texture::GenerateMipmaps()
{
	u32 mipWidth = extent.width;
	u32 mipHeight = extent.height;
	vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			GetImage(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

	for (u32 i = 1; i < mipmapLevels; i++)
	{
		// Transition previous mipmap level from dst optimal/preinit to src optimal
		barrier.subresourceRange.baseMipLevel = i - 1;
		if (i == 1 && !needsStaging)
		{
			barrier.oldLayout = vk::ImageLayout::ePreinitialized;
			barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
		}
		else
		{
			barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
			barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		}
		barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, barrier);

		// Blit previous mipmap level on current
		vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i - 1, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(mipWidth, mipHeight, 1) } },
				 vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i, 0, 1),
				 { { vk::Offset3D(0, 0, 0), vk::Offset3D(std::max(mipWidth / 2, 1u), std::max(mipHeight / 2, 1u), 1) } });
		commandBuffer.blitImage(GetImage(), vk::ImageLayout::eTransferSrcOptimal, GetImage(), vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);

		// Transition previous mipmap level from src optimal to shader read-only optimal
		barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);

		mipWidth = std::max(mipWidth / 2, 1u);
		mipHeight = std::max(mipHeight / 2, 1u);
	}
	// Transition last mipmap level from dst optimal to shader read-only optimal
	barrier.subresourceRange.baseMipLevel = mipmapLevels - 1;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
	barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, barrier);
}

void FramebufferAttachment::Init(u32 width, u32 height, vk::Format format, const vk::ImageUsageFlags& usage, bool readable,
		vk::SampleCountFlagBits samples)
{
	this->format = format;
	this->extent = vk::Extent2D { width, height };
	bool depth = format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD16UnormS8Uint;

	if (readable && (usage & vk::ImageUsageFlagBits::eTransferSrc))
	{
		stagingBufferData = std::unique_ptr<BufferData>(new BufferData(width * height * 4,
				vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst));
	}
	vk::ImageCreateInfo imageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, format, vk::Extent3D(extent, 1), 1, 1, samples,
			vk::ImageTiling::eOptimal, usage,
			vk::SharingMode::eExclusive, 0, nullptr, vk::ImageLayout::eUndefined);
	image = device.createImageUnique(imageCreateInfo);

	allocation = VulkanContext::Instance()->GetHeap().AllocateForImage(*image, false, false,
			(bool)(usage & vk::ImageUsageFlagBits::eTransientAttachment));

	vk::ImageViewCreateInfo imageViewCreateInfo(vk::ImageViewCreateFlags(), image.get(), vk::ImageViewType::e2D,
			format, vk::ComponentMapping(),	vk::ImageSubresourceRange(depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	imageView = device.createImageViewUnique(imageViewCreateInfo);

	if ((usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) && (usage & vk::ImageUsageFlagBits::eInputAttachment))
	{
		// Also create an imageView for the stencil
		imageViewCreateInfo.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1);
		stencilView = device.createImageViewUnique(imageViewCreateInfo);
	}
}

void TextureCache::Cleanup()
{
	std::vector<u64> list;

	u32 TargetFrame = std::max((u32)120, FrameCount) - 120;

	for (const auto& pair : cache)
	{
		if (pair.second.dirty && pair.second.dirty < TargetFrame)
			list.push_back(pair.first);

		if (list.size() > 5)
			break;
	}

	for (u64 id : list)
	{
		if (clearTexture(&cache[id]))
			cache.erase(id);
	}
}

/* Bytes a texel of a texture image's @format */
static u32 DebugTexelBytes(vk::Format format)
{
	switch (format)
	{
	case vk::Format::eR8Unorm:
		return 1;
	case vk::Format::eR8G8B8A8Unorm:
	case vk::Format::eB8G8R8A8Unorm:
		return 4;
	default:
		return 2;
	}
}

/* The texture debug log (settings.rend.TextureDebug), for finding out on a
 * machine where a texture is drawn wrong what the textures and their
 * samplers are there. About every ten seconds, a line for each texture of
 * the frame drawn last: what the game asked for, the image made of it, how
 * it was written, the sampler it is drawn with, and each level of the image
 * read back from the GPU with its checksum, beside the checksum of what the
 * CPU uploaded into that level (the GPU's own decode and generated levels
 * have none). The read back waits for the GPU, and the buffer for it is made
 * each time: this is a diagnostic, off unless asked for. */
void TextureCache::DebugLog(const std::unordered_set<Texture *>& textures)
{
	static u32 lastFrame;
	if (FrameCount < 300 || (lastFrame != 0 && FrameCount - lastFrame < 600) || textures.empty())
		return;
	lastFrame = FrameCount;
	VulkanContext *context = VulkanContext::Instance();
	const vk::Device device = context->GetDevice();
	vk::PhysicalDeviceProperties props;
	context->GetPhysicalDevice().getProperties(&props);
	NOTICE_LOG(RENDERER, "TexDebug: frame %u, %u textures; %s (driver %08x); sub-pixel bits %u, sub-texel bits %u, mipmap bits %u, LOD bias max %.1f; force LOD0 %d, anisotropy %d, GPU decode %d, upscale %d",
			FrameCount, (u32)textures.size(), (const char *)&props.deviceName[0], props.driverVersion, props.limits.subPixelPrecisionBits,
			props.limits.subTexelPrecisionBits, props.limits.mipmapPrecisionBits, props.limits.maxSamplerLodBias,
			settings.rend.ForceTextureLOD0, settings.rend.AnisotropicFiltering, settings.rend.GpuTextureDecode, settings.rend.TextureUpscale);

	// what is read back, and where
	struct Read { Texture *t; u32 level; u32 offset; u32 bytes; };
	std::vector<Read> reads;
	vk::DeviceSize total = 0;
	for (Texture *t : textures)
	{
		if (t->format == vk::Format::eUndefined || t->fresh || t->decodeJob >= 0 || t->IsNew()
				|| !(t->imageUsage & vk::ImageUsageFlagBits::eTransferSrc) || t->debugPath == Texture::DBG_TARGET)
			continue;
		const u32 texel = DebugTexelBytes(t->format);
		for (u32 l = 0; l < t->mipmapLevels; l++)
		{
			const u32 w = std::max(t->extent.width >> l, 1u), h = std::max(t->extent.height >> l, 1u);
			reads.push_back({ t, l, (u32)total, w * h * texel });
			total = (total + w * h * texel + 3) & ~(vk::DeviceSize)3;
		}
	}
	std::unique_ptr<BufferData> buffer;
	if (total != 0)
	{
		buffer.reset(new BufferData(total, vk::BufferUsageFlagBits::eTransferDst,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent));
		vk::UniqueCommandPool pool = device.createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient,
				context->GetGraphicsQueueFamilyIndex()));
		vk::CommandBuffer cmd = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 1)).front();
		cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		Texture *last = nullptr;
		for (const Read& r : reads)
		{
			Texture *t = r.t;
			const vk::ImageSubresourceRange all(vk::ImageAspectFlagBits::eColor, 0, t->mipmapLevels, 0, 1);
			if (t != last)
			{
				// (after everything sent before, which wrote it or drew with it)
				const vk::ImageMemoryBarrier toSrc(vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eTransferRead,
						vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
						t->GetImage(), all);
				cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, toSrc);
			}
			const u32 w = std::max(t->extent.width >> r.level, 1u), h = std::max(t->extent.height >> r.level, 1u);
			cmd.copyImageToBuffer(t->GetImage(), vk::ImageLayout::eTransferSrcOptimal, *buffer->buffer,
					vk::BufferImageCopy(r.offset, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, r.level, 0, 1),
							vk::Offset3D(0, 0, 0), vk::Extent3D(w, h, 1)));
			if (r.level + 1 == t->mipmapLevels)
			{
				const vk::ImageMemoryBarrier back(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
						vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
						t->GetImage(), all);
				cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, nullptr, nullptr, back);
			}
			last = t;
		}
		const vk::MemoryBarrier toHost(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost, {}, toHost, nullptr, nullptr);
		cmd.end();
		vk::UniqueFence fence = device.createFenceUnique(vk::FenceCreateInfo());
		context->SubmitCommandBuffers(1, &cmd, *fence);
		if (device.waitForFences(1, &fence.get(), true, UINT64_MAX) != vk::Result::eSuccess)
			reads.clear();
	}
	const u8 *mem = buffer ? (const u8 *)buffer->MapMemory() : nullptr;

	static const char * const paths[] = { "none", "cpu-staged", "cpu-linear", "gpu-kernel", "gpu-copy", "render-target" };
	static const char * const filters[] = { "point", "bilinear", "trilinear-a", "trilinear-b" };
	for (Texture *t : textures)
	{
		char line[1024];
		const TSP tsp = t->tsp;
		const char *addr = tsp.ClampU ? "clamp" : tsp.FlipU ? "mirror" : "repeat";
		const char *addv = tsp.ClampV ? "clamp" : tsp.FlipV ? "mirror" : "repeat";
		int n = snprintf(line, sizeof(line), "TexDebug: %08x tcw %08x tsp %08x %ux%u %s%s%s%s image %ux%u vk-format %d levels %u %s%s path %s; sampler %s u %s v %s bias %.2f aniso %d lod 0-256 mip nearest;",
				t->sa_tex, t->tcw.full, tsp.full, t->w, t->h, t->GetPixelFormatName(), t->tcw.VQ_Comp ? " vq" : "",
				t->tcw.MipMapped ? " mip-bit" : "", t->tcw.ScanOrder && !t->IsPaletted() ? " planar" : "",
				t->extent.width, t->extent.height, (int)t->format, t->mipmapLevels, t->pooled != nullptr ? "pooled" : "own",
				t->storage ? " storage" : "", paths[t->debugPath < 6 ? t->debugPath : 0],
				filters[tsp.FilterMode], addr, addv, D_Adjust_LoD_Bias[tsp.MipMapD],
				tsp.FilterMode != 0 && settings.rend.AnisotropicFiltering > 1 && context->SupportsSamplerAnisotropy() ? settings.rend.AnisotropicFiltering : 0);
		bool any = false;
		for (const Read& r : reads)
		{
			if (r.t != t || n >= (int)sizeof(line) - 48)
				continue;
			any = true;
			const u32 gpu = DebugSum(mem + r.offset, r.bytes);
			if (r.level < t->debugLevels)
				n += snprintf(line + n, sizeof(line) - n, " L%u %08x/%08x %s", r.level, gpu, t->debugSum[r.level],
						gpu == t->debugSum[r.level] ? "ok" : "DIFF");
			else
				n += snprintf(line + n, sizeof(line) - n, " L%u %08x", r.level, gpu);
		}
		if (!any)
			snprintf(line + n, sizeof(line) - n, " not read back");
		NOTICE_LOG(RENDERER, "%s", line);
	}
}
