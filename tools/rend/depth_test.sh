#!/bin/sh
# The raster renderers' depth (rend/depth_scale.h), shown on a device: a
# pass that tests EQUAL against another pass's depth - the per-pixel
# renderers' colour pass over their depth pass, a polygon drawn again for
# its shadow - is exact when the depth is the position's and the same
# vertex text places the vertices, and is not when each pass writes its
# own depth from its fragment shader and the second pass's comes out a
# unit in the last place off, as it does where a driver compiles each
# pipeline apart. tools/rend/depth_test.c draws the cases:
#
#   1. the depth the position's (vulkan/shaders/position.glsl, as shipped),
#      the second pass another pipeline (another fragment shader, blending
#      on): no pixel missed
#   2. the old way, both passes writing log2(1 + 100000/w)/34 from the
#      fragment shader, the same text: none missed on a software device,
#      which rounds every pipeline alike
#   3. the old way with the second pass's depth a unit in the last place
#      off: pixels missed - the black specks
#   4. the position's depth with the second pass's position a unit off:
#      pixels missed - why the position is one text, declared invariant
#   5. the shipped modules (vulkan/raster_spv.inc): every vertex module's
#      position invariant, no fragment module writing the depth
#
#   sh tools/rend/depth_test.sh        PASS when 1, 2 and 5 miss nothing and 3 and 4 do
#
# Needs glslangValidator and a Vulkan device; Mesa's software one
# (lavapipe) is used where it is installed. The program and shaders are
# made in $WORK (default: a new directory in $TMPDIR).
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/depth_test.$$
   trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
command -v glslangValidator > /dev/null || { echo "depth_test: glslangValidator is needed" >&2; exit 2; }
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP

${CC:-cc} -std=c99 -O1 -Wall -o "$WORK/depth_test" tools/rend/depth_test.c -lvulkan

RS=core/rend/vulkan/shaders
# the position's depth, by the shipped text
{ echo "#version 450"; cat $RS/position.glsl; echo "layout (location = 0) in vec4 in_pos;"
  echo "void main() { gl_Position = pvr_position(in_pos); }"; } > "$WORK/new.vert"
# ...and a unit in the last place off
{ echo "#version 450"; sed 's/vpos.z = min(uniformBuffer.depth_scale \* uniformBuffer.depth_mul, vpos.w);/vpos.z = min(uintBitsToFloat(floatBitsToUint(uniformBuffer.depth_scale * uniformBuffer.depth_mul) + 1u), vpos.w);/' $RS/position.glsl
  echo "layout (location = 0) in vec4 in_pos;"
  echo "void main() { gl_Position = pvr_position(in_pos); }"; } > "$WORK/new_ulp.vert"
grep -q uintBitsToFloat "$WORK/new_ulp.vert" || { echo "depth_test: position.glsl is not what this expects" >&2; exit 2; }
cat > "$WORK/a.frag" <<'GLSL'
#version 450
layout (location = 0) out vec4 FragColor;
void main() { FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$WORK/b.frag" <<'GLSL'
#version 450
layout (location = 0) out vec4 FragColor;
void main() { FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
# the old way: the position's z left at 1, the depth from the fragment shader
cat > "$WORK/old.vert" <<'GLSL'
#version 450
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms { mat4 normal_matrix; float depth_scale; float depth_mul; } uniformBuffer;
layout (location = 0) in vec4 in_pos;
void main()
{
	vec4 vpos = uniformBuffer.normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w;
	vpos.xy *= vpos.w;
	gl_Position = vpos;
}
GLSL
old_frag() {
   echo "#version 450"
   echo "layout (location = 0) out vec4 FragColor;"
   echo "void main() {"
   echo "	float w = gl_FragCoord.w * 100000.0;"
   echo "	float d = log2(1.0 + max(w, -0.999999)) / 34.0;"
   [ "$2" = ulp ] && echo "	d = uintBitsToFloat(floatBitsToUint(d) + 1u);"
   echo "	gl_FragDepth = d;"
   echo "	FragColor = $1;"
   echo "}"
}
old_frag "vec4(1.0, 0.0, 0.0, 1.0)" > "$WORK/old_a.frag"
old_frag "vec4(0.0, 1.0, 0.0, 1.0)" > "$WORK/old_b.frag"
old_frag "vec4(0.0, 1.0, 0.0, 1.0)" ulp > "$WORK/old_b_ulp.frag"
for f in new.vert new_ulp.vert a.frag b.frag old.vert old_a.frag old_b.frag old_b_ulp.frag; do
   glslangValidator -V -g0 --target-env vulkan1.0 -o "$WORK/${f%.*}_${f##*.}.spv" "$WORK/$f" > "$WORK/log" || { cat "$WORK/log" >&2; exit 2; }
done

rc=0
run() {
   # @1 the case, @2 what is expected (0: nothing missed, 1: specks), then the modules
   want=$2; name=$1; shift 2
   set +e
   "$WORK/depth_test" "$@" > "$WORK/out"
   got=$?
   set -e
   printf '%s: ' "$name"; tail -n 1 "$WORK/out"
   if [ $got -gt 1 ]; then echo "FAIL: $name: the program failed" >&2; rc=1
   elif [ $got != $want ]; then echo "FAIL: $name: expected $([ $want = 0 ] && echo nothing missed || echo specks)" >&2; rc=1; fi
}
run "1 position's depth, another pipeline" 0 "$WORK/new_vert.spv" "$WORK/a_frag.spv" "$WORK/new_vert.spv" "$WORK/b_frag.spv"
run "2 fragment depth, the same text" 0 "$WORK/old_vert.spv" "$WORK/old_a_frag.spv" "$WORK/old_vert.spv" "$WORK/old_b_frag.spv"
run "3 fragment depth, the second pass's a unit off" 1 "$WORK/old_vert.spv" "$WORK/old_a_frag.spv" "$WORK/old_vert.spv" "$WORK/old_b_ulp_frag.spv"
run "4 position's depth, the second pass's position a unit off" 1 "$WORK/new_vert.spv" "$WORK/a_frag.spv" "$WORK/new_ulp_vert.spv" "$WORK/b_frag.spv"
set +e
"$WORK/depth_test" -s > "$WORK/out"; got=$?
set -e
printf '5 shipped modules: '; tail -n 1 "$WORK/out"
[ $got = 0 ] || rc=1
if [ $rc = 0 ]; then echo "depth_test: PASS"; else echo "depth_test: FAIL"; fi
exit $rc
