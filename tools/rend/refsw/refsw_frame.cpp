/* A frame of this core's, drawn by the reference software renderer
 * (refsw.cpp, refsw_pixel.cpp) the way the chip draws it: tile by tile,
 * the opaque and punch-through lists into the depth and tag buffers, the
 * modifier volumes into the stencil, the tags shaded, then the translucent
 * list peeled a layer a pass from the back. The frame is a dump of what the
 * TA parse left (refsw_dump.h, written by tools/rend/tile_dump.cpp).
 *
 *   refsw_frame FRAME.bin OUT.ppm [PROBE...]
 *
 * OUT.ppm is the picture, 8 bits a channel (the chip writes 16-bit 565 to
 * its framebuffer: the picture is what it has before that). Each PROBE is
 * "X,Y": for that pixel, every triangle that covers it is printed as it is
 * rasterized, with the depth (1/w) the renderer interpolates for it and the
 * depth, tag and stencil the tile buffers hold after the list, so that what
 * the chip compares at a pixel can be read off.
 *
 * This file is the part of libswirl's refrend_base.cpp that walked the
 * chip's object lists in video memory, written again over the parse's
 * lists (this core's TA keeps the polygons in its own lists and writes no
 * object lists): the sequence of operations a tile goes through is that
 * file's RenderPVR(), step for step. The triangles themselves are
 * rasterized and shaded by refsw.cpp and refsw_pixel.cpp, untouched.
 *
 * Textures are made by this core's texture cache (core/rend/TexCache.o is
 * linked in) and laid out as libswirl's software renderers want them. The
 * background polygon is read from video memory as the chip reads it. */
/* the registers the frame is drawn with, as the texture cache reads them */
#define PVR_REGS_FOR_RENDERER
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "types.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/TexCache.h"
#include "refsw_dump.h"
#include "refsw.h"
#include "refsw_pixel.h"

RefRendInterface* refsw_create(u8* vram);

/* what the texture cache takes from the rest of the core */
settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 * 1024 * 1024;
unsigned VRAM_MASK = 8 * 1024 * 1024 - 1;
u8 *rend_pvr_regs;
void _vmem_protect_vram(u32, u32) {}
void _vmem_unprotect_vram(u32, u32) {}
u32 _vmem_get_vram_offset(void *) { return (u32)-1; }
bool _vmem_vram_page_protected(u32) { return false; }
void os_DebugBreak() { abort(); }
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}
template<> u32 DYNACALL pvr_read_area1<u32>(u32 addr) { return *(u32 *)&vram.data[addr & VRAM_MASK]; }
template<> u16 DYNACALL pvr_read_area1<u16>(u32 addr) { return *(u16 *)&vram.data[addr & VRAM_MASK]; }
extern VArray2 vram;

static u8 *vram_bytes;
u32 vri(u32 addr) { return *(u32 *)&vram_bytes[addr & VRAM_MASK]; }
f32 vrf(u32 addr) { return *(f32 *)&vram_bytes[addr & VRAM_MASK]; }

/*
 * Textures
 */
namespace {
struct RefTexture : public BaseTextureCacheData
{
	std::vector<u8> texels;	/* the top level, RGBA as the cache makes it */
	u32 got_w = 0, got_h = 0;
	std::string GetId() override { return "refsw"; }
	bool Force32BitTexture(TextureType) const override { return true; }
	void UploadToGPU(int width, int height, u8 *data, bool, bool) override
	{
		if (tex_type != TextureType::_8888)
			return;
		texels.assign(data, data + (size_t)width * height * 4);
		got_w = width;
		got_h = height;
	}
};
}

static std::map<u64, text_info> textures;
static std::vector<u8*> texture_blocks;

/* The texture a polygon draws with, as refsw's TextureFetch reads it: 16
 * bytes a texel, the 2x2 block of (u+1,v+1), (u,v+1), (u+1,v), (u,v) in
 * 32-bit BGRA, the neighbours wrapping at the edges (libswirl's
 * raw_GetTexture laid it out like this for its software renderers). The
 * largest level only (settings.rend.ForceTextureLOD0): refsw has no
 * mipmapping. */
text_info raw_GetTexture(u8 *, TSP tsp, TCW tcw)
{
	const u64 key = ((u64)tcw.full << 32) | (tsp.full & 0x3F);	/* the format and address, and the size */
	std::map<u64, text_info>::iterator it = textures.find(key);
	if (it != textures.end())
		return it->second;
	RefTexture t;
	t.tcw = tcw;
	t.tsp = tsp;
	t.area = 0;
	t.Create();
	t.Update();
	t.Delete();
	text_info info;
	memset(&info, 0, sizeof(info));
	if (t.got_w == 0 || t.got_h == 0)
	{
		fprintf(stderr, "refsw_frame: texture %08x/%08x not converted (type %d)\n", tcw.full, tsp.full, (int)t.tex_type);
		/* a 1x1 white texture, so that the polygon is still drawn */
		t.got_w = t.got_h = 1;
		t.texels.assign(4, 0xFF);
	}
	const u32 w = t.got_w, h = t.got_h;
	if (getenv("REFSW_TEXTURES") != NULL)
	{
		u32 sum = 0;
		for (size_t i = 0; i < t.texels.size(); i++)
			sum += t.texels[i];
		fprintf(stderr, "refsw_frame: texture tcw %08x tsp %08x: %u x %u, format %s, type %d, sum %u, first %02x%02x%02x%02x\n", tcw.full, tsp.full, w, h,
				t.GetPixelFormatName(), (int)t.tex_type, sum, t.texels[0], t.texels[1], t.texels[2], t.texels[3]);
	}
	u8 *blocks = (u8 *)malloc((size_t)w * h * 16);
	for (u32 y = 0; y < h; y++)
		for (u32 x = 0; x < w; x++)
		{
			u32 *out = (u32 *)(blocks + ((size_t)y * w + x) * 16);
			const u32 x1 = (x + 1) % w, y1 = (y + 1) % h;
			const u32 at[4][2] = { { x1, y1 }, { x, y1 }, { x1, y }, { x, y } };
			for (int i = 0; i < 4; i++)
			{
				const u8 *p = &t.texels[((size_t)at[i][1] * w + at[i][0]) * 4];
				out[i] = (u32)p[2] | ((u32)p[1] << 8) | ((u32)p[0] << 16) | ((u32)p[3] << 24);	/* RGBA to BGRA */
			}
		}
	info.width = w;
	info.height = h;
	info.pdata = blocks;
	info.textype = 0;
	texture_blocks.push_back(blocks);
	textures[key] = info;
	return info;
}

static void textures_clear()
{
	for (size_t i = 0; i < texture_blocks.size(); i++)
		free(texture_blocks[i]);
	texture_blocks.clear();
	textures.clear();
}

/*
 * The frame
 */
struct Frame
{
	refsw_dump_header h;
	std::vector<u8> regs, vram;
	std::vector<Vertex> verts;
	std::vector<u32> idx;
	std::vector<PolyParam> op, pt, tr;
	std::vector<ModTriangle> modtrig;
	std::vector<ModifierVolumeParam> mvo, mvo_tr;
	std::vector<RenderPass> passes;
	std::vector<u32> region;
};

template<class T>
static bool get(FILE *f, std::vector<T>& v, u32 n, u32 size)
{
	if (size != sizeof(T))
	{
		fprintf(stderr, "refsw_frame: the dump's structures are %u bytes, this program's %u\n", size, (unsigned)sizeof(T));
		return false;
	}
	v.resize(n);
	return n == 0 || fread(v.data(), sizeof(T), n, f) == n;
}

static bool load(const char *path, Frame& fr)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL)
	{
		perror(path);
		return false;
	}
	bool ok = fread(&fr.h, sizeof(fr.h), 1, f) == 1 && fr.h.magic == REFSW_DUMP_MAGIC && fr.h.version == REFSW_DUMP_VERSION;
	if (ok)
	{
		fr.regs.resize(fr.h.regs_size);
		fr.vram.resize(fr.h.vram_size);
		ok = fread(fr.regs.data(), 1, fr.h.regs_size, f) == fr.h.regs_size
			&& fread(fr.vram.data(), 1, fr.h.vram_size, f) == fr.h.vram_size
			&& get(f, fr.verts, fr.h.n_verts, fr.h.sizeof_vertex)
			&& get(f, fr.idx, fr.h.n_idx, 4)
			&& get(f, fr.op, fr.h.n_op, fr.h.sizeof_polyparam)
			&& get(f, fr.pt, fr.h.n_pt, fr.h.sizeof_polyparam)
			&& get(f, fr.tr, fr.h.n_tr, fr.h.sizeof_polyparam)
			&& get(f, fr.modtrig, fr.h.n_modtrig, fr.h.sizeof_modtriangle)
			&& get(f, fr.mvo, fr.h.n_mvo, fr.h.sizeof_mvparam)
			&& get(f, fr.mvo_tr, fr.h.n_mvo_tr, fr.h.sizeof_mvparam)
			&& get(f, fr.passes, fr.h.n_passes, fr.h.sizeof_renderpass)
			&& get(f, fr.region, fr.h.n_region_tiles, 4);
	}
	fclose(f);
	if (!ok)
		fprintf(stderr, "refsw_frame: %s is not a frame this program reads\n", path);
	return ok;
}

/*
 * The walk
 */
struct Probe
{
	int x, y;
};
static std::vector<Probe> probes;
static int cur_pass;
static const char *cur_list;

/* refsw's RasterizeTriangle() decides whether a pixel is in the triangle
 * with these half-edge functions at the pixel's sampling position; the same
 * here, so that a probe reports the triangles refsw shades at the pixel. */
static bool covers(const Vertex& v1, const Vertex& v2, const Vertex& v3, float x_ps, float y_ps)
{
	const float X1 = v1.x, X2 = v2.x, X3 = v3.x, Y1 = v1.y, Y2 = v2.y, Y3 = v3.y;
	int sgn = 1;
	if (refsw_far(v1, v2, v3))
	{
		const double area = ((double)X1 - X3) * ((double)Y2 - Y3) - ((double)Y1 - Y3) * ((double)X2 - X3);
		if ((float)area > 0)
			sgn = -1;
		const float X[3] = { X1, X2, X3 }, Y[3] = { Y1, Y2, Y3 };
		RefswExactEdges e;
		e.Setup(sgn, X, Y, 3);
		return e.In(x_ps, y_ps);
	}
	float area = ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));
	if (area > 0)
		sgn = -1;
	const float DX12 = sgn * (X1 - X2), DX23 = sgn * (X2 - X3), DX31 = sgn * (X3 - X1);
	const float DY12 = sgn * (Y1 - Y2), DY23 = sgn * (Y2 - Y3), DY31 = sgn * (Y3 - Y1);
	const float C1 = DY12 * X1 - DX12 * Y1, C2 = DY23 * X2 - DX23 * Y2, C3 = DY31 * X3 - DX31 * Y3;
	const float Xhs12 = C1 + DX12 * y_ps - DY12 * x_ps;
	const float Xhs23 = C2 + DX23 * y_ps - DY23 * x_ps;
	const float Xhs31 = C3 + DX31 * y_ps - DY31 * x_ps;
	return Xhs12 >= 0 && Xhs23 >= 0 && Xhs31 >= 0;
}

static void probe_triangle(const taRECT& rect, const DrawParameters& params, parameter_tag_t tag, const char *what, u32 index, u32 tri,
		const Vertex& v1, const Vertex& v2, const Vertex& v3)
{
	if (probes.empty())
		return;
	const float halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	for (size_t i = 0; i < probes.size(); i++)
	{
		const int px = probes[i].x, py = probes[i].y;
		if (px < rect.left || px >= rect.right || py < rect.top || py >= rect.bottom)
			continue;
		const float x_ps = px + halfpixel, y_ps = py + halfpixel;
		if (!covers(v1, v2, v3, x_ps, y_ps))
			continue;
		PlaneStepper3 Z;
		Z.Setup(v1, v2, v3, v1.z, v2.z, v3.z);
		const float invW = Z.Ip(x_ps, y_ps);
		union { float f; u32 u; } b;
		b.f = invW;
		printf("probe %d,%d: pass %d %s #%u tri %u tag %u: 1/w %.9g (0x%08x) w %.6f; depth mode %u zwrite %s cull %u tex %u offs %u gouraud %u; "
				"tsp %08x (src %u dst %u usealpha %u fog %u) tcw %08x; z %.9g %.9g %.9g\n",
				px, py, cur_pass, what, index, tri, tag, invW, b.u, 1.0 / invW,
				params.isp.DepthMode, params.isp.ZWriteDis ? "off" : "on", params.isp.CullMode, params.isp.Texture, params.isp.Offset, params.isp.Gouraud,
				params.tsp.full, params.tsp.SrcInstr, params.tsp.DstInstr, params.tsp.UseAlpha, params.tsp.FogCtrl, params.tcw.full, v1.z, v2.z, v3.z);
	}
}

static void probe_buffers(RefRendInterface *backend, const taRECT& rect, const char *when)
{
	if (probes.empty())
		return;
	const u32 *rb = (const u32 *)backend->DebugGetAllBuffers();
	for (size_t i = 0; i < probes.size(); i++)
	{
		const int px = probes[i].x, py = probes[i].y;
		if (px < rect.left || px >= rect.right || py < rect.top || py >= rect.bottom)
			continue;
		const u32 at = (py - rect.top) * MAX_RENDER_WIDTH + (px - rect.left);
		union { float f; u32 u; } d1, d2;
		d1.u = rb[DEPTH1_BUFFER_PIXEL_OFFSET + at];
		d2.u = rb[DEPTH2_BUFFER_PIXEL_OFFSET + at];
		const u32 tag = rb[PARAM_BUFFER_PIXEL_OFFSET + at];
		const u32 stencil = rb[STENCIL_BUFFER_PIXEL_OFFSET + at];
		const u32 accum = rb[ACCUM1_BUFFER_PIXEL_OFFSET + at];
		printf("probe %d,%d: pass %d after %s: depth1 %.9g (0x%08x) depth2 %.9g (0x%08x) tag %u%s stencil %u colour %08x\n",
				px, py, cur_pass, when, d1.f, d1.u, d2.f, d2.u, tag >> 1, (tag & TAG_INVALID) ? " (shaded)" : "", stencil, accum);
	}
}

/* REFSW_GAPS=R: how close the depths the chip compares come to each
 * other, over the whole frame. For every pixel of a tile the two largest
 * 1/w of the opaque and punch-through lists are kept (as the chip's
 * depth test sees them), and the largest translucent 1/w with the
 * nearest other translucent one, and the frame's smallest relative gaps
 * are printed with their pixel, tags and bit patterns: a gap under R
 * (relative) is counted. The 1/w is the plane equation refsw evaluates,
 * at the pixel refsw samples. Analysis only: it changes nothing drawn. */
static float gap_rel = 0;
struct GapSlot { float z; u32 tag; };
static GapSlot gap_op[MAX_RENDER_PIXELS][2];	/* the two largest opaque/PT depths */
static GapSlot gap_tr[MAX_RENDER_PIXELS][2];	/* the two largest translucent depths */
static u32 gap_count_op, gap_count_tr, gap_count_tr_op, gap_examples;
static float gap_min_op = 1, gap_min_tr = 1, gap_min_tr_op = 1;

static void gap_clear()
{
	for (int i = 0; i < MAX_RENDER_PIXELS; i++)
	{
		gap_op[i][0].z = gap_op[i][1].z = -1; gap_op[i][0].tag = gap_op[i][1].tag = 0;
		gap_tr[i][0].z = gap_tr[i][1].z = -1; gap_tr[i][0].tag = gap_tr[i][1].tag = 0;
	}
}

static void gap_put(GapSlot *two, float z, u32 tag)
{
	if (z > two[0].z) { two[1] = two[0]; two[0].z = z; two[0].tag = tag; }
	else if (z > two[1].z) { two[1].z = z; two[1].tag = tag; }
}

/* the triangle's 1/w at every pixel of the tile it covers, as refsw's
 * RasterizeTriangle() finds them */
static void gap_triangle(const taRECT& rect, RenderMode rm, u32 tag, const Vertex& v1, const Vertex& v2, const Vertex& v3)
{
	if (gap_rel == 0 || rm == RM_MODIFIER)
		return;
	const float halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	PlaneStepper3 Z;
	Z.Setup(v1, v2, v3, v1.z, v2.z, v3.z);
	int minx = (int)std::max(std::min(std::min(v1.x, v2.x), v3.x), (float)rect.left);
	int maxx = (int)std::min(std::max(std::max(v1.x, v2.x), v3.x) + 1, (float)rect.right - 1);
	int miny = (int)std::max(std::min(std::min(v1.y, v2.y), v3.y), (float)rect.top);
	int maxy = (int)std::min(std::max(std::max(v1.y, v2.y), v3.y) + 1, (float)rect.bottom - 1);
	for (int y = miny; y <= maxy; y++)
		for (int x = minx; x <= maxx; x++)
		{
			const float x_ps = x + halfpixel, y_ps = y + halfpixel;
			if (!covers(v1, v2, v3, x_ps, y_ps))
				continue;
			const float invW = Z.Ip(x_ps, y_ps);
			GapSlot *two = (rm == RM_TRANSLUCENT ? gap_tr : gap_op)[(y - rect.top) * MAX_RENDER_WIDTH + (x - rect.left)];
			gap_put(two, invW, tag);
		}
}

static void gap_report(const char *what, const taRECT& rect, int i, const GapSlot& a, const GapSlot& b, float rel, float& least, u32& count)
{
	if (rel < least)
		least = rel;
	if (rel >= gap_rel)
		return;
	count++;
	union { float f; u32 u; } ua, ub;
	ua.f = a.z; ub.f = b.z;
	/* (REFSW_GAPS_EXAMPLES: up to this many lines of each kind, default 20) */
	static u32 cap = getenv("REFSW_GAPS_EXAMPLES") ? atoi(getenv("REFSW_GAPS_EXAMPLES")) : 20;
	if (count <= cap)
	{
		gap_examples++;
		printf("gap %s: pixel %d,%d: 1/w %.9g (0x%08x) tag %u and %.9g (0x%08x) tag %u: relative %.3g, %d ulps\n", what,
				rect.left + i % MAX_RENDER_WIDTH, rect.top + i / MAX_RENDER_WIDTH, a.z, ua.u, a.tag >> 1, b.z, ub.u, b.tag >> 1, rel, (int)(ua.u - ub.u));
	}
}

static void gap_tile(const taRECT& rect)
{
	if (gap_rel == 0)
		return;
	for (int i = 0; i < MAX_RENDER_PIXELS; i++)
	{
		if (gap_op[i][1].z > 0 && gap_op[i][0].tag != gap_op[i][1].tag)
			gap_report("opaque vs opaque", rect, i, gap_op[i][0], gap_op[i][1], (gap_op[i][0].z - gap_op[i][1].z) / gap_op[i][0].z, gap_min_op, gap_count_op);
		if (gap_tr[i][1].z > 0 && gap_tr[i][0].tag != gap_tr[i][1].tag)
			gap_report("translucent vs translucent", rect, i, gap_tr[i][0], gap_tr[i][1], (gap_tr[i][0].z - gap_tr[i][1].z) / gap_tr[i][0].z, gap_min_tr, gap_count_tr);
		if (gap_tr[i][0].z > 0 && gap_op[i][0].z > 0)
			gap_report("translucent vs opaque", rect, i, gap_tr[i][0], gap_op[i][0], fabsf(gap_tr[i][0].z - gap_op[i][0].z) / gap_op[i][0].z, gap_min_tr_op, gap_count_tr_op);
	}
}

/* REFSW_GEO=1: refsw's coverage held to the exact geometry, as
 * tools/rend/tile_geo.c holds the compute renderer's. Every triangle drawn
 * into a tile, polygons and modifier volumes, is culled and tested at
 * the pixels of the tile the way RasterizeTriangle() does (covers(), its
 * cull), and again in long double from the half-edge constants as
 * libswirl writes them (C1 = DY12 X1 - DX12 Y1, whose rounding at 64 bits
 * is under 1e-10 pixel for a vertex 1e9 pixels out) - every pixel within
 * 1.5 pixel of an exact edge, and one in 8 each way elsewhere (as
 * tile_geo.c looks at a band and a grid). A pixel one covers
 * and the other does not is a fault, unless its sampling position is
 * within 1/256 pixel of an edge (counted apart); so is a triangle one
 * culls and the other draws. Analysis only: it changes nothing drawn. */
static bool geo_on;
static u64 geo_tris, geo_samples, geo_near, geo_faults, geo_fault_tris, geo_cull_faults;

static bool geo_culled(const DrawParameters *params, int vertex_offset, double area)
{
	if (params->isp.CullMode == 0)
		return false;
	if (fabs(area) < FPU_CULL_VAL)
		return true;
	if (params->isp.CullMode >= 2)
	{
		const u32 mode = vertex_offset ^ (params->isp.CullMode & 1);
		return (mode == 0 && area < 0) || (mode == 1 && area > 0);
	}
	return false;
}

static void geo_triangle(const DrawParameters *params, int vertex_offset, const Vertex& v1, const Vertex& v2, const Vertex& v3, const taRECT *rect)
{
	const float X1 = v1.x, X2 = v2.x, X3 = v3.x, Y1 = v1.y, Y2 = v2.y, Y3 = v3.y;
	if (std::isnan(X1) || std::isnan(X2) || std::isnan(X3) || std::isnan(Y1) || std::isnan(Y2) || std::isnan(Y3))
		return;
	/* the pixels of the tile within a pixel of the triangle's box: outside
	 * it neither covers anything (each end kept to the tile before it is
	 * an integer: a box billions of pixels out does not fit one) */
	const float l = (float)rect->left, r = (float)rect->right, t = (float)rect->top, b = (float)rect->bottom;
	const int x0 = (int)std::min(std::max(l, floorf(std::min(std::min(X1, X2), X3)) - 1), r);
	const int x1 = (int)std::max(std::min(r, ceilf(std::max(std::max(X1, X2), X3)) + 1), l);
	const int y0 = (int)std::min(std::max(t, floorf(std::min(std::min(Y1, Y2), Y3)) - 1), b);
	const int y1 = (int)std::max(std::min(b, ceilf(std::max(std::max(Y1, Y2), Y3)) + 1), t);
	if (x0 >= x1 || y0 >= y1)
		return;
	geo_tris++;
	/* refsw's cull, in its arithmetic */
	const bool far = refsw_far(v1, v2, v3);
	const float area_r = far ? (float)(((double)X1 - X3) * ((double)Y2 - Y3) - ((double)Y1 - Y3) * ((double)X2 - X3))
	                         : ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));
	const long double area_x = ((long double)X1 - X3) * ((long double)Y2 - Y3) - ((long double)Y1 - Y3) * ((long double)X2 - X3);
	const bool cull_r = geo_culled(params, vertex_offset, area_r);
	const bool cull_x = geo_culled(params, vertex_offset, (double)area_x);
	if (cull_r != cull_x)
	{
		geo_cull_faults++;
		printf("geo: tile %d,%d: a triangle refsw %s and the geometry %s: area %.9g (exact %.9Lg), vertices %.9g,%.9g %.9g,%.9g %.9g,%.9g\n",
				rect->left, rect->top, cull_r ? "culls" : "draws", cull_x ? "culls" : "draws", area_r, area_x, X1, Y1, X2, Y2, X3, Y3);
		return;
	}
	if (cull_r)
		return;
	const int sgn = area_x > 0 ? -1 : 1;
	const long double X[3] = { X1, X2, X3 }, Y[3] = { Y1, Y2, Y3 };
	long double dx[3], dy[3], c[3], len[3];
	for (int k = 0; k < 3; k++)
	{
		const int a = k, b = (k + 1) % 3;
		dx[k] = sgn * (X[a] - X[b]);
		dy[k] = sgn * (Y[a] - Y[b]);
		c[k] = dy[k] * X[a] - dx[k] * Y[a];
		len[k] = sqrtl(dx[k] * dx[k] + dy[k] * dy[k]);
	}
	const float halfpixel = HALF_OFFSET.fpu_pixel_half_offset ? 0.5f : 0;
	/* where every pixel of the box lies more than 1.5 pixel inside all the
	 * edges, or outside one, only a pixel in 8 each way is looked at: a
	 * misplaced edge's error begins at the exact edge, which the pixels
	 * near it see, and the grid sees one that is not near (as tile_geo
	 * looks at a band about the edges and a grid) */
	int step = 1;
	{
		bool all_in = true, all_out = false;
		for (int k = 0; k < 3 && len[k] > 0; k++)
		{
			long double lo = 1e30L, hi = -1e30L;
			for (int corner = 0; corner < 4; corner++)
			{
				const long double px = (corner & 1 ? x1 - 1 : x0) + halfpixel, py = (corner & 2 ? y1 - 1 : y0) + halfpixel;
				const long double e = (c[k] + dx[k] * py - dy[k] * px) / len[k];
				lo = std::min(lo, e);
				hi = std::max(hi, e);
			}
			if (lo <= 1.5L)
				all_in = false;
			if (hi < -1.5L)
				all_out = true;
		}
		if (all_in || all_out)
			step = 8;
	}
	u32 faults = 0;
	for (int y = y0; y < y1; y += step)
		for (int x = x0; x < x1; x += step)
		{
			const float x_ps = x + halfpixel, y_ps = y + halfpixel;
			bool in_x = true;
			long double nearest = 1e30L;
			for (int k = 0; k < 3; k++)
			{
				const long double e = c[k] + dx[k] * y_ps - dy[k] * x_ps;
				if (e < 0)
					in_x = false;
				if (len[k] > 0 && fabsl(e) / len[k] < nearest)
					nearest = fabsl(e) / len[k];
			}
			/* (and within the box, the same: a pixel 1.5 pixel or more from
			 * every edge only on the grid) */
			if (step == 1 && nearest >= 1.5L && ((x - x0) % 8 != 0 || (y - y0) % 8 != 0))
				continue;
			geo_samples++;
			if (covers(v1, v2, v3, x_ps, y_ps) == in_x)
				continue;
			if (nearest < 1.0L / 256)
			{
				geo_near++;
				continue;
			}
			geo_faults++;
			if (faults++ == 0 && geo_fault_tris++ < 8)	/* (every triangle at fault counted, the first 8 printed) */
				printf("geo: tile %d,%d: pixel %d,%d %s by refsw and %s, %.3Lg pixel from an edge; vertices %.9g,%.9g %.9g,%.9g %.9g,%.9g\n",
						rect->left, rect->top, x, y, in_x ? "not covered" : "covered", in_x ? "covered" : "not covered", nearest,
						X1, Y1, X2, Y2, X3, Y3);
		}
}

/* refrend_base.cpp's RenderTriangle(): a modifier volume's last triangle
 * has the stencil summarized after it */
static void render_triangle(RefRendInterface *backend, RenderMode render_mode, DrawParameters *params, parameter_tag_t tag, int vertex_offset,
		const Vertex& v1, const Vertex& v2, const Vertex& v3, const Vertex *v4, taRECT *area)
{
	if (geo_on && v4 == NULL)
		geo_triangle(params, vertex_offset, v1, v2, v3, area);
	backend->RasterizeTriangle(render_mode, params, tag, vertex_offset, v1, v2, v3, v4, area);
	if (render_mode == RM_MODIFIER)
	{
		if (params->isp.modvol.VolumeMode == 1)
			backend->SummarizeStencilOr();
		else if (params->isp.modvol.VolumeMode == 2)
			backend->SummarizeStencilAnd();
	}
}

/* The TA puts a polygon sent under a user tile clip only in the tiles the
 * clip says (inside the rectangle, or outside it): the same here */
static bool tile_clipped(u32 tileclip, int tx, int ty)
{
	const u32 mode = tileclip >> 28;
	if (mode < 2)
		return false;
	const int csx = tileclip & 63, cex = (tileclip >> 6) & 63, csy = (tileclip >> 12) & 31, cey = (tileclip >> 17) & 31;
	const bool in = tx >= csx && tx <= cex && ty >= csy && ty <= cey;
	return mode == 2 ? !in : in;
}

static ISP_BACKGND_T_type tag_key(u32 id)
{
	ISP_BACKGND_T_type t;
	t.full = 0x80000000u | id;
	return t;
}

/* A polygon list's polygons [from, to) rasterized into the tile. After
 * the parse a polygon is a triangle strip of indices into the vertices
 * (make_index()): the TA's strips of one state joined into one by repeating
 * a vertex, with the winding of each triangle kept (an index repeated where
 * needed), so the strip's i-th triangle has the parity the chip gives it.
 * The joins are triangles with a repeated vertex, which the chip never sees
 * and which are left out here. Each triangle's tag is its ordinal in the
 * frame (@base + the strip's first + i): the same triangle gets the same
 * tag each time the list is walked (the translucent list is, once a peeled
 * layer), which the peeling's coplanar tie-break relies on. */
static void render_polys(RefRendInterface *backend, RenderMode rm, const Frame& fr, const std::vector<PolyParam>& list, u32 from, u32 to,
		u32 base, const std::vector<u32>& first_tri, taRECT *rect, int tx, int ty, const char *what)
{
	cur_list = what;
	for (u32 p = from; p < to && p < list.size(); p++)
	{
		const PolyParam& pp = list[p];
		if (pp.count < 3 || pp.first + pp.count > fr.idx.size())
			continue;
		if (tile_clipped(pp.tileclip, tx, ty))
			continue;
		DrawParameters params;
		memset(&params, 0, sizeof(params));
		params.isp.full = pp.isp.full;
		/* the TA writes the control word's texture, offset, gouraud and
		 * 16-bit UV bits into the ISP word it stores; the parse keeps
		 * the two words apart */
		params.isp.Texture = pp.pcw.Texture;
		params.isp.Offset = pp.pcw.Offset;
		params.isp.Gouraud = pp.pcw.Gouraud;
		params.isp.UV_16b = pp.pcw.UV_16bit;
		params.tsp.full = pp.tsp.full;
		params.tcw.full = pp.tcw.full;
		params.tsp2.full = pp.tsp1.full;
		params.tcw2.full = pp.tcw1.full;
		params.shadow = pp.pcw.Shadow;
		const u32 *idx = &fr.idx[pp.first];
		for (u32 i = 0; i + 2 < pp.count; i++)
		{
			const u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
			if (a == b || b == c || a == c)
				continue;
			if (a >= fr.verts.size() || b >= fr.verts.size() || c >= fr.verts.size())
				continue;
			Vertex tri[3] = { fr.verts[a], fr.verts[b], fr.verts[c] };
			parameter_tag_t tag = backend->AddFpuEntry(&params, tri, rm, tag_key(base + first_tri[p] + i));
			probe_triangle(*rect, params, tag, what, p, i, tri[0], tri[1], tri[2]);
			gap_triangle(*rect, rm, tag, tri[0], tri[1], tri[2]);
			render_triangle(backend, rm, &params, tag, i & 1, tri[0], tri[1], tri[2], NULL, rect);
		}
	}
}

/* A modifier volume list's volumes [from, to): each is a run of triangles
 * under one parameter word; the last polygon's word carries the volume
 * instruction, which summarizes the stencil after each of its triangles */
static void render_modvols(RefRendInterface *backend, const Frame& fr, const std::vector<ModifierVolumeParam>& list, u32 from, u32 to,
		taRECT *rect, int tx, int ty, const char *what)
{
	cur_list = what;
	for (u32 p = from; p < to && p < list.size(); p++)
	{
		const ModifierVolumeParam& mv = list[p];
		if (mv.first + mv.count > fr.modtrig.size())
			continue;
		if (tile_clipped(mv.tileclip, tx, ty))
			continue;
		DrawParameters params;
		memset(&params, 0, sizeof(params));
		params.isp.full = mv.isp.full;
		for (u32 i = 0; i < mv.count; i++)
		{
			const ModTriangle& t = fr.modtrig[mv.first + i];
			Vertex v[3];
			memset(v, 0, sizeof(v));
			v[0].x = t.x0; v[0].y = t.y0; v[0].z = t.z0;
			v[1].x = t.x1; v[1].y = t.y1; v[1].z = t.z1;
			v[2].x = t.x2; v[2].y = t.y2; v[2].z = t.z2;
			probe_triangle(*rect, params, 0, what, p, i, v[0], v[1], v[2]);
			render_triangle(backend, RM_MODIFIER, &params, 0, 0, v[0], v[1], v[2], NULL, rect);
		}
	}
}

/* refrend_base.cpp's decode_pvr_vertex() / decode_pvr_vetrices(): the
 * background polygon, which the chip reads from video memory where
 * ISP_BACKGND_T points */
static f32 f16(u16 v)
{
	u32 z = v << 16;
	return *(f32 *)&z;
}

#define vert_packed_color_(to,src) \
	{ \
	u32 t=src; \
	to[2] = (u8)(t);t>>=8;\
	to[1] = (u8)(t);t>>=8;\
	to[0] = (u8)(t);t>>=8;\
	to[3] = (u8)(t);      \
	}

static void decode_pvr_vertex(DrawParameters *params, u32 ptr, Vertex *cv)
{
	cv->x = vrf(ptr); ptr += 4;
	cv->y = vrf(ptr); ptr += 4;
	cv->z = vrf(ptr); ptr += 4;
	if (params->isp.Texture)
	{
		if (params->isp.UV_16b)
		{
			u32 uv = vri(ptr);
			cv->u = f16((u16)(uv >> 16));
			cv->v = f16((u16)(uv >> 0));
			ptr += 4;
		}
		else
		{
			cv->u = vrf(ptr); ptr += 4;
			cv->v = vrf(ptr); ptr += 4;
		}
	}
	u32 col = vri(ptr); ptr += 4;
	vert_packed_color_(cv->col, col);
	if (params->isp.Offset)
	{
		u32 col = vri(ptr); ptr += 4;
		vert_packed_color_(cv->vtx_spc, col);
	}
}

static u32 decode_pvr_vetrices(DrawParameters *params, u32 base, u32 skip, u32 shadow, Vertex *vtx, int count)
{
	bool PSVM = FPU_SHAD_SCALE.intensity_shadow == 0;
	if (!PSVM)
		shadow = 0;
	params->isp.full = vri(base);
	params->tsp.full = vri(base + 4);
	params->tcw.full = vri(base + 8);
	base += 12;
	if (shadow)
	{
		params->tsp2.full = vri(base + 0);
		params->tcw2.full = vri(base + 4);
		base += 8;
	}
	for (int i = 0; i < count; i++)
	{
		decode_pvr_vertex(params, base, &vtx[i]);
		base += (3 + skip * (shadow + 1)) * 4;
	}
	return base;
}

/* The tile, through every render pass: refrend_base.cpp's RenderPVR() for
 * the region array entries of one tile, one an entry. The first pass
 * clears the buffers; a later one does when its region array entry says
 * (the parse's RenderPass::z_clear). */
static void render_tile(RefRendInterface *backend, const Frame& fr, int tx, int ty, const std::vector<u32> first_tri[3], const u32 base[3], u8 *picture)
{
	taRECT rect;
	rect.top = ty * 32;
	rect.left = tx * 32;
	rect.bottom = rect.top + 32;
	rect.right = rect.left + 32;
	backend->DebugOnTileStart(rect.left, rect.top);
	gap_clear();

	u32 op0 = 0, pt0 = 0, tr0 = 0, mvo0 = 0, mvotr0 = 0;
	for (u32 pass = 0; pass < fr.passes.size(); pass++)
	{
		const RenderPass& rp = fr.passes[pass];
		cur_pass = (int)pass;

		// register BGPOLY to fpu
		parameter_tag_t bgTag;
		{
			DrawParameters params;
			Vertex vtx[8];
			memset(&params, 0, sizeof(params));
			memset(vtx, 0, sizeof(vtx));
			decode_pvr_vetrices(&params, PARAM_BASE + ISP_BACKGND_T.tag_address * 4, ISP_BACKGND_T.skip, ISP_BACKGND_T.shadow, vtx, 8);
			params.shadow = ISP_BACKGND_T.shadow;
			bgTag = backend->AddFpuEntry(&params, &vtx[ISP_BACKGND_T.tag_offset], RM_OPAQUE, ISP_BACKGND_T);
			if (pass == 0)
				probe_triangle(rect, params, bgTag, "background", 0, ISP_BACKGND_T.tag_offset,
						vtx[ISP_BACKGND_T.tag_offset], vtx[ISP_BACKGND_T.tag_offset + 1], vtx[ISP_BACKGND_T.tag_offset + 2]);
		}

		// Tile needs clear?
		if (pass == 0 || rp.z_clear)
			backend->ClearBuffers(bgTag, ISP_BACKGND_D.f, 0);

		// Render OPAQ to TAGS
		render_polys(backend, RM_OPAQUE, fr, fr.op, op0, rp.op_count, base[0], first_tri[0], &rect, tx, ty, "opaque");
		probe_buffers(backend, rect, "opaque list");

		// render PT to TAGS
		render_polys(backend, RM_PUNCHTHROUGH, fr, fr.pt, pt0, rp.pt_count, base[1], first_tri[1], &rect, tx, ty, "punch-through");
		probe_buffers(backend, rect, "punch-through list");

		// Render OPAQ modvols
		render_modvols(backend, fr, fr.mvo, mvo0, rp.mvo_count, &rect, tx, ty, "opaque modvol");
		probe_buffers(backend, rect, "opaque modifier volumes");

		// Render TAGS to ACCUM
		backend->RenderParamTags(RM_OPAQUE, rect.left, rect.top);
		probe_buffers(backend, rect, "opaque shading");

		// layer peeling rendering
		if (rp.tr_count > tr0)
		{
			backend->ClearParamBuffer(TAG_INVALID);
			int layer = 0;
			do
			{
				backend->ClearPixelsDrawn();
				backend->PeelBuffers(FLT_MAX, 0);
				const float keep_rel = gap_rel;
				const bool keep_geo = geo_on;
				if (layer > 0)
					gap_rel = 0, geo_on = false;	// (the list is the same each layer: counted once)
				render_polys(backend, RM_TRANSLUCENT, fr, fr.tr, tr0, rp.tr_count, base[2], first_tri[2], &rect, tx, ty, "translucent");
				gap_rel = keep_rel;
				if (rp.mv_op_tr_shared)
					render_modvols(backend, fr, fr.mvo, mvo0, rp.mvo_count, &rect, tx, ty, "translucent modvol (the opaque ones)");
				else
					render_modvols(backend, fr, fr.mvo_tr, mvotr0, rp.mvo_tr_count, &rect, tx, ty, "translucent modvol");
				geo_on = keep_geo;
				char when[64];
				snprintf(when, sizeof(when), "translucent layer %d (%u pixels)", layer, backend->GetPixelsDrawn());
				probe_buffers(backend, rect, when);
				backend->RenderParamTags(RM_TRANSLUCENT, rect.left, rect.top);
				snprintf(when, sizeof(when), "translucent layer %d shading", layer);
				probe_buffers(backend, rect, when);
				layer++;
			} while (backend->GetPixelsDrawn() != 0 && layer < 1024);
		}

		op0 = rp.op_count;
		pt0 = rp.pt_count;
		tr0 = rp.tr_count;
		mvo0 = rp.mvo_count;
		mvotr0 = rp.mvo_tr_count;

		// clear the tsp cache
		backend->ClearFpuEntries();
	}
	gap_tile(rect);

	/* Copy to vram: here to the picture, 8 bits a channel, BGRA to RGBA;
	 * the chip writes nothing outside the pixel clip */
	FB_X_CLIP_type xclip;
	FB_Y_CLIP_type yclip;
	xclip.full = fr.h.fb_x_clip;
	yclip.full = fr.h.fb_y_clip;
	const u8 *src = backend->GetColorOutputBuffer();
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++, src += 4)
		{
			const int px = rect.left + x, py = rect.top + y;
			if (px >= (int)fr.h.width || py >= (int)fr.h.height)
				continue;
			if (px < (int)xclip.min || px > (int)xclip.max || py < (int)yclip.min || py > (int)yclip.max)
				continue;
			u8 *dst = picture + ((size_t)py * fr.h.width + px) * 4;
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			dst[3] = 0xFF;
		}
}

static void first_triangles(const std::vector<PolyParam>& list, std::vector<u32>& first, u32& total)
{
	first.resize(list.size());
	u32 n = 0;
	for (size_t i = 0; i < list.size(); i++)
	{
		first[i] = n;
		n += list[i].count >= 3 ? list[i].count - 2 : 0;
	}
	total = n;
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: refsw_frame FRAME.bin OUT.ppm [X,Y ...]\n");
		return 2;
	}
	for (int i = 3; i < argc; i++)
	{
		Probe p;
		if (sscanf(argv[i], "%d,%d", &p.x, &p.y) == 2)
			probes.push_back(p);
	}
	Frame fr;
	if (!load(argv[1], fr))
		return 1;
	if (fr.h.width == 0 || fr.h.height == 0 || fr.h.width > 4096 || fr.h.height > 4096)
	{
		fprintf(stderr, "refsw_frame: the frame is %u x %u\n", fr.h.width, fr.h.height);
		return 1;
	}

	/* the core's texture cache sees this frame's memory and registers */
	rend_pvr_regs = fr.regs.data();
	vram_bytes = fr.vram.data();
	vram.data = fr.vram.data();
	vram.size = fr.h.vram_size;
	VRAM_SIZE = fr.h.vram_size;
	VRAM_MASK = fr.h.vram_size - 1;
	memset(&settings, 0, sizeof(settings));
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	settings.rend.GpuTextureDecode = false;
	settings.rend.ForceTextureLOD0 = true;
	palette_update();

	printf("refsw_frame: frame %u, %u x %u, %u passes, %u/%u/%u polygons, %u/%u modifier volumes (%u triangles); "
			"ISP_BACKGND_D %.9g, HALF_OFFSET %u, FPU_CULL_VAL %g, PT_ALPHA_REF %u, FPU_SHAD_SCALE %02x/%u, ISP_FEED_CFG %08x\n",
			fr.h.frame, fr.h.width, fr.h.height, fr.h.n_passes, fr.h.n_op, fr.h.n_pt, fr.h.n_tr, fr.h.n_mvo, fr.h.n_mvo_tr, fr.h.n_modtrig,
			ISP_BACKGND_D.f, HALF_OFFSET.full, FPU_CULL_VAL, PT_ALPHA_REF, FPU_SHAD_SCALE.scale_factor, FPU_SHAD_SCALE.intensity_shadow, ISP_FEED_CFG);
	for (size_t i = 0; i < fr.passes.size(); i++)
		printf("refsw_frame: pass %u: autosort %d z_clear %d op %u pt %u tr %u mvo %u mvo_tr %u%s\n", (unsigned)i, fr.passes[i].autosort, fr.passes[i].z_clear,
				fr.passes[i].op_count, fr.passes[i].pt_count, fr.passes[i].tr_count, fr.passes[i].mvo_count, fr.passes[i].mvo_tr_count,
				fr.passes[i].mv_op_tr_shared ? " (translucent modvols are the opaque ones)" : "");
	if (fr.passes.empty())
	{
		RenderPass rp;
		memset(&rp, 0, sizeof(rp));
		rp.autosort = true;
		rp.z_clear = true;
		rp.op_count = fr.op.size();
		rp.pt_count = fr.pt.size();
		rp.tr_count = fr.tr.size();
		rp.mvo_count = fr.mvo.size();
		rp.mvo_tr_count = fr.mvo_tr.size();
		fr.passes.push_back(rp);
	}

	/* the tags: a triangle's ordinal in the frame, opaque then
	 * punch-through then translucent */
	std::vector<u32> first_tri[3];
	u32 base[3], n;
	first_triangles(fr.op, first_tri[0], n);
	base[0] = 1;
	base[1] = base[0] + n;
	first_triangles(fr.pt, first_tri[1], n);
	base[2] = base[1] + n;
	first_triangles(fr.tr, first_tri[2], n);

	if (getenv("REFSW_GAPS") != NULL)
		gap_rel = (float)atof(getenv("REFSW_GAPS"));
	geo_on = getenv("REFSW_GEO") != NULL && atoi(getenv("REFSW_GEO")) != 0;
	RefRendInterface *backend = refsw_create(fr.vram.data());
	backend->Init();
	backend->DebugOnFrameStart(fr.h.frame);
	std::vector<u8> picture((size_t)fr.h.width * fr.h.height * 4, 0);
	const int tiles_x = (fr.h.width + 31) / 32, tiles_y = (fr.h.height + 31) / 32;
	for (int ty = 0; ty < tiles_y; ty++)
		for (int tx = 0; tx < tiles_x; tx++)
			render_tile(backend, fr, tx, ty, first_tri, base, picture.data());
	delete backend;
	textures_clear();
	if (gap_rel != 0)
		printf("gaps under %g: opaque vs opaque %u pixels (smallest %.3g), translucent vs translucent %u (smallest %.3g), translucent vs opaque %u (smallest %.3g)\n",
				gap_rel, gap_count_op, gap_min_op, gap_count_tr, gap_min_tr, gap_count_tr_op, gap_min_tr_op);
	if (geo_on)
		printf("geo: %llu triangles in tiles, %llu samples: %llu faults in %llu triangles, %llu triangles culled by one and not the other, "
				"%llu within 1/256 pixel of an edge: %s\n", (unsigned long long)geo_tris, (unsigned long long)geo_samples,
				(unsigned long long)geo_faults, (unsigned long long)geo_fault_tris, (unsigned long long)geo_cull_faults,
				(unsigned long long)geo_near, geo_faults == 0 && geo_cull_faults == 0 ? "PASS" : "FAIL");

	FILE *out = fopen(argv[2], "wb");
	if (out == NULL)
	{
		perror(argv[2]);
		return 1;
	}
	fprintf(out, "P6\n%u %u\n255\n", fr.h.width, fr.h.height);
	for (size_t i = 0; i < (size_t)fr.h.width * fr.h.height; i++)
		fwrite(&picture[i * 4], 1, 3, out);
	fclose(out);
	printf("refsw_frame: %s written\n", argv[2]);
	return 0;
}
