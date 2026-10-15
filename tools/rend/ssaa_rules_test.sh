#!/bin/sh
# The shared rules of tiled rendering and SSAA. See ssaa_rules_test.c.
# The test is built as C89 and as C++ (the TA decoder includes the same
# headers) and run. Then, when glslangValidator is installed, the headers
# are compiled as the shaders will read them: Vulkan GLSL 4.50 with
# subgroup operations, OpenGL GLSL 4.30 with and without them, and HLSL
# with shader model 6.0 wave operations and without.
set -e
cd "$(dirname "$0")/../.."
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
echo "ssaa_rules_test: shaders compile (Vulkan GLSL, GL 4.30 with and without subgroups, HLSL with and without waves)"
