/*
	 This file is part of reicast.

	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.

	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <algorithm>
#include "sorter.h"

struct IndexTrig
{
	u32 id[3];
	u16 pid;
	f32 z;
};

#if 0
static float min3(float v0, float v1, float v2)
{
	return std::min(std::min(v0,v1),v2);
}

static float max3(float v0, float v1, float v2)
{
	return std::max(std::max(v0,v1),v2);
}
#endif

static float minZ(const Vertex *v, const u32 *mod)
{
	return std::min(std::min(v[mod[0]].z, v[mod[1]].z), v[mod[2]].z);
}

static bool operator<(const IndexTrig& left, const IndexTrig& right)
{
	return left.z<right.z;
}

static bool operator<(const PolyParam& left, const PolyParam& right)
{
/* put any condition you want to sort on here */
	return left.zvZ<right.zvZ;
	//return left.zMin<right.zMax;
}

void SortPParams(int first, int count)
{
	if (pvrrc.verts.used() == 0 || count <= 1)
		return;

	Vertex* vtx_base=pvrrc.verts.head();
	u32* idx_base = pvrrc.idx.head();

	PolyParam* pp = &pvrrc.global_param_tr.head()[first];
	PolyParam* pp_end = pp + count;

	while(pp!=pp_end)
	{
		if (pp->count<2)
		{
			pp->zvZ=0;
		}
		else
		{
			u32* idx = idx_base + pp->first;

			Vertex* vtx=vtx_base+idx[0];
			Vertex* vtx_end=vtx_base + idx[pp->count-1]+1;

			u32 zv=0xFFFFFFFF;
			while(vtx!=vtx_end)
			{
				zv = std::min(zv,(u32&)vtx->z);
				vtx++;
			}

			pp->zvZ=(f32&)zv;
		}
		pp++;
	}

	std::stable_sort(pvrrc.global_param_tr.head() + first, pvrrc.global_param_tr.head() + first + count);
}

const static Vertex *vtx_sort_base;

#if 0
/*

	Per triangle sorting experiments

*/

//approximate the triangle area
float area_x2(Vertex* v)
{
	return 2/3*fabs( (v[0].x-v[2].x)*(v[1].y-v[0].y) - (v[0].x-v[1].x)*(v[2].y-v[0].y)) ;
}

//approximate the distance ^2
float distance_apprx(Vertex* a, Vertex* b)
{
	float xd=a->x-b->x;
	float yd=a->y-b->y;

	return xd*xd+yd*yd;
}

//was good idea, but not really working ..
bool Intersect(Vertex* a, Vertex* b)
{
	float a1=area_x2(a);
	float a2=area_x2(b);

	float d = distance_apprx(a,b);

	return (a1+a1)>d;
}

//root for quick-union
u16 rid(std::vector<u16>& v, u16 id)
{
	while(id!=v[id]) id=v[id];
	return id;
}

struct TrigBounds
{
	float xs,xe;
	float ys,ye;
	float zs,ze;
};

//find 3d bounding box for triangle
TrigBounds bound(Vertex* v)
{
	TrigBounds rv = {	std::min(std::min(v[0].x,v[1].x),v[2].x), std::max(std::max(v[0].x,v[1].x),v[2].x),
						std::min(std::min(v[0].y,v[1].y),v[2].y), std::max(std::max(v[0].y,v[1].y),v[2].y),
						std::min(std::min(v[0].z,v[1].z),v[2].z), std::max(std::max(v[0].z,v[1].z),v[2].z),
					};

	return rv;
}

//bounding box 2d intersection
bool Intersect(TrigBounds& a, TrigBounds& b)
{
	return  ( !(a.xe<b.xs || a.xs>b.xe) && !(a.ye<b.ys || a.ys>b.ye) /*&& !(a.ze<b.zs || a.zs>b.ze)*/ );
}


bool operator<(const IndexTrig &left, const IndexTrig &right)
{
	/*
	TrigBounds l=bound(vtx_sort_base+left.id);
	TrigBounds r=bound(vtx_sort_base+right.id);

	if (!Intersect(l,r))
	{
		return true;
	}
	else
	{
		return (l.zs + l.ze) < (r.zs + r.ze);
	}*/

	return minZ(&vtx_sort_base[left.id])<minZ(&vtx_sort_base[right.id]);
}

//Not really working cuz of broken intersect
bool Intersect(const IndexTrig &left, const IndexTrig &right)
{
	TrigBounds l=bound(vtx_sort_base+left.id);
	TrigBounds r=bound(vtx_sort_base+right.id);

	return Intersect(l,r);
}

#endif

//are two poly params the same?
static bool PP_EQ(const PolyParam *pp0, const PolyParam *pp1)
{
	return SortSameDraw(pp0, pp1);
}

static void fill_id(u32 *d, const Vertex *v0, const Vertex *v1, const Vertex *v2,  const Vertex *vb)
{	
	d[0] = (u32)(v0 - vb);
	d[1] = (u32)(v1 - vb);
	d[2] = (u32)(v2 - vb);
}

void GenSorted(int first, int count, std::vector<SortTrigDrawParam>& pidx_sort, std::vector<u32>& vidx_sort,
		bool firstProvoking)
{
	u32 tess_gen=0;

	pidx_sort.clear();

	if (pvrrc.verts.used() == 0 || count == 0)
		return;

	const Vertex * const vtx_base = pvrrc.verts.head();
	const u32 * const idx_base = pvrrc.idx.head();

	const PolyParam * const pp_base = &pvrrc.global_param_tr.head()[first];
	const PolyParam *pp = pp_base;
	const PolyParam * const pp_end = pp + count;
	while (pp->count == 0 && pp < pp_end)
		pp++;
	if (pp == pp_end)
		return;

	vtx_sort_base=vtx_base;

	static u32 vtx_cnt;

	int vtx_count = pvrrc.verts.used() - idx_base[pp->first];
	if ((u32)vtx_count > vtx_cnt)
		vtx_cnt=vtx_count;

#if PRINT_SORT_STATS
	printf("TVTX: %d || %d\n",vtx_cnt,vtx_count);
#endif

	if (vtx_count<=0)
		return;

	//make lists of all triangles, with their pid and vid
	static std::vector<IndexTrig> lst;

	lst.resize(vtx_count*4);


	int pfsti=0;

	while(pp!=pp_end)
	{
		u32 ppid = (u32)(pp - pp_base);

		if (pp->count>2)
		{
			const u32 *idx = idx_base + pp->first;
			u32 flip = 0;

			for (u32 i = 0; i < pp->count - 2; i++)
			{
				const Vertex *v0, *v1, *v2;
				if (flip && firstProvoking)
				{
					// The same winding, starting with the vertex that holds
					// this triangle's colour (strip vertex i, not i + 1).
					v0 = vtx_base + idx[i];
					v1 = vtx_base + idx[i + 2];
					v2 = vtx_base + idx[i + 1];
				}
				else if (flip)
				{
					v0 = vtx_base + idx[i + 1];
					v1 = vtx_base + idx[i];
					v2 = vtx_base + idx[i + 2];
				}
				else
				{
					v0 = vtx_base + idx[i];
					v1 = vtx_base + idx[i + 1];
					v2 = vtx_base + idx[i + 2];
				}
				{
					fill_id(lst[pfsti].id,v0,v1,v2,vtx_base);
					lst[pfsti].pid= ppid ;
					lst[pfsti].z = minZ(vtx_base,lst[pfsti].id);
					pfsti++;
				}

				flip ^= 1;
			}
		}
		pp++;
	}

	u32 aused=pfsti;

	lst.resize(aused);

	//sort them
#if 1
	std::stable_sort(lst.begin(),lst.end());

	//Merge pids/draw cmds if two different pids are actually equal
	if (true)
	{
		for (u32 k=1;k<aused;k++)
		{
			if (lst[k].pid!=lst[k-1].pid)
			{
				if (PP_EQ(&pp_base[lst[k].pid],&pp_base[lst[k-1].pid]))
				{
					lst[k].pid=lst[k-1].pid;
				}
			}
		}
	}
#endif


#if 0
	//tries to optimise draw calls by reordering non-intersecting polygons
	//uber slow and not very effective
	{
		int opid=lst[0].pid;

		for (int k=1;k<aused;k++)
		{
			if (lst[k].pid!=opid)
			{
				if (opid>lst[k].pid)
				{
					//MOVE UP
					for (int j=k;j>0 && lst[j].pid!=lst[j-1].pid && !Intersect(lst[j],lst[j-1]);j--)
					{
						swap(lst[j],lst[j-1]);
					}
				}
				else
				{
					//move down
					for (int j=k+1;j<aused && lst[j].pid!=lst[j-1].pid && !Intersect(lst[j],lst[j-1]);j++)
					{
						swap(lst[j],lst[j-1]);
					}
				}
			}

			opid=lst[k].pid;
		}
	}
#endif

	//re-assemble them into drawing commands
	vidx_sort.resize(aused*3);

	int idx=-1;

	for (u32 i=0; i<aused; i++)
	{
		int pid=lst[i].pid;
		u32* midx = lst[i].id;

		vidx_sort[i*3 + 0]=midx[0];
		vidx_sort[i*3 + 1]=midx[1];
		vidx_sort[i*3 + 2]=midx[2];

		if (idx!=pid /* && !PP_EQ(&pp_base[pid],&pp_base[idx]) */ )
		{
			SortTrigDrawParam stdp = { pp_base + pid, i * 3, 0 };

			if (idx!=-1)
			{
				SortTrigDrawParam& last = pidx_sort.back();
				last.count = stdp.first - last.first;
			}

			pidx_sort.push_back(stdp);
			idx=pid;
		}
	}

	if (!pidx_sort.empty())
	{
		SortTrigDrawParam& last = pidx_sort.back();
		last.count = aused * 3 - last.first;
	}

#if PRINT_SORT_STATS
	printf("Reassembled into %d from %d\n",pidx_sort.size(),pp_end-pp_base);
#endif

	if (tess_gen) DEBUG_LOG(RENDERER, "Generated %.2fK Triangles !", tess_gen / 1000.0);
}

/* Whether a polygon that blends with one minus its alpha can have a pixel
 * at alpha 1, as far as its vertices say: not if its alpha is theirs (or
 * theirs times the texture's) and no vertex has 255 */
static bool PrepassVerticesOpaque(const PolyParam *pp, const Vertex *vtx_base, const u32 *idx_base)
{
	if (!PrepassAlphaFromVertices(pp))
		return true;
	const u32 *idx = idx_base + pp->first;
	for (u32 i = 0; i < pp->count; i++)
		if (vtx_base[idx[i]].col[3] == 255)
			return true;
	return false;
}

void GenPrepass(int first, int count, bool lastPass, std::vector<SortTrigDrawParam>& draws, std::vector<u32>& vidx,
		bool firstProvoking)
{
	/* The polygons that can hide, in their order (order), in runs of
	 * neighbours drawn alike (runs): a model's polygons come one after the
	 * other, so it is the runs that are sorted, few of them. (Kept from one
	 * frame to the next: no allocation once they have grown.) */
	struct Run
	{
		PrepassKey key;
		u32 start, count;	// in order
		u32 tris;		// the most triangles they make
	};
	static std::vector<u32> order;
	static std::vector<Run> runs;

	if (count <= 0 || pvrrc.verts.used() == 0 || !PrepassWanted(lastPass))
		return;
	const Vertex * const vtx_base = pvrrc.verts.head();
	const u32 * const idx_base = pvrrc.idx.head();
	const PolyParam * const pp_base = pvrrc.global_param_tr.head();

	order.clear();
	runs.clear();
	u32 tris = 0;
	for (int n = first; n < first + count; n++)
	{
		const PolyParam *pp = pp_base + n;
		if (!PrepassCandidate(pp, lastPass) || !PrepassVerticesOpaque(pp, vtx_base, idx_base))
			continue;
		const PrepassKey key = PrepassMakeKey(pp, pp->texid);
		if (!runs.empty() && runs.back().start + runs.back().count == order.size() && runs.back().key == key)
		{
			runs.back().count++;
			runs.back().tris += pp->count - 2;
		}
		else
		{
			const Run r = { key, (u32)order.size(), 1, pp->count - 2 };
			runs.push_back(r);
		}
		order.push_back((u32)n);
		tris += pp->count - 2;
	}
	if (runs.empty())
		return;
	std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
		return a.key < b.key || (a.key == b.key && a.start < b.start);
	});

	// the triangles, as GenSorted() makes them of a strip, written in place
	const size_t base = vidx.size();
	vidx.resize(base + (size_t)tris * 3);
	u32 *out = vidx.data() + base;
	u32 *dst = out;
	for (size_t k = 0; k < runs.size(); k++)
	{
		const Run& r = runs[k];
		if (k == 0 || !(r.key == runs[k - 1].key))
		{
			// (a group before it all of whose triangles drew nothing is not kept)
			if (k != 0 && draws.back().count == 0)
				draws.pop_back();
			const SortTrigDrawParam d = { pp_base + order[r.start], (u32)(base + (dst - out)), 0 };
			draws.push_back(d);
		}
		u32 * const runStart = dst;
		for (u32 j = r.start; j < r.start + r.count; j++)
		{
			const PolyParam *pp = pp_base + order[j];
			const u32 *idx = idx_base + pp->first;
			u32 flip = 0;
			for (u32 i = 0; i + 2 < pp->count; i++, flip ^= 1)
			{
				const u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
				// (a triangle with two corners the same draws nothing)
				if (a == b || b == c || a == c)
					continue;
				dst[0] = a;
				if (flip && firstProvoking)
				{
					dst[1] = c;
					dst[2] = b;
				}
				else if (flip)
				{
					dst[0] = b;
					dst[1] = a;
					dst[2] = c;
				}
				else
				{
					dst[1] = b;
					dst[2] = c;
				}
				dst += 3;
			}
		}
		draws.back().count += (u32)(dst - runStart);
	}
	if (draws.back().count == 0)
		draws.pop_back();
	vidx.resize(base + (dst - out));
}
