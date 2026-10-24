#pragma once
/*
	This file is part of libswirl
*/
#include "license/bsd"

#include "refsw_port.h" // in place of libswirl's rend/gles/gles.h (see README)
#include "refrend_base.h"
#include "hw/pvr/ta_ctx.h" // for Vertex
#include <cmath>
#include <algorithm>


#define MAX_RENDER_WIDTH 32
#define MAX_RENDER_HEIGHT 32
#define MAX_RENDER_PIXELS (MAX_RENDER_WIDTH * MAX_RENDER_HEIGHT)

#define STRIDE_PIXEL_OFFSET MAX_RENDER_WIDTH

#define PARAM_BUFFER_PIXEL_OFFSET   0
#define DEPTH1_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*1)
#define DEPTH2_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*2)
#define STENCIL_BUFFER_PIXEL_OFFSET (MAX_RENDER_PIXELS*3)
#define ACCUM1_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*4)
#define ACCUM2_BUFFER_PIXEL_OFFSET  (MAX_RENDER_PIXELS*5)

/*
    (this core) A triangle with a vertex far out of the screen - a game's
    shadow volume near the eye: Jet Grind Radio's are 1e9 pixels out - is
    set up in double precision from its vertices nearer the screen: its
    half-edge functions (RefswExactEdges) and its plane equations
    (PlaneStepper3). libswirl's float setup takes an edge's constant as
    DY * X1 - DX * Y1, whose rounding at X1 = 1e9 is thousands of pixels,
    and a plane's constant the same way, so such a triangle covered pixels
    the geometry says it does not and had a wrong 1/w. Within REFSW_NEAR
    (2^16) pixels the float setup is kept to the bit: its edges are then
    within about 1/256 pixel of the exact ones, as near as the renderers
    keep a vertex (REFSW_GEO=1 in refsw_frame.cpp holds them to it).
*/
#define REFSW_NEAR 65536.f

static inline bool refsw_far(float x, float y)
{
    return !(fabsf(x) < REFSW_NEAR && fabsf(y) < REFSW_NEAR);
}

static inline bool refsw_far(const Vertex& v1, const Vertex& v2, const Vertex& v3)
{
    return refsw_far(v1.x, v1.y) || refsw_far(v2.x, v2.y) || refsw_far(v3.x, v3.y);
}

// the one of two vertices nearer the screen's origin
static inline bool refsw_nearer(double xa, double ya, double xb, double yb)
{
    return std::max(fabs(xa), fabs(ya)) <= std::max(fabs(xb), fabs(yb));
}

// a triangle's (or quad's) half-edge functions, each taken from its vertex
// nearer the screen, so that a sample's place loses nothing to a vertex
// billions of pixels out; the same sign convention and the same >= 0 test
// as RasterizeTriangle()'s float ones
struct RefswExactEdges
{
    double dx[4], dy[4], x0[4], y0[4];
    int n;

    void Setup(int sgn, const float* X, const float* Y, int count)
    {
        n = count;
        for (int i = 0; i < n; i++)
        {
            int a = i, b = (i + 1) % n;
            dx[i] = sgn * ((double)X[a] - X[b]);
            dy[i] = sgn * ((double)Y[a] - Y[b]);
            if (!refsw_nearer(X[a], Y[a], X[b], Y[b]))
                a = b;
            x0[i] = X[a];
            y0[i] = Y[a];
        }
    }

    bool In(double x, double y) const
    {
        for (int i = 0; i < n; i++)
            if (dx[i] * (y - y0[i]) - dy[i] * (x - x0[i]) < 0)
                return false;
        return true;
    }
};

/*
    Surface equation solver
*/
struct PlaneStepper3
{
    float ddx, ddy;
    float c;
    // (this core) a triangle with a vertex far out: the plane in double,
    // from its vertex nearer the screen (see REFSW_NEAR)
    bool exact;
    double e_ddx, e_ddy, e_a, e_x, e_y;

    void Setup(const Vertex& v1, const Vertex& v2, const Vertex& v3, float v1_a, float v2_a, float v3_a)
    {
        exact = refsw_far(v1, v2, v3);
        if (exact)
        {
            // rotated so that the first vertex is the nearest, which keeps
            // the winding and so every sign below
            const Vertex* v[3] = { &v1, &v2, &v3 };
            const double a[3] = { v1_a, v2_a, v3_a };
            int k = 0;
            for (int i = 1; i < 3; i++)
                if (!refsw_nearer(v[k]->x, v[k]->y, v[i]->x, v[i]->y))
                    k = i;
            const Vertex& p = *v[k];
            const Vertex& q = *v[(k + 1) % 3];
            const Vertex& r = *v[(k + 2) % 3];
            const double pa = a[k], qa = a[(k + 1) % 3], ra = a[(k + 2) % 3];
            const double qx = (double)q.x - p.x, qy = (double)q.y - p.y;
            const double rx = (double)r.x - p.x, ry = (double)r.y - p.y;
            const double Aa = (ra - pa) * qy - (qa - pa) * ry;
            const double Ba = rx * (qa - pa) - qx * (ra - pa);
            const double C = qx * ry - rx * qy;
            e_ddx = -Aa / C;
            e_ddy = -Ba / C;
            e_a = pa;
            e_x = p.x;
            e_y = p.y;
            ddx = ddy = c = 0;
            return;
        }
        float Aa = ((v3_a - v1_a) * (v2.y - v1.y) - (v2_a - v1_a) * (v3.y - v1.y));
        float Ba = ((v3.x - v1.x) * (v2_a - v1_a) - (v2.x - v1.x) * (v3_a - v1_a));

        float C = ((v2.x - v1.x) * (v3.y - v1.y) - (v3.x - v1.x) * (v2.y - v1.y));
        
        ddx = -Aa / C;
        ddy = -Ba / C;

        c = (v1_a - ddx * v1.x - ddy * v1.y);
    }

    __forceinline float Ip(float x, float y) const
    {
        if (exact)
            return (float)(e_a + e_ddx * (x - e_x) + e_ddy * (y - e_y));
        return x * ddx + y * ddy + c;
    }

    __forceinline float Ip(float x, float y, float W) const
    {
        return Ip(x, y) * W;
    }
};

/*
    Interpolation helper
*/
struct IPs3
{
    PlaneStepper3 U;
    PlaneStepper3 V;
    PlaneStepper3 Col[4];
    PlaneStepper3 Ofs[4];

    void Setup(DrawParameters* params, text_info* texture, const Vertex& v1, const Vertex& v2, const Vertex& v3)
    {
        u32 w = 0, h = 0;
        if (texture) {
            w = texture->width;
            h = texture->height;
        }

        U.Setup(v1, v2, v3, v1.u * w * v1.z, v2.u * w * v2.z, v3.u * w * v3.z);
        V.Setup(v1, v2, v3, v1.v * h * v1.z, v2.v * h * v2.z, v3.v * h * v3.z);
        if (params->isp.Gouraud) {
            for (int i = 0; i < 4; i++)
                Col[i].Setup(v1, v2, v3, v1.col[i] * v1.z, v2.col[i] * v2.z, v3.col[i] * v3.z);

            for (int i = 0; i < 4; i++)
                Ofs[i].Setup(v1, v2, v3, v1.vtx_spc[i] * v1.z, v2.vtx_spc[i] * v2.z, v3.vtx_spc[i] * v3.z);
        } else {
            for (int i = 0; i < 4; i++)
                Col[i].Setup(v1, v2, v3, v3.col[i] * v1.z, v3.col[i] * v2.z, v3.col[i] * v3.z);

            for (int i = 0; i < 4; i++)
                Ofs[i].Setup(v1, v2, v3, v3.vtx_spc[i] * v1.z, v3.vtx_spc[i] * v2.z, v3.vtx_spc[i] * v3.z);
        }
    }
};

struct refsw;
struct FpuEntry;

union Color {
    u32 raw;
    u8 bgra[4];
    struct {
        u8 b;
        u8 g;
        u8 r;
        u8 a;
    };
};

struct PixelPipeline {
    // lookup tables
    typedef void(*IspFn)(refsw* backend, float x, float y, float invW, u8 *pb, parameter_tag_t tag);
    typedef bool(*TspFn)(const FpuEntry *entry, float x, float y, float invW, u8 *pb);
    typedef Color(*TextureFetchFn)(const text_info *texture, float u, float v);
    typedef Color(*ColorCombinerFn)(Color base, Color textel, Color offset);
    typedef bool(*BlendingUnitFn)(Color* cb, Color col);

    virtual IspFn GetIsp(RenderMode render_mode, ISP_TSP isp)= 0;
    virtual TspFn GetTsp(ISP_TSP isp, TSP tsp)= 0;
    virtual TextureFetchFn GetTextureFetch(TSP tsp)= 0;
    virtual ColorCombinerFn GetColorCombiner(ISP_TSP isp, TSP tsp)= 0;
    virtual BlendingUnitFn GetBlendingUnit(RenderMode render_mode, TSP tsp) = 0;

    virtual ~PixelPipeline() { }
};

// Used for deferred TSP processing lookups
struct FpuEntry
{
    IPs3 ips;
    DrawParameters params;
    text_info texture;
    PixelPipeline::TspFn tsp;
    PixelPipeline::TextureFetchFn textureFetch;
    PixelPipeline::ColorCombinerFn colorCombiner;
    PixelPipeline::BlendingUnitFn blendingUnit;
};

struct refsw : RefRendInterface
{
    vector<FpuEntry> fpu_entires;
    map<u32, parameter_tag_t> fpu_entires_lookup;

    // Used by layer peeling to determine end of processing
    int PixelsDrawn;
};