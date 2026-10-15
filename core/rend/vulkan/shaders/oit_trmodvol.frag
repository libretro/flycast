// The per-pixel renderer's translucent modifier volumes (oit_shaders.cpp), after oit_header.glsl,
// with MV_MODE defined (0 to 3)
/* A pixel's fragments sorted, at most. With SSAA a translucent fragment
 * is kept once a pixel with the samples it covers, so where a polygon's
 * triangles share an edge that crosses the pixel, each of them has one
 * there: small triangles - a character's - give a pixel two or three for
 * each layer, and the layers kept are three times as many. */
#ifdef SSAA
#define MAX_PIXELS_PER_FRAGMENT 96
#else
#define MAX_PIXELS_PER_FRAGMENT 32
#endif

// Must match ModifierVolumeMode enum values
#define MV_XOR		 0
#define MV_OR		 1
#define MV_INCLUSION 2
#define MV_EXCLUSION 3

void main()
{
#ifdef SSAA
	/* Once a pixel, where the volume covers the pixel's first sample: two
	 * of its triangles that share an edge cover each sample once between
	 * them, but may both have a fragment at a pixel the edge crosses */
	if ((gl_SampleMaskIn[0] & 1) == 0)
		return;
#endif
#if MV_MODE == MV_XOR || MV_MODE == MV_OR
	setFragDepth();
#endif
	ivec2 coords = ivec2(gl_FragCoord.xy);
	
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int list_len = 0;
	while (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)
	{
		const Pixel pixel = loadPixel(idx);
		const PolyParam pp = TrPolyParam.tr_poly_params[getPolyNumber(pixel)];
		if (getShadowEnable(pp))
		{
#if MV_MODE == MV_XOR
			if (gl_FragDepth >= pixel.depth)
				pixelAtomicXor(idx, SHADOW_STENCIL);
#elif MV_MODE == MV_OR
			if (gl_FragDepth >= pixel.depth)
				pixelAtomicOr(idx, SHADOW_STENCIL);
#elif MV_MODE == MV_INCLUSION
			uint prev_val = pixelAtomicAnd(idx, ~(SHADOW_STENCIL));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)
				storeSeqNum(idx, bitfieldInsert(pixel.seq_num, 1u, 31, 1));
#elif MV_MODE == MV_EXCLUSION
			uint prev_val = pixelAtomicAnd(idx, ~(SHADOW_STENCIL|SHADOW_ACC));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)
				storeSeqNum(idx, bitfieldInsert(pixel.seq_num, 1u, 31, 1));
#endif
		}
		idx = pixel.next;
		list_len++;
	}
}
