/* Modifier volume triangles for the raster renderers, with a vertex far off
 * the screen clipped first.
 *
 * A game's shadow volume near the eye has vertices hundreds of millions of
 * pixels out (Jet Grind Radio's are 1e9 pixels out, at 1/w 1e5). The raster
 * renderers hand such a vertex to the GPU as it is, and the GPU's clipper
 * then cuts the triangle to the screen in 32-bit float: where an edge
 * between two such vertices crosses the screen's side, the point it finds
 * is the difference of two coordinates of 30 (x w) that should come to
 * 1e-5 (w), and its place on the screen is out by a fifth of the screen's
 * width. The triangle covers a strip by the screen's edge it does not, or
 * misses one it covers, and every pixel there is the wrong side of the
 * shadow: on the jgr13 frame a band down the right of the street, and in
 * Direct3D 12 a strip along the top, on lavapipe. (refsw and the compute
 * renderer, which take the exact geometry, draw neither.)
 *
 * So a triangle with a vertex more than MV_CLIP_LIMIT pixels from the
 * screen's origin is cut here, in double precision, to the square of that
 * size about the origin - which holds the screen at any resolution and
 * any aspect the matrix gives, many times over - and handed on as the fan
 * of up to five triangles the polygon left makes. What is inside the
 * square is the triangle's, to well under a 1/256 pixel: each point is
 * the edge's, and 1/w, which is linear across the screen, is taken along
 * the edge to it. The GPU then has vertices no farther than the square,
 * whose clipping float keeps. The fan's triangles have the triangle's
 * winding, and share their edges exactly, so a sample is in one of them or
 * none, as it was in the triangle or not.
 *
 * An edge two triangles share is cut to the same points in both (each cut
 * is worked out from the edge's two ends taken in one order, whatever the
 * order the triangle has them in), so a volume's faces still meet with no
 * gap and no sample in both. A triangle whose vertices are all within the
 * square is not touched: a frame with none farther than it is drawn as it
 * always was, to the bit. */
#pragma once
#include "types.h"
#include "hw/pvr/ta_ctx.h"
#include <cmath>

#define MV_CLIP_LIMIT 32768.0f
#define MV_CLIP_MAX 5	/* triangles a clipped one can become */

/* Whether the triangle is to be cut: a vertex beyond the square, and all
 * three in front of the eye with a 1/w the volumes' shader draws (it
 * leaves a triangle with a 1/w below 0, above 3.4e37 or not a number
 * undrawn, and this does not change that) */
static inline bool mv_clip_wanted(const ModTriangle& t)
{
	const float L = MV_CLIP_LIMIT;
	const bool beyond = !(std::fabs(t.x0) <= L && std::fabs(t.y0) <= L && std::fabs(t.x1) <= L && std::fabs(t.y1) <= L
			&& std::fabs(t.x2) <= L && std::fabs(t.y2) <= L);
	return beyond && t.z0 > 0.f && t.z1 > 0.f && t.z2 > 0.f && t.z0 <= 3.4e37f && t.z1 <= 3.4e37f && t.z2 <= 3.4e37f
			&& !std::isnan(t.x0) && !std::isnan(t.y0) && !std::isnan(t.x1) && !std::isnan(t.y1)
			&& !std::isnan(t.x2) && !std::isnan(t.y2);
}

struct MvClipPoint
{
	double x, y, z;
};

/* Where the edge from @a to @b crosses the line coord @k = @at: from the
 * ends taken in one order, so that the edge gives the same point in both
 * of the triangles that have it */
static inline MvClipPoint mv_clip_cross(MvClipPoint a, MvClipPoint b, int k, double at)
{
	if (a.x > b.x || (a.x == b.x && (a.y > b.y || (a.y == b.y && a.z > b.z))))
		std::swap(a, b);
	const double ca = k == 0 ? a.x : a.y, cb = k == 0 ? b.x : b.y;
	const double t = (at - ca) / (cb - ca);
	MvClipPoint p;
	p.x = k == 0 ? at : a.x + t * (b.x - a.x);
	p.y = k == 1 ? at : a.y + t * (b.y - a.y);
	p.z = a.z + t * (b.z - a.z);
	return p;
}

/* The triangle cut to the square: up to MV_CLIP_MAX triangles into @out,
 * how many (0: it does not reach into the square) */
static inline u32 mv_clip_triangle(const ModTriangle& t, ModTriangle *out)
{
	MvClipPoint buf[2][9];
	MvClipPoint *p = buf[0], *q = buf[1];
	u32 n = 3;
	p[0] = { t.x0, t.y0, t.z0 };
	p[1] = { t.x1, t.y1, t.z1 };
	p[2] = { t.x2, t.y2, t.z2 };
	const double L = MV_CLIP_LIMIT;
	for (int side = 0; side < 4 && n != 0; side++)
	{
		const int k = side >> 1;
		const double s = side & 1 ? -1.0 : 1.0;	// inside: s * coord <= L
		u32 m = 0;
		for (u32 i = 0; i < n; i++)
		{
			const MvClipPoint& a = p[i];
			const MvClipPoint& b = p[i + 1 == n ? 0 : i + 1];
			const bool ain = s * (k == 0 ? a.x : a.y) <= L;
			const bool bin = s * (k == 0 ? b.x : b.y) <= L;
			if (ain)
				q[m++] = a;
			if (ain != bin)
				q[m++] = mv_clip_cross(a, b, k, s * L);
		}
		std::swap(p, q);
		n = m;
	}
	if (n < 3)
		return 0;
	u32 count = 0;
	for (u32 i = 1; i + 1 < n; i++)
	{
		ModTriangle& o = out[count++];
		o.x0 = (f32)p[0].x; o.y0 = (f32)p[0].y; o.z0 = (f32)p[0].z;
		o.x1 = (f32)p[i].x; o.y1 = (f32)p[i].y; o.z1 = (f32)p[i].z;
		o.x2 = (f32)p[i + 1].x; o.y2 = (f32)p[i + 1].y; o.z2 = (f32)p[i + 1].z;
	}
	return count;
}

/* The frame's mv_clip and mv_clip_range (rend_context), from its
 * @triangles volume triangles, @get(t) the triangle t: left empty when
 * none of them is to be cut, or when the cut ones do not fit (the volumes
 * are then drawn as they are). A volume left with no triangle keeps one
 * with no area, so that a closing draw that goes with its last triangle
 * is still made. Each list's volumes have their triangles in increasing
 * order, the opaque list's and the translucent list's numbered together. */
template<typename Get>
static inline void mv_clip_build(rend_context& rc, u32 triangles, Get& get)
{
	rc.mv_clip.Clear();
	rc.mv_clip_range.Clear();
	bool any = false;
	for (u32 t = 0; t < triangles && !any; t++)
		any = mv_clip_wanted(get(t));
	if (!any)
		return;
	const u32 lists = rc.global_param_mvo.used() + rc.global_param_mvo_tr.used();
	if ((u32)rc.mv_clip_range.avail < 2 * lists)
		return;
	for (int l = 0; l < 2; l++)
	{
		const List<ModifierVolumeParam>& list = l == 0 ? rc.global_param_mvo : rc.global_param_mvo_tr;
		get.restart();
		for (const ModifierVolumeParam *p = list.head(); p != list.end(); p++)
		{
			u32 *range = rc.mv_clip_range.Append(2);
			range[0] = rc.mv_clip.used();
			for (u32 t = p->first; t < p->first + p->count && t < triangles; t++)
			{
				if (rc.mv_clip.avail < MV_CLIP_MAX + 1)
				{
					rc.mv_clip.Clear();
					rc.mv_clip_range.Clear();
					return;
				}
				const ModTriangle tri = get(t);
				if (mv_clip_wanted(tri))
				{
					const u32 n = mv_clip_triangle(tri, rc.mv_clip.daty);
					rc.mv_clip.Append(n);
				}
				else
					*rc.mv_clip.Append() = tri;
			}
			if ((u32)rc.mv_clip.used() == range[0] && p->count != 0)
			{
				ModTriangle *o = rc.mv_clip.Append();
				o->x0 = o->x1 = o->x2 = 0.f;
				o->y0 = o->y1 = o->y2 = 0.f;
				o->z0 = o->z1 = o->z2 = 1.f;
			}
			range[1] = rc.mv_clip.used() - range[0];
		}
	}
}

/* A volume's first triangle and count as the raster renderers draw them:
 * in mv_clip when the frame has it, else its own. @list is the list the
 * parameter is in (global_param_mvo's head or global_param_mvo_tr's). */
static inline void mv_clip_range_of(const rend_context& rc, const ModifierVolumeParam *list, const ModifierVolumeParam& p, u32& first, u32& count)
{
	if (rc.mv_clip.used() == 0)
	{
		first = p.first;
		count = p.count;
		return;
	}
	const u32 i = (u32)(&p - list) + (list == rc.global_param_mvo.head() ? 0 : rc.global_param_mvo.used());
	first = rc.mv_clip_range.head()[2 * i];
	count = rc.mv_clip_range.head()[2 * i + 1];
}
