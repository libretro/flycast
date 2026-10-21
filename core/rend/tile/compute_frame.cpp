#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/* The compute renderer's frame: see compute_frame.h */
#include "compute_frame.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/TexCache.h"
#include "rend/tileclip.h"
#include "rend/depth_scale.h"
#include "rend/ssaa_rate.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static_assert(sizeof(Vertex) == PVRC_VTX_WORDS * 4, "the kernels read the parse's vertices word for word");
static_assert(sizeof(ModTriangle) == 9 * 4, "...and its modifier volume triangles");
// (a record keeps a vertex's number in PVRC_RX_VTX: NAOMI 2's four times the TA data, all vertices)
static_assert(4u * 4 * 1024 * 1024 / sizeof(Vertex) <= PVRC_RX_VTX, "a vertex's number in a record");

/* ---- The frame's words ---- */

static u32 machineScale()
{
	return ta_data_size() / TA_DATA_SIZE;
}

/* (what TA_context::Alloc() gives each list, the most a frame has) */
u32 ComputeFrame::MaxPrims()
{
	return (120 * 1024 + 16384) * machineScale();
}

u32 ComputeFrame::MaxWords()
{
	const u32 s = machineScale();
	const u32 polys = (16384 + 5120 + 10240) * s;
	const u32 mvps = 8192 * s;
	const u32 verts = 4 * 1024 * 1024 * s / sizeof(Vertex);
	return PVRC_F_WORDS + polys * PVRC_POLY_WORDS + mvps * PVRC_MVP_WORDS + (polys + mvps) * PVRC_SRC_WORDS
			+ verts * PVRC_VTX_WORDS + 120 * 1024 * s + 16384 * s * 9;
}

u32 ComputeFrame::RttScale()
{
	return settings.rend.RenderToTextureUpscale > 1 ? (u32)settings.rend.RenderToTextureUpscale : 1;
}

/* (a texture rendered to is at most 1024 x 1024, times Render To Texture
 * Upscaling's 8 at the most: OITTextureDrawer::GetSize()) */
u32 ComputeFrame::MaxRttTiles()
{
	const u32 side = 1024 * 8;
	const u32 tiles = (side + (1u << TileLog2(side)) - 1) >> TileLog2(side);
	return tiles * tiles;
}

static inline u32 rectWord(int x, int y)
{
	return (u32)std::max(x, 0) | ((u32)std::max(y, 0) << 16);
}

static inline u32 fbits(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	return u;
}

u32 ComputeFrame::Pack(const Layout& layout, Slots& slots, u32 *out, u32 room, u32 *primsOut)
{
	const rend_context& rc = pvrrc;
	/* A frame of the GPU's TA decode (rc.gpu) has its vertices, indices and
	 * volume triangles in the decode's buffers, its draw groups' index ranges
	 * only there, and its 1/fZ_max: the numbering is PVRC_K_GPUSRC's (see
	 * PVRC_SRC_WORDS), and none of those buffers is copied */
	const bool gpu = rc.gpu;
	const u32 passCount = (u32)rc.render_passes.used();
	why = Why_Passes;
	if (passCount == 0 || passCount > PVRC_PASS_MAX)
		return 0;
	const u32 nop = rc.global_param_op.used(), npt = rc.global_param_pt.used(), ntr = rc.global_param_tr.used();
	const u32 nmvo = rc.global_param_mvo.used(), nmvt = rc.global_param_mvo_tr.used();
	const u32 polys = nop + npt + ntr, mvps = nmvo + nmvt;
	const u32 oPoly = PVRC_F_WORDS;
	const u32 oMvp = oPoly + polys * PVRC_POLY_WORDS;
	const u32 oSrc = oMvp + mvps * PVRC_MVP_WORDS;
	const u32 oVtx = oSrc + (polys + mvps) * PVRC_SRC_WORDS;
	const u32 oIdx = gpu ? oVtx : oVtx + rc.verts.used() * PVRC_VTX_WORDS;
	const u32 oMod = gpu ? oVtx : oIdx + rc.idx.used();
	const u32 total = gpu ? oVtx : oMod + rc.modtrig.used() * 9;
	why = Why_Room;
	if (total > room)
		return 0;

	const xform& vp = Begin();
	const u32 rttScale = rc.isRTT ? std::max(layout.rttScale, 1u) : 1;
	// the base scissor: x, y, width, height
	int scissor[4];
	if (rc.isRTT)
	{
		/* only the pixel clip is written, as the renderers' renders to a
		 * texture have it; the texture is cleared around it */
		scissor[0] = rc.fb_X_CLIP.min * rttScale;
		scissor[1] = rc.fb_Y_CLIP.min * rttScale;
		scissor[2] = (rc.fb_X_CLIP.max + 1 - rc.fb_X_CLIP.min) * rttScale;
		scissor[3] = (rc.fb_Y_CLIP.max + 1 - rc.fb_Y_CLIP.min) * rttScale;
	}
	else
		BaseScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
	/* A polygon's clip rectangle (x, y, width, height), as the raster
	 * renderers' SetTileClip() makes it */
	auto tileClip = [&](u32 val, int rect[4]) {
		const TileClipping mode = ::GetTileClip(val, vp, rect);
		if (mode == TileClipping::Outside)
			IntersectClipRect(rect, scissor[0], scissor[1], scissor[2], scissor[3]);
		return mode;
	};

	memset(out, 0, PVRC_F_WORDS * 4);
	const int bx0 = std::min(scissor[0], (int)layout.width);
	const int by0 = std::min(scissor[1], (int)layout.height);
	const int bx1 = std::min(scissor[0] + scissor[2], (int)layout.width);
	const int by1 = std::min(scissor[1] + scissor[3], (int)layout.height);
	const u32 baseRect0 = rectWord(bx0, by0), baseRect1 = rectWord(bx1, by1);

	/* The numbering, render pass by render pass, and the table of starts.
	 * (A frame of the decode's: entries of the table where there would be
	 * numbers, and the third word a volume's count or a group's draw.) */
	u32 prim = 0, sources = 0;
	auto source = [&](u32 kind, u32 index, u32 third) {
		out[oSrc + sources * PVRC_SRC_WORDS] = prim;
		out[oSrc + sources * PVRC_SRC_WORDS + 1] = kind | index;
		out[oSrc + sources * PVRC_SRC_WORDS + 2] = third;
		sources++;
	};
	auto at = [&]() { return gpu ? sources : prim; };
	// the modifier volumes' parameters: where the volume each is in ends
	for (u32 m = 0; m < mvps; m++)
	{
		out[oMvp + m * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = PVRC_NONE;
		out[oMvp + m * PVRC_MVP_WORDS + PVRC_M_CLOSE_BY] = PVRC_NONE;
	}
	auto volumes = [&](const ModifierVolumeParam *params, u32 first, u32 last, u32 indexBase) {
		/* DrawModifierVolumes()'s rule: a parameter in an inclusion or
		 * exclusion mode ends the volume, which is every triangle since the
		 * last one that did; parameters without triangles are passed over */
		int base = -1;
		for (u32 m = first; m < last; m++)
		{
			const ModifierVolumeParam& p = params[m];
			if (p.count == 0)
				continue;
			source(PVRC_SRC_MODVOL, indexBase + m, p.count);
			prim += p.count;
			if (base == -1)
				base = (int)m;
			if (p.isp.DepthMode == 1 || p.isp.DepthMode == 2)
			{
				// (its last triangle's number; a frame of the decode's: its entry)
				const u32 end = gpu ? sources - 1 : prim - 1;
				for (u32 k = (u32)base; k <= m; k++)
				{
					if (params[k].count == 0)
						continue;
					out[oMvp + (indexBase + k) * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = end;
					out[oMvp + (indexBase + k) * PVRC_MVP_WORDS + PVRC_M_CLOSE_BY] = indexBase + m;
				}
				base = -1;
			}
		}
	};
	auto strips = [&](const List<PolyParam>& list, u32 first, u32 last, u32 indexBase) {
		for (u32 i = first; i < last; i++)
		{
			const PolyParam& pp = list.head()[i];
			/* (the decode's groups: count is their vertices, first their
			 * command's slot, or the translucent's place in the draw records) */
			if (pp.count <= 2)
				continue;
			source(gpu && list.head() == rc.global_param_tr.head() ? PVRC_SRC_TRD : 0, indexBase + i, pp.first);
			prim += pp.count - 2;
		}
	};
	RenderPass previous = {};
	for (u32 p = 0; p < passCount; p++)
	{
		const RenderPass& pass = rc.render_passes.head()[p];
		u32 *pw = out + PVRC_F_PASS + p * PVRC_PASS_WORDS;
		pw[PVRC_P_OP] = at();
		strips(rc.global_param_op, previous.op_count, pass.op_count, 0);
		pw[PVRC_P_MVO] = at();
		volumes(rc.global_param_mvo.head(), previous.mvo_count, pass.mvo_count, 0);
		pw[PVRC_P_PT] = at();
		strips(rc.global_param_pt, previous.pt_count, pass.pt_count, nop);
		pw[PVRC_P_TR] = at();
		strips(rc.global_param_tr, previous.tr_count, pass.tr_count, nop + npt);
		pw[PVRC_P_TR_END] = at();
		if (pass.mv_op_tr_shared)
		{
			pw[PVRC_P_MVT] = pw[PVRC_P_MVO];
			pw[PVRC_P_MVT_END] = pw[PVRC_P_PT];
		}
		else
		{
			pw[PVRC_P_MVT] = at();
			volumes(rc.global_param_mvo_tr.head(), previous.mvo_tr_count, pass.mvo_tr_count, nmvo);
			pw[PVRC_P_MVT_END] = at();
		}
		pw[PVRC_P_FLAGS] = pass.autosort ? PVRC_PF_AUTOSORT : 0;
		previous = pass;
	}
	// (the decode's frame: its numbering keeps to the room, PVRC_F_PRIM_ROOM)
	if (!gpu && (prim > MaxPrims() || ((prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2) > layout.chunks))
		return 0;

	/* The polygon parameters, with what the per-pixel renderer's pipeline
	 * key and push constants have of them (OITPipelineManager::Key(),
	 * OITDrawer::DrawPoly()) */
	auto poly = [&](const PolyParam& pp, u32 listType, u32 *q) -> bool {
		const bool two = pp.tsp1.full != (u32)-1 || pp.tcw1.full != (u32)-1;
		u32 f = 0;
		if (listType == ListType_Punch_Through)
			f |= PVRC_QF_ALPHA_TEST;
		if (pp.tcw.PixelFmt == PixelBumpMap)
			f |= PVRC_QF_BUMP;
		if (pp.tsp.ColorClamp)
			f |= PVRC_QF_CLAMP;
		if ((pp.tileclip >> 28) == 3)
			f |= PVRC_QF_CLIP_INSIDE;
		f |= (u32)pp.tsp.FogCtrl << PVRC_QF_FOG_SHIFT;
		if (pp.tsp.IgnoreTexA || pp.tcw.PixelFmt == Pixel565)
			f |= PVRC_QF_IGNORE_TEXA;
		if (pp.pcw.Offset)
			f |= PVRC_QF_OFFSET;
		f |= (u32)pp.tsp.ShadInstr << PVRC_QF_SHAD_SHIFT;
		if (pp.pcw.Texture)
			f |= PVRC_QF_TEXTURE;
		if (pp.tsp.UseAlpha)
			f |= PVRC_QF_USE_ALPHA;
		const bool palette = BaseTextureCacheData::IsGpuHandledPaletted(pp.tsp, pp.tcw);
		if (palette)
			f |= PVRC_QF_PALETTE;
		if (pp.pcw.Gouraud)
			f |= PVRC_QF_GOURAUD;
		if (pp.pcw.Shadow)
			f |= PVRC_QF_SHADOW;
		if (SsaaSingleRate(pp))
			f |= PVRC_QF_SINGLE_RATE;
		if (two)
		{
			f |= PVRC_QF_TWO_VOLUMES;
			f |= (u32)pp.tsp1.ShadInstr << PVRC_QF_SHAD1_SHIFT;
			f |= (u32)pp.tsp1.FogCtrl << PVRC_QF_FOG1_SHIFT;
			if (pp.tsp1.UseAlpha)
				f |= PVRC_QF_USE_ALPHA1;
			if (pp.tsp1.IgnoreTexA)
				f |= PVRC_QF_IGNORE_TEXA1;
			if (pp.tsp.IgnoreTexA)
				f |= PVRC_QF_IGNORE_TEXA0;
			if (pp.tsp.UseAlpha)
				f |= PVRC_QF_USE_ALPHA0;
		}
		u32 slot0 = 0xffff, slot1 = 0xffff;
		if (pp.pcw.Texture)
		{
			slot0 = slots.Slot(pp.texid, pp.tsp);
			if (slot0 == PVRC_NONE)
				return false;
			slot1 = slot0;
			if (two && pp.texid1 != (u64)-1)
			{
				slot1 = slots.Slot(pp.texid1, pp.tsp1);
				if (slot1 == PVRC_NONE)
					return false;
			}
		}
		float trilinearAlpha = 1.f;
		if (pp.tsp.FilterMode > 1 && pp.pcw.Texture && listType != ListType_Punch_Through && pp.tcw.MipMapped == 1)
		{
			trilinearAlpha = 0.25f * (pp.tsp.MipMapD & 0x3);
			if (pp.tsp.FilterMode == 2)
				trilinearAlpha = 1.f - trilinearAlpha;
		}
		u32 pal = 0;
		if (palette)
			pal = pp.tcw.PixelFmt == PixelPal4 ? (u32)pp.tcw.PalSelect << 4 : (u32)(pp.tcw.PalSelect >> 4) << 8;
		int r[4] = {};
		const TileClipping clip = tileClip(pp.tileclip, r);
		q[PVRC_Q_ISP] = pp.isp.full;
		q[PVRC_Q_TSP] = pp.tsp.full;
		q[PVRC_Q_TCW] = pp.tcw.full;
		q[PVRC_Q_PCW] = pp.pcw.full;
		q[PVRC_Q_TSP1] = pp.tsp1.full;
		q[PVRC_Q_TCW1] = pp.tcw1.full;
		q[PVRC_Q_TEX] = slot0 | (slot1 << 16);
		q[PVRC_Q_FLAGS] = f;
		q[PVRC_Q_TRILIN] = fbits(trilinearAlpha);
		q[PVRC_Q_PAL] = pal;
		if (clip == TileClipping::Outside)
		{
			q[PVRC_Q_RECT] = rectWord(std::min(r[0], bx1), std::min(r[1], by1));
			q[PVRC_Q_RECT + 1] = rectWord(std::min(r[0] + r[2], bx1), std::min(r[1] + r[3], by1));
		}
		else
		{
			q[PVRC_Q_RECT] = baseRect0;
			q[PVRC_Q_RECT + 1] = baseRect1;
		}
		q[PVRC_Q_CLIP] = rectWord(std::min(r[0], 0xffff), std::min(r[1], 0xffff));
		q[PVRC_Q_CLIP + 1] = rectWord(std::min(r[0] + r[2], 0xffff), std::min(r[1] + r[3], 0xffff));
		q[PVRC_Q_FIRST] = pp.first;
		q[PVRC_Q_LIST] = listType;
		return true;
	};
	why = Why_Textures;
	for (u32 i = 0; i < nop; i++)
		if (!poly(rc.global_param_op.head()[i], ListType_Opaque, out + oPoly + i * PVRC_POLY_WORDS))
			return 0;
	for (u32 i = 0; i < npt; i++)
		if (!poly(rc.global_param_pt.head()[i], ListType_Punch_Through, out + oPoly + (nop + i) * PVRC_POLY_WORDS))
			return 0;
	for (u32 i = 0; i < ntr; i++)
		if (!poly(rc.global_param_tr.head()[i], ListType_Translucent, out + oPoly + (nop + npt + i) * PVRC_POLY_WORDS))
			return 0;
	auto mvp = [&](const ModifierVolumeParam& p, u32 *m) {
		int r[4] = {};
		m[PVRC_M_ISP] = p.isp.full;
		if (tileClip(p.tileclip, r) == TileClipping::Outside)
		{
			m[PVRC_M_RECT] = rectWord(std::min(r[0], bx1), std::min(r[1], by1));
			m[PVRC_M_RECT + 1] = rectWord(std::min(r[0] + r[2], bx1), std::min(r[1] + r[3], by1));
		}
		else
		{
			m[PVRC_M_RECT] = baseRect0;
			m[PVRC_M_RECT + 1] = baseRect1;
		}
		m[PVRC_M_FIRST] = p.first;
	};
	for (u32 i = 0; i < nmvo; i++)
		mvp(rc.global_param_mvo.head()[i], out + oMvp + i * PVRC_MVP_WORDS);
	for (u32 i = 0; i < nmvt; i++)
		mvp(rc.global_param_mvo_tr.head()[i], out + oMvp + (nmvo + i) * PVRC_MVP_WORDS);

	/* The header */
	const u32 tilesX = (layout.width + (1u << layout.tileLog2) - 1) >> layout.tileLog2;
	const u32 tilesY = (layout.height + (1u << layout.tileLog2) - 1) >> layout.tileLog2;
	out[PVRC_F_WIDTH] = layout.width;
	out[PVRC_F_HEIGHT] = layout.height;
	out[PVRC_F_TILES_X] = tilesX;
	out[PVRC_F_TILES_Y] = tilesY;
	out[PVRC_F_TILE_LOG2] = layout.tileLog2;
	out[PVRC_F_PRIMS] = gpu ? 0 : prim;
	out[PVRC_F_PASSES] = passCount;
	out[PVRC_F_FLAGS] = layout.lastPicture ? (layout.lastScaled ? PVRC_FF_LAST_PICTURE | PVRC_FF_LAST_SCALED : PVRC_FF_LAST_PICTURE) : 0;
	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other black (OITDrawer::Draw()), a texture rendered to too */
	out[PVRC_F_CLEAR] = rc.clearFramebuffer
			? (VO_BORDER_COL.Red | (VO_BORDER_COL.Green << 8) | (VO_BORDER_COL.Blue << 16) | 0xff000000u)
			: 0xff000000u;
	out[PVRC_F_SCISSOR] = (u32)bx0;
	out[PVRC_F_SCISSOR + 1] = (u32)by0;
	out[PVRC_F_SCISSOR + 2] = (u32)bx1;
	out[PVRC_F_SCISSOR + 3] = (u32)by1;
	/* (a render to a texture: the matrix is to the frame's own pixels, and
	 * the raster renderer's viewport is that many times larger) */
	const float vs = (float)rttScale;
	out[PVRC_F_XFORM] = fbits(vp.sx * vs);
	out[PVRC_F_XFORM + 1] = fbits(vp.sy * vs);
	out[PVRC_F_XFORM + 2] = fbits(vp.tx * vs);
	out[PVRC_F_XFORM + 3] = fbits(vp.ty * vs);
	out[PVRC_F_XFORM + 4] = fbits(vp.sz);
	/* Where the raster renderers clip a triangle with a vertex behind the
	 * eye, which the kernels clip there too (pvr_compute.h,
	 * rec_clip_plane()): their depth scale, over the kernels' 1/w scale */
	out[PVRC_F_ZCLIP] = fbits(gpu ? 1.f / vp.sz : pvr_depth_scale(rc.fZ_max) * (1.f / vp.sz));
	out[PVRC_F_GPU] = gpu ? (u32)rc.idx.size : 0;
	out[PVRC_F_PRIM_ROOM] = MaxPrims();
	/* the raster renderers' fragment constants (Vulkan's
	 * MakeFragmentUniforms(), Direct3D 12's MakeFragmentConstants()) */
	{
		const u8 *fogVert = (const u8 *)&FOG_COL_VERT;	// (BGRA)
		const u8 *fogRam = (const u8 *)&FOG_COL_RAM;
		for (int c = 0; c < 3; c++)
		{
			out[PVRC_F_FOG_RAM + c] = fbits(fogRam[2 - c] / 255.0f);
			out[PVRC_F_FOG_VERT + c] = fbits(fogVert[2 - c] / 255.0f);
		}
		const u8 *density = (const u8 *)&FOG_DENSITY;
		out[PVRC_F_FOG_DEN] = fbits(density[1] / 128.0f * powf(2.0f, (float)(s8)density[0]));
		const int shifts[4] = { 16, 8, 0, 24 };
		for (int c = 0; c < 4; c++)
		{
			out[PVRC_F_CLAMP_MIN + c] = fbits(((rc.fog_clamp_min >> shifts[c]) & 0xFF) / 255.0f);
			out[PVRC_F_CLAMP_MAX + c] = fbits(((rc.fog_clamp_max >> shifts[c]) & 0xFF) / 255.0f);
		}
		// (half a step under the reference: see the same in gles/gles.cpp)
		out[PVRC_F_ALPHA_REF] = fbits(((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f);
	}
	out[PVRC_F_SHADE] = fbits(FPU_SHAD_SCALE.scale_factor / 256.f);
	out[PVRC_F_CHUNKS] = gpu ? 0 : (prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	out[PVRC_F_SOURCES] = sources;
	out[PVRC_F_POOL] = layout.pool;
	out[PVRC_F_O_POLY] = oPoly;
	out[PVRC_F_O_MVP] = oMvp;
	out[PVRC_F_O_SRC] = oSrc;
	out[PVRC_F_O_VTX] = gpu ? 0 : oVtx;
	out[PVRC_F_O_IDX] = gpu ? 0 : oIdx;
	out[PVRC_F_O_MOD] = gpu ? 0 : oMod;
	out[PVRC_F_RUNS] = layout.runs;
	MakeFogTexture((u8 *)(out + PVRC_F_FOG));

	/* The parse's buffers, with flat shading's colours where the raster
	 * renderers put them: only now, when nothing can leave the frame to
	 * them any more, for they change the vertices as well */
	if (!gpu)
	{
		ProvokingVertices();
		memcpy(out + oVtx, rc.verts.head(), rc.verts.used() * sizeof(Vertex));
		memcpy(out + oIdx, rc.idx.head(), rc.idx.used() * 4);
		memcpy(out + oMod, rc.modtrig.head(), rc.modtrig.used() * sizeof(ModTriangle));
	}

	*primsOut = gpu ? 0 : prim;
	why = Why_None;
	return total;
}
