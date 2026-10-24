/* GPU-side TA processing's decode against the core's own parse, on the
 * frames a game draws. Linked into one program with the core's objects and
 * tools/threads/headless.c as the frontend (ta_gpu_test.sh): the link
 * puts this file's __wrap_ functions in the place of ta_parse_vdrc() and
 * of headless.c's dlopen(), which then opens the program itself.
 *
 * For each frame the renderer parses: the header pass and the decode's
 * tables (ta_gpu_prepare()), the full parse as it always runs, and the
 * decode, on the device and as ta_gpu_test.c's model; the vertices, the
 * modifier volume triangles, the indices, every polygon parameter's index
 * range, the render passes' counts, fZ_max, the draw commands and whether
 * the frame is overrun are then compared with what the parse made, byte
 * for byte. The totals are printed at the end; the first differences as
 * they come. */
#include "types.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/ta_gpu.h"
#include "rend/depth_scale.h"
#include "rend/sorter.h"
#include "rend/shadows.h"
#include "ta_gpu_test.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <vector>

extern "C" void *__real_dlopen(const char *file, int mode);
extern "C" void *__wrap_dlopen(const char *file, int mode)
{
	return __real_dlopen(file != NULL && strcmp(file, "self") == 0 ? NULL : file, mode);
}

extern "C" bool __real__Z13ta_parse_vdrcP10TA_context(TA_context *ctx);

typedef std::chrono::steady_clock clk;

static struct
{
	u32 seen, prepared, compared, same, differ, model_differ, scan_differ;
	u32 no_table, fallback, empty, corner, overrun, overrun_agree;
	u32 drawn, draw_same, draw_differ, draw_left;	// the frames made ready to draw (TA_GPU_DRAW)
	u32 draw_why[TA_GPU_WHY_COUNT];
	u32 edge_frames, edge_trials, edge_agree;	// the overrun check at the exact boundary
	u32 verts, idx, strips;
	std::vector<double> prep_us, parse_us;
	std::vector<double> draw_us;	// the header pass made ready to draw, sort and shadows and all (TA_GPU_DRAW)
	std::vector<double> flat_us;	// ...of it, the flat strips found (TAG_FLAT)
	double gpu_ms;
	// the translucent triangles sorted from the frame's depths (TA_GPU_SORT), against GenSorted()
	u32 sort_frames, sort_same, sort_differ;
	u64 sort_tris, sort_runs;
	/* ...the depth prepass's checks (TAG_K_PREPASS): checks, the prepass's
	 * triangles and those the GPU left out, and their areas in pixels of
	 * the 640 x 480 screen */
	u64 prep_checks, prep_tris, prep_out;
	double prep_area, prep_area_out;
	std::vector<double> sort_us, gensorted_us;
	// the draw groups drawn again for shadows (TA_GPU_SHADOWS), against PolyOverlaps()
	u32 shadow_frames, shadow_same, shadow_differ;
	// the raster renderers' volume triangles with those far out cut (rend/modvol_clip.h), from the blocks against the parse's
	u32 mvclip_frames, mvclip_differ;
	u64 mvclip_tris;
	u64 shadow_groups, shadow_drawn;
} st;

static TaGpuLayout layout;
static u32 *table;
static u32 table_room;
static bool ready;
/* The device is OpenGL (TAG_GL=1, ta_gpu_test_gl.c): its renderers take a flat
 * triangle's colours from its last vertex, so the frames are made without
 * TA_GPU_PROVOKE, and GenSorted() and the GPU sort wind as for OpenGL */
static bool gl_device;
// the frame as the header pass takes it, before the parse
static PolyParam saved_bg;
static FB_X_CLIP_type saved_x_clip;
static FB_Y_CLIP_type saved_y_clip;

static double pct(std::vector<double> &v, double q)
{
	if (v.empty())
		return 0;
	std::vector<double> s = v;
	std::sort(s.begin(), s.end());
	return s[std::min(s.size() - 1, (size_t)(q * s.size()))];
}

static double mean(const std::vector<double> &v)
{
	double t = 0;
	for (double x : v)
		t += x;
	return v.empty() ? 0 : t / v.size();
}

static void report(void)
{
	printf("ta_gpu_test: %u frames, %u decoded on the GPU, %u compared: %u the same, %u different (model against GPU: %u different, scan %u)\n",
			st.seen, st.prepared, st.compared, st.same, st.differ, st.model_differ, st.scan_differ);
	printf("ta_gpu_test: not decoded: %u without a table, %u the header pass left to the full parse; not compared: %u with nothing to draw, "
			"%u where a parameter came inside an unterminated strip, %u overrun (%u with the GPU's overrun flag the same)\n",
			st.no_table, st.fallback, st.empty, st.corner, st.overrun, st.overrun_agree);
	if (st.compared)
		printf("ta_gpu_test: per frame %.0f vertices, %.0f indices, %.0f strips; the header pass and tables mean %.1f us, p99 %.1f us, max %.1f us; "
				"the full parse mean %.1f us, p99 %.1f us, max %.1f us; the device (submit to fence) mean %.2f ms\n",
				(double)st.verts / st.compared, (double)st.idx / st.compared, (double)st.strips / st.compared,
				mean(st.prep_us), pct(st.prep_us, 0.99), pct(st.prep_us, 1.0),
				mean(st.parse_us), pct(st.parse_us, 0.99), pct(st.parse_us, 1.0), st.gpu_ms / st.compared);
	printf("ta_gpu_test: made ready to draw from the GPU: %u frames, %u with the parse's draws, %u different, %u left to the parse "
			"(%u frame skipping, %u that might run out of room)\n",
			st.drawn, st.draw_same, st.draw_differ, st.draw_left, st.draw_why[TA_GPU_WHY_PARSE], st.draw_why[TA_GPU_WHY_ROOM]);
	if (!st.draw_us.empty())
		printf("ta_gpu_test: the header pass made ready to draw%s: mean %.1f us, p99 %.1f us, max %.1f us\n",
				ta_flat_strips() ? ", flat strips looked for" : "", mean(st.draw_us), pct(st.draw_us, 0.99), pct(st.draw_us, 1.0));
	if (!st.flat_us.empty())
		printf("ta_gpu_test: of it, the flat strips found from the depth words: mean %.1f us, p99 %.1f us, max %.1f us\n",
				mean(st.flat_us), pct(st.flat_us, 0.99), pct(st.flat_us, 1.0));
	printf("ta_gpu_test: overrun at the exact room: %u frames, %u trials, the parse and the GPU agree on %u\n",
			st.edge_frames, st.edge_trials, st.edge_agree);
	printf("ta_gpu_test: sorted from the frame's depths: %u frames, %u the same as GenSorted(), %u different; per frame %.0f triangles, %.0f draws; "
			"the sort mean %.1f us, p99 %.1f us, max %.1f us; GenSorted() mean %.1f us, p99 %.1f us, max %.1f us\n",
			st.sort_frames, st.sort_same, st.sort_differ, st.sort_frames ? (double)st.sort_tris / st.sort_frames : 0.0,
			st.sort_frames ? (double)st.sort_runs / st.sort_frames : 0.0,
			mean(st.sort_us), pct(st.sort_us, 0.99), pct(st.sort_us, 1.0), mean(st.gensorted_us), pct(st.gensorted_us, 0.99), pct(st.gensorted_us, 1.0));
	printf("ta_gpu_test: the depth prepass: %llu checks, %llu triangles (%.0f pixels), %llu of them left out by the GPU (%.0f pixels), "
			"per sorted frame %.0f pixels of %.0f\n",
			(unsigned long long)st.prep_checks, (unsigned long long)st.prep_tris, st.prep_area, (unsigned long long)st.prep_out, st.prep_area_out,
			st.sort_frames ? st.prep_area_out / st.sort_frames : 0.0, st.sort_frames ? st.prep_area / st.sort_frames : 0.0);
	printf("ta_gpu_test: shadows the GPU decides: %u frames, %u the same as the parse's, %u different; %llu groups named, %llu drawn again\n",
			st.shadow_frames, st.shadow_same, st.shadow_differ, (unsigned long long)st.shadow_groups, (unsigned long long)st.shadow_drawn);
	printf("ta_gpu_test: volume triangles cut: %u frames with some, %u different from the parse's; %llu triangles drawn for them\n",
			st.mvclip_frames, st.mvclip_differ, (unsigned long long)st.mvclip_tris);
	printf("ta_gpu_test: validation errors: %d\n", tgt_validation_errors());
	printf("ta_gpu_test: %s\n", st.differ == 0 && st.model_differ == 0 && st.scan_differ == 0 && tgt_validation_errors() == 0
			&& st.overrun == st.overrun_agree && st.compared > 0 && st.draw_differ == 0 && st.edge_agree == st.edge_trials
			&& st.sort_differ == 0 && st.shadow_differ == 0 && st.mvclip_differ == 0 ? "PASS" : "FAIL");
	fflush(stdout);
}

static void setup(void)
{
	// (TAG_GL=1: the device is OpenGL, and the frames are made as its renderers draw them: see gl_device)
	gl_device = getenv("TAG_GL") != NULL && atoi(getenv("TAG_GL")) != 0;
	TaGpuSizes sz;
	struct tgt_sizes ts;
	unsigned words[13];

	ta_set_gpu_processing(true);
	ta_gpu_sort_reserve(true);
	// (as the raster renderers have it: the cut volume triangles made by the parse and from the blocks, and compared)
	ta_set_modvol_clip(true);
	// (TAG_FLAT=1: flat strips looked for, as with SSAA; the draw groups' single-rate bits compared too)
	if (getenv("TAG_FLAT") != NULL && atoi(getenv("TAG_FLAT")) != 0)
		ta_set_flat_strips(true);
	ta_gpu_sizes(&sz);
	ts.ta_words = ta_data_size() / 4;
	ts.table_words = sz.table_words;
	ts.verts = sz.verts;
	ts.idx = sz.idx;
	ts.list[0] = sz.list[0];
	ts.list[1] = sz.list[1];
	ts.list[2] = sz.list[2];
	ts.modtrig = sz.modtrig;
	ts.headers = sz.headers;
	tgt_init(getenv("TAG_SPV") ? getenv("TAG_SPV") : ".", getenv("TAG_VALIDATE") && atoi(getenv("TAG_VALIDATE")), &ts, words);
	memcpy(layout.words, words, sizeof(words));
	table_room = sz.table_words;
	table = (u32 *)malloc(table_room * 4);
	if (!tgt_softfloat_test())
		st.differ++;
	atexit(report);
	ready = true;
}

/* The frontend ends with _exit(), which runs no atexit() handler: the
 * totals and the verdict are printed on the way out here instead */
extern "C" void __real__exit(int code);
extern "C" void __wrap__exit(int code)
{
	if (ready)
		report();
	fflush(stdout);
	__real__exit(code);
}

/* What differs between the parse's frame and a decode's: NULL for nothing */
static const char *compare(TA_context *ctx, const TaGpuFrame &gf, const tgt_out &o, char *why, size_t room)
{
	const rend_context &rc = ctx->rend;
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	const List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
	const u32 nv = (u32)rc.verts.used();

	/* the vertices as the Vulkan renderers draw them: the parse's, with the
	 * provoking vertex's colours moved (BaseDrawer::SetProvokingVertices());
	 * as the OpenGL ones do (TAG_GL), the parse's */
	static std::vector<Vertex> drawn;
	drawn.assign(rc.verts.head(), rc.verts.head() + nv);
	for (u32 l = 0; l < (gl_device ? 0u : 3u); l++)
		for (int i = 0; i < lists[l]->used(); i++)
		{
			const PolyParam &pp = lists[l]->head()[i];
			if (!pp.pcw.Gouraud && pp.count > 2)
				for (u32 n = 0; n < pp.count - 2; n++)
				{
					Vertex *vertex = &drawn[rc.idx.head()[pp.first + n]];
					const Vertex *lastVertex = &drawn[rc.idx.head()[pp.first + n + 2]];
					memcpy(vertex->col, lastVertex->col, 4);
					memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
					memcpy(vertex->col1, lastVertex->col1, 4);
					memcpy(vertex->spc1, lastVertex->spc1, 4);
				}
		}
	for (u32 v = 0; v < nv; v++)
		if (memcmp(&drawn[v], &o.vtx[v * TAG_V_WORDS], sizeof(Vertex)) != 0)
		{
			const u32 *c = (const u32 *)&drawn[v];
			for (u32 k = 0; k < TAG_V_WORDS; k++)
				if (c[k] != o.vtx[v * TAG_V_WORDS + k])
				{
					snprintf(why, room, "vertex %u of %u word %u: %08x, %08x", v, nv, k, c[k], o.vtx[v * TAG_V_WORDS + k]);
					return why;
				}
		}
	if ((u32)rc.modtrig.used() != gf.modtrig || memcmp(rc.modtrig.head(), o.mod, gf.modtrig * sizeof(ModTriangle)) != 0)
		return "the modifier volume triangles";
	if ((u32)rc.idx.used() != o.frm[TAG_O_NIDX])
	{
		snprintf(why, room, "indices: %d, %u", rc.idx.used(), o.frm[TAG_O_NIDX]);
		return why;
	}
	for (u32 i = 0; i < (u32)rc.idx.used(); i++)
		if (rc.idx.head()[i] != o.idx[i])
		{
			snprintf(why, room, "index %u of %d: %u, %u", i, rc.idx.used(), rc.idx.head()[i], o.idx[i]);
			return why;
		}
	const u32 last = TAG_O_PASS + (gf.passes - 1) * 3;
	for (u32 l = 0; l < 3; l++)
	{
		const u32 n = (u32)lists[l]->used();
		if (n != o.frm[last + l])
		{
			snprintf(why, room, "%s polygon parameters: %u, %u", names[l], n, o.frm[last + l]);
			return why;
		}
		const u32 base = layout.words[10 + l];
		for (u32 i = 0; i < n; i++)
		{
			const PolyParam &pp = lists[l]->head()[i];
			const u32 *r = &o.rec[base + i * TAG_C_WORDS];
			if (pp.first != r[TAG_C_FIRST] || pp.count != r[TAG_C_COUNT])
			{
				snprintf(why, room, "%s polygon parameter %u: first %u/%u count %u/%u", names[l], i, pp.first, r[TAG_C_FIRST], pp.count, r[TAG_C_COUNT]);
				return why;
			}
		}
	}
	/* the render passes: the header pass's, but for those the parse finds
	 * nothing new in */
	{
		u32 kept = 0, prev[3] = { 0, 0, 0 };
		for (u32 p = 0; p < gf.passes; p++)
		{
			const u32 *c = &o.frm[TAG_O_PASS + p * 3];
			if (p != 0 && c[0] == prev[0] && c[1] == prev[1] && c[2] == prev[2])
				continue;
			if (kept >= (u32)rc.render_passes.used())
				return "more render passes";
			const RenderPass &a = rc.render_passes.head()[kept];
			const RenderPass &b = gf.pass[p];
			if (a.op_count != c[0] || a.pt_count != c[1] || a.tr_count != c[2] || a.autosort != b.autosort
					|| a.z_clear != b.z_clear || a.mv_op_tr_shared != b.mv_op_tr_shared
					|| a.mvo_count != b.mvo_count || a.mvo_tr_count != b.mvo_tr_count)
			{
				snprintf(why, room, "render pass %u: op %u/%u pt %u/%u tr %u/%u", kept, a.op_count, c[0], a.pt_count, c[1], a.tr_count, c[2]);
				return why;
			}
			prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
			kept++;
		}
		if (kept != (u32)rc.render_passes.used())
			return "fewer render passes";
	}
	{
		u32 z;
		memcpy(&z, &rc.fZ_max, 4);
		if (z != o.frm[TAG_O_ZMAX])
		{
			snprintf(why, room, "fZ_max: %08x, %08x", z, o.frm[TAG_O_ZMAX]);
			return why;
		}
		/* its reciprocal, which the vertex shaders take from the frame
		 * (within two units in the last place: a device need not divide
		 * exactly) */
		const float s = pvr_depth_scale(rc.fZ_max);
		memcpy(&z, &s, 4);
		if (z > o.frm[TAG_O_ZSCALE] + 2 || z + 2 < o.frm[TAG_O_ZSCALE])
		{
			snprintf(why, room, "1/fZ_max: %08x, %08x", z, o.frm[TAG_O_ZSCALE]);
			return why;
		}
	}
	if (gf.x_clip.full != rc.fb_X_CLIP.full || gf.y_clip.full != rc.fb_Y_CLIP.full)
		return "the clipping rectangle";
	/* the draw commands: the opaque and punch-through groups that have
	 * indices, by where they start */
	{
		std::vector<std::pair<u32, u32>> a, b;
		for (u32 l = 0; l < 2; l++)
			for (int i = 0; i < lists[l]->used(); i++)
				if (lists[l]->head()[i].count != 0)
					a.push_back(std::make_pair(lists[l]->head()[i].first, lists[l]->head()[i].count));
		for (u32 s = 0; s < gf.slots; s++)
			if (o.cmd[s * TAG_C_WORDS + TAG_C_COUNT] != 0)
				b.push_back(std::make_pair(o.cmd[s * TAG_C_WORDS + TAG_C_FIRST], o.cmd[s * TAG_C_WORDS + TAG_C_COUNT]));
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		if (a != b)
		{
			snprintf(why, room, "draw commands: %u groups, %u", (u32)a.size(), (u32)b.size());
			return why;
		}
	}
	/* the translucent polygon parameters' words, as the per-pixel renderer
	 * makes them (OITDrawer::UploadMainBuffer()); and the translucent draw
	 * records, which group by group are the parse's polygon parameters in
	 * their order, numbered as they are in the list, and nothing more */
	{
		const List<PolyParam> &tr = rc.global_param_tr;
		for (int i = 0; i < tr.used(); i++)
		{
			const PolyParam &pp = tr.head()[i];
			if (o.trp[i * TAG_P_WORDS] != ((pp.tsp.full & 0xffff00c0) | ((pp.isp.full >> 16) & 0xe400) | ((pp.pcw.full >> 7) & 1)
					| (pp.tsp1.full != (u32)-1 || pp.tcw1.full != (u32)-1 ? TAG_P_TWOVOL : 0u))
					|| o.trp[i * TAG_P_WORDS + 1] != pp.tsp1.full)
			{
				snprintf(why, room, "translucent polygon parameter words %d", i);
				return why;
			}
		}
		u32 next = 0;
		for (u32 at = 0; at < gf.trd; )
		{
			const u32 n = o.trd[at * TAG_C_WORDS];
			u32 j;
			for (j = 0; j < n; j++, next++)
			{
				const u32 *r = &o.trd[(at + 1 + j) * TAG_C_WORDS];
				if ((int)next >= tr.used() || r[TAG_C_COUNT] != tr.head()[next].count || r[TAG_C_FIRST] != tr.head()[next].first
						|| r[TAG_C_INST] != 1 || r[3] != 0 || r[TAG_C_INSTANCE] != next)
				{
					snprintf(why, room, "translucent draw record %u (polygon parameter %u)", at + 1 + j, next);
					return why;
				}
			}
			/* the group's room past its count: zeroes, up to the next group,
			 * whose count is the first word that is not (a group has a
			 * strip at least) */
			at += 1 + n;
			while (at < gf.trd && o.trd[at * TAG_C_WORDS] == 0)
			{
				for (j = 0; j < TAG_C_WORDS; j++)
					if (o.trd[at * TAG_C_WORDS + j] != 0)
						return "the translucent draw records' room";
				at++;
			}
		}
		if ((int)next != tr.used())
		{
			snprintf(why, room, "translucent draw records: %u, %d polygon parameters", next, tr.used());
			return why;
		}
	}
	if (o.frm[TAG_O_OVERRUN] != 0)
		return "an overrun the parse did not have";
	return NULL;
}

/* The decode against the model's, word for word where both wrote */
static const char *compare_model(const TaGpuFrame &gf, const tgt_out &g, const tgt_out &m, u32 *table)
{
	// (the model has no indirect dispatches to give arguments for)
	if (memcmp(g.frm, m.frm, TAG_O_ARGS64 * 4) != 0 || memcmp(g.frm + TAG_O_PASS, m.frm + TAG_O_PASS, (TAG_O_WORDS - TAG_O_PASS) * 4) != 0)
		return "the frame words";
	if (memcmp(g.vtx, m.vtx, gf.verts * TAG_V_WORDS * 4) != 0)
		return "vertices";
	if (memcmp(g.mod, m.mod, gf.modtrig * 9 * 4) != 0)
		return "modifier volume triangles";
	if (memcmp(g.idx, m.idx, std::min(g.frm[TAG_O_NIDX], table[TAG_T_ROOM_IDX]) * 4) != 0)
		return "indices";
	if (memcmp(g.cmd, m.cmd, gf.slots * TAG_C_WORDS * 4) != 0)
		return "commands";
	if (memcmp(g.work + table[TAG_T_W_SSTATE], m.work + table[TAG_T_W_SSTATE], g.frm[TAG_O_NSTRIPS] * TAG_ST_WORDS * 4) != 0)
		return "strip states";
	if (memcmp(g.trd, m.trd, gf.trd * TAG_C_WORDS * 4) != 0)
		return "translucent draw records";
	if (memcmp(g.trp, m.trp, std::min(g.frm[TAG_O_PASS + (gf.passes - 1) * 3 + 2], table[TAG_T_ROOM_TR]) * TAG_P_WORDS * 4) != 0)
		return "translucent polygon parameter words";
	return NULL;
}

extern int ta_parse_cnt;

/* The frame as it was before the parse (FillBGP()'s), to parse it again */
static struct
{
	PolyParam bg;
	Vertex bgv[4];
	FB_X_CLIP_type x_clip;
	FB_Y_CLIP_type y_clip;
	bool rtt, fb, clear;
} pre;

static void snap_pre(TA_context *ctx)
{
	const rend_context &rc = ctx->rend;
	pre.bg = *rc.global_param_op.head();
	memcpy(pre.bgv, rc.verts.head(), sizeof(pre.bgv));
	pre.x_clip = rc.fb_X_CLIP;
	pre.y_clip = rc.fb_Y_CLIP;
	pre.rtt = rc.isRTT;
	pre.fb = rc.isRenderFramebuffer;
	pre.clear = rc.clearFramebuffer;
}

// the parse again from the frame as it was, with list @l (0-2, 3 the indices) given room @room
static bool parse_again(TA_context *ctx, int l, int room)
{
	rend_context &rc = ctx->rend;
	List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
	int saved = 0;
	rc.Clear();
	if (l >= 0)
	{
		saved = l < 3 ? lists[l]->size : rc.idx.size;
		if (l < 3)
			lists[l]->size = lists[l]->avail = room;
		else
			rc.idx.size = rc.idx.avail = room;
	}
	*rc.global_param_op.Append() = pre.bg;
	memcpy(rc.verts.Append(4), pre.bgv, sizeof(pre.bgv));
	rc.fb_X_CLIP = pre.x_clip;
	rc.fb_Y_CLIP = pre.y_clip;
	rc.isRTT = pre.rtt;
	rc.isRenderFramebuffer = pre.fb;
	rc.clearFramebuffer = pre.clear;
	const int cnt = ta_parse_cnt;
	const bool rv = __real__Z13ta_parse_vdrcP10TA_context(ctx);
	ta_parse_cnt = cnt;
	const bool overrun = rc.Overrun;
	if (l >= 0)
	{
		if (l < 3)
		{
			lists[l]->Clear();
			lists[l]->size = lists[l]->avail = saved;
		}
		else
		{
			rc.idx.Clear();
			rc.idx.size = rc.idx.avail = saved;
		}
	}
	(void)rv;
	return overrun;
}

/* The overrun check at the exact boundary: each list given the room the
 * frame takes - its strips and the one being filled, the indices - and one
 * less, and the parse's verdict against the GPU's with the same room in
 * its tables. The frame is parsed again as it was after. */
static void edge_check(TA_context *ctx, const TaGpuFrame &gf, const tgt_out &o)
{
	static std::vector<u32> t;
	const u32 last = TAG_O_PASS + (gf.passes - 1) * 3;
	const u32 need[4] = { o.frm[last] + 1, o.frm[last + 1] + 1, o.frm[last + 2] + 1, o.frm[TAG_O_NIDX] };
	static const u32 room_word[4] = { TAG_T_ROOM_OP, TAG_T_ROOM_PT, TAG_T_ROOM_TR, TAG_T_ROOM_IDX };
	static const u32 bit[4] = { 2, 4, 8, 1 };
	const u32 bytes = (u32)(ctx->tad.End() - ctx->tad.thd_root);
	t.assign(table, table + gf.table_words);
	st.edge_frames++;
	for (int l = 0; l < 4; l++)
	{
		/* (a list with no polygon of its own - the opaque one has the
		 * background - never has one being filled: the parse's room is
		 * then its strips, the GPU's one more, and a list has room for
		 * thousands) */
		if (need[l] <= (l == 0 ? 2u : 1u))
			continue;
		for (u32 room = need[l] - 1; room <= need[l]; room++)
		{
			const bool cpu = parse_again(ctx, l, (int)room);
			t[room_word[l]] = room;
			tgt_out g;
			double ms;
			tgt_gpu(ctx->tad.thd_root, bytes, t.data(), gf.table_words, gf.trd, &g, &ms);
			const bool gpu = (g.frm[TAG_O_OVERRUN] & bit[l]) != 0;
			st.edge_trials++;
			if (cpu == gpu && cpu == (room < need[l]))
				st.edge_agree++;
			else if (st.edge_trials - st.edge_agree < 10)
				printf("ta_gpu_test: frame %u: list %d with room for %u of %u: the parse %s, the GPU %s\n", st.seen, l, room, need[l],
						cpu ? "runs out" : "does not", gpu ? "runs out" : "does not");
		}
		t[room_word[l]] = table[room_word[l]];
	}
	parse_again(ctx, -1, 0);
}

/* Whether two polygon parameters are drawn the same way, texture and all */
static bool same_draw_state(const PolyParam &a, const PolyParam &b)
{
	return a.tsp.full == b.tsp.full && a.tcw.full == b.tcw.full && ((a.pcw.full ^ b.pcw.full) & ~0x100u) == 0
			&& a.isp.full == b.isp.full && a.tsp1.full == b.tsp1.full && a.tcw1.full == b.tcw1.full
			&& a.tileclip == b.tileclip && a.texid == b.texid && a.texid1 == b.texid1;
}

// ...and at the same rate
static bool same_draw_rate(const PolyParam &a, const PolyParam &b)
{
	return same_draw_state(a, b) && ((a.pcw.full ^ b.pcw.full) & 0x100u) == 0;
}

/* GenSorted() of the parse's frame, each sorted render pass, the draws'
 * first counting from the first pass's first index, as the Vulkan
 * per-triangle renderer has them (Drawer::SortTriangles()); the polygon
 * parameters' words kept, which the header pass's lists then replace */
struct SortRef
{
	std::vector<SortTrigDrawParam> runs;
	std::vector<PolyParam> pp;
	std::vector<u32> idx;
	/* each sorted pass's depth prepass (GenPrepass()), as the header pass
	 * lays it out: every pass's sorted indices first, then every pass's
	 * prepass indices, the prepass draws' first counting from the start */
	std::vector<SortTrigDrawParam> pruns;
	std::vector<PolyParam> ppp;
	std::vector<u32> pidx;
	std::vector<u32> prun_pass;
};

static void gensorted_ref(TA_context *ctx, SortRef &ref, double *us)
{
	static std::vector<SortTrigDrawParam> polys;
	static std::vector<u32> idx;
	TA_context *const saved = _pvrrc;
	const rend_context &rc = ctx->rend;
	RenderPass prev = {};
	_pvrrc = ctx;
	ref.runs.clear();
	ref.pp.clear();
	ref.idx.clear();
	ref.pruns.clear();
	ref.ppp.clear();
	ref.pidx.clear();
	ref.prun_pass.clear();
	*us = 0;
	/* (GenPrepass() leaves out a polygon none of whose vertices has an
	 * alpha of 255 where its alpha is theirs: PrepassVerticesOpaque(). The
	 * header pass has not got the vertices; the GPU leaves such a strip
	 * out, TAG_K_PREPASS) */
	for (int p = 0; p < rc.render_passes.used(); p++)
	{
		const RenderPass &cur = rc.render_passes.head()[p];
		if (cur.autosort)
		{
			idx.clear();
			const clk::time_point t0 = clk::now();
			GenSorted(prev.tr_count, cur.tr_count - prev.tr_count, polys, idx, !gl_device);
			*us += std::chrono::duration<double, std::micro>(clk::now() - t0).count();
			for (const SortTrigDrawParam &r : polys)
			{
				SortTrigDrawParam q = r;
				q.first += (u32)ref.idx.size();
				ref.runs.push_back(q);
				ref.pp.push_back(*r.ppid);
			}
			ref.idx.insert(ref.idx.end(), idx.begin(), idx.end());
			/* (the prepass's, kept apart: GenPrepass() adds to idx after the
			 * sorted indices, its draws' first counting from idx's start) */
			const size_t sorted = idx.size();
			polys.clear();
			GenPrepass(prev.tr_count, cur.tr_count - prev.tr_count,
					p == rc.render_passes.used() - 1, polys, idx, !gl_device);
			for (const SortTrigDrawParam &r : polys)
			{
				SortTrigDrawParam q = r;
				q.first = q.first - (u32)sorted + (u32)ref.pidx.size();
				ref.pruns.push_back(q);
				ref.ppp.push_back(*r.ppid);
				ref.prun_pass.push_back((u32)p);
			}
			ref.pidx.insert(ref.pidx.end(), idx.begin() + sorted, idx.end());
		}
		prev = cur;
	}
	/* (the prepass's indices go after all the sorted ones) */
	for (SortTrigDrawParam &q : ref.pruns)
		q.first += (u32)ref.idx.size();
	ref.idx.insert(ref.idx.end(), ref.pidx.begin(), ref.pidx.end());
	_pvrrc = saved;
}

/* A prepass draw's triangles, each as its three indices (provoking vertex
 * and winding as they are), in a canonical order: a depth-only draw that
 * passes on what is nearer writes the same depth whatever the order of its
 * triangles, and so does a prepass whatever the order of its draws */
static void prepass_tris(const std::vector<u32> &idx, u32 first, u32 count, std::vector<std::pair<u64, u32>> &tris)
{
	tris.clear();
	for (u32 i = first; i + 3 <= first + count && i + 3 <= idx.size(); i += 3)
		// (one the GPU left out, TAG_K_PREPASS, is a point: it draws nothing)
		if (idx[i] != idx[i + 1] || idx[i] != idx[i + 2])
			tris.push_back(std::make_pair(((u64)idx[i] << 32) | idx[i + 1], idx[i + 2]));
	std::sort(tris.begin(), tris.end());
}

// a triangle's area in pixels of the 640 x 480 screen, from the device's vertices
static double tri_area(const unsigned *vtx, u32 a, u32 b, u32 c)
{
	// (what is off the screen does not count)
	auto f = [vtx](u32 v, u32 w) {
		float x;
		memcpy(&x, &vtx[v * TAG_V_WORDS + w], 4);
		const double most = w == TAG_V_X ? 640.0 : 480.0;
		return std::isfinite(x) ? std::min(std::max((double)x, 0.0), most) : 0.0;
	};
	const double ax = f(a, TAG_V_X), ay = f(a, TAG_V_Y);
	const double s = (f(b, TAG_V_X) - ax) * (f(c, TAG_V_Y) - ay) - (f(c, TAG_V_X) - ax) * (f(b, TAG_V_Y) - ay);
	return std::isfinite(s) ? std::fabs(s) / 2 : 0.0;
}

/* The header pass's sort (TA_GPU_SORT) against GenSorted()'s: the same
 * indices, and the same draws of them, drawn the same way; then each
 * pass's depth prepass against GenPrepass()'s: the same draws, drawn the
 * same way with the same triangles, in any order (GenPrepass() orders its
 * groups by texture first, which the header pass has not got) */
static const char *compare_sorted(TA_context *ctx, const SortRef &ref, const TaGpuFrame &gf, char *why, size_t room)
{
	static std::vector<u32> idx, cpu, model;
	idx.assign(ta_gpu_sorted_words(), 0);
	if (!idx.empty())
		ta_gpu_sorted_write(idx.data());
	/* the prepass's checks (TAG_K_PREPASS), on the device and by the model,
	 * the same; the device's indices from here on */
	const u32 checks = ta_gpu_prepass_checks();
	if (checks != 0)
	{
		if (table[TAG_T_PREP] != ta_gpu_sorted_indices())
		{
			snprintf(why, room, "the prepass's checks at %u, the sorted indices end at %u", table[TAG_T_PREP], ta_gpu_sorted_indices());
			return why;
		}
		cpu = idx;
		model = idx;
		tgt_out g, m;
		double ms;
		tgt_sorted(model.data(), (unsigned)model.size(), checks);
		tgt_model(ctx->tad.thd_root, table, gf.trd, &m);
		tgt_sorted(idx.data(), (unsigned)idx.size(), checks);
		tgt_gpu(ctx->tad.thd_root, (u32)(ctx->tad.End() - ctx->tad.thd_root), table, gf.table_words, gf.trd, &g, &ms);
		if (idx != model)
		{
			snprintf(why, room, "the prepass's checks: the GPU and the model differ");
			return why;
		}
		st.prep_checks += checks;
		// (the prepass's triangles: its draws')
		for (u32 p = 0; p < gf.passes; p++)
		{
			u32 n;
			const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
			for (u32 j = 0; j < n; j++)
				for (u32 i = r[j].first; i + 3 <= r[j].first + r[j].count && i + 3 <= ta_gpu_sorted_indices(); i += 3)
				{
					const double a = tri_area(g.vtx, cpu[i], cpu[i + 1], cpu[i + 2]);
					st.prep_tris++;
					st.prep_area += a;
					if (idx[i] == idx[i + 1] && idx[i] == idx[i + 2])
					{
						st.prep_out++;
						st.prep_area_out += a;
					}
				}
		}
	}
	idx.resize(ta_gpu_sorted_indices());
	// (the sorted ones; the prepass's, which the GPU makes fewer of, are compared by their draws below)
	size_t gpu_pre = 0;
	for (u32 p = 0; p < gf.passes; p++)
	{
		u32 n;
		const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
		for (u32 j = 0; j < n; j++)
			gpu_pre += r[j].count;
	}
	const size_t sorted = ref.idx.size() - ref.pidx.size();
	if (gpu_pre > idx.size() || idx.size() - gpu_pre != sorted)
	{
		snprintf(why, room, "sorted indices: %zu, GenSorted() %zu", idx.size() - std::min(gpu_pre, idx.size()), sorted);
		return why;
	}
	for (size_t i = 0; i < sorted; i++)
		if (idx[i] != ref.idx[i])
		{
			snprintf(why, room, "sorted index %zu of %zu: %u, GenSorted() %u", i, sorted, idx[i], ref.idx[i]);
			return why;
		}
	size_t k = 0;
	for (u32 p = 0; p < gf.passes; p++)
	{
		u32 n;
		const SortTrigDrawParam *r = ta_gpu_sorted(p, &n);
		for (u32 j = 0; j < n; j++, k++)
			if (k >= ref.runs.size() || r[j].first != ref.runs[k].first || r[j].count != ref.runs[k].count
					|| !same_draw_state(*r[j].ppid, ref.pp[k]))
			{
				snprintf(why, room, "sorted draw %zu (pass %u): first %u count %u, GenSorted() %s", k, p, r[j].first, r[j].count,
						k >= ref.runs.size() ? "has fewer" : "differs");
				return why;
			}
	}
	if (k != ref.runs.size())
	{
		snprintf(why, room, "sorted draws: %zu, GenSorted() %zu", k, ref.runs.size());
		return why;
	}
	/* the depth prepass: its draws cover the indices after the sorted ones,
	 * each once, and each pass's draws are GenPrepass()'s */
	static std::vector<std::pair<u32, u32>> span;
	static std::vector<std::pair<u64, u32>> a, b;
	static std::vector<bool> used;
	span.clear();
	used.assign(ref.pruns.size(), false);
	size_t gpu_draws = 0;
	for (u32 p = 0; p < gf.passes; p++)
	{
		u32 n;
		const SortTrigDrawParam *r = ta_gpu_prepass(p, &n);
		gpu_draws += n;
		for (u32 j = 0; j < n; j++)
		{
			if (r[j].first < sorted || r[j].count == 0 || r[j].count % 3 != 0 || r[j].first + r[j].count > idx.size())
			{
				snprintf(why, room, "prepass draw %u of pass %u: first %u count %u, outside its indices", j, p, r[j].first, r[j].count);
				return why;
			}
			span.push_back(std::make_pair(r[j].first, r[j].count));
			prepass_tris(idx, r[j].first, r[j].count, a);
			// (a draw all of whose triangles the GPU left out is one GenPrepass() has not got)
			if (a.empty())
			{
				gpu_draws--;
				continue;
			}
			bool found = false;
			for (size_t q = 0; q < ref.pruns.size() && !found; q++)
			{
				if (used[q] || ref.prun_pass[q] != p || ref.pruns[q].count != a.size() * 3 || !same_draw_state(*r[j].ppid, ref.ppp[q]))
					continue;
				prepass_tris(ref.idx, ref.pruns[q].first, ref.pruns[q].count, b);
				if (a == b)
					used[q] = found = true;
			}
			if (!found)
			{
				snprintf(why, room, "prepass draw %u of pass %u (first %u count %u): not one of GenPrepass()'s", j, p, r[j].first, r[j].count);
				return why;
			}
		}
	}
	if (gpu_draws != ref.pruns.size())
	{
		snprintf(why, room, "prepass draws: %zu, GenPrepass() %zu", gpu_draws, ref.pruns.size());
		return why;
	}
	std::sort(span.begin(), span.end());
	u32 at = (u32)sorted;
	for (const std::pair<u32, u32> &s : span)
	{
		if (s.first != at)
		{
			snprintf(why, room, "prepass draws: index %u drawn %s", at, s.first < at ? "twice" : "by none");
			return why;
		}
		at += s.second;
	}
	if (at != idx.size())
	{
		snprintf(why, room, "prepass draws: indices %u to %zu drawn by none", at, idx.size());
		return why;
	}
	return NULL;
}

/* The draw groups drawn again for shadows (TA_GPU_SHADOWS): each render
 * pass's volumes' bounds against ModVolBounds() of the parse's triangles,
 * and the groups whose second commands the GPU (and the model) gives
 * indices against those the CPU draws again (CanDrawShadowed() and
 * PolyOverlaps() of the parse's groups), with the header pass's frame in
 * the context and the parse's vertices, indices and triangles */
static const char *compare_shadows(TA_context *ctx, const TaGpuFrame &gf, const std::vector<PolyParam> *parse, char *why, size_t room)
{
	rend_context &rc = ctx->rend;
	TA_context *const saved = _pvrrc;
	const char *d = NULL;
	tgt_out g, m;
	double ms;
	const u32 bytes = (u32)(ctx->tad.End() - ctx->tad.thd_root);
	tgt_model(ctx->tad.thd_root, table, gf.trd, &m);
	tgt_gpu(ctx->tad.thd_root, bytes, table, gf.table_words, gf.trd, &g, &ms);
	const u32 base = table[TAG_T_OFF_SHD], n = table[TAG_T_NSHD], sc = table[TAG_T_SHD_CMD];
	std::vector<std::pair<u32, u32>> gpu, cpu;
	for (u32 e = 0; e < n && d == NULL; e++)
	{
		const u32 at = (sc + TAG_SH_SLOT(table[base + TAG_SH_ENTRY + e])) * TAG_C_WORDS;
		if (memcmp(&g.cmd[at], &m.cmd[at], TAG_C_WORDS * 4) != 0)
		{
			snprintf(why, room, "shadow entry %u: the GPU and the model differ", e);
			d = why;
		}
		else if (g.cmd[at + TAG_C_COUNT] > 2)
			gpu.push_back(std::make_pair(g.cmd[at + TAG_C_FIRST], g.cmd[at + TAG_C_COUNT]));
	}
	st.shadow_groups += n;
	st.shadow_drawn += gpu.size();
	_pvrrc = ctx;
	RenderPass prev = {};
	List<PolyParam> *lists[2] = { &rc.global_param_op, &rc.global_param_pt };
	for (int p = 0; p < rc.render_passes.used() && d == NULL; p++)
	{
		const RenderPass &cur = rc.render_passes.head()[p];
		const ScreenBounds area = ModVolBounds(prev.mvo_count, cur.mvo_count - prev.mvo_count);
		if (memcmp(&area, rc.gpu_shadow_area[p], sizeof(area)) != 0 || memcmp(&area, &table[base + TAG_SH_AREA + p * 4], sizeof(area)) != 0)
		{
			snprintf(why, room, "render pass %d: the volumes' bounds", p);
			d = why;
			break;
		}
		const u32 from[2] = { prev.op_count, prev.pt_count }, to[2] = { cur.op_count, cur.pt_count };
		for (u32 l = 0; l < 2 && !area.empty(); l++)
			for (u32 i = from[l]; i < to[l]; i++)
			{
				// the parse's group of the header pass's (by its command), and what the CPU does with it
				const PolyParam &gp = lists[l]->head()[i];
				const u32 *c = &g.cmd[gp.first * TAG_C_WORDS];
				if (c[TAG_C_COUNT] == 0)
					continue;
				const PolyParam *pp = NULL;
				for (const PolyParam &q : parse[l])
					if (q.count == c[TAG_C_COUNT] && q.first == c[TAG_C_FIRST])
					{
						pp = &q;
						break;
					}
				if (pp == NULL)
				{
					snprintf(why, room, "render pass %d: no group of the parse's at %u", p, c[TAG_C_FIRST]);
					d = why;
					break;
				}
				/* (a group of two vertices, which the header pass's count is,
				 * makes only triangles of no area, whatever its indices: the
				 * renderer leaves it) */
				if (CanDrawShadowed(l == 0 ? ListType_Opaque : ListType_Punch_Through, pp) && gp.count > 2 && PolyOverlaps(pp, area))
					cpu.push_back(std::make_pair(pp->first, pp->count));
			}
		prev = cur;
	}
	_pvrrc = saved;
	std::sort(gpu.begin(), gpu.end());
	std::sort(cpu.begin(), cpu.end());
	if (d == NULL && gpu != cpu)
	{
		snprintf(why, room, "the groups drawn again for shadows: %zu, the parse's %zu", gpu.size(), cpu.size());
		d = why;
	}
	return d;
}

/* The frame made ready to draw from the GPU, as the Vulkan renderers have
 * it (TA_GPU_DRAW), against the parse's: each opaque and punch-through
 * draw group's command, in the lists' order, and each translucent one's
 * strips, are the parse's polygon parameters that have indices, in order,
 * drawn the same way; the render passes count the groups, the modifier
 * volumes and the clipping rectangle are the same. The parse's lists are
 * put back after. NULL for nothing different, "" for a frame left to the
 * parse. */
static const char *compare_draw(TA_context *ctx, const tgt_out &o, u32 flags, char *why, size_t room)
{
	rend_context &rc = ctx->rend;
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
	static std::vector<PolyParam> parse[3];
	static std::vector<RenderPass> passes;
	static std::vector<ModifierVolumeParam> mvo[2];
	static std::vector<ModTriangle> mvclip;
	static std::vector<u32> mvclip_range;
	const rend_context saved = rc;
	mvclip.assign(rc.mv_clip.head(), rc.mv_clip.head() + rc.mv_clip.used());
	mvclip_range.assign(rc.mv_clip_range.head(), rc.mv_clip_range.head() + rc.mv_clip_range.used());
	for (u32 l = 0; l < 3; l++)
		parse[l].assign(lists[l]->head(), lists[l]->head() + lists[l]->used());
	passes.assign(rc.render_passes.head(), rc.render_passes.head() + rc.render_passes.used());
	mvo[0].assign(rc.global_param_mvo.head(), rc.global_param_mvo.head() + rc.global_param_mvo.used());
	mvo[1].assign(rc.global_param_mvo_tr.head(), rc.global_param_mvo_tr.head() + rc.global_param_mvo_tr.used());
	// (the background as the frame had it before the parse, which the header pass takes)
	*rc.global_param_op.head() = saved_bg;
	rc.fb_X_CLIP = saved_x_clip;
	rc.fb_Y_CLIP = saved_y_clip;

	static SortRef ref;
	double gensorted_us = 0;
	if (flags & TA_GPU_SORT)
		gensorted_ref(ctx, ref, &gensorted_us);

	TaGpuFrame gf;
	const int cnt = ta_parse_cnt;
	const clk::time_point t0 = clk::now();
	const bool ok = ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags | TA_GPU_DRAW);
	if (ok)
	{
		st.draw_us.push_back(std::chrono::duration<double, std::micro>(clk::now() - t0).count());
		if (ta_flat_strips())
			st.flat_us.push_back(ta_gpu_flat_ns() / 1e3);
	}
	const char *d = NULL;
	if (!ok)
	{
		d = "";
		st.draw_why[gf.why < TA_GPU_WHY_COUNT ? gf.why : 0]++;
	}
	else
	{
		ta_parse_cnt = cnt;
		/* with flat strips looked for, the single-rate bit is the same too:
		 * a group's, and each translucent strip's */
		bool (*same_state)(const PolyParam &, const PolyParam &) = ta_flat_strips() ? same_draw_rate : same_draw_state;
		for (u32 l = 0; l < 3 && d == NULL; l++)
		{
			size_t next = 0;
			for (int i = 0; i < lists[l]->used() && d == NULL; i++)
			{
				const PolyParam &g = lists[l]->head()[i];
				if (l < 2)
				{
					const u32 *c = &o.cmd[g.first * TAG_C_WORDS];
					// (the parse's group, its head and the ones joined to it, which have no indices)
					while (next < parse[l].size() && parse[l][next].count == 0)
						next++;
					if (c[TAG_C_COUNT] == 0)
						continue;
					if (next >= parse[l].size() || parse[l][next].first != c[TAG_C_FIRST] || parse[l][next].count != c[TAG_C_COUNT]
							|| !same_state(parse[l][next], g))
					{
						snprintf(why, room, "%s draw group %d", names[l], i);
						d = why;
					}
					next++;
				}
				else
				{
					const u32 n = o.trd[g.first * TAG_C_WORDS];
					if (n > g.count)
					{
						snprintf(why, room, "translucent draw group %d: %u strips, room for %u", i, n, g.count);
						d = why;
					}
					for (u32 j = 0; j < n && d == NULL; j++, next++)
						if (next >= parse[l].size() || !same_state(parse[l][next], g)
								|| o.trd[(g.first + 1 + j) * TAG_C_WORDS + TAG_C_FIRST] != parse[l][next].first)
						{
							snprintf(why, room, "translucent draw group %d strip %u", i, j);
							d = why;
						}
				}
			}
			if (d == NULL && l < 2)
				while (next < parse[l].size() && parse[l][next].count == 0)
					next++;
			if (d == NULL && next != parse[l].size())
			{
				snprintf(why, room, "%s draw groups: %zu of %zu polygon parameters", names[l], next, parse[l].size());
				d = why;
			}
		}
		if (d == NULL && (rc.render_passes.used() != (int)passes.size()))
			d = "the render passes";
		for (size_t p = 0; d == NULL && p < passes.size(); p++)
		{
			const RenderPass &a = passes[p], &b = rc.render_passes.head()[p];
			if (a.autosort != b.autosort || a.z_clear != b.z_clear || a.mv_op_tr_shared != b.mv_op_tr_shared
					|| a.mvo_count != b.mvo_count || a.mvo_tr_count != b.mvo_tr_count
					|| (a.op_count == (p ? passes[p - 1].op_count : 0)) != (b.op_count == (p ? rc.render_passes.head()[p - 1].op_count : 0))
					|| (a.tr_count == (p ? passes[p - 1].tr_count : 0)) != (b.tr_count == (p ? rc.render_passes.head()[p - 1].tr_count : 0)))
			{
				snprintf(why, room, "render pass %zu", p);
				d = why;
			}
		}
		if (d == NULL && (rc.global_param_mvo.used() != (int)mvo[0].size() || rc.global_param_mvo_tr.used() != (int)mvo[1].size()
				|| memcmp(rc.global_param_mvo.head(), mvo[0].data(), mvo[0].size() * sizeof(ModifierVolumeParam)) != 0
				|| memcmp(rc.global_param_mvo_tr.head(), mvo[1].data(), mvo[1].size() * sizeof(ModifierVolumeParam)) != 0))
			d = "the modifier volumes";
		if (d == NULL && (rc.fb_X_CLIP.full != saved.fb_X_CLIP.full || rc.fb_Y_CLIP.full != saved.fb_Y_CLIP.full))
			d = "the clipping rectangle";
		if (d == NULL && (!rc.gpu || rc.gpu_modtrig != (u32)saved.modtrig.used()))
			d = "the modifier volume triangles";
		if (d == NULL && !mvclip.empty())
			st.mvclip_frames++, st.mvclip_tris += mvclip.size();
		if (d == NULL && (rc.mv_clip.used() != (int)mvclip.size() || rc.mv_clip_range.used() != (int)mvclip_range.size()
				|| memcmp(rc.mv_clip.head(), mvclip.data(), mvclip.size() * sizeof(ModTriangle)) != 0
				|| memcmp(rc.mv_clip_range.head(), mvclip_range.data(), mvclip_range.size() * sizeof(u32)) != 0))
		{
			st.mvclip_differ++;
			snprintf(why, room, "the cut volume triangles: %d and %d ranges from the blocks, %zu and %zu from the parse",
					rc.mv_clip.used(), rc.mv_clip_range.used(), mvclip.size(), mvclip_range.size());
			d = why;
		}
		if (d == NULL && (flags & TA_GPU_SORT) && !ref.idx.empty())
		{
			st.sort_frames++;
			st.sort_tris += ref.idx.size() / 3;
			st.sort_runs += ref.runs.size();
			st.sort_us.push_back(ta_gpu_sorted_ns() / 1e3);
			st.gensorted_us.push_back(gensorted_us);
			d = compare_sorted(ctx, ref, gf, why, room);
			if (d == NULL)
				st.sort_same++;
			else
				st.sort_differ++;
		}
		else if (d == NULL && (flags & TA_GPU_SORT) && ta_gpu_sorted_indices() != 0)
		{
			st.sort_differ++;
			d = "sorted triangles where GenSorted() has none";
		}
		if (d == NULL && gf.shadows != 0)
		{
			st.shadow_frames++;
			d = compare_shadows(ctx, gf, parse, why, room);
			if (d == NULL)
				st.shadow_same++;
			else
				st.shadow_differ++;
		}
	}
	// the parse's frame, back
	for (u32 l = 0; l < 3; l++)
	{
		lists[l]->Clear();
		if (!parse[l].empty())
			memcpy(lists[l]->Append((int)parse[l].size()), parse[l].data(), parse[l].size() * sizeof(PolyParam));
	}
	rc.render_passes.Clear();
	if (!passes.empty())
		memcpy(rc.render_passes.Append((int)passes.size()), passes.data(), passes.size() * sizeof(RenderPass));
	rc.global_param_mvo.Clear();
	if (!mvo[0].empty())
		memcpy(rc.global_param_mvo.Append((int)mvo[0].size()), mvo[0].data(), mvo[0].size() * sizeof(ModifierVolumeParam));
	rc.global_param_mvo_tr.Clear();
	if (!mvo[1].empty())
		memcpy(rc.global_param_mvo_tr.Append((int)mvo[1].size()), mvo[1].data(), mvo[1].size() * sizeof(ModifierVolumeParam));
	rc.fb_X_CLIP = saved.fb_X_CLIP;
	rc.fb_Y_CLIP = saved.fb_Y_CLIP;
	rc.mv_clip.Clear();
	if (!mvclip.empty())
		memcpy(rc.mv_clip.Append((int)mvclip.size()), mvclip.data(), mvclip.size() * sizeof(ModTriangle));
	rc.mv_clip_range.Clear();
	if (!mvclip_range.empty())
		memcpy(rc.mv_clip_range.Append((int)mvclip_range.size()), mvclip_range.data(), mvclip_range.size() * sizeof(u32));
	rc.gpu = false;
	rc.gpu_modtrig = 0;
	return d;
}

extern "C" bool __wrap__Z13ta_parse_vdrcP10TA_context(TA_context *ctx)
{
	if (!ready)
		setup();
	st.seen++;
	TaGpuFrame gf;
	const bool table_ok = ctx->walk_count != TA_WALK_INVALID || ctx->tad.End() == ctx->tad.thd_root;
	/* (once untimed: the textures the frame asks for are converted, and the
	 * pass timed below finds them made, as the parse after it does) */
	const u32 flags = (gl_device ? 0 : TA_GPU_PROVOKE) | TA_GPU_INSTANCE;
	snap_pre(ctx);
	saved_bg = *ctx->rend.global_param_op.head();
	saved_x_clip = ctx->rend.fb_X_CLIP;
	saved_y_clip = ctx->rend.fb_Y_CLIP;
	ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags);
	clk::time_point t0 = clk::now();
	const bool prepared = ta_gpu_prepare(ctx, &layout, table, table_room, &gf, flags);
	clk::time_point t1 = clk::now();
	if (prepared)
		memset(ctx->rend.verts.head() + 4, 0, (gf.verts - 4) * sizeof(Vertex));
	clk::time_point t2 = clk::now();
	const bool rv = __real__Z13ta_parse_vdrcP10TA_context(ctx);
	clk::time_point t3 = clk::now();

	if (!prepared)
	{
		if (!table_ok)
			st.no_table++;
		else
			st.fallback++;
		return rv;
	}
	st.prepared++;
	if (!rv && !ctx->rend.Overrun)
	{
		st.empty++;
		return rv;
	}
	tgt_out g, m;
	double ms;
	const u32 bytes = (u32)(ctx->tad.End() - ctx->tad.thd_root);
	const int scan_ok = tgt_model(ctx->tad.thd_root, table, gf.trd, &m);
	tgt_gpu(ctx->tad.thd_root, bytes, table, gf.table_words, gf.trd, &g, &ms);
	char why[256];
	const char *d = compare_model(gf, g, m, table);
	if (d != NULL && st.model_differ++ < 10)
		printf("ta_gpu_test: frame %u: the GPU differs from the model: %s\n", st.seen, d);
	if (!scan_ok && st.scan_differ++ < 10)
		printf("ta_gpu_test: frame %u: the model's scan differs from its states\n", st.seen);

	if (ctx->rend.Overrun)
	{
		st.overrun++;
		if (g.frm[TAG_O_OVERRUN] != 0)
			st.overrun_agree++;
		return rv;
	}
	if ((u32)ctx->rend.verts.used() != gf.verts)
	{
		if (st.corner++ < 5)
			printf("ta_gpu_test: frame %u: the parse made %d vertices, the walk %u\n", st.seen, ctx->rend.verts.used(), gf.verts);
		return rv;
	}
	st.compared++;
	st.verts += gf.verts;
	st.idx += g.frm[TAG_O_NIDX];
	st.strips += g.frm[TAG_O_NSTRIPS];
	st.prep_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
	st.parse_us.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
	st.gpu_ms += ms;
	d = compare(ctx, gf, g, why, sizeof(why));
	if (d == NULL)
		st.same++;
	else if (st.differ++ < 20)
		printf("ta_gpu_test: frame %u differs from the parse: %s\n", st.seen, d);
	/* and as the renderers draw it: its draw groups, by the device's
	 * commands and records (the frame's tables made again, the same) */
	d = compare_draw(ctx, g, flags | TA_GPU_SORT | TA_GPU_SHADOWS, why, sizeof(why));
	if (d != NULL && *d == 0)
		st.draw_left++;
	else
	{
		st.drawn++;
		if (d == NULL)
			st.draw_same++;
		else if (st.draw_differ++ < 20)
			printf("ta_gpu_test: frame %u made ready to draw differs from the parse: %s\n", st.seen, d);
	}
	// (every so often: it parses and decodes the frame nine times more)
	if (st.compared % 50 == 1)
		edge_check(ctx, gf, g);
	if (st.compared % 300 == 0)
		report();
	return rv;
}
