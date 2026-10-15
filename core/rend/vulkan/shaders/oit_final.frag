// The per-pixel renderer's last subpass (oit_shaders.cpp), after oit_header.glsl;
// with SSAA and SSAA_PER_SAMPLE defined, of a render pass that is not the last
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

#ifdef SSAA
layout (input_attachment_index = 0, set = 2, binding = 0) uniform subpassInputMS tex;
#else
layout (input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput tex;
#endif

layout (location = 0) out vec4 FragColor;

uint pixel_list[MAX_PIXELS_PER_FRAGMENT];


int fillAndSortFragmentArray(ivec2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int count = 0;
	for (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)
	{
		const Pixel p = loadPixel(idx);
		// Insertion sort. The entry before the first one is not looked at:
		// this used to read pixel_list[-1], and the pixel buffer at whatever
		// index that held, before finding out there was nothing there.
		int j = count - 1;
		while (j >= 0)
		{
			const Pixel jp = loadPixel(pixel_list[j]);
			if (!(jp.depth > p.depth
					|| (jp.depth == p.depth && getPolyNumber(jp) > getPolyNumber(p))))
				break;
			pixel_list[j + 1] = pixel_list[j];
			j--;
		}
		pixel_list[j + 1] = idx;
		idx = p.next;
	}
	return count;
}

/* Blend fragments back-to-front: the @num_frag sorted ones onto @finalColor;
 * with SSAA, those of a sample, @sampleBit */
vec4 blendFragments(int num_frag, vec4 finalColor, uint sampleBit)
{
	vec4 secondaryBuffer = vec4(0.0); // Secondary accumulation buffer
	
	for (int i = 0; i < num_frag; i++)
	{
		const Pixel pixel = loadPixel(pixel_list[i]);
#ifdef SSAA
		if ((getCoverage(pixel) & sampleBit) == 0u)
			continue;
#endif
		const PolyParam pp = TrPolyParam.tr_poly_params[getPolyNumber(pixel)];
		bool area1 = false;
		bool shadowed = false;
		if (isShadowed(pixel))
		{
			if (isTwoVolumes(pp))
				area1 = true;
			else
				shadowed = true;
		}
		vec4 srcColor;
		if (getSrcSelect(pp, area1))
			srcColor = secondaryBuffer;
		else
		{
			srcColor = unpackColors(pixel.color);
			if (shadowed)
				srcColor.rgb *= uniformBuffer.shade_scale_factor;
		}
		vec4 dstColor = getDstSelect(pp, area1) ? secondaryBuffer : finalColor;
		vec4 srcCoef;
		vec4 dstCoef;
		
		int srcBlend = getSrcBlendFunc(pp, area1);
		switch (srcBlend)
		{
			case ZERO:
				srcCoef = vec4(0.0);
				break;
			case ONE:
				srcCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's
				srcCoef = dstColor;
				break;
			case INVERSE_OTHER_COLOR:
				srcCoef = vec4(1.0) - dstColor;
				break;
			case SRC_ALPHA:
				srcCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				srcCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				srcCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				srcCoef = vec4(1.0 - dstColor.a);
				break;
		}
		int dstBlend = getDstBlendFunc(pp, area1);
		switch (dstBlend)
		{
			case ZERO:
				dstCoef = vec4(0.0);
				break;
			case ONE:
				dstCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				dstCoef = srcColor;
				break;
			case INVERSE_OTHER_COLOR:
				dstCoef = vec4(1.0) - srcColor;
				break;
			case SRC_ALPHA:
				dstCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				dstCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				dstCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				dstCoef = vec4(1.0 - dstColor.a);
				break;
		}
		const vec4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);
		if (getDstSelect(pp, area1))
			secondaryBuffer = result;
		else
			finalColor = result;
	}
	
	return finalColor;
	
}

#if defined(SSAA) && !defined(SSAA_PER_SAMPLE)
/* SSAA's last subpass: the list sorted once, put together at each sample,
 * and the samples resolved by the shared rule (rend/tile/pvr_resolve.h):
 * each as the unorm8 the attachment holds, (sum + N/2) / N per channel */
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	int num_frag = fillAndSortFragmentArray(coords);
	uint rb = 0u;
	uint ga = 0u;
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		uint c = packUnorm4x8(blendFragments(num_frag, subpassLoad(tex, s), 1u << s));
		rb += pvr_resolve_rb(c);
		ga += pvr_resolve_ga(c);
	}
	FragColor = unpackUnorm4x8(pvr_resolve_join(rb, ga, SSAA_LOG2N));
}
#elif defined(SSAA)
/* ...and of a render pass that is not the last: each sample put together
 * into the samples the next one draws over */
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	FragColor = blendFragments(fillAndSortFragmentArray(coords), subpassLoad(tex, gl_SampleID), 1u << gl_SampleID);
}
#else
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	// Compute and output final color for the frame buffer
	// Visualize the number of layers in use
	//FragColor = vec4(float(fillAndSortFragmentArray(coords)) / MAX_PIXELS_PER_FRAGMENT * 4, 0, 0, 1);
	FragColor = blendFragments(fillAndSortFragmentArray(coords), subpassLoad(tex), 1u);
}
#endif
