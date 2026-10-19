// What the per-pixel renderer's fragment shaders share (oit_shaders.cpp),
// after pso_key.h; with SSAA defined, after rend/tile/pvr_lane.h and
// pvr_resolve.h too: tools/rend/kernels_spv.sh.

layout (std140, set = 0, binding = 1) uniform FragmentShaderUniforms
{
	vec4 colorClampMin;
	vec4 colorClampMax;
	vec4 sp_FOG_COL_RAM;
	vec4 sp_FOG_COL_VERT;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
	float shade_scale_factor;
} uniformBuffer;

#ifdef SSAA
layout (constant_id = PSO_SC_SSAA_SAMPLES) const int SSAA_SAMPLES = 4;
layout (constant_id = PSO_SC_SSAA_LOG2N) const uint SSAA_LOG2N = 2u;
#endif

layout(set = 3, binding = 2, r32ui) uniform coherent restrict uimage2D abufferPointerImg;
struct Pixel {
	uint color;
	float depth;
	uint seq_num;
	uint next;
};
#define EOL 0xFFFFFFFFu
/* The fragments, in up to four storage buffers of one size when the
 * device cannot bind them as one (OITBuffers): fragment i is in the
 * first while i is below its length, and so on. A buffer that is not
 * needed is the first again, and never reached. */
layout (set = 3, binding = 0, std430) coherent restrict buffer PixelBuffer_ {
	Pixel pixels[];
} PixelBuffer[4];
layout(set = 3, binding = 1) buffer PixelCounter_ {
	uint buffer_index;
	uint capacity;	// fragments there is room for
} PixelCounter;

Pixel loadPixel(uint i)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return PixelBuffer[0].pixels[i];
	if (i < 2u * n)
		return PixelBuffer[1].pixels[i - n];
	if (i < 3u * n)
		return PixelBuffer[2].pixels[i - 2u * n];
	return PixelBuffer[3].pixels[i - 3u * n];
}

void storePixel(uint i, Pixel pixel)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		PixelBuffer[0].pixels[i] = pixel;
	else if (i < 2u * n)
		PixelBuffer[1].pixels[i - n] = pixel;
	else if (i < 3u * n)
		PixelBuffer[2].pixels[i - 2u * n] = pixel;
	else
		PixelBuffer[3].pixels[i - 3u * n] = pixel;
}

// A fragment's seq_num, changed in place by the translucent modifier volumes
uint pixelAtomicOr(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicOr(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicOr(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicOr(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicOr(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

uint pixelAtomicXor(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicXor(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicXor(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicXor(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicXor(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

uint pixelAtomicAnd(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		return atomicAnd(PixelBuffer[0].pixels[i].seq_num, v);
	if (i < 2u * n)
		return atomicAnd(PixelBuffer[1].pixels[i - n].seq_num, v);
	if (i < 3u * n)
		return atomicAnd(PixelBuffer[2].pixels[i - 2u * n].seq_num, v);
	return atomicAnd(PixelBuffer[3].pixels[i - 3u * n].seq_num, v);
}

void storeSeqNum(uint i, uint v)
{
	const uint n = uint(PixelBuffer[0].pixels.length());
	if (i < n)
		PixelBuffer[0].pixels[i].seq_num = v;
	else if (i < 2u * n)
		PixelBuffer[1].pixels[i - n].seq_num = v;
	else if (i < 3u * n)
		PixelBuffer[2].pixels[i - 2u * n].seq_num = v;
	else
		PixelBuffer[3].pixels[i - 3u * n].seq_num = v;
}

#define ZERO				0
#define ONE					1
#define OTHER_COLOR			2
#define INVERSE_OTHER_COLOR	3
#define SRC_ALPHA			4
#define INVERSE_SRC_ALPHA	5
#define DST_ALPHA			6
#define INVERSE_DST_ALPHA	7

uint getNextPixelIndex()
{
	// (every fragment is counted, those that find no room too: see OITBuffers::CountPass())
	uint index = atomicAdd(PixelCounter.buffer_index, 1);
	if (index >= PixelCounter.capacity)
		// Buffer overflow
		discard;
	
	return index;
}

// A fragment's depth: the position's, which the depth test took (position.glsl)
#define FRAG_DEPTH gl_FragCoord.z
struct PolyParam {
	int tsp_isp_pcw;
	int tsp1;
};
layout (set = 0, binding = 3, std430) readonly buffer TrPolyParamBuffer {
	PolyParam tr_poly_params[];
} TrPolyParam;

#define GET_TSP_FOR_AREA int tsp = area1 ? pp.tsp1 : pp.tsp_isp_pcw;

int getSrcBlendFunc(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 29) & 7;
}

int getDstBlendFunc(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 26) & 7;
}

bool getSrcSelect(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 25) & 1) != 0;
}

bool getDstSelect(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 24) & 1) != 0;
}

int getFogControl(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 22) & 3;
}

bool getUseAlpha(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 20) & 1) != 0;
}

bool getIgnoreTexAlpha(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return ((tsp >> 19) & 1) != 0;
}

int getShadingInstruction(const PolyParam pp, bool area1)
{
	GET_TSP_FOR_AREA
	return (tsp >> 6) & 3;
}

int getDepthFunc(const PolyParam pp)
{
	return (pp.tsp_isp_pcw >> 13) & 7;
}

bool getDepthMask(const PolyParam pp)
{
	return ((pp.tsp_isp_pcw >> 10) & 1) != 1;
}

bool getShadowEnable(const PolyParam pp)
{
	return (pp.tsp_isp_pcw & 1) != 0;
}

/* SSAA: a fragment covers some of its pixel's samples, the eight bits
 * above its polygon's number (which is then under 2^22) */
#ifdef SSAA
#define POLY_NUMBER_MASK 0x003FFFFFu
#define COVERAGE_SHIFT 22
uint getCoverage(const Pixel pixel)
{
	return (pixel.seq_num >> COVERAGE_SHIFT) & 0xFFu;
}
#else
#define POLY_NUMBER_MASK 0x3FFFFFFFu
#endif

uint getPolyNumber(const Pixel pixel)
{
	return pixel.seq_num & POLY_NUMBER_MASK;
}

#define SHADOW_STENCIL 0x40000000u
#define SHADOW_ACC	   0x80000000u

bool isShadowed(const Pixel pixel)
{
	return (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC;
}

bool isTwoVolumes(const PolyParam pp)
{
	return pp.tsp1 != -1;
}

uint packColors(vec4 v)
{
	return (uint(round(v.r * 255.0)) << 24) | (uint(round(v.g * 255.0)) << 16) | (uint(round(v.b * 255.0)) << 8) | uint(round(v.a * 255.0));
}

vec4 unpackColors(uint u)
{
	return vec4(float((u >> 24) & 255) / 255.0, float((u >> 16) & 255) / 255.0, float((u >> 8) & 255) / 255.0, float(u & 255) / 255.0);
}
