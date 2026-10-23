// SSAA: the per-pixel renderer's lists (oit.frag, shaded once a pixel),
// a translucent fragment's depth test at each sample and the depth it is
// kept and sorted with. After rend/tile/pvr_lane.h, rend/ssaa_pattern.h
// (where the samples are) and oit_header.glsl, before oit.frag:
// tools/rend/kernels_spv.sh; tools/rend/oit_depth_test.sh runs it on a
// device.
//
// The depth is a plane across the screen (rend/depth_scale.h): its value
// where gl_FragCoord is, gl_FragCoord.z, and its differences to the next
// pixels (dFdxFine, dFdyFine of it: the 2x2 block's other pixels run too,
// as helpers where the polygon does not cover them) give it at any
// sample. Each sample the polygon covers is tested with the depth there,
// where the opaque pass's depth test took the depth it is tested against.
// The fragment is then kept with its depth at the centroid of the samples
// it is seen at, which is in the triangle. With the centre's depth for all
// of it - extrapolated past the triangle where the triangle only clips the
// pixel - the samples were tested with the wrong depth, and the fragments
// of a model's neighbouring polygons were sorted wrongly at its edges: a
// dotted line of wrong colours (Jet Grind Radio, along the hem of the
// skater's jacket). Seen at every sample, the fragment keeps the centre's
// depth, which is that centroid: the standard patterns are centred.
//
// gl_FragCoord is at the pixel's centre on a device that shades this once
// a pixel, as asked. A device may shade it at each sample instead (Mesa's
// lavapipe does, for a shader that reads a sample of an input
// attachment), with gl_FragCoord at the sample and one sample in the mask:
// the offsets are from where gl_FragCoord is, so the depth is then that
// sample's own, untouched.

// the opaque polygons' depth at sample @s (the test reads it its own way)
#ifndef OIT_DEPTH_AT
layout (input_attachment_index = 0, set = 0, binding = 5) uniform subpassInputMS DepthTex;
#define OIT_DEPTH_AT(s) subpassLoad(DepthTex, s).r
#endif

// sample @s's place in the pixel
vec2 ssaaSamplePlace(int s)
{
	return vec2(float(SSAA_SAMPLE_X16(uint(SSAA_SAMPLES), uint(s))),
			float(SSAA_SAMPLE_Y16(uint(SSAA_SAMPLES), uint(s)))) * (1.0 / 16.0);
}

// the depth - @z where gl_FragCoord is, @dz its differences in x and y - at the centroid of the samples of @seen
highp float oitKeptDepth(uint seen, highp float z, highp vec2 dz)
{
	if (seen == uint((1 << SSAA_SAMPLES) - 1))
		return z;
	const vec2 at = fract(gl_FragCoord.xy);
	vec2 sum = vec2(0.0);
	float n = 0.0;
	for (int s = 0; s < SSAA_SAMPLES; s++)
		if ((seen & (1u << s)) != 0u)
		{
			sum += ssaaSamplePlace(s) - at;
			n += 1.0;
		}
	return n == 0.0 ? z : z + dot(sum / n, dz);
}

/* The samples of @mask at which the depth (@z, @dz) is at or in front of
 * the opaque polygons' (larger is nearer). Where the device says which
 * samples its own depth test passed (VK_EXT_post_depth_coverage: oit.frag)
 * that is used instead, and is exact; this is not, quite: the device
 * interpolates the depth at a sample its own way, and the plane taken
 * from gl_FragCoord.z and its differences comes out a few units in the
 * last place off it (5 at most on lavapipe, shading this once a pixel:
 * tools/rend/oit_depth_test.sh). Where the polygon is the opaque one's
 * plane - a translucent decal on it, drawn again - the test would then
 * miss a fifth of the samples at random. So a depth that close to the
 * opaque one, 16 units of 2^-23 of it, counts as equal: a distance far
 * below anything a game puts between two polygons it means apart. */
#define OIT_DEPTH_NEAR (1.0 - 16.0 / 8388608.0)
uint oitSampleDepthTest(uint mask, highp float z, highp vec2 dz)
{
	const vec2 at = fract(gl_FragCoord.xy);
	uint seen = 0u;
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		const vec2 o = ssaaSamplePlace(s) - at;
		if ((mask & (1u << s)) != 0u && z + dz.x * o.x + dz.y * o.y >= OIT_DEPTH_AT(s) * OIT_DEPTH_NEAR)
			seen |= 1u << s;
	}
	return seen;
}
