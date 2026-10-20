#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/* The compute renderer on Vulkan: see compute_drawer.h */
#include "compute_drawer.h"
#include "vulkan_context.h"
#include "ssaa.h"
#include "rend/TexCache.h"
#include "hw/pvr/pvr_mem.h"

#include <algorithm>

/* The kernels, by PVRC_K_*, as SPIR-V made from rend/tile/pvr_compute.h
 * when the source was: tools/rend/kernels_spv.sh, which checks they still
 * are its. Nothing is compiled when the core runs. */
#include "compute_spv.inc"
static_assert(sizeof(compute_spv) / sizeof(compute_spv[0]) == PVRC_KERNELS + 1, "a kernel for each PVRC_K_*");
static_assert(sizeof(Vertex) == PVRC_VTX_WORDS * 4, "the kernels read the parse's vertices word for word");
static_assert(sizeof(ModTriangle) == 9 * 4, "...and its modifier volume triangles");
// (a record keeps a vertex's number in PVRC_RX_VTX: NAOMI 2's four times the TA data, all vertices)
static_assert(4u * 4 * 1024 * 1024 / sizeof(Vertex) <= PVRC_RX_VTX, "a vertex's number in a record");

/* The bin pool's entries: a run for each tile a primitive touches, for a
 * frame's every primitive (4 MiB) */
static const u32 PoolEntries = 2u << 20;

// The bindings of the kernels' one set
enum { B_FRAME, B_REC, B_BIN, B_OUT, B_LAST, B_PAL, B_TEX, B_COUNT };

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
	const u32 passCount = (u32)rc.render_passes.used();
	if (passCount == 0 || passCount > PVRC_PASS_MAX)
		return 0;
	const u32 nop = rc.global_param_op.used(), npt = rc.global_param_pt.used(), ntr = rc.global_param_tr.used();
	const u32 nmvo = rc.global_param_mvo.used(), nmvt = rc.global_param_mvo_tr.used();
	const u32 polys = nop + npt + ntr, mvps = nmvo + nmvt;
	const u32 oPoly = PVRC_F_WORDS;
	const u32 oMvp = oPoly + polys * PVRC_POLY_WORDS;
	const u32 oSrc = oMvp + mvps * PVRC_MVP_WORDS;
	const u32 oVtx = oSrc + (polys + mvps) * PVRC_SRC_WORDS;
	const u32 oIdx = oVtx + rc.verts.used() * PVRC_VTX_WORDS;
	const u32 oMod = oIdx + rc.idx.used();
	const u32 total = oMod + rc.modtrig.used() * 9;
	if (total > room)
		return 0;

	matrices.CalcMatrices(&pvrrc);
	SetBaseScissor();

	memset(out, 0, PVRC_F_WORDS * 4);
	const int bx0 = std::min((int)baseScissor.offset.x, (int)layout.width);
	const int by0 = std::min((int)baseScissor.offset.y, (int)layout.height);
	const int bx1 = std::min((int)(baseScissor.offset.x + baseScissor.extent.width), (int)layout.width);
	const int by1 = std::min((int)(baseScissor.offset.y + baseScissor.extent.height), (int)layout.height);
	const u32 baseRect0 = rectWord(bx0, by0), baseRect1 = rectWord(bx1, by1);

	/* The numbering, render pass by render pass, and the table of starts */
	u32 prim = 0, sources = 0;
	auto source = [&](u32 kind, u32 index) {
		out[oSrc + sources * PVRC_SRC_WORDS] = prim;
		out[oSrc + sources * PVRC_SRC_WORDS + 1] = kind | index;
		sources++;
	};
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
		u32 at = prim;
		for (u32 m = first; m < last; m++)
		{
			const ModifierVolumeParam& p = params[m];
			if (p.count == 0)
				continue;
			source(PVRC_SRC_MODVOL, indexBase + m);
			at += p.count;
			prim += p.count;
			if (base == -1)
				base = (int)m;
			if (p.isp.DepthMode == 1 || p.isp.DepthMode == 2)
			{
				for (u32 k = (u32)base; k <= m; k++)
				{
					if (params[k].count == 0)
						continue;
					out[oMvp + (indexBase + k) * PVRC_MVP_WORDS + PVRC_M_CLOSE_END] = at - 1;
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
			if (pp.count <= 2)
				continue;
			source(0, indexBase + i);
			prim += pp.count - 2;
		}
	};
	RenderPass previous = {};
	for (u32 p = 0; p < passCount; p++)
	{
		const RenderPass& pass = rc.render_passes.head()[p];
		u32 *pw = out + PVRC_F_PASS + p * PVRC_PASS_WORDS;
		pw[PVRC_P_OP] = prim;
		strips(rc.global_param_op, previous.op_count, pass.op_count, 0);
		pw[PVRC_P_MVO] = prim;
		volumes(rc.global_param_mvo.head(), previous.mvo_count, pass.mvo_count, 0);
		pw[PVRC_P_PT] = prim;
		strips(rc.global_param_pt, previous.pt_count, pass.pt_count, nop);
		pw[PVRC_P_TR] = prim;
		strips(rc.global_param_tr, previous.tr_count, pass.tr_count, nop + npt);
		pw[PVRC_P_TR_END] = prim;
		if (pass.mv_op_tr_shared)
		{
			pw[PVRC_P_MVT] = pw[PVRC_P_MVO];
			pw[PVRC_P_MVT_END] = pw[PVRC_P_PT];
		}
		else
		{
			pw[PVRC_P_MVT] = prim;
			volumes(rc.global_param_mvo_tr.head(), previous.mvo_tr_count, pass.mvo_tr_count, nmvo);
			pw[PVRC_P_MVT_END] = prim;
		}
		pw[PVRC_P_FLAGS] = pass.autosort ? PVRC_PF_AUTOSORT : 0;
		previous = pass;
	}
	if (prim > MaxPrims() || ((prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2) > layout.chunks)
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
		vk::Rect2D r;
		const TileClipping clip = SetTileClip(pp.tileclip, r);
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
			q[PVRC_Q_RECT] = rectWord(std::min(r.offset.x, bx1), std::min(r.offset.y, by1));
			q[PVRC_Q_RECT + 1] = rectWord(std::min(r.offset.x + (int)r.extent.width, bx1), std::min(r.offset.y + (int)r.extent.height, by1));
		}
		else
		{
			q[PVRC_Q_RECT] = baseRect0;
			q[PVRC_Q_RECT + 1] = baseRect1;
		}
		q[PVRC_Q_CLIP] = rectWord(std::min(r.offset.x, 0xffff), std::min(r.offset.y, 0xffff));
		q[PVRC_Q_CLIP + 1] = rectWord(std::min(r.offset.x + (int)r.extent.width, 0xffff), std::min(r.offset.y + (int)r.extent.height, 0xffff));
		q[PVRC_Q_FIRST] = pp.first;
		q[PVRC_Q_LIST] = listType;
		return true;
	};
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
		vk::Rect2D r;
		m[PVRC_M_ISP] = p.isp.full;
		if (SetTileClip(p.tileclip, r) == TileClipping::Outside)
		{
			m[PVRC_M_RECT] = rectWord(std::min(r.offset.x, bx1), std::min(r.offset.y, by1));
			m[PVRC_M_RECT + 1] = rectWord(std::min(r.offset.x + (int)r.extent.width, bx1), std::min(r.offset.y + (int)r.extent.height, by1));
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
	out[PVRC_F_PRIMS] = prim;
	out[PVRC_F_PASSES] = passCount;
	out[PVRC_F_FLAGS] = layout.lastPicture ? PVRC_FF_LAST_PICTURE : 0;
	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour; any other black (OITDrawer::Draw()) */
	out[PVRC_F_CLEAR] = rc.clearFramebuffer
			? (VO_BORDER_COL.Red | (VO_BORDER_COL.Green << 8) | (VO_BORDER_COL.Blue << 16) | 0xff000000u)
			: 0xff000000u;
	out[PVRC_F_SCISSOR] = (u32)bx0;
	out[PVRC_F_SCISSOR + 1] = (u32)by0;
	out[PVRC_F_SCISSOR + 2] = (u32)bx1;
	out[PVRC_F_SCISSOR + 3] = (u32)by1;
	const xform& vp = matrices.GetViewportMatrix();
	out[PVRC_F_XFORM] = fbits(vp.sx);
	out[PVRC_F_XFORM + 1] = fbits(vp.sy);
	out[PVRC_F_XFORM + 2] = fbits(vp.tx);
	out[PVRC_F_XFORM + 3] = fbits(vp.ty);
	out[PVRC_F_XFORM + 4] = fbits(vp.sz);
	/* Where the raster renderers clip a triangle with a vertex behind the
	 * eye, which the kernels clip there too (pvr_compute.h,
	 * rec_clip_plane()): their depth scale, over the kernels' 1/w scale */
	out[PVRC_F_ZCLIP] = fbits(pvr_depth_scale(rc.fZ_max) * (1.f / vp.sz));
	struct FragmentUniforms
	{
		float colorClampMin[4];
		float colorClampMax[4];
		float sp_FOG_COL_RAM[4];
		float sp_FOG_COL_VERT[4];
		float cp_AlphaTestValue;
		float sp_FOG_DENSITY;
	} u = MakeFragmentUniforms<FragmentUniforms>();
	out[PVRC_F_FOG_DEN] = fbits(u.sp_FOG_DENSITY);
	for (int c = 0; c < 3; c++)
	{
		out[PVRC_F_FOG_RAM + c] = fbits(u.sp_FOG_COL_RAM[c]);
		out[PVRC_F_FOG_VERT + c] = fbits(u.sp_FOG_COL_VERT[c]);
	}
	for (int c = 0; c < 4; c++)
	{
		out[PVRC_F_CLAMP_MIN + c] = fbits(u.colorClampMin[c]);
		out[PVRC_F_CLAMP_MAX + c] = fbits(u.colorClampMax[c]);
	}
	out[PVRC_F_ALPHA_REF] = fbits(u.cp_AlphaTestValue);
	out[PVRC_F_SHADE] = fbits(FPU_SHAD_SCALE.scale_factor / 256.f);
	out[PVRC_F_CHUNKS] = (prim + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	out[PVRC_F_SOURCES] = sources;
	out[PVRC_F_POOL] = layout.pool;
	out[PVRC_F_O_POLY] = oPoly;
	out[PVRC_F_O_MVP] = oMvp;
	out[PVRC_F_O_SRC] = oSrc;
	out[PVRC_F_O_VTX] = oVtx;
	out[PVRC_F_O_IDX] = oIdx;
	out[PVRC_F_O_MOD] = oMod;
	out[PVRC_F_RUNS] = layout.runs;
	MakeFogTexture((u8 *)(out + PVRC_F_FOG));

	/* The parse's buffers, with flat shading's colours where the raster
	 * renderers put them: only now, when nothing can leave the frame to
	 * them any more, for they change the vertices as well */
	SetProvokingVertices();
	memcpy(out + oVtx, rc.verts.head(), rc.verts.used() * sizeof(Vertex));
	memcpy(out + oIdx, rc.idx.head(), rc.idx.used() * 4);
	memcpy(out + oMod, rc.modtrig.head(), rc.modtrig.used() * sizeof(ModTriangle));

	*primsOut = prim;
	return total;
}

/* ---- The Vulkan side ---- */

/* The textures a frame's polygons have, each given a slot: a table of a
 * fixed size, emptied by a new generation each frame */
struct SlotTable : public ComputeFrame::Slots
{
	static const u32 Size = 2 * PVRC_TEX_SLOTS;
	struct Entry
	{
		u64 texid;
		u32 tsp;
		u32 gen;
		u32 slot;
	};
	Entry entries[Size];
	u64 slotTex[PVRC_TEX_SLOTS];
	u32 slotTsp[PVRC_TEX_SLOTS];
	u32 gen = 0;
	u32 used = 0;

	SlotTable() { memset(entries, 0, sizeof(entries)); }
	void Reset()
	{
		if (++gen == 0)
		{
			memset(entries, 0, sizeof(entries));
			gen = 1;
		}
		used = 0;
	}
	u32 Slot(u64 texid, TSP tsp) override
	{
		const u32 mask = tsp.full & SamplerManager::TSP_Mask;
		u32 h = (u32)(texid >> 4) * 2654435761u ^ mask * 40503u;
		for (u32 i = 0; i < Size; i++)
		{
			Entry& e = entries[(h + i) & (Size - 1)];
			if (e.gen != gen)
			{
				if (used == PVRC_TEX_SLOTS)
					return PVRC_NONE;
				e.gen = gen;
				e.texid = texid;
				e.tsp = tsp.full;
				e.slot = used;
				slotTex[used] = texid;
				slotTsp[used] = tsp.full;
				return used++;
			}
			if (e.texid == texid && (e.tsp & SamplerManager::TSP_Mask) == mask)
				return e.slot;
		}
		return PVRC_NONE;
	}
};

struct ComputeDrawer::Resources
{
	std::unique_ptr<BufferData> ring;	// a piece for each slot of the pool
	u32 ringWords = 0;
	u32 *ringMap = nullptr;
	std::unique_ptr<BufferData> records;
	std::unique_ptr<BufferData> bins;
	std::unique_ptr<BufferData> stats;	// the counts, a piece a slot, read when it comes round
	u32 *statsMap = nullptr;
	std::vector<bool> statsWritten;
	u32 binWords = 0;
	u32 runs = 0, chunks = 0, tileLog2 = 5;
	u32 keep = 4;
	bool binsZero = false;
	vk::UniqueDescriptorSetLayout setLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniquePipeline pipelines[PVRC_KERNELS + 1];
	vk::UniqueDescriptorPool descriptorPool;
	std::vector<vk::DescriptorSet> sets;
	// what each set's texture slots, picture and last picture have
	std::vector<std::vector<VkImageView>> setViews;
	std::vector<std::vector<VkSampler>> setSamplers;
	std::vector<u32> setUsed;
	SlotTable slots;
	VkImageView dummyView = VK_NULL_HANDLE;
	std::vector<vk::DescriptorImageInfo> imageInfos;
	std::vector<vk::WriteDescriptorSet> writes;
	vk::UniqueSampler nearest;
	// this frame's, between Prepare and Record
	u32 words = 0, prims = 0;
	u64 skipped = 0, dropped = 0, hiz = 0;
	u32 peels = 0;
};

ComputeDrawer::ComputeDrawer() {}
ComputeDrawer::~ComputeDrawer() {}

const char *ComputeDrawer::DeviceLacks()
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::PhysicalDeviceLimits limits = context->GetPhysicalDevice().getProperties().limits;
	if (!context->SupportsShaderInt64())
		return "no 64-bit integers in shaders";
	if (!context->SupportsSampledImageArrayIndexing())
		return "no indexing of an array of textures";
	if (limits.maxComputeWorkGroupInvocations < PVRC_WG || limits.maxComputeWorkGroupSize[0] < PVRC_WG)
		return "workgroups too small";
	if (limits.maxPerStageDescriptorSamplers < PVRC_TEX_SLOTS + 2 || limits.maxPerStageDescriptorSampledImages < PVRC_TEX_SLOTS + 2
			|| limits.maxDescriptorSetSamplers < PVRC_TEX_SLOTS + 2 || limits.maxDescriptorSetSampledImages < PVRC_TEX_SLOTS + 2)
		return "too few texture bindings";
	if (limits.maxComputeSharedMemorySize < PVRC_SORT_MAX * 4)
		return "too little shared memory";
	return nullptr;
}

bool ComputeDrawer::Init(vk::Extent2D extent, u32 samples, CommandPool *pool, SamplerManager *samplerManager)
{
	Term();
	if (failed)
		return false;
	VulkanContext *context = VulkanContext::Instance();
	const vk::Device device = context->GetDevice();
	const vk::PhysicalDeviceLimits limits = context->GetPhysicalDevice().getProperties().limits;
	this->extent = extent;
	this->samples = samples > 4 ? 8 : samples > 2 ? 4 : samples > 1 ? 2 : 1;
	this->commandPool = pool;
	this->samplerManager = samplerManager;

	auto fail = [&](const char *why) {
		INFO_LOG(RENDERER, "Vulkan compute renderer: not on this device (%s); the raster renderer draws", why);
		failed = true;
		res.reset();
		return false;
	};
	const char *lacks = DeviceLacks();
	if (lacks != nullptr)
		return fail(lacks);
	std::unique_ptr<Resources> r(new Resources());
	r->keep = pvrc_fragments_kept(limits.maxComputeSharedMemorySize);
	r->tileLog2 = extent.width > 1280 ? 6 : 5;
	const u32 tiles = ((extent.width + (1u << r->tileLog2) - 1) >> r->tileLog2) * ((extent.height + (1u << r->tileLog2) - 1) >> r->tileLog2);
	r->chunks = (ComputeFrame::MaxPrims() + PVR_CHUNK_PRIMS - 1) >> PVR_CHUNK_LOG2;
	r->runs = r->chunks * tiles;
	r->binWords = pvrc_bin_words(r->runs, PoolEntries);
	r->ringWords = (ComputeFrame::MaxWords() + 63) & ~63u;
	const int slots = pool->GetSize();
	const u64 most = std::min<u64>(context->GetMaxStorageBufferRange(), context->GetMaxMemoryAllocationSize());
	if ((u64)r->ringWords * 4 > most || (u64)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4 > most || (u64)r->binWords * 4 > most)
		return fail("buffers larger than the device has");
	const u64 deviceBytes = (u64)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4 + (u64)r->binWords * 4;
	if ((deviceBytes >> 10) > context->GetVramBudget().available_kib())
		return fail("not enough video memory");
	try {
		const vk::BufferUsageFlags storage = vk::BufferUsageFlagBits::eStorageBuffer;
		r->ring = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->ringWords * 4 * slots, storage));
		r->ringMap = (u32 *)r->ring->MapMemory();
		r->records = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)ComputeFrame::MaxPrims() * PVRC_REC_WORDS * 4,
				storage, vk::MemoryPropertyFlagBits::eDeviceLocal));
		r->bins = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)r->binWords * 4,
				storage | vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
				vk::MemoryPropertyFlagBits::eDeviceLocal));
		r->stats = std::unique_ptr<BufferData>(new BufferData((vk::DeviceSize)PVRC_STATS * 4 * slots, vk::BufferUsageFlagBits::eTransferDst));
		r->statsMap = (u32 *)r->stats->MapMemory();
		r->statsWritten.assign(slots, false);
	} catch (const vk::SystemError& e) {
		return fail(e.what());
	}

	vk::DescriptorSetLayoutBinding bindings[B_COUNT] = {
		{ B_FRAME, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_REC, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_BIN, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_OUT, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_LAST, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_PAL, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute },
		{ B_TEX, vk::DescriptorType::eCombinedImageSampler, PVRC_TEX_SLOTS, vk::ShaderStageFlagBits::eCompute },
	};
	r->setLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo(vk::DescriptorSetLayoutCreateFlags(), B_COUNT, bindings));
	r->pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo(vk::PipelineLayoutCreateFlags(), 1, &r->setLayout.get()));

	// every kernel, now, for this many samples: none is made while a game runs
	const u32 spec[4] = { this->samples, SSAA_LOG2(this->samples), r->keep, 0 };
	const vk::SpecializationMapEntry entries[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
	const vk::SpecializationInfo specInfo(4, entries, sizeof(spec), spec);
	for (int k = 1; k <= PVRC_KERNELS; k++)
	{
		vk::UniqueShaderModule module = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(),
				compute_spv_size[k], compute_spv[k]));
		const vk::PipelineShaderStageCreateInfo stage(vk::PipelineShaderStageCreateFlags(), vk::ShaderStageFlagBits::eCompute, *module, "main", &specInfo);
		r->pipelines[k] = device.createComputePipelineUnique(context->GetPipelineCache(),
				vk::ComputePipelineCreateInfo(vk::PipelineCreateFlags(), stage, *r->pipelineLayout));
	}

	const vk::DescriptorPoolSize sizes[] = {
		{ vk::DescriptorType::eStorageBuffer, 3u * slots },
		{ vk::DescriptorType::eStorageImage, (u32)slots },
		{ vk::DescriptorType::eCombinedImageSampler, (PVRC_TEX_SLOTS + 2u) * slots },
	};
	r->descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlags(), slots, 3, sizes));
	std::vector<vk::DescriptorSetLayout> layouts(slots, *r->setLayout);
	r->sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*r->descriptorPool, slots, layouts.data()));
	r->setViews.assign(slots, std::vector<VkImageView>(PVRC_TEX_SLOTS, VK_NULL_HANDLE));
	r->setSamplers.assign(slots, std::vector<VkSampler>(PVRC_TEX_SLOTS, VK_NULL_HANDLE));
	r->setUsed.assign(slots, PVRC_TEX_SLOTS);	// (all of them to be written the first time)
	r->imageInfos.reserve(PVRC_TEX_SLOTS + 8);
	r->writes.reserve(PVRC_TEX_SLOTS + 8);
	r->nearest = device.createSamplerUnique(vk::SamplerCreateInfo(vk::SamplerCreateFlags(), vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge));
	std::vector<vk::DescriptorBufferInfo> buffers;
	buffers.reserve(3 * slots);
	std::vector<vk::WriteDescriptorSet> writes;
	for (int s = 0; s < slots; s++)
	{
		buffers.push_back(vk::DescriptorBufferInfo(*r->ring->buffer, (vk::DeviceSize)s * r->ringWords * 4, (vk::DeviceSize)r->ringWords * 4));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_FRAME, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		buffers.push_back(vk::DescriptorBufferInfo(*r->records->buffer, 0, VK_WHOLE_SIZE));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_REC, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
		buffers.push_back(vk::DescriptorBufferInfo(*r->bins->buffer, 0, VK_WHOLE_SIZE));
		writes.push_back(vk::WriteDescriptorSet(r->sets[s], B_BIN, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers.back()));
	}
	device.updateDescriptorSets(writes, nullptr);

	timer.Init(slots, "compute renderer");
	res = std::move(r);
	ready = true;
	NOTICE_LOG(RENDERER, "Vulkan compute renderer: %u x %u, %u sample%s a pixel, tiles of %u, %u translucent fragments a sample at a time; %u KiB of video memory, %u KiB of host memory",
			extent.width, extent.height, this->samples, this->samples > 1 ? "s" : "", 1u << res->tileLog2, res->keep,
			(u32)(deviceBytes >> 10), (u32)(((u64)res->ringWords * 4 * slots) >> 10));
	return true;
}

void ComputeDrawer::Term()
{
	if (res)
	{
		if (statFrames + statFallback != 0)
			NOTICE_LOG(RENDERER, "Vulkan compute renderer: %llu frames drawn, %llu left to the raster renderer; %llu runs walked over their chunk, %llu triangles at no depth left out, at most %u rounds of translucent fragments, %llu primitives passed over by the tiles' depths",
					(unsigned long long)statFrames, (unsigned long long)statFallback, (unsigned long long)res->skipped,
					(unsigned long long)res->dropped, res->peels, (unsigned long long)res->hiz);
	}
	timer.Term();
	res.reset();
	ready = false;
	statFrames = statFallback = 0;
}

bool ComputeDrawer::CanDraw() const
{
	if (!ready || pvrrc.isRTT || pvrrc.isRenderFramebuffer || pvrrc.gpu)
		return false;
	return true;
}

bool ComputeDrawer::Prepare(bool last, const Texture *paletteTexture)
{
	Resources& r = *res;
	const int slot = commandPool->GetIndex();
	// the counts of the frame that had this slot before, done by now
	if (r.statsWritten[slot])
	{
		const u32 *st = r.statsMap + slot * PVRC_STATS;
		r.skipped += st[PVRC_ST_SKIPPED];
		r.dropped += st[PVRC_ST_DROPPED];
		r.hiz += st[PVRC_ST_HIZ];
		r.peels = std::max(r.peels, st[PVRC_ST_PEELS]);
	}

	ComputeFrame::Layout layout;
	layout.width = extent.width;
	layout.height = extent.height;
	layout.tileLog2 = r.tileLog2;
	layout.pool = PoolEntries;
	layout.runs = r.runs;
	layout.chunks = r.chunks;
	layout.lastPicture = last;
	r.slots.Reset();
	u32 *words = r.ringMap + (size_t)slot * r.ringWords;
	r.words = frame.Pack(layout, r.slots, words, r.ringWords, &r.prims);
	if (r.words == 0)
	{
		statFallback++;
		return false;
	}

	/* The set's textures, picture and palette, where they changed */
	VulkanContext *context = VulkanContext::Instance();
	const vk::DescriptorSet set = r.sets[slot];
	r.imageInfos.clear();
	r.writes.clear();
	const VkImageView dummyView = (VkImageView)paletteTexture->GetImageView();
	const VkSampler dummySampler = (VkSampler)*r.nearest;
	auto write = [&](u32 binding, u32 element, vk::DescriptorType type, VkSampler sampler, VkImageView v, vk::ImageLayout l) {
		r.imageInfos.push_back(vk::DescriptorImageInfo(vk::Sampler(sampler), vk::ImageView(v), l));
		r.writes.push_back(vk::WriteDescriptorSet(set, binding, element, 1, type, &r.imageInfos.back()));
	};
	write(B_PAL, 0, vk::DescriptorType::eCombinedImageSampler, dummySampler, dummyView, vk::ImageLayout::eShaderReadOnlyOptimal);
	std::vector<VkImageView>& views = r.setViews[slot];
	std::vector<VkSampler>& samplers = r.setSamplers[slot];
	const u32 wasUsed = r.setUsed[slot];
	for (u32 i = 0; i < std::max(r.slots.used, wasUsed); i++)
	{
		VkImageView v = dummyView;
		VkSampler s = dummySampler;
		if (i < r.slots.used)
		{
			TSP tsp;
			tsp.full = r.slots.slotTsp[i];
			v = (VkImageView)reinterpret_cast<Texture *>(r.slots.slotTex[i])->GetReadOnlyImageView();
			s = (VkSampler)samplerManager->GetSampler(tsp);
		}
		/* (a slot this frame uses is written whatever it had: a view made in
		 * the place of one gone can have its handle) */
		if (i < r.slots.used || views[i] != v || samplers[i] != s)
		{
			write(B_TEX, i, vk::DescriptorType::eCombinedImageSampler, s, v, vk::ImageLayout::eShaderReadOnlyOptimal);
			views[i] = v;
			samplers[i] = s;
		}
	}
	r.setUsed[slot] = r.slots.used;
	r.dummyView = dummyView;
	return true;
}

void ComputeDrawer::Record(vk::Image image, vk::ImageView view, vk::ImageView last)
{
	Resources& r = *res;
	const int slot = commandPool->GetIndex();
	const u32 *words = r.ringMap + (size_t)slot * r.ringWords;
	VulkanContext *context = VulkanContext::Instance();
	const vk::DescriptorSet set = r.sets[slot];
	const VkImageView dummyView = r.dummyView;
	const VkSampler dummySampler = (VkSampler)*r.nearest;
	auto write = [&](u32 binding, u32 element, vk::DescriptorType type, VkSampler sampler, VkImageView v, vk::ImageLayout l) {
		r.imageInfos.push_back(vk::DescriptorImageInfo(vk::Sampler(sampler), vk::ImageView(v), l));
		r.writes.push_back(vk::WriteDescriptorSet(set, binding, element, 1, type, &r.imageInfos.back()));
	};
	// (the pictures and the palette every frame: three words)
	write(B_OUT, 0, vk::DescriptorType::eStorageImage, VK_NULL_HANDLE, (VkImageView)view, vk::ImageLayout::eGeneral);
	write(B_LAST, 0, vk::DescriptorType::eCombinedImageSampler, dummySampler, last ? (VkImageView)last : dummyView, vk::ImageLayout::eShaderReadOnlyOptimal);
	if (!r.writes.empty())
		context->GetDevice().updateDescriptorSets(r.writes, nullptr);

	/* The dispatches */
	vk::CommandBuffer cmd = commandPool->Allocate();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	timer.Begin(cmd, slot);
	const vk::Buffer bins = *r.bins->buffer;
	// (the counts and the counts for the log start at 0; the pool is 0 after the frame before's clear)
	// (after the frame before is done with them)
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
			vk::DependencyFlags(), vk::MemoryBarrier(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferRead,
					vk::AccessFlagBits::eTransferWrite), nullptr, nullptr);
	cmd.fillBuffer(bins, 0, r.binsZero ? (vk::DeviceSize)PVRC_B_POOL(r.runs) * 4 : VK_WHOLE_SIZE, 0);
	r.binsZero = true;
	/* (and what the frame's textures were made with before: the texture
	 * decoder's and the uploads' barriers, and a render to texture's
	 * render pass, make them ready for fragment shaders, not these) */
	const vk::MemoryBarrier toCompute(vk::AccessFlagBits::eMemoryWrite,
			vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
	const vk::ImageMemoryBarrier toGeneral(vk::AccessFlags(), vk::AccessFlagBits::eShaderWrite, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
			vk::PipelineStageFlagBits::eComputeShader, vk::DependencyFlags(), toCompute, nullptr, toGeneral);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *r.pipelineLayout, 0, 1, &set, 0, nullptr);
	auto step = [&](int kernel, u32 x, u32 y) {
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[kernel]);
		if (x != 0 && y != 0)
			cmd.dispatch(x, y, 1);
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect,
				vk::DependencyFlags(), vk::MemoryBarrier(vk::AccessFlagBits::eShaderWrite,
						vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eIndirectCommandRead), nullptr, nullptr);
	};
	const u32 tiles = words[PVRC_F_TILES_X] * words[PVRC_F_TILES_Y];
	const u32 groups = (r.prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP;
	step(PVRC_K_SETUP, groups, 1);
	step(PVRC_K_SCAN, 1, 1);
	step(PVRC_K_SCATTER, groups, 1);
	step(PVRC_K_SORT, tiles, words[PVRC_F_CHUNKS]);
	const u32 fw = PVRC_FINE_W(samples), fh = PVRC_FINE_H(samples);
	step(PVRC_K_WALK, (extent.width + fw - 1) / fw, (extent.height + fh - 1) / fh);
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *r.pipelines[PVRC_K_CLEAR]);
	cmd.dispatchIndirect(bins, (vk::DeviceSize)(PVRC_B_STATS(r.runs) + PVRC_ST_CLEAR_X) * 4);
	// the counts, for when this slot comes round
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(),
			vk::MemoryBarrier(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead), nullptr, nullptr);
	cmd.copyBuffer(bins, *r.stats->buffer, vk::BufferCopy((vk::DeviceSize)PVRC_B_STATS(r.runs) * 4, (vk::DeviceSize)slot * PVRC_STATS * 4, PVRC_STATS * 4));
	r.statsWritten[slot] = true;
	// the picture, for the frontend, as the raster renderer leaves it
	const vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eGeneral,
			vk::ImageLayout::eShaderReadOnlyOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader, vk::DependencyFlags(),
			nullptr, nullptr, toRead);
	timer.End(cmd, slot);
	cmd.end();
	statFrames++;
}
