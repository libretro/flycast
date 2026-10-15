#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
	Created on: Oct 8, 2019

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
#include <math.h>
#include "rtt_read.h"
#include "rend/last_picture.h"
#include "drawer.h"
#include "hw/pvr/pvr_mem.h"

void Drawer::SortTriangles()
{
	sortedPolys.resize(pvrrc.render_passes.used());
	sortedIndexes.resize(pvrrc.render_passes.used());
	sortedIndexCount = 0;
	RenderPass previousPass = {};

	for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];
		sortedIndexes[render_pass].clear();
		if (current_pass.autosort)
		{
			GenSorted(previousPass.tr_count, current_pass.tr_count - previousPass.tr_count, sortedPolys[render_pass], sortedIndexes[render_pass], true);
			for (auto& poly : sortedPolys[render_pass])
				poly.first += sortedIndexCount;
			sortedIndexCount += sortedIndexes[render_pass].size();
		}
		else
			sortedPolys[render_pass].clear();
		previousPass = current_pass;
	}
}

TileClipping BaseDrawer::SetTileClip(u32 val, vk::Rect2D& clipRect)
{
	int rect[4] = {};
	TileClipping clipmode = ::GetTileClip(val, matrices.GetViewportMatrix(), rect);
	if (clipmode == TileClipping::Outside)
		IntersectClipRect(rect, baseScissor.offset.x, baseScissor.offset.y,
				(int)baseScissor.extent.width, (int)baseScissor.extent.height);
	clipRect.offset.x = rect[0];
	clipRect.offset.y = rect[1];
	clipRect.extent.width = rect[2];
	clipRect.extent.height = rect[3];

	return clipmode;
}

void BaseDrawer::SetBaseScissor()
{
	bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer
			&& !matrices.IsClipped();
	if (!wide_screen_on)
	{
		if (pvrrc.isRenderFramebuffer)
		{
			baseScissor = vk::Rect2D(vk::Offset2D(0, 0),
					vk::Extent2D(640, 480));
		}
		else
		{
			float width;
			float height;
			float min_x;
			float min_y;
			min_x = xform_x(&matrices.GetScissorMatrix(), pvrrc.fb_X_CLIP.min);
				min_y = xform_y(&matrices.GetScissorMatrix(), pvrrc.fb_Y_CLIP.min);
				width = xform_w(&matrices.GetScissorMatrix(), pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
				height = xform_h(&matrices.GetScissorMatrix(), pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
			if (width < 0)
			{
				min_x += width;
				width = -width;
			}
			if (height < 0)
			{
				min_y += height;
				height = -height;
			}

			baseScissor = vk::Rect2D(
					vk::Offset2D((u32) std::max(lroundf(min_x), 0L),
							(u32) std::max(lroundf(min_y), 0L)),
					vk::Extent2D((u32) std::max(lroundf(width), 0L),
							(u32) std::max(lroundf(height), 0L)));
		}
	}
	else
	{
		baseScissor = vk::Rect2D(vk::Offset2D(0, 0),
				vk::Extent2D(screen_width, screen_height));
	}
	currentScissor = { 0, 0, 0, 0 };
}

// Vulkan uses the color values of the first vertex for flat shaded triangle strips.
// On Dreamcast the last vertex is the provoking one so we must copy it onto the first.
void BaseDrawer::SetProvokingVertices()
{
	auto setProvokingVertex = [](const List<PolyParam>& list) {
		for (int i = 0; i < list.used(); i++)
		{
			const PolyParam& pp = list.head()[i];
			if (!pp.pcw.Gouraud && pp.count > 2)
			{
				for (int i = 0; i < pp.count - 2; i++)
				{
					Vertex *vertex = &pvrrc.verts.head()[pvrrc.idx.head()[pp.first + i]];
					Vertex *lastVertex = &pvrrc.verts.head()[pvrrc.idx.head()[pp.first + i + 2]];
					memcpy(vertex->col, lastVertex->col, 4);
					memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
					memcpy(vertex->col1, lastVertex->col1, 4);
					memcpy(vertex->spc1, lastVertex->spc1, 4);
				}
			}
		}
	};
	setProvokingVertex(pvrrc.global_param_op);
	setProvokingVertex(pvrrc.global_param_pt);
	setProvokingVertex(pvrrc.global_param_tr);
}

void Drawer::DrawPoly(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
		PipelineManager::ShadowPass shadowPass, const vk::Rect2D *within)
{
	vk::Rect2D scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	if (within != nullptr)
	{
		// only the part of it inside this rectangle, and nothing if there is none
		const vk::Rect2D& clip = tileClip == TileClipping::Outside ? scissorRect : baseScissor;
		const int x0 = std::max(within->offset.x, clip.offset.x);
		const int y0 = std::max(within->offset.y, clip.offset.y);
		const int x1 = std::min(within->offset.x + (int)within->extent.width, clip.offset.x + (int)clip.extent.width);
		const int y1 = std::min(within->offset.y + (int)within->extent.height, clip.offset.y + (int)clip.extent.height);
		if (x1 <= x0 || y1 <= y0)
			return;
		SetScissor(cmdBuffer, vk::Rect2D(vk::Offset2D(x0, y0), vk::Extent2D(x1 - x0, y1 - y0)));
	}
	else if (tileClip == TileClipping::Outside)
		SetScissor(cmdBuffer, scissorRect);
	else
		SetScissor(cmdBuffer, baseScissor);

	float trilinearAlpha = 1.f;
	if (poly.tsp.FilterMode > 1 && poly.pcw.Texture && listType != ListType_Punch_Through && poly.tcw.MipMapped == 1)
	{
		trilinearAlpha = 0.25 * (poly.tsp.MipMapD & 0x3);
		if (poly.tsp.FilterMode == 2)
			// Trilinear pass A
			trilinearAlpha = 1.0 - trilinearAlpha;
	}
	bool palette = BaseTextureCacheData::IsGpuHandledPaletted(poly.tsp, poly.tcw);
	float palette_index = 0.f;
	if (palette)
	{
		if (poly.tcw.PixelFmt == PixelPal4)
			palette_index = (float(poly.tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			palette_index = (float((poly.tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}

	if (tileClip == TileClipping::Inside || trilinearAlpha != 1.f || palette)
	{
		std::array<float, 6> pushConstants = {
			(float)scissorRect.offset.x,
			(float)scissorRect.offset.y,
			(float)scissorRect.offset.x + (float)scissorRect.extent.width,
			(float)scissorRect.offset.y + (float)scissorRect.extent.height,
			trilinearAlpha,
			palette_index
		};
		cmdBuffer.pushConstants<float>(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pushConstants);
	}

	if (poly.pcw.Texture)
		GetCurrentDescSet().SetTexture(poly.texid, poly.tsp);

	vk::Pipeline pipeline = pipelineManager->GetPipeline(listType, sortTriangles, poly, shadowPass);
	cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	if (poly.pcw.Texture)
		GetCurrentDescSet().BindPerPolyDescriptorSets(cmdBuffer, poly.texid, poly.tsp);

	DrawIndices(cmdBuffer, listType, first, count);
}

// The polygons of a list that take shadows, drawn again where they are in a
// modifier volume: see rend/shadows.h.
void Drawer::DrawShadowed(const vk::CommandBuffer& cmdBuffer, u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
		const ScreenBounds& area, const vk::Rect2D& scissor)
{
	/* (of a frame decoded on the GPU, the GPU has said which groups reach
	 * into the volumes: the others' second commands draw nothing) */
	gpuDraws = GpuDraws::Shadows;
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (CanDrawShadowed(listType, pp) && (pvrrc.gpu || PolyOverlaps(pp, area)))
			DrawPoly(cmdBuffer, listType, false, *pp, pp->first, pp->count,
					ShadowLaterWins(listType, pp) ? PipelineManager::ShadowPass::LaterWins : PipelineManager::ShadowPass::EarlierWins,
					&scissor);
	gpuDraws = GpuDraws::Groups;
}

void Drawer::DrawSorted(const vk::CommandBuffer& cmdBuffer, const SortTrigDrawParam *polys, u32 count, bool multipass)
{
	/* (a frame decoded on the GPU: its sorted indices are this frame's in
	 * the decoder's room for them, and name the GPU's vertices) */
	const u32 base = pvrrc.gpu ? 0 : pvrrc.idx.used();
	if (pvrrc.gpu)
	{
		if (count == 0)
			return;
		cmdBuffer.bindIndexBuffer(gpuDraw->sorted, gpuDraw->sortedOffset, vk::IndexType::eUint32);
		gpuDraws = GpuDraws::Sorted;
	}
	for (u32 i = 0; i < count; i++)
		DrawPoly(cmdBuffer, ListType_Translucent, true, *polys[i].ppid, base + polys[i].first, polys[i].count);

	if (multipass && settings.rend.TranslucentPolygonDepthMask)
	{
		// Sorted translucent polygons are drawn without writing depth. Where
		// a later render pass needs their depth (Cosmic Smash), it is written
		// now, for those that do not have Z write disabled.
		for (u32 i = 0; i < count; i++)
		{
			const SortTrigDrawParam& param = polys[i];
			if (param.count <= 2 || param.ppid->isp.ZWriteDis)
				continue;
			vk::Rect2D scissorRect;
			SetScissor(cmdBuffer, SetTileClip(param.ppid->tileclip, scissorRect) == TileClipping::Outside ? scissorRect : baseScissor);
			cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
					pipelineManager->GetModifierVolumePipeline(ModVolMode::Depth, param.ppid->isp.CullMode));
			cmdBuffer.drawIndexed(param.count, 1, base + param.first, 0, 0);
		}
	}
	if (pvrrc.gpu)
	{
		cmdBuffer.bindIndexBuffer(gpuDraw->indices, 0, vk::IndexType::eUint32);
		gpuDraws = GpuDraws::Groups;
	}
}

void Drawer::DrawList(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (pp->count > 2)
			DrawPoly(cmdBuffer, listType, sortTriangles, *pp, pp->first, pp->count);
}

void Drawer::DrawModVols(const vk::CommandBuffer& cmdBuffer, int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass,
		int render_pass)
{
	if (count == 0 || ModVolTriangles() == 0)
		return;

	vk::Buffer buffer = GetMainBuffer(0)->buffer.get();
	BindModVolTriangles(cmdBuffer, buffer, offsets.modVolOffset);
	SetScissor(cmdBuffer, baseScissor);

	ModifierVolumeParam* params = &pvrrc.global_param_mvo.head()[first];

	int mod_base = -1;
	vk::Pipeline pipeline;

	for (int cmv = 0; cmv < count; cmv++)
	{
		ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		{
			vk::Rect2D clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				SetScissor(cmdBuffer, clipRect);
			else
				SetScissor(cmdBuffer, baseScissor);
		}

		if (mod_base == -1)
			mod_base = param.first;

		if (!param.isp.VolumeLast && mv_mode > 0)
			pipeline = pipelineManager->GetModifierVolumePipeline(ModVolMode::Or, param.isp.CullMode);	// OR'ing (open volume or quad)
		else
			pipeline = pipelineManager->GetModifierVolumePipeline(ModVolMode::Xor, param.isp.CullMode);	// XOR'ing (closed volume)
		cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		cmdBuffer.draw(param.count * 3, 1, param.first * 3, 0);

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			pipeline = pipelineManager->GetModifierVolumePipeline(mv_mode == 1 ? ModVolMode::Inclusion : ModVolMode::Exclusion, param.isp.CullMode);
			cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
			cmdBuffer.draw((param.first + param.count - mod_base) * 3, 1, mod_base * 3, 0);
			mod_base = -1;
		}
	}
	SetScissor(cmdBuffer, baseScissor);
	BindVertices(cmdBuffer, buffer);

	/* The polygons that take shadows, again, where they are in a volume.
	 * Of a frame decoded on the GPU, the volumes' bounds are the header
	 * pass's, from their blocks (TA_GPU_SHADOWS) */
	const f32 *ga = pvrrc.gpu_shadow_area[render_pass < 10 ? render_pass : 0];
	const ScreenBounds area = pvrrc.gpu ? ScreenBounds{ ga[0], ga[1], ga[2], ga[3] } : ModVolBounds(first, count);
	if (!area.empty())
	{
		int rect[4];
		BoundsToScissor(area, matrices.GetViewportMatrix(), rect);
		const vk::Rect2D scissor(vk::Offset2D(rect[0], rect[1]), vk::Extent2D(rect[2], rect[3]));
		DrawShadowed(cmdBuffer, ListType_Opaque, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count, area, scissor);
		DrawShadowed(cmdBuffer, ListType_Punch_Through, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count, area, scissor);
		SetScissor(cmdBuffer, baseScissor);
	}

	// What is left: darkened as it is
	// (all six: the polygon shaders' block is six floats, and what is not pushed is not anything in particular)
	const std::array<float, 6> pushConstants = { 1 - FPU_SHAD_SCALE.scale_factor / 256.f, 0, 0, 0, 0, 0 };
	cmdBuffer.pushConstants<float>(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pushConstants);

	pipeline = pipelineManager->GetModifierVolumePipeline(ModVolMode::Final, 0);
	cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
	cmdBuffer.drawIndexed(4, 1, 0, 0, 0);
}

void Drawer::UploadMainBuffer(const VertexShaderUniforms& vertexUniforms, const FragmentShaderUniforms& fragmentUniforms)
{
	// TODO Put this logic in an allocator
	std::vector<const void *> chunks;
	std::vector<u32> chunkSizes;

	// (a frame decoded on the GPU has its geometry there)
	const u32 vertexBytes = pvrrc.gpu ? 0 : pvrrc.verts.bytes();
	const u32 modVolBytes = pvrrc.gpu ? 0 : pvrrc.modtrig.bytes();
	const u32 indexBytes = pvrrc.gpu ? 0 : pvrrc.idx.bytes();

	// Vertex
	chunks.push_back(pvrrc.verts.head());
	chunkSizes.push_back(vertexBytes);

	u32 padding = align(vertexBytes, 4);
	offsets.modVolOffset = vertexBytes + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	// Modifier Volumes
	chunks.push_back(pvrrc.modtrig.head());
	chunkSizes.push_back(modVolBytes);
	padding = align(offsets.modVolOffset + modVolBytes, 4);
	offsets.indexOffset = offsets.modVolOffset + modVolBytes + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	// Index
	chunks.push_back(pvrrc.idx.head());
	chunkSizes.push_back(indexBytes);
	for (const std::vector<u32>& idx : sortedIndexes)
	{
		if (!idx.empty())
		{
			chunks.push_back(&idx[0]);
			chunkSizes.push_back(idx.size() * sizeof(u32));
		}
	}
	// Uniform buffers
	u32 indexSize = indexBytes + sortedIndexCount * sizeof(u32);
	padding = align(offsets.indexOffset + indexSize, std::max(4, (int)GetContext()->GetUniformBufferAlignment()));
	offsets.vertexUniformOffset = offsets.indexOffset + indexSize + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	chunks.push_back(&vertexUniforms);
	chunkSizes.push_back(sizeof(vertexUniforms));
	padding = align(offsets.vertexUniformOffset + sizeof(VertexShaderUniforms), std::max(4, (int)GetContext()->GetUniformBufferAlignment()));
	offsets.fragmentUniformOffset = offsets.vertexUniformOffset + sizeof(VertexShaderUniforms) + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	chunks.push_back(&fragmentUniforms);
	chunkSizes.push_back(sizeof(fragmentUniforms));
	u32 totalSize = offsets.fragmentUniformOffset + sizeof(FragmentShaderUniforms);

	BufferData *buffer = GetMainBuffer(totalSize);
	buffer->upload(chunks.size(), &chunkSizes[0], &chunks[0]);
}

bool Drawer::Draw(const Texture *fogTexture, const Texture *paletteTexture)
{
	/* (a frame decoded on the GPU has its sorted passes sorted already, and
	 * their indices go straight to the decoder's room for them: see
	 * TA_GPU_SORT) */
	if (!pvrrc.gpu)
		SortTriangles();
	else
	{
		for (std::vector<u32>& idx : sortedIndexes)
			idx.clear();
		sortedIndexCount = 0;
		if (ta_gpu_sorted_indices() != 0)
			ta_gpu_sorted_write(gpuDraw->sortedMap);
	}
	currentScissor = vk::Rect2D();

	vk::CommandBuffer cmdBuffer = BeginRenderPass();

	/* After BeginRenderPass(), which works the matrices out for this
	 * render. Taken before it, they were the last render's: the same from
	 * one frame to the next for the screen, so it did not show - but of two
	 * renders to textures of different sizes in one frame, each was drawn
	 * with the other's, at the wrong scale. */
	VertexShaderUniforms vtxUniforms;
	xform_to_mat4(&matrices.GetNormalMatrix(), vtxUniforms.normal_matrix);

	FragmentShaderUniforms fragUniforms = MakeFragmentUniforms<FragmentShaderUniforms>();
	fragUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	// (the GPU's decode has done it to a frame it made)
	if (!pvrrc.gpu)
		SetProvokingVertices();

	// Upload vertex and index buffers
	UploadMainBuffer(vtxUniforms, fragUniforms);

	// Update per-frame descriptor set and bind it
	GetCurrentDescSet().UpdateUniforms(GetMainBuffer(0)->buffer.get(), offsets.vertexUniformOffset, offsets.fragmentUniformOffset,
			fogTexture->GetImageView(), paletteTexture->GetImageView());
	GetCurrentDescSet().BindPerFrameDescriptorSets(cmdBuffer);
	// Reset per-poly descriptor set pool
	GetCurrentDescSet().Reset();

	// Bind vertex and index buffers
	BindGeometry(cmdBuffer, GetMainBuffer(0)->buffer.get(), offsets.indexOffset);

	// Make sure to push constants even if not used
	const std::array<float, 6> pushConstants = { 0, 0, 0, 0, 0, 0 };
	cmdBuffer.pushConstants<float>(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pushConstants);

	RenderPass previous_pass = {};
	for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d autosort %d", render_pass + 1,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.mvo_count - previous_pass.mvo_count, current_pass.autosort);
		DrawList(cmdBuffer, ListType_Opaque, false, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(cmdBuffer, ListType_Punch_Through, false, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);
		DrawModVols(cmdBuffer, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, previous_pass, current_pass,
				render_pass);
		if (current_pass.autosort)
		{
			// (a frame decoded on the GPU with a sorted pass was sorted by triangle: TA_GPU_SORT)
			if (pvrrc.gpu)
			{
				u32 n;
				const SortTrigDrawParam *sorted = ta_gpu_sorted(render_pass, &n);
				DrawSorted(cmdBuffer, sorted, n, render_pass < pvrrc.render_passes.used() - 1);
			}
			else if (!settings.pvr.Emulation.AlphaSortMode)
			{
				DrawSorted(cmdBuffer, sortedPolys[render_pass].data(), (u32)sortedPolys[render_pass].size(),
						render_pass < pvrrc.render_passes.used() - 1);
			}
			else
			{
				SortPParams(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				DrawList(cmdBuffer, ListType_Translucent, true, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			}
		}
		else
			DrawList(cmdBuffer, ListType_Translucent, false, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		previous_pass = current_pass;
	}

	return !pvrrc.isRTT;
}

void TextureDrawer::Init(SamplerManager *samplerManager, ShaderManager *shaderManager, TextureCache *textureCache)
{
	if (!rttPipelineManager)
		rttPipelineManager = std::unique_ptr<RttPipelineManager>(new RttPipelineManager());
	rttPipelineManager->Init(shaderManager);
	Drawer::Init(samplerManager, rttPipelineManager.get());

	this->textureCache = textureCache;
}

vk::CommandBuffer TextureDrawer::BeginRenderPass()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d @ %08x", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max, FB_W_SOF1 & VRAM_MASK);
	matrices.CalcMatrices(&pvrrc);

	textureAddr = FB_W_SOF1 & VRAM_MASK;
	// the frame from its first pixel, as far as the pixel clip goes (see TransformMatrix)
	u32 origWidth = pvrrc.fb_X_CLIP.max + 1;
	u32 origHeight = pvrrc.fb_Y_CLIP.max + 1;
	const u32 clipX = pvrrc.fb_X_CLIP.min, clipY = pvrrc.fb_Y_CLIP.min;
	const u32 clipWidth = origWidth - clipX, clipHeight = origHeight - clipY;
	u32 upscaledWidth = origWidth;
	u32 upscaledHeight = origHeight;
	u32 heightPow2 = 8;
	while (heightPow2 < upscaledHeight)
		heightPow2 *= 2;
	u32 widthPow2 = 8;
	while (widthPow2 < upscaledWidth)
		widthPow2 *= 2;

	if (settings.rend.RenderToTextureUpscale > 1)
	{
		upscaledWidth *= settings.rend.RenderToTextureUpscale;
		upscaledHeight *= settings.rend.RenderToTextureUpscale;
		widthPow2 *= settings.rend.RenderToTextureUpscale;
		heightPow2 *= settings.rend.RenderToTextureUpscale;
	}

	VulkanContext *context = GetContext();
	vk::Device device = context->GetDevice();

	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	if (!depthAttachment || widthPow2 > depthAttachment->getExtent().width || heightPow2 > depthAttachment->getExtent().height)
	{
		if (depthAttachment)
			commandPool->DeferDelete(std::move(depthAttachment));
		depthAttachment = std::unique_ptr<FramebufferAttachment>(new FramebufferAttachment(context->GetPhysicalDevice(), device));
		depthAttachment->Init(widthPow2, heightPow2, GetContext()->GetDepthFormat(),
				vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment);
	}
	vk::Image colorImage;
	vk::ImageView colorImageView;
	vk::ImageLayout colorImageCurrentLayout;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(textureAddr, clipX, clipY, clipWidth, clipHeight);
	// TexAddr : fb_rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1
	TCW tcw = { { textureAddr >> 3, 0, 0, 1 } };
	switch (FB_W_CTRL.fb_packmode) {
	case 0:
	case 3:
		tcw.PixelFmt = Pixel1555;
		break;
	case 1:
		tcw.PixelFmt = Pixel565;
		break;
	case 2:
		tcw.PixelFmt = Pixel4444;
		break;
	}

	TSP tsp = { 0 };
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < origWidth; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < origHeight; tsp.TexV++);

	texture = textureCache->getTextureCacheData(tsp, tcw);
	if (texture->IsNew())
	{
		texture->Create();
		texture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
		texture->SetDevice(device);
	}
	else if (textureCache->IsInFlight(texture))
	{
		texture->readOnlyImageView = *texture->imageView;
		textureCache->DestroyLater(texture);
	}
	textureCache->SetInFlight(texture);

	/* A texture the game uploaded can become one it renders to, at the
	 * same address, the same size and in this format: its image was not
	 * made to be rendered to, nor to be copied from, and is made again. */
	const vk::ImageUsageFlags renderTargetUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
			| vk::ImageUsageFlagBits::eTransferSrc;	// read back when the game touches it in video memory
	if (texture->format != vk::Format::eR8G8B8A8Unorm || texture->extent.width != widthPow2 || texture->extent.height != heightPow2
			|| (texture->imageUsage & renderTargetUsage) != renderTargetUsage)
	{
		texture->extent = vk::Extent2D(widthPow2, heightPow2);
		texture->format = vk::Format::eR8G8B8A8Unorm;
		texture->needsStaging = true;
		texture->CreateImage(vk::ImageTiling::eOptimal, renderTargetUsage, vk::ImageLayout::eUndefined, vk::ImageAspectFlagBits::eColor);
		colorImageCurrentLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		colorImageCurrentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	colorImage = *texture->image;
	colorImageView = texture->GetImageView();
	width = widthPow2;
	height = heightPow2;

	setImageLayout(commandBuffer, colorImage, vk::Format::eR8G8B8A8Unorm, 1, colorImageCurrentLayout, vk::ImageLayout::eColorAttachmentOptimal);

	vk::ImageView imageViews[] = {
		colorImageView,
		depthAttachment->GetImageView(),
	};
	framebuffers.resize(GetContext()->GetSwapChainSize());
	framebuffers[GetCurrentImage()] = device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
			rttPipelineManager->GetRenderPass(), ARRAY_SIZE(imageViews), imageViews, widthPow2, heightPow2, 1));

	const vk::ClearValue clear_colors[] = { vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }), vk::ClearDepthStencilValue { 0.f, 0 } };
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(rttPipelineManager->GetRenderPass(),	*framebuffers[GetCurrentImage()],
			vk::Rect2D( { 0, 0 }, { width, height }), 2, clear_colors), vk::SubpassContents::eInline);
	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, (float)upscaledWidth, (float)upscaledHeight, 1.0f, 0.0f));
	{
		// only the pixel clip is written
		const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		baseScissor = vk::Rect2D(vk::Offset2D(clipX * scale, clipY * scale), vk::Extent2D(clipWidth * scale, clipHeight * scale));
	}
	commandBuffer.setScissor(0, baseScissor);
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}

void TextureDrawer::EndRenderPass()
{
	currentCommandBuffer.endRenderPass();

	currentCommandBuffer.end();

	currentCommandBuffer = nullptr;
	commandPool->EndFrame();

	texture->dirty = 0;
	texture->stride_read = 0;	// (rendered, not read)
   libCore_vramlock_Lock(texture->sa_tex, texture->sa + texture->size - 1, texture);
	/* video memory gets it when something first touches it there */
	vk_rtt_watch(texture, textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}

void ScreenDrawer::Init(SamplerManager *samplerManager, ShaderManager *shaderManager)
{
	this->shaderManager = shaderManager;
	if (viewport != GetContext()->GetViewPort())
	{
		/* Not destroyed on the spot: the last picture was handed to the
		 * frontend, which shows it again for as long as no new one comes -
		 * and may, before the first frame at the new size is drawn. They
		 * go when the frames that could still use them have gone by.
		 * (Upstream: a crash starting the 240p test suite.) */
		if (commandPool != nullptr)
		{
			/* The image with the last frame does not go with the rest. A
			 * game that draws part of the screen has the rest from the
			 * last picture, and this is it: the next render starts from it
			 * as it does from one kept across a new context, at whatever
			 * size the screen now is - and here nothing is read back or
			 * loaded again. It goes after the next frame. */
			if (carriedPicture)
				commandPool->DeferDelete(std::move(carriedPicture));
			if (havePicture && (size_t)GetCurrentImage() < colorAttachments.size() && colorAttachments[GetCurrentImage()])
			{
				carriedPicture = std::move(colorAttachments[GetCurrentImage()]);
				restoredPicture = carriedPicture->GetImageView();
			}
			for (auto& framebuffer : framebuffers)
				if (framebuffer)
					commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
			for (auto& attachment : colorAttachments)
				if (attachment)
					commandPool->DeferDelete(std::move(attachment));
			if (depthAttachment)
				commandPool->DeferDelete(std::move(depthAttachment));
		}
		framebuffers.clear();
		colorAttachments.clear();
		depthAttachment.reset();
		havePicture = false;
	}
	viewport = GetContext()->GetViewPort();
	if (!depthAttachment)
	{
		depthAttachment = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
		depthAttachment->Init(viewport.width, viewport.height, GetContext()->GetDepthFormat(),
				vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eTransientAttachment);
	}

	if (!renderPass)
	{
		vk::AttachmentDescription attachmentDescriptions[] = {
				// Color attachment
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetColorFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
						vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
				// Depth attachment
				vk::AttachmentDescription(vk::AttachmentDescriptionFlags(), GetContext()->GetDepthFormat(), vk::SampleCountFlagBits::e1,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
						vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
						vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal),
		};
		vk::AttachmentReference colorReference(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::AttachmentReference depthReference(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);

		vk::SubpassDescription subpasses[] = {
				vk::SubpassDescription(vk::SubpassDescriptionFlags(), vk::PipelineBindPoint::eGraphics,
						0, nullptr,
						1, &colorReference,
						nullptr,
						&depthReference),
		};

		std::vector<vk::SubpassDependency> dependencies;
		dependencies.emplace_back(0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
				vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead, vk::DependencyFlagBits::eByRegion);
		/* The depth attachment is the same one every frame: the frame before
		 * has to be done with it before this one clears it. Nothing said
		 * so. (The per-pixel renderer's render pass has the same, from
		 * upstream.) */
		// (the colour attachment is in it as well: once one dependency into the
		// subpass is spelled out, the one Vulkan would have assumed for the rest is gone)
		dependencies.emplace_back(VK_SUBPASS_EXTERNAL, 0,
				vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
					| vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
					| vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eColorAttachmentWrite,
				vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
					| vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);

		renderPass = GetContext()->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo(vk::RenderPassCreateFlags(),
				ARRAY_SIZE(attachmentDescriptions), attachmentDescriptions,
				ARRAY_SIZE(subpasses), subpasses,
				dependencies.size(), dependencies.data()));
	}
	size_t size = VulkanContext::Instance()->GetSwapChainSize();
	if (colorAttachments.size() > size)
	{
		colorAttachments.resize(size);
		framebuffers.resize(size);
	}
	else
	{
		vk::ImageView attachments[] = {
				nullptr,
				depthAttachment->GetImageView(),
		};
		while (colorAttachments.size() < size)
		{
			colorAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
					new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
			// (copied from when the context goes, to keep the picture: rend/last_picture.h)
			colorAttachments.back()->Init(viewport.width, viewport.height, GetContext()->GetColorFormat(),
					vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc, false);
			attachments[0] = colorAttachments.back()->GetImageView();
			vk::FramebufferCreateInfo createInfo(vk::FramebufferCreateFlags(), *renderPass,
					ARRAY_SIZE(attachments), attachments, viewport.width, viewport.height, 1);
			framebuffers.push_back(GetContext()->GetDevice().createFramebufferUnique(createInfo));
		}
	}

	if (!screenPipelineManager)
		screenPipelineManager = std::unique_ptr<PipelineManager>(new PipelineManager());
	screenPipelineManager->Init(shaderManager, *renderPass);
	Drawer::Init(samplerManager, screenPipelineManager.get());
}

vk::CommandBuffer ScreenDrawer::BeginRenderPass()
{
	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to; any other
	 * starts black, and has the last picture put into it below. */
	const std::array<float, 4> clear_color = pvrrc.clearFramebuffer
			? std::array<float, 4> { VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f }
			: std::array<float, 4> { 0.f, 0.f, 0.f, 1.f };
	const vk::ClearValue clear_colors[] = { vk::ClearColorValue(clear_color), vk::ClearDepthStencilValue { 0.f, 0 } };
	commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(*renderPass, *framebuffers[GetCurrentImage()],
			vk::Rect2D( { 0, 0 }, viewport), 2, clear_colors), vk::SubpassContents::eInline);
	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, viewport.width, viewport.height, 1.0f, 0.0f));

	matrices.CalcMatrices(&pvrrc);

	SetBaseScissor();
	/* A render that covers only part of the screen leaves the rest of the
	 * framebuffer as it was. Every frame here goes to a fresh image,
	 * cleared, so the part that is not drawn was black: the last frame is
	 * drawn into it first, whole, and the render goes over it. Only for
	 * such a render - one that covers the screen pays nothing - and not
	 * into a framebuffer the game has not drawn to lately, which starts
	 * empty. */
	if ((havePicture || restoredPicture) && quadPipeline != nullptr && !pvrrc.clearFramebuffer && matrices.IsClipped())
	{
		const int count = (int)colorAttachments.size();
		const std::array<float, 4> opaque = { 1.f, 1.f, 1.f, 1.f };

		commandBuffer.setScissor(0, vk::Rect2D( { 0, 0 }, viewport));
		commandBuffer.setBlendConstants(opaque.data());
		quadPipeline->BindPipeline(commandBuffer);
		/* (the picture kept from another context can be of another size) */
		if (havePicture)
			lastPicture.Draw(commandBuffer, colorAttachments[(GetCurrentImage() + count - 1) % count]->GetImageView(), nullptr, true, commandPool);
		else
			lastPicture.Draw(commandBuffer, restoredPicture, nullptr, false, commandPool);
	}
	commandBuffer.setScissor(0, baseScissor);
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}

void ScreenDrawer::KeepPicture()
{
	if (!havePicture)
		return;
	u8 *pixels = last_picture_keep(viewport.width, viewport.height);
	if (pixels != NULL)
		vk_read_picture(colorAttachments[GetCurrentImage()]->GetImage(), GetContext()->GetColorFormat(),
				viewport.width, viewport.height, pixels);
}

void ScreenDrawer::EndRenderPass()
{
	havePicture = true;
	restoredPicture = nullptr;
	// (this frame may have drawn it: it goes when the frame is done with)
	if (carriedPicture)
		commandPool->DeferDelete(std::move(carriedPicture));
	currentCommandBuffer.endRenderPass();
	currentCommandBuffer.end();
	currentCommandBuffer = nullptr;
	commandPool->EndFrame();
	GetContext()->PresentFrame(colorAttachments[GetCurrentImage()]->GetImage(), colorAttachments[GetCurrentImage()]->GetImageView(),
			vk::Offset2D(viewport.width, viewport.height));
}
