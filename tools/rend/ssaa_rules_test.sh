#!/bin/sh
# The shared rules of tiled rendering and SSAA. See ssaa_rules_test.c.
# The test is built as C89 and as C++ (the TA decoder includes the same
# headers) and run. Then, when glslangValidator is installed, the headers
# are compiled as the shaders will read them: Vulkan GLSL 4.50 with
# subgroup operations, OpenGL GLSL 4.30 with and without them, and HLSL
# with shader model 6.0 wave operations and without; and the resolve as the
# Vulkan renderers' last subpass has it (core/rend/tile/pvr_resolve_text.inc,
# its text: checked to be the headers', and compiled as a fragment shader).
#
#   sh tools/rend/ssaa_rules_test.sh update
#        core/rend/tile/pvr_resolve_text.inc made again from the headers
set -e
cd "$(dirname "$0")/../.."
RINC=core/rend/tile/pvr_resolve_text.inc
make_resolve_text() {
   echo '/* rend/tile/pvr_lane.h and pvr_resolve.h as text, for the shaders that resolve: made by tools/rend/ssaa_rules_test.sh update from them. Not to be edited. */'
   echo 'R"PVR_TEXT('
   cat core/rend/tile/pvr_lane.h core/rend/tile/pvr_resolve.h | grep -v '^#include'
   echo ')PVR_TEXT"'
}
if [ "$1" = update ]; then
   make_resolve_text > $RINC
   echo "made $RINC"
   exit 0
fi
WORK=${TMPDIR:-/tmp}/ssaa_rules_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
INC="-Icore/rend -Icore/libretro-common/include"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror -Wdeclaration-after-statement -O2 \
   $INC -o "$WORK/ssaa_rules_test" tools/rend/ssaa_rules_test.c -lm
${CXX:-c++} -x c++ -std=c++11 -Wall -Wextra -Werror -O2 \
   $INC -o "$WORK/ssaa_rules_test_cxx" tools/rend/ssaa_rules_test.c -lm
"$WORK/ssaa_rules_test"
"$WORK/ssaa_rules_test_cxx" >/dev/null

if ! command -v glslangValidator >/dev/null 2>&1; then
   echo "ssaa_rules_test: no glslangValidator, shaders not checked"
   exit 0
fi
G="glslangValidator -I$PWD/core/rend"
{ echo "#version 450"; echo "#define PVR_LANE_SUBGROUP"; cat tools/rend/ssaa_rules_shader.comp; } > "$WORK/vk.comp"
{ echo "#version 430"; echo "#define PVR_LANE_SUBGROUP"; cat tools/rend/ssaa_rules_shader.comp; } > "$WORK/gls.comp"
{ echo "#version 430"; cat tools/rend/ssaa_rules_shader.comp; } > "$WORK/gl.comp"
$G --target-env vulkan1.1 -V -o "$WORK/vk.spv" "$WORK/vk.comp" >"$WORK/log" || { cat "$WORK/log"; exit 1; }
# (a GL driver gets the headers' text put in place: as here)
for k in gls gl; do
   $G -E "$WORK/$k.comp" > "$WORK/${k}_pp.comp"
   glslangValidator "$WORK/${k}_pp.comp" >"$WORK/log" || { cat "$WORK/log"; exit 1; }
done
$G -D -e main -S comp -V --target-env vulkan1.1 -DPVR_LANE_SUBGROUP -o "$WORK/h6.spv" \
   tools/rend/ssaa_rules_shader.hlsl >"$WORK/log" || { cat "$WORK/log"; exit 1; }
$G -D -e main -S comp -V -o "$WORK/h5.spv" tools/rend/ssaa_rules_shader.hlsl >"$WORK/log" || { cat "$WORK/log"; exit 1; }
# the resolve's text, as the renderers put it in their shaders
make_resolve_text > "$WORK/resolve.inc"
cmp -s "$WORK/resolve.inc" $RINC || { echo "FAIL: $RINC is not the headers: run '$0 update'" >&2; exit 1; }
for n in 2 4 8; do
   l=$(( n == 8 ? 3 : n == 4 ? 2 : 1 ))
   { echo "#version 450"; echo "#define PVR_SHADER_GLSL"; echo "#define PVR_LANE_NONE"
     sed -e '1,2d' -e '$d' $RINC
     echo "layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInputMS tex;"
     echo "layout (location = 0) out vec4 FragColor;"
     echo "void main() { uint rb = 0u, ga = 0u; for (int s = 0; s < $n; s++) { uint c = packUnorm4x8(subpassLoad(tex, s)); rb += pvr_resolve_rb(c); ga += pvr_resolve_ga(c); }"
     echo "   FragColor = unpackUnorm4x8(pvr_resolve_join(rb, ga, ${l}u)); }"
   } > "$WORK/resolve$n.frag"
   glslangValidator --target-env vulkan1.0 -V -o "$WORK/resolve$n.spv" "$WORK/resolve$n.frag" >"$WORK/log" || { cat "$WORK/log"; exit 1; }
done
echo "ssaa_rules_test: shaders compile (Vulkan GLSL, GL 4.30 with and without subgroups, HLSL with and without waves, the resolve)"
