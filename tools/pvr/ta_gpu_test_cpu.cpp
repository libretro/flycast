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

static struct
{
	u32 seen, prepared, compared, same, differ, model_differ, scan_differ;
	u32 no_table, fallback, empty, corner, overrun, overrun_agree;
	u32 verts, idx, strips;
	std::vector<double> prep_us, parse_us;
	double gpu_ms;
} st;

static TaGpuLayout layout;
static u32 *table;
static u32 table_room;
static bool ready;

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
	printf("ta_gpu_test: validation errors: %d\n", tgt_validation_errors());
	printf("ta_gpu_test: %s\n", st.differ == 0 && st.model_differ == 0 && st.scan_differ == 0 && tgt_validation_errors() == 0
			&& st.overrun == st.overrun_agree && st.compared > 0 ? "PASS" : "FAIL");
	fflush(stdout);
}

static void setup(void)
{
	TaGpuSizes sz;
	struct tgt_sizes ts;
	unsigned words[13];

	ta_set_gpu_processing(true);
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

/* What differs between the parse's frame and a decode's: NULL for nothing */
static const char *compare(TA_context *ctx, const TaGpuFrame &gf, const tgt_out &o, char *why, size_t room)
{
	const rend_context &rc = ctx->rend;
	static const char *const names[3] = { "opaque", "punch-through", "translucent" };
	const List<PolyParam> *lists[3] = { &rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr };
	const u32 nv = (u32)rc.verts.used();

	for (u32 v = 0; v < nv; v++)
		if (memcmp(&rc.verts.head()[v], &o.vtx[v * TAG_V_WORDS], sizeof(Vertex)) != 0)
		{
			const u32 *c = (const u32 *)&rc.verts.head()[v];
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
	return NULL;
}

typedef std::chrono::steady_clock clk;

extern "C" bool __wrap__Z13ta_parse_vdrcP10TA_context(TA_context *ctx)
{
	if (!ready)
		setup();
	st.seen++;
	TaGpuFrame gf;
	const bool table_ok = ctx->walk_count != TA_WALK_INVALID || ctx->tad.End() == ctx->tad.thd_root;
	/* (once untimed: the textures the frame asks for are converted, and the
	 * pass timed below finds them made, as the parse after it does) */
	ta_gpu_prepare(ctx, &layout, table, table_room, &gf);
	clk::time_point t0 = clk::now();
	const bool prepared = ta_gpu_prepare(ctx, &layout, table, table_room, &gf);
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
	const int scan_ok = tgt_model(ctx->tad.thd_root, table, &m);
	tgt_gpu(ctx->tad.thd_root, bytes, table, gf.table_words, &g, &ms);
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
	if (st.compared % 300 == 0)
		report();
	return rv;
}
