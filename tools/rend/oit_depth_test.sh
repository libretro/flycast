#!/bin/sh
# The per-pixel renderer's depth test of a translucent fragment, on a
# device (tools/rend/oit_depth_test.c): a model of small triangles that are
# not coplanar drawn into the depth buffer, then drawn again the way the
# Vulkan per-pixel renderer puts its translucent polygons in its lists.
# Every sample is covered by the same triangle in both passes, so a test
# done right sees all of them:
#
#   1 sample   gl_FragCoord.z, the pixel's centre, which is the sample the
#              depth test took: the lists' test as shipped (oit.frag), to
#              the bit
#   2, 4, 8    SSAA, shaded once a pixel, the shipped text
#              (vulkan/shaders/oit_sample_depth.glsl):
#              - each sample tested by hand with the plane's depth there,
#                from gl_FragCoord.z and its differences to the next
#                pixels (a device without VK_EXT_post_depth_coverage),
#                the depth read as the renderer reads it (an input
#                attachment: lavapipe shades that at each sample, which is
#                exact) and through a sampler (shaded once a pixel, as a
#                GPU shades both): a few units in the last place off the
#                device's own depth, within what the test takes as equal
#              - the samples the device's own depth test passed
#                (VK_EXT_post_depth_coverage), exact by construction
#              and in each, the fragment kept with its depth at the
#              centroid of the samples it is seen at, which has to be
#              within those samples' depths
#
# It also prints what the centre's depth for all the samples, as the
# lists' pass had it before, makes of the same model, and whether the
# device gives gl_FragCoord.z at the centre or at a sample.
#
#   sh tools/rend/oit_depth_test.sh        PASS when nothing is missed or kept outside its samples
#        (exit code 0; a sample count or extension the device lacks is skipped)
#
# Needs glslangValidator and a Vulkan device; Mesa's software one
# (lavapipe) is used where it is installed and VK_ICD_FILENAMES is not
# set - set it to a GPU's to test that. The program and shaders are made
# in $WORK (default: a new directory in $TMPDIR).
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/oit_depth_test.$$
   trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
command -v glslangValidator > /dev/null || { echo "oit_depth_test: glslangValidator is needed" >&2; exit 2; }
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP

${CC:-cc} -std=c99 -O1 -Wall -o "$WORK/oit_depth_test" tools/rend/oit_depth_test.c -lvulkan

RS=core/rend/vulkan/shaders
{ echo "#version 450"; cat $RS/position.glsl; echo "layout (location = 0) in vec4 in_pos;"
  echo "void main() { gl_Position = pvr_position(in_pos); }"; } > "$WORK/model.vert"
cat > "$WORK/a.frag" <<'GLSL'
#version 450
layout (location = 0) out vec4 FragColor;
void main() { FragColor = vec4(1.0); }
GLSL
# what each pass B counts (oit_depth_test.c prints them)
COUNTS='layout (set = 0, binding = 6, std430) buffer Counts { uint c[16]; } counts;
layout (location = 0) out vec4 FragColor;
// (a and b more than @n units in the last place apart)
bool ulpApart(highp float a, highp float b, int n) { return abs(int(floatBitsToUint(a)) - int(floatBitsToUint(b))) > n; }'
# 1 sample: the shipped test, gl_FragCoord.z against the depth
{ echo "#version 450"; echo "layout (constant_id = 0) const int SSAA_SAMPLES = 1;"
  echo "layout (input_attachment_index = 0, set = 0, binding = 5) uniform subpassInput DepthTex;"
  echo "$COUNTS"; cat <<'GLSL'
void main()
{
	const highp float d = subpassLoad(DepthTex).r;
	atomicAdd(counts.c[0], 1u);
	atomicAdd(counts.c[2], 1u);
	if (gl_FragCoord.z < d)
		atomicAdd(counts.c[3], 1u);
	if (gl_FragCoord.z == d)
		atomicAdd(counts.c[4], 1u);
	else
		atomicMax(counts.c[5], uint(abs(int(floatBitsToUint(gl_FragCoord.z)) - int(floatBitsToUint(d)))));
	FragColor = vec4(0.0);
}
GLSL
} > "$WORK/b1.frag"
# SSAA: the shipped text (kernels_spv.sh's "pattern" part, then
# oit_sample_depth.glsl), the depth read @1: as the renderer reads it (an
# input attachment), or through a sampler, which a device that shades the
# other at each sample (lavapipe) shades once a pixel, as a GPU does both
ssaa_frag() {
  echo "#version 450"
  if [ $1 = pdc ]; then
     echo "#extension GL_ARB_post_depth_coverage : require"
     echo "#define PDC"
     echo "layout (early_fragment_tests) in;"
     echo "layout (post_depth_coverage) in;"
  fi
  echo "layout (constant_id = 0) const int SSAA_SAMPLES = 4;"
  if [ $1 != attachment ]; then
     echo "layout (set = 0, binding = 7) uniform sampler2DMS DepthMS;"
     echo "#define OIT_DEPTH_AT(s) texelFetch(DepthMS, ivec2(gl_FragCoord.xy), s).r"
  fi
  echo "#define PVR_SHADER_GLSL"; echo "#define PVR_LANE_NONE"
  sed '/^#include/d' core/rend/tile/pvr_lane.h core/rend/ssaa_pattern.h
  cat $RS/oit_sample_depth.glsl
  echo "$COUNTS"; cat <<'GLSL'
void main()
{
	const highp vec2 dz = vec2(dFdxFine(gl_FragCoord.z), dFdyFine(gl_FragCoord.z));
	const uint mask = uint(gl_SampleMaskIn[0]);
	const bool edge = mask != uint((1 << SSAA_SAMPLES) - 1);
	const vec2 at = fract(gl_FragCoord.xy);
#ifdef PDC
	// (the samples the depth test passed)
	const uint seen = mask;
#else
	const uint seen = oitSampleDepthTest(mask, gl_FragCoord.z, dz);
#endif
	const highp float kept = oitKeptDepth(seen, gl_FragCoord.z, dz);
	atomicAdd(counts.c[0], 1u);
	if (edge)
		atomicAdd(counts.c[1], 1u);
	if (at != vec2(0.5))
		atomicAdd(counts.c[12], 1u);
	atomicAdd(counts.c[2], uint(bitCount(mask)));
	atomicAdd(counts.c[3], uint(bitCount(mask & ~seen)));
	uint exact = 0u, worst = 0u, centreMissed = 0u;
	highp float lo = 3.0, hi = -1.0, clo = 3.0, chi = -1.0;
	bool atSample = false;
	const int first = findLSB(mask);
	for (int s = 0; s < SSAA_SAMPLES; s++)
	{
		if ((mask & (1u << s)) == 0u)
			continue;
		const highp float d = OIT_DEPTH_AT(s);
		const vec2 o = ssaaSamplePlace(s) - at;
		const highp float p = gl_FragCoord.z + dz.x * o.x + dz.y * o.y;
		if (p == d)
			exact++;
		else
			worst = max(worst, uint(abs(int(floatBitsToUint(p)) - int(floatBitsToUint(d)))));
		if (gl_FragCoord.z < d)
			centreMissed++;
		if (gl_FragCoord.z == d)
			atSample = true;
		clo = min(clo, d);
		chi = max(chi, d);
		if ((seen & (1u << s)) != 0u)
		{
			lo = min(lo, d);
			hi = max(hi, d);
		}
	}
	atomicAdd(counts.c[4], exact);
	atomicMax(counts.c[5], worst);
	atomicAdd(counts.c[7], centreMissed);
	if (seen != 0u && ((kept < lo && ulpApart(kept, lo, 8)) || (kept > hi && ulpApart(kept, hi, 8))))
		atomicAdd(counts.c[8], 1u);
	if (edge && ((gl_FragCoord.z < clo && ulpApart(gl_FragCoord.z, clo, 2)) || (gl_FragCoord.z > chi && ulpApart(gl_FragCoord.z, chi, 2))))
		atomicAdd(counts.c[9], 1u);
	if (edge && atSample)
		atomicAdd(counts.c[10], 1u);
	if (edge)
	{
		const vec2 o = ssaaSamplePlace(first) - vec2(0.5);
		const highp float centre = OIT_DEPTH_AT(first) - (dz.x * o.x + dz.y * o.y);
		if (!ulpApart(gl_FragCoord.z, centre, 2))
			atomicAdd(counts.c[11], 1u);
	}
	FragColor = vec4(0.0);
}
GLSL
}
ssaa_frag attachment > "$WORK/bn.frag"
ssaa_frag sampled > "$WORK/bs.frag"
ssaa_frag pdc > "$WORK/bp.frag"
for f in model.vert a.frag b1.frag bn.frag bs.frag bp.frag; do
   glslangValidator -V -g0 --target-env vulkan1.0 -o "$WORK/${f%.*}_${f##*.}.spv" "$WORK/$f" > "$WORK/log" || { cat "$WORK/log" >&2; exit 2; }
done

rc=0
for n in 1 2 4 8; do
   covered=
   for b in bn bs bp; do
      [ $n = 1 ] && [ $b != bn ] && continue
      f="$WORK/${b}_frag.spv"; [ $n = 1 ] && f="$WORK/b1_frag.spv"
      case $b in
      bn) [ $n != 1 ] && echo "-- the depth read as an input attachment, each sample tested by hand (as the renderer has it without VK_EXT_post_depth_coverage)" ;;
      bs) echo "-- the same, the depth read through a sampler (shaded once a pixel on any device)" ;;
      bp) echo "-- the samples the device's depth test passed (VK_EXT_post_depth_coverage, as the renderer has it with that)" ;;
      esac
      set +e
      "$WORK/oit_depth_test" $n "$WORK/model_vert.spv" "$WORK/a_frag.spv" "$f" $([ $b = bp ] && echo pdc) > "$WORK/out"
      got=$?
      set -e
      if [ $n = 1 ]; then grep -v 'centre\|kept\|FragCoord' "$WORK/out"; else cat "$WORK/out"; fi
      [ $got = 3 ] && continue
      [ $got = 0 ] || rc=1
      # (with the depth test's samples, those it sees are all there are: as many as the others cover)
      c=$(sed -n 's/.*samples covered \([0-9]*\).*/\1/p' "$WORK/out")
      if [ -z "$covered" ]; then covered=$c
      elif [ "$c" != "$covered" ]; then echo "FAIL: $c samples seen, $covered covered" >&2; rc=1; fi
   done
done
if [ $rc = 0 ]; then echo "oit_depth_test: PASS"; else echo "oit_depth_test: FAIL"; fi
exit $rc
