#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
    Created on: Nov 7, 2019

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
#include "oit_drawer.h"
#include "../rtt_read.h"
#include "rend/last_picture.h"
#include "hw/pvr/pvr_mem.h"

#include <algorithm>

/* The translucent modifier volumes' draws change the per-pixel lists from
 * their shaders (oit_trmodvol.frag), and nothing orders what the shaders of
 * two draws do to the same fragment but a barrier between them: a GPU runs
 * the draws of a subpass at once, in any order, where a software device
 * runs them one after the other. They go through here. Built with
 * RACE_TEST (tools/rend/speckle_test.sh; never in a release) the draws
 * between two barriers are recorded a triangle at a time, the draws'
 * triangles taken in turn, the last draw's first: one draw's shaders run
 * while another's are half done, as on a GPU, and a picture that changes
 * then depends on an order nothing keeps. */
namespace {
struct ListDraws
{
	OITDrawer& drawer;
	const vk::CommandBuffer& cmd;
#ifdef RACE_TEST
	struct Held { vk::Pipeline pipeline; vk::Rect2D scissor; u32 vertices, first; };
	Held held[256];
	u32 count = 0;
#endif

	ListDraws(OITDrawer& drawer, const vk::CommandBuffer& cmd) : drawer(drawer), cmd(cmd) {}
	~ListDraws() { Flush(); }

	void Draw(vk::Pipeline pipeline, const vk::Rect2D& scissor, u32 vertices, u32 first)
	{
#ifdef RACE_TEST
		if (count == ARRAY_SIZE(held))
			Flush();
		held[count++] = { pipeline, scissor, vertices, first };
#else
		Record(pipeline, scissor, vertices, first);
#endif
	}
	/* what the draws before did to the lists is done before the draws after start */
	void Barrier()
	{
		Flush();
		vk::MemoryBarrier barrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eFragmentShader,
				vk::DependencyFlagBits::eByRegion, barrier, nullptr, nullptr);
	}
	void Flush()
	{
#ifdef RACE_TEST
		for (u32 v = 0, more = count; more != 0; v += 3)
		{
			more = 0;
			for (u32 i = count; i-- > 0; )
				if (v < held[i].vertices)
				{
					Record(held[i].pipeline, held[i].scissor, 3, held[i].first + v);
					more++;
				}
		}
		count = 0;
#endif
	}

private:
	void Record(vk::Pipeline pipeline, const vk::Rect2D& scissor, u32 vertices, u32 first)
	{
		drawer.SetScissor(cmd, scissor);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		cmd.draw(vertices, 1, first, 0);
	}
};
}

void OITDrawer::DrawPoly(const vk::CommandBuffer& cmdBuffer, u32 listType, bool autosort, Pass pass,
		const PolyParam& poly, u32 first, u32 count)
{
	vk::Rect2D scissorRect;
	TileClipping tileClip = SetTileClip(poly.tileclip, scissorRect);
	if (tileClip == TileClipping::Outside)
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

	bool twoVolumes = poly.tsp1.full != (u32)-1 || poly.tcw1.full != (u32)-1;

	const bool keepAll = twoVolumes || poly.tsp.SrcSelect;

	bool palette = BaseTextureCacheData::IsGpuHandledPaletted(poly.tsp, poly.tcw);
	float palette_index = 0.f;
	if (palette)
	{
		if (poly.tcw.PixelFmt == PixelPal4)
			palette_index = (float(poly.tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			palette_index = (float((poly.tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}

	OITDescriptorSets::PushConstants pushConstants = {
			{
				(float)scissorRect.offset.x,
				(float)scissorRect.offset.y,
				(float)scissorRect.offset.x + (float)scissorRect.extent.width,
				(float)scissorRect.offset.y + (float)scissorRect.extent.height
			},
			// The per-pixel pass drops a fragment whose blend would leave the
			// pixel as it is. It cannot tell that when the source is the second
			// accumulation buffer, or when the polygon has a second volume and
			// which blend applies is known only when the lists are put
			// together: those get a blend (8) that none of its cases take.
			{ keepAll ? 8 : (int)poly.tsp.SrcInstr, keepAll ? 8 : (int)poly.tsp.DstInstr, 0, 0 },
			trilinearAlpha,
			// (a frame decoded on the GPU numbers its strips by their draws: see TA_GPU_INSTANCE)
			listType == ListType_Translucent && !pvrrc.gpu ? (int)(&poly - pvrrc.global_param_tr.head()) : 0,
			palette_index,
	};
	if (twoVolumes)
	{
		pushConstants.blend_mode1[0] = (int)poly.tsp1.SrcInstr;
		pushConstants.blend_mode1[1] = (int)poly.tsp1.DstInstr;
		pushConstants.blend_mode1[2] = 0;
		pushConstants.blend_mode1[3] = 0;
		pushConstants.shading_instr0 = poly.tsp.ShadInstr;
		pushConstants.shading_instr1 = poly.tsp1.ShadInstr;
		pushConstants.fog_control0 = poly.tsp.FogCtrl;
		pushConstants.fog_control1 = poly.tsp1.FogCtrl;
		pushConstants.use_alpha0 = poly.tsp.UseAlpha;
		pushConstants.use_alpha1 = poly.tsp1.UseAlpha;
		pushConstants.ignore_tex_alpha0 = poly.tsp.IgnoreTexA;
		pushConstants.ignore_tex_alpha1 = poly.tsp1.IgnoreTexA;
	}
	const OITPsoChoice pso = pipelineManager->GetPipeline(listType, autosort, poly, pass);
	if (pso.pipeline == VK_NULL_HANDLE)
		return;
	// (the general pipeline is told the polygon's key too: pso.h)
	pushConstants.key = (int)(pso.key & ((1u << OIT_SC_BITS) - 1));
	cmdBuffer.pushConstants(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0,
			pso.general ? (u32)sizeof(pushConstants) : (u32)OIT_PUSH_KEY_OFFSET, &pushConstants);

	bool needTexture = poly.pcw.Texture;
	if (needTexture)
		GetCurrentDescSet().SetTexture(poly.texid, poly.tsp, poly.texid1, poly.tsp1);

	cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, vk::Pipeline(pso.pipeline));
	if (pso.general)
		pipelineManager->SetDynamicState(cmdBuffer, pso.key);
	if (needTexture)
		GetCurrentDescSet().BindPerPolyDescriptorSets(cmdBuffer, poly.texid, poly.tsp, poly.texid1, poly.tsp1);

	DrawIndices(cmdBuffer, listType, first, count);
}

void OITDrawer::DrawList(const vk::CommandBuffer& cmdBuffer, u32 listType, bool sortTriangles, Pass pass,
		const List<PolyParam>& polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys.head() + last;
	for (const PolyParam *pp = polys.head() + first; pp != pp_end; pp++)
		if (pp->count > 2)
			DrawPoly(cmdBuffer, listType, sortTriangles, pass, *pp, pp->first, pp->count);
}

template<bool Translucent>
void OITDrawer::DrawModifierVolumes(const vk::CommandBuffer& cmdBuffer, int first, int count, const ModifierVolumeParam *modVolParams)
{
	if (count == 0 || ModVolTriangles() == 0)
		return;

	vk::Buffer buffer = GetMainBuffer(0)->buffer.get();
	BindModVolTriangles(cmdBuffer, buffer, offsets.modVolOffset);
	SetScissor(cmdBuffer, baseScissor);

	const ModifierVolumeParam* params = &modVolParams[first];

	int mod_base = -1;
	vk::Pipeline pipeline;
	ListDraws listDraws(*this, cmdBuffer);

	for (int cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam& param = params[cmv];
		u32 mvFirst, mvCount;
		mv_clip_range_of(pvrrc, modVolParams, param, mvFirst, mvCount);

		if (mvCount == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		// clipped like a polygon, to the rectangle it was sent under if it asked to be
		vk::Rect2D scissor = baseScissor;
		{
			vk::Rect2D clipRect;
			if (SetTileClip(param.tileclip, clipRect) == TileClipping::Outside)
				scissor = clipRect;
		}

		verify(mvFirst + mvCount <= ModVolTriangles());

		if (mod_base == -1)
			mod_base = mvFirst;

		if (!param.isp.VolumeLast && mv_mode > 0)
		{
			// OR'ing (open volume or quad)
			if (Translucent)
				pipeline = pipelineManager->GetTrModifierVolumePipeline(ModVolMode::Or, param.isp.CullMode);
			else
				pipeline = pipelineManager->GetModifierVolumePipeline(ModVolMode::Or, param.isp.CullMode);
		}
		else
		{
			// XOR'ing (closed volume)
			if (Translucent)
				pipeline = pipelineManager->GetTrModifierVolumePipeline(ModVolMode::Xor, param.isp.CullMode);
			else
				pipeline = pipelineManager->GetModifierVolumePipeline(ModVolMode::Xor, param.isp.CullMode);
		}
		if (Translucent)
			listDraws.Draw(pipeline, scissor, mvCount * 3, mvFirst * 3);
		else
		{
			SetScissor(cmdBuffer, scissor);
			cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
			cmdBuffer.draw(mvCount * 3, 1, mvFirst * 3, 0);
		}

		if (mv_mode == 1 || mv_mode == 2)
		{
			//Sum the area
			if (Translucent)
			{
				listDraws.Barrier();
				pipeline = pipelineManager->GetTrModifierVolumePipeline(mv_mode == 1 ? ModVolMode::Inclusion : ModVolMode::Exclusion, param.isp.CullMode);
				listDraws.Draw(pipeline, scissor, (mvFirst + mvCount - mod_base) * 3, mod_base * 3);
				/* ...and done before the next volume's draws start: this
				 * draw clears the stencil bit the next volume's toggle, and
				 * sets the fragments it finds inside from what it read */
				listDraws.Barrier();
			}
			else
			{
				pipeline = pipelineManager->GetModifierVolumePipeline(mv_mode == 1 ? ModVolMode::Inclusion : ModVolMode::Exclusion, param.isp.CullMode);
				cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
				cmdBuffer.draw((mvFirst + mvCount - mod_base) * 3, 1, mod_base * 3, 0);
			}

			mod_base = -1;
		}
	}
	listDraws.Flush();
	SetScissor(cmdBuffer, baseScissor);
	BindVertices(cmdBuffer, buffer);
}

void OITDrawer::UploadMainBuffer(const OITDescriptorSets::FragmentShaderUniforms& fragmentUniforms)
{
	using FragmentShaderUniforms = OITDescriptorSets::FragmentShaderUniforms;

	// TODO Put this logic in an allocator
	std::vector<const void *> chunks;
	std::vector<u32> chunkSizes;

	// (a frame decoded on the GPU has its geometry and translucent polygon parameters there)
	const u32 vertexBytes = pvrrc.gpu ? 0 : pvrrc.verts.bytes();
	const u32 modVolBytes = ModVolsOnCpu() ? ModVolList().bytes() : 0;
	const u32 indexBytes = pvrrc.gpu ? 0 : pvrrc.idx.bytes();

	// Vertex
	chunks.push_back(pvrrc.verts.head());
	chunkSizes.push_back(vertexBytes);

	u32 padding = align(vertexBytes, 4);
	offsets.modVolOffset = vertexBytes + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	// Modifier Volumes
	chunks.push_back(ModVolList().head());
	chunkSizes.push_back(modVolBytes);
	padding = align(offsets.modVolOffset + modVolBytes, 4);
	offsets.indexOffset = offsets.modVolOffset + modVolBytes + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	// Index
	chunks.push_back(pvrrc.idx.head());
	chunkSizes.push_back(indexBytes);

	// Fragment uniform buffer (the vertex one is its own: WriteVertexUniforms())
	u32 indexSize = indexBytes;
	padding = align(offsets.indexOffset + indexSize, std::max(4, (int)GetContext()->GetUniformBufferAlignment()));
	offsets.fragmentUniformOffset = offsets.indexOffset + indexSize + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	chunks.push_back(&fragmentUniforms);
	chunkSizes.push_back(sizeof(fragmentUniforms));

	// Translucent poly params
	padding = align(offsets.fragmentUniformOffset + sizeof(FragmentShaderUniforms), std::max(4, (int)GetContext()->GetStorageBufferAlignment()));
	offsets.polyParamsOffset = offsets.fragmentUniformOffset + sizeof(FragmentShaderUniforms) + padding;
	chunks.push_back(nullptr);
	chunkSizes.push_back(padding);

	std::vector<u32> trPolyParams(pvrrc.gpu ? 0 : pvrrc.global_param_tr.used() * 2);
	if (pvrrc.gpu || pvrrc.global_param_tr.used() == 0)
		trPolyParams.push_back(0);	// makes the validation layers happy
	else
	{
		const PolyParam *pp_end = pvrrc.global_param_tr.LastPtr(0);
		const PolyParam *pp = pvrrc.global_param_tr.head();
		for (int i = 0; pp != pp_end; i += 2, pp++)
		{
			trPolyParams[i] = (pp->tsp.full & 0xffff00c0) | ((pp->isp.full >> 16) & 0xe400) | ((pp->pcw.full >> 7) & 1);
			trPolyParams[i + 1] = pp->tsp1.full;
		}
	}
	offsets.polyParamsSize = trPolyParams.size() * 4;
	chunks.push_back(trPolyParams.data());
	chunkSizes.push_back(offsets.polyParamsSize);
	u32 totalSize = offsets.polyParamsOffset + offsets.polyParamsSize;

	BufferData *buffer = GetMainBuffer(totalSize);
	buffer->upload(chunks.size(), &chunkSizes[0], &chunks[0]);
}

bool OITDrawer::Draw(const Texture *fogTexture, const Texture *paletteTexture)
{
	vk::CommandBuffer cmdBuffer = NewFrame();

	if (needDepthTransition)
	{
		needDepthTransition = false;
		// Not convinced that this is really needed but it makes validation layers happy
		setImageLayout(cmdBuffer, depthAttachment->GetImage(), GetContext()->GetDepthFormat(), 1, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
	}

	OITDescriptorSets::FragmentShaderUniforms fragUniforms = MakeFragmentUniforms<OITDescriptorSets::FragmentShaderUniforms>();
	fragUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	currentScissor = vk::Rect2D();

	oitBuffers->OnNewFrame(cmdBuffer);

	// (the GPU's decode has done it to a frame it made)
	if (!pvrrc.gpu)
		SetProvokingVertices();

	// Upload vertex and index buffers
	UploadMainBuffer(fragUniforms);

	quadBuffer->Update();

	/* Translucent polygons that are not sorted are drawn in the colour
	 * subpass and write depth there. A frame that has any is drawn with the
	 * render passes that allow that: see RenderPasses::GetRenderPass(). */
	bool depthWrites = false;
	{
		int tr_count = 0;
		for (int i = 0; i < pvrrc.render_passes.used(); i++)
		{
			const RenderPass& pass = pvrrc.render_passes.head()[i];
			if (!pass.autosort && pass.tr_count > tr_count)
				depthWrites = true;
			tr_count = pass.tr_count;
		}
	}

	// Update per-frame descriptor set and bind it
	const vk::Buffer mainBuffer = GetMainBuffer(0)->buffer.get();
	GetCurrentDescSet().UpdateUniforms(vertexUniformBuffer, mainBuffer, offsets.fragmentUniformOffset,
			fogTexture->GetImageView(), offsets.polyParamsOffset,
			offsets.polyParamsSize, depthAttachment->GetStencilView(),
			depthAttachment->GetImageView(), paletteTexture->GetImageView(),
			RenderPasses::DepthInputLayout(depthWrites),
			pvrrc.gpu ? gpuDraw->trParams : vk::Buffer(), pvrrc.gpu ? gpuDraw->trParamsSize : 0);
	GetCurrentDescSet().BindPerFrameDescriptorSets(cmdBuffer);
	/* SSAA: the samples' colour, one image for every render pass, read in
	 * the general layout; without, the two that take turns */
	const bool ssaa = pipelineManager->GetSamples() > 1;
	if (ssaa)
		GetCurrentDescSet().UpdateColorInputDescSet(0, colorAttachments[0]->GetImageView(), vk::ImageLayout::eGeneral);
	else
	{
		GetCurrentDescSet().UpdateColorInputDescSet(0, colorAttachments[0]->GetImageView());
		GetCurrentDescSet().UpdateColorInputDescSet(1, colorAttachments[1]->GetImageView());
	}
	oitBuffers->BindDescriptorSet(cmdBuffer, pipelineManager->GetPipelineLayout(), 3);

	// Reset per-poly descriptor set pool
	GetCurrentDescSet().Reset();

	// Bind vertex and index buffers
	BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);

	// Make sure to push constants even if not used
	OITDescriptorSets::PushConstants pushConstants = { };
	cmdBuffer.pushConstants<OITDescriptorSets::PushConstants>(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pushConstants);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	const vk::ClearColorValue clear_color(pvrrc.clearFramebuffer
			? std::array<float, 4>{ VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f }
			: std::array<float, 4>{ 0.f, 0.f, 0.f, 1.f });
	const std::array<vk::ClearValue, 3> clear_colors = {
			clear_color,
			clear_color,
			vk::ClearDepthStencilValue{ 0.f, 0 },
	};

	/* In bands, when the frames before had more translucent fragments than
	 * there is room for: the bands are drawn one after the other, each with
	 * all of the frame's render passes. The picture is cleared in the
	 * first band only (RenderPasses::GetRenderPass(), @resumed), and the
	 * last band's final render pass is the whole picture's, as the frame's
	 * final render pass always is: what is drawn after it (the screen's
	 * VMUs, the texture's copy) is drawn over the whole of it. */
	ChooseBands();
	const u32 height = std::max(viewport.extent.height, 1u);
	const u32 bandHeight = (height + std::min(bands, height) - 1) / std::min(bands, height);
	const u32 bandCount = (height + bandHeight - 1) / bandHeight;
	oitBuffers->SetBands(bandCount);
	for (u32 bandIndex = 0; bandIndex < bandCount; bandIndex++)
	{
	const bool lastBand = bandIndex == bandCount - 1;
	if (bandCount > 1)
	{
		const u32 top = bandIndex * bandHeight;
		band = vk::Rect2D(vk::Offset2D(viewport.offset.x, viewport.offset.y + (s32)top),
				vk::Extent2D(viewport.extent.width, std::min(bandHeight, height - top)));
		currentScissor = vk::Rect2D();
		if (bandIndex > 0)
			// (the band before ended with the quad's)
			BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);
	}
	RenderPass previous_pass = {};
    for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
    {
        const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

        DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d", render_pass + 1,
        		current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.mvo_count - previous_pass.mvo_count,
				current_pass.mvo_tr_count - previous_pass.mvo_tr_count, current_pass.autosort);

        // Reset the pixel counter
    	oitBuffers->ResetPixelCounter(cmdBuffer);

    	const bool initialPass = render_pass == 0;
    	const bool finalPass = render_pass == pvrrc.render_passes.used() - 1;

    	vk::Framebuffer targetFramebuffer;
    	if (ssaa)
    		// (the samples carry on to the next render pass; the picture is written by the last)
    		targetFramebuffer = GetFinalFramebuffer();
    	else if (!finalPass)
    		targetFramebuffer = *tempFramebuffers[(pvrrc.render_passes.used() - 1 - render_pass) % 2];
    	else
    		targetFramebuffer = GetFinalFramebuffer();
    	cmdBuffer.beginRenderPass(
    			vk::RenderPassBeginInfo(pipelineManager->GetRenderPass(initialPass, finalPass, depthWrites, bandIndex > 0),
    					targetFramebuffer, bandCount > 1 && !(finalPass && lastBand) ? band : viewport, clear_colors.size(), clear_colors.data()),
    			vk::SubpassContents::eInline);

        // Depth + stencil subpass
		DrawList(cmdBuffer, ListType_Opaque, false, Pass::Depth, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(cmdBuffer, ListType_Punch_Through, false, Pass::Depth, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);

		DrawModifierVolumes<false>(cmdBuffer, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());

		// Color subpass
		cmdBuffer.nextSubpass(vk::SubpassContents::eInline);

		/* A render that covers only part of the screen leaves the rest as
		 * it was: the last picture goes in first, under everything, and
		 * what this frame binds is bound again after it. */
		if (render_pass == 0 && DrawLastPicture(cmdBuffer))
		{
			GetCurrentDescSet().BindPerFrameDescriptorSets(cmdBuffer);
			oitBuffers->BindDescriptorSet(cmdBuffer, pipelineManager->GetPipelineLayout(), 3);
			BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);
			cmdBuffer.pushConstants<OITDescriptorSets::PushConstants>(pipelineManager->GetPipelineLayout(), vk::ShaderStageFlagBits::eFragment, 0, pushConstants);
			SetScissor(cmdBuffer, baseScissor);
		}

		// OP + PT
		DrawList(cmdBuffer, ListType_Opaque, false, Pass::Color, pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count);
		DrawList(cmdBuffer, ListType_Punch_Through, false, Pass::Color, pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count);

		// TR
		/* (In the first frame after the per-pixel lists are made, too: they
		 * start empty - OITBuffers::OnNewFrame - and that frame's translucent
		 * polygons are drawn like any other's. They used to be left out of
		 * it, and where a game draws one frame and then waits, they stayed
		 * missing. Upstream: the background of the progress window in
		 * Fighting Force 2.) */
		if (current_pass.autosort)
			DrawList(cmdBuffer, ListType_Translucent, true, Pass::OIT, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
		else
			DrawList(cmdBuffer, ListType_Translucent, false, Pass::Color, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);

		// Final subpass (SSAA: the translucent modifier volumes' one)
		cmdBuffer.nextSubpass(vk::SubpassContents::eInline);
		GetCurrentDescSet().BindColorInputDescSet(cmdBuffer, ssaa ? 0 : (pvrrc.render_passes.used() - 1 - render_pass) % 2);
		SetScissor(cmdBuffer, baseScissor);

		{
			// Tr modifier volumes
			if (GetContext()->GetVendorID() != VENDOR_QUALCOMM)	// Adreno bug
			{
				if (current_pass.mv_op_tr_shared)
					DrawModifierVolumes<true>(cmdBuffer, previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, pvrrc.global_param_mvo.head());
				else
					DrawModifierVolumes<true>(cmdBuffer, previous_pass.mvo_tr_count, current_pass.mvo_tr_count - previous_pass.mvo_tr_count, pvrrc.global_param_mvo_tr.head());
			}

			/* The whole picture is put together here, not the part the game
			 * clips its drawing to: what is outside that has to be written
			 * too, or it is left as it was in whichever of the picture's
			 * images this frame goes to - two or three frames old, and a
			 * different one every frame. (Upstream: the loading screen
			 * before a fight in Soul Calibur blinking.) */
			SetScissor(cmdBuffer, viewport);
			if (!ssaa)
			{
				vk::Pipeline pipeline = pipelineManager->GetFinalPipeline();
				cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
				quadBuffer->Bind(cmdBuffer);
				quadBuffer->Draw(cmdBuffer);
			}
		}
		SetScissor(cmdBuffer, viewport);
		if (ssaa)
		{
			if (!finalPass)
			{
				// each sample put together, into the samples the next render pass draws over...
				cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelineManager->GetFinalPipeline(true));
				quadBuffer->Bind(cmdBuffer);
				quadBuffer->Draw(cmdBuffer);
				// ...which sees the translucent depth too
				BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);
				DrawList(cmdBuffer, ListType_Translucent, current_pass.autosort, Pass::Depth, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);
			}
			// The picture put together and resolved, by the last render pass, all of it
			cmdBuffer.nextSubpass(vk::SubpassContents::eInline);
			SetScissor(cmdBuffer, viewport);
			quadBuffer->Bind(cmdBuffer);
			if (finalPass)
			{
				cmdBuffer.setViewport(0, vk::Viewport(0.f, 0.f, (float)viewport.extent.width, (float)viewport.extent.height, 0.f, 1.f));
				cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelineManager->GetFinalPipeline());
				quadBuffer->Draw(cmdBuffer);
			}
		}

		// Clear
		vk::MemoryBarrier memoryBarrier(vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eShaderWrite);
		cmdBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eFragmentShader,
				vk::DependencyFlagBits::eByRegion, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
		vk::Pipeline pipeline = pipelineManager->GetClearPipeline();
		cmdBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
		quadBuffer->Draw(cmdBuffer);

		if (!finalPass && ssaa)
		{
			// (the next render pass draws from the frame's geometry, not the quad)
			BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);
			cmdBuffer.endRenderPass();
			oitBuffers->CountPass(cmdBuffer);
		}
		else if (!finalPass)
		{
	    	// Re-bind vertex and index buffers
	    	BindGeometry(cmdBuffer, mainBuffer, offsets.indexOffset);

			// Tr depth-only pass
			DrawList(cmdBuffer, ListType_Translucent, current_pass.autosort, Pass::Depth, pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count);

			cmdBuffer.endRenderPass();
			oitBuffers->CountPass(cmdBuffer);
		}
		else if (!lastBand)
		{
			// (the last band's is ended by EndFrame())
			cmdBuffer.endRenderPass();
			oitBuffers->CountPass(cmdBuffer);
		}

		previous_pass = current_pass;
    }
	}
	band = vk::Rect2D();

	return !pvrrc.isRTT;
}

/* The bands this frame is drawn in, from the counts of the frame drawn
 * in this frame slot before (OITBuffers::Poll()): never this frame's, which
 * nothing waits for. More at once when that one had fragments that found no
 * room; fewer when a second of frames in a row would have fitted in fewer.
 * Each band is taken to need what the fullest one did, with a quarter more
 * room. */
void OITDrawer::ChooseBands()
{
	constexpr u32 MaxBands = 64;
	u32 fragments, drawnIn;
	if (oitBuffers->Poll(commandPool->GetIndex(), this, fragments, drawnIn))
	{
		const u64 room = std::max(oitBuffers->GetCapacity(), 1u);
		const u64 frame = (u64)fragments * std::max(drawnIn, 1u);
		const u32 wanted = (u32)std::max<u64>(std::min<u64>((frame + frame / 4 + room - 1) / room, MaxBands), 1);
		const u32 was = bands;
		if (fragments > room)
		{
			bands = std::min(std::max({ bands, wanted, drawnIn + 1 }), MaxBands);
			roomyFrames = 0;
		}
		else if (wanted < bands)
		{
			if (++roomyFrames >= 60)
			{
				bands = wanted;
				roomyFrames = 0;
			}
		}
		else
			roomyFrames = 0;
		if (bands != was)
			NOTICE_LOG(RENDERER, "Vulkan per-pixel renderer: %u translucent fragments in a band of %u, room for %u: %u band%s",
					fragments, drawnIn, (u32)room, bands, bands > 1 ? "s" : "");
	}
}

void OITDrawer::MakeBuffers(int width, int height)
{
	oitBuffers->Init(width, height, commandPool);

	// (made again for another count of samples)
	const u32 samples = pipelineManager->GetSamples();
	if (samples != attachmentSamples)
	{
		maxWidth = maxHeight = 0;
		attachmentSamples = samples;
	}
	if (width <= maxWidth && height <= maxHeight)
		return;
	maxWidth = std::max(maxWidth, width);
	maxHeight = std::max(maxHeight, height);

	if (samples > 1)
	{
		MakeSampleAttachments();
		return;
	}

	for (auto& attachment : colorAttachments)
	{
		if (attachment)
			commandPool->DeferDelete(std::move(attachment));
		attachment = std::unique_ptr<FramebufferAttachment>(
				new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
		attachment->Init(maxWidth, maxHeight, GetColorFormat(),
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);
	}

	if (depthAttachment)
		commandPool->DeferDelete(std::move(depthAttachment));
	depthAttachment = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
	depthAttachment->Init(maxWidth, maxHeight, GetContext()->GetDepthFormat(),
			vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eInputAttachment);
	needDepthTransition = true;

	vk::ImageView attachments[] = {
			colorAttachments[1]->GetImageView(),
			colorAttachments[0]->GetImageView(),
			depthAttachment->GetImageView(),
	};
	vk::FramebufferCreateInfo createInfo(vk::FramebufferCreateFlags(), pipelineManager->GetRenderPass(true, true),
			ARRAY_SIZE(attachments), attachments, width, height, 1);
	tempFramebuffers[0] = GetContext()->GetDevice().createFramebufferUnique(createInfo);
	attachments[0] = attachments[1];
	attachments[1] = colorAttachments[1]->GetImageView();
	tempFramebuffers[1] = GetContext()->GetDevice().createFramebufferUnique(createInfo);
}

/* SSAA: one image of samples for the colour, one for the depth, for all
 * the render passes of a frame (RenderPasses::MakeSsaaRenderPass()), of
 * the largest size asked for. Not transient: a frame of several render
 * passes stores them for the next one (and a device may drop what is
 * stored to an attachment made transient). */
void OITDrawer::MakeSampleAttachments()
{
	const vk::SampleCountFlagBits n = (vk::SampleCountFlagBits)pipelineManager->GetSamples();
	for (auto& attachment : colorAttachments)
		if (attachment)
			commandPool->DeferDelete(std::move(attachment));
	for (auto& framebuffer : tempFramebuffers)
		if (framebuffer)
			commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
	colorAttachments[0] = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
	colorAttachments[0]->Init(maxWidth, maxHeight, GetColorFormat(),
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment, true, n);
	if (depthAttachment)
		commandPool->DeferDelete(std::move(depthAttachment));
	depthAttachment = std::unique_ptr<FramebufferAttachment>(
			new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice()));
	depthAttachment->Init(maxWidth, maxHeight, GetContext()->GetDepthFormat(),
			vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eInputAttachment, true, n);
	needDepthTransition = true;
}

void OITScreenDrawer::KeepPicture()
{
	if (!havePicture)
		return;
	u8 *pixels = last_picture_keep(viewport.extent.width, viewport.extent.height);
	if (pixels != NULL)
		vk_read_picture(finalColorAttachments[GetCurrentImage()]->GetImage(), GetContext()->GetColorFormat(),
				viewport.extent.width, viewport.extent.height, pixels);
}

void OITScreenDrawer::MakeFramebuffers()
{
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent = GetContext()->GetViewPort();

	SetScreenSize(viewport.extent.width, viewport.extent.height);
	MakeBuffers(viewport.extent.width, viewport.extent.height);
	// (kept until the frames that could still use them have gone by: see the per-triangle screen drawer)
	if (commandPool != nullptr)
	{
		// the image with the last frame stays, as the picture the next render starts from
		if (carriedPicture)
			commandPool->DeferDelete(std::move(carriedPicture));
		if (havePicture && (size_t)GetCurrentImage() < finalColorAttachments.size() && finalColorAttachments[GetCurrentImage()])
		{
			carriedPicture = std::move(finalColorAttachments[GetCurrentImage()]);
			restoredPicture = carriedPicture->GetImageView();
		}
		for (auto& framebuffer : framebuffers)
			if (framebuffer)
				commandPool->DeferDelete(std::unique_ptr<vk::UniqueFramebuffer>(new vk::UniqueFramebuffer(std::move(framebuffer))));
		for (auto& attachment : finalColorAttachments)
			if (attachment)
				commandPool->DeferDelete(std::move(attachment));
	}
	framebuffers.clear();
	finalColorAttachments.clear();
	havePicture = false;
	while (finalColorAttachments.size() < GetContext()->GetSwapChainSize())
	{
		finalColorAttachments.push_back(std::unique_ptr<FramebufferAttachment>(
				new FramebufferAttachment(GetContext()->GetPhysicalDevice(), GetContext()->GetDevice())));
		// (copied from when the context goes, to keep the picture: rend/last_picture.h)
		finalColorAttachments.back()->Init(viewport.extent.width, viewport.extent.height, GetContext()->GetColorFormat(),
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc
				| (storagePicture ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlags()), false);
		vk::ImageView attachments[] = {
				finalColorAttachments.back()->GetImageView(),
				colorAttachments[0]->GetImageView(),
				depthAttachment->GetImageView(),
		};
		vk::FramebufferCreateInfo createInfo(vk::FramebufferCreateFlags(), screenPipelineManager->GetRenderPass(true, true),
				ARRAY_SIZE(attachments), attachments, viewport.extent.width, viewport.extent.height, 1);
		framebuffers.push_back(GetContext()->GetDevice().createFramebufferUnique(createInfo));
	}
}

OITTextureDrawer::Size OITTextureDrawer::GetSize()
{
	Size s;
	// the frame from its first pixel, as far as the pixel clip goes (see TransformMatrix)
	s.origWidth = pvrrc.fb_X_CLIP.max + 1;
	s.origHeight = pvrrc.fb_Y_CLIP.max + 1;
	s.clipX = pvrrc.fb_X_CLIP.min;
	s.clipY = pvrrc.fb_Y_CLIP.min;
	s.clipWidth = s.origWidth - s.clipX;
	s.clipHeight = s.origHeight - s.clipY;
	s.upscaledWidth = s.origWidth;
	s.upscaledHeight = s.origHeight;
	s.heightPow2 = 8;
	while (s.heightPow2 < s.upscaledHeight)
		s.heightPow2 *= 2;
	s.widthPow2 = 8;
	while (s.widthPow2 < s.upscaledWidth)
		s.widthPow2 *= 2;

	if (settings.rend.RenderToTextureUpscale > 1)
	{
		s.upscaledWidth *= settings.rend.RenderToTextureUpscale;
		s.upscaledHeight *= settings.rend.RenderToTextureUpscale;
		s.widthPow2 *= settings.rend.RenderToTextureUpscale;
		s.heightPow2 *= settings.rend.RenderToTextureUpscale;
	}
	return s;
}

vk::ImageLayout OITTextureDrawer::TakeTexture(const Size& size, const vk::ImageUsageFlags& usage)
{
	const u32 origWidth = size.origWidth, origHeight = size.origHeight;
	const u32 widthPow2 = size.widthPow2, heightPow2 = size.heightPow2;
	vk::ImageLayout colorImageCurrentLayout;

	/* what an earlier render left waiting in this memory, while its picture is still what it was: see rtt_read.h */
	vk_rtt_supersede(textureAddr, size.clipX, size.clipY, size.clipWidth, size.clipHeight);
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

	TSP tsp = {};
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < origWidth; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < origHeight; tsp.TexV++);

	texture = textureCache->getTextureCacheData(tsp, tcw);
	if (texture->IsNew())
	{
		texture->Create();
		texture->SetPhysicalDevice(GetContext()->GetPhysicalDevice());
		texture->SetDevice(GetContext()->GetDevice());
	}
	else if (textureCache->IsInFlight(texture))
	{
		texture->readOnlyImageView = texture->GetImageView();
		textureCache->DestroyLater(texture);
	}
	textureCache->SetInFlight(texture);

	/* A texture the game uploaded can become one it renders to, at the
	 * same address, the same size and in this format: its image was not
	 * made to be rendered to, nor to be copied from, and is made again. */
	const vk::ImageUsageFlags renderTargetUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled
			| vk::ImageUsageFlagBits::eTransferSrc	// read back when the game touches it in video memory
			| storageUsage | usage;
	if (texture->format != vk::Format::eR8G8B8A8Unorm || texture->extent.width != widthPow2 || texture->extent.height != heightPow2
			|| (texture->imageUsage & renderTargetUsage) != renderTargetUsage)
	{
		texture->extent = vk::Extent2D(widthPow2, heightPow2);
		texture->format = vk::Format::eR8G8B8A8Unorm;
		texture->needsStaging = true;
		texture->CreateRenderTarget(renderTargetUsage);
		colorImageCurrentLayout = vk::ImageLayout::eUndefined;
	}
	else
	{
		colorImageCurrentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	colorImage = texture->GetImage();
	viewport.offset.x = 0;
	viewport.offset.y = 0;
	viewport.extent.width = widthPow2;
	viewport.extent.height = heightPow2;
	return colorImageCurrentLayout;
}

vk::CommandBuffer OITTextureDrawer::NewFrame()
{
	DEBUG_LOG(RENDERER, "RenderToTexture packmode=%d stride=%d - %d,%d -> %d,%d", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
			FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
	NewImage();

	matrices.CalcMatrices(&pvrrc);

	textureAddr = FB_W_SOF1 & VRAM_MASK;
	const Size size = GetSize();
	const u32 clipX = size.clipX, clipY = size.clipY;
	const u32 clipWidth = size.clipWidth, clipHeight = size.clipHeight;
	const u32 upscaledWidth = size.upscaledWidth, upscaledHeight = size.upscaledHeight;
	const u32 widthPow2 = size.widthPow2, heightPow2 = size.heightPow2;

	VulkanContext *context = GetContext();
	vk::Device device = context->GetDevice();

	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	vertexUniformBuffer = WriteVertexUniforms<OITDescriptorSets::VertexShaderUniforms>(commandBuffer, GetCurrentImage());

	MakeBuffers(widthPow2, heightPow2);

	const vk::ImageLayout colorImageCurrentLayout = TakeTexture(size, vk::ImageUsageFlags());
	const vk::ImageView colorImageView = texture->GetImageView();

	setImageLayout(commandBuffer, colorImage, vk::Format::eR8G8B8A8Unorm, 1, colorImageCurrentLayout, vk::ImageLayout::eColorAttachmentOptimal);

	vk::ImageView imageViews[] = {
		colorImageView,
		colorAttachments[0]->GetImageView(),
		depthAttachment->GetImageView(),
	};
	framebuffers.resize(GetContext()->GetSwapChainSize());
	framebuffers[GetCurrentImage()] = device.createFramebufferUnique(vk::FramebufferCreateInfo(vk::FramebufferCreateFlags(),
			rttPipelineManager->GetRenderPass(true, true), ARRAY_SIZE(imageViews), imageViews, widthPow2, heightPow2, 1));

	commandBuffer.setViewport(0, vk::Viewport(0.0f, 0.0f, (float)upscaledWidth, (float)upscaledHeight, 0.0f, 1.0f));
	{
		// only the pixel clip is written
		const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		baseScissor = vk::Rect2D(vk::Offset2D(clipX * scale, clipY * scale), vk::Extent2D(clipWidth * scale, clipHeight * scale));
	}
	commandBuffer.setScissor(0, baseScissor);
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}

void OITTextureDrawer::EndFrame()
{
	currentCommandBuffer.endRenderPass();
	CountPass(currentCommandBuffer);

	currentCommandBuffer.end();

	colorImage = nullptr;
	currentCommandBuffer = nullptr;
	commandPool->EndFrame();

	Watch();
}

void OITTextureDrawer::Watch()
{
	texture->dirty = 0;
	texture->stride_read = 0;	// (rendered, not read)
   libCore_vramlock_Lock(texture->sa_tex, texture->sa + texture->size - 1, texture);
	/* video memory gets it when something first touches it there */
	vk_rtt_watch(texture, textureAddr, pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min,
			pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
}

/* The compute renderer's render to a texture goes to the texture a render
 * of this drawer's would, which is left as one of its is: in the cache,
 * locked in video memory and watched */
OITTextureDrawer::ComputeTarget OITTextureDrawer::ComputeBegin()
{
	NewImage();
	textureAddr = FB_W_SOF1 & VRAM_MASK;
	// (the compute renderer's own barrier makes it ready to be written, from whatever it held)
	TakeTexture(GetSize(), vk::ImageUsageFlagBits::eStorage);
	ComputeTarget t;
	t.image = colorImage;
	t.view = texture->GetImageView();
	return t;
}

void OITTextureDrawer::ComputeEnd()
{
	colorImage = nullptr;
	commandPool->EndFrame();
	Watch();
}

vk::CommandBuffer OITScreenDrawer::NewFrame()
{
	NewImage();
	vk::CommandBuffer commandBuffer = commandPool->Allocate();
	commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	timer.Begin(commandBuffer, commandPool->GetIndex());

	matrices.CalcMatrices(&pvrrc);
	vertexUniformBuffer = WriteVertexUniforms<OITDescriptorSets::VertexShaderUniforms>(commandBuffer, GetCurrentImage());

	SetBaseScissor();

	commandBuffer.setScissor(0, baseScissor);
	commandBuffer.setViewport(0, vk::Viewport(viewport.offset.x, viewport.offset.y, viewport.extent.width, viewport.extent.height, 0.0f, 1.0f));
	currentCommandBuffer = commandBuffer;

	return commandBuffer;
}
