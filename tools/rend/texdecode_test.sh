#!/bin/sh
# Textures made on the GPU against the CPU's converters. See texdecode_test.c.
#
#   sh tools/rend/texdecode_test.sh          the test
#   sh tools/rend/texdecode_test.sh update   core/rend/texdecode_text.inc made
#                                            again from core/rend/texdecode.h
#
# Needs the core built for this machine first (it links core/rend/TexCache.o),
# glslangValidator, and a Vulkan device: Mesa's software one (lavapipe,
# mesa-vulkan-drivers) is used when it is installed. The validation layer
# is on when it is installed; TEXD_VALIDATE=0 turns it off.
set -e
cd "$(dirname "$0")/../.."
INC=core/rend/texdecode_text.inc
make_text() {
   echo '/* rend/texdecode.h as text, for the kernels: made by tools/rend/texdecode_test.sh update from it. Not to be edited. */'
   echo 'R"TEXD_TEXT('
   cat core/rend/texdecode.h
   echo ')TEXD_TEXT"'
}
if [ "$1" = update ]; then
   make_text > $INC
   echo "made $INC"
   exit 0
fi
WORK=${TMPDIR:-/tmp}/texdecode_test.$$
mkdir -p "$WORK"
[ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
make_text > "$WORK/text.inc"
cmp -s "$WORK/text.inc" $INC || { echo "FAIL: $INC is not core/rend/texdecode.h: run '$0 update'" >&2; exit 1; }
[ -f core/rend/TexCache.o ] || { echo "build the core first: core/rend/TexCache.o is not there" >&2; exit 1; }

# the header as C89 and as C++
printf '#include "texdecode.h"\nint main(void) { struct texd_plan p; (void)p; return (int)texd_texel(0, 0, 0, 0, 0, 0); }\n' > "$WORK/c89.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/c89.o" "$WORK/c89.c"
${CXX:-c++} -x c++ -std=c++11 -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/cxx.o" "$WORK/c89.c"

# the kernels, as the renderer makes them (vulkan/tex_decoder.cpp), and the Direct3D one
for f in r8ui r16ui r32ui; do
   { echo "#version 450"; echo "#define TEXD_SHADER_GLSL"; echo "#define TEXD_KERNEL_FORMAT $f"; cat core/rend/texdecode.h; } > "$WORK/k_$f.comp"
   glslangValidator -V --target-env vulkan1.1 -o "$WORK/k_$f.spv" "$WORK/k_$f.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
done
{ echo "#version 450"; cat tools/rend/texdecode_sample.comp; } > "$WORK/sample.comp"
glslangValidator -V --target-env vulkan1.1 -o "$WORK/sample.spv" "$WORK/sample.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
glslangValidator -D -e main -S comp -V -I"$PWD/core/rend" -o "$WORK/hlsl.spv" tools/rend/texdecode_kernel.hlsl > "$WORK/log" || { cat "$WORK/log"; exit 1; }
echo "texdecode: kernels compile (Vulkan GLSL r8ui, r16ui, r32ui; HLSL)"

${CC:-cc} -std=c99 -O2 -Wall -iquote core/rend -Icore/libretro-common/include -iquote tools/rend -c -o "$WORK/test.o" tools/rend/texdecode_test.c
${CXX:-g++} -I./core/libretro -I./core -I./core/deps -I./core/libretro-common/include -I./core/deps/khronos -Itools/rend \
   -DTARGET_LINUX_x64 -DHOST_CPU=0x20000004 -D__LIBRETRO__ -DHAVE_GLSYM_PRIVATE -DNO_VERIFY -DNDEBUG \
   -DHAVE_GL3 -DHAVE_GL4 -DCORE -DHAVE_TEXUPSCALE -DHAVE_OPENGL -DHAVE_VULKAN -O2 -fopenmp -w \
   -c -o "$WORK/cpu.o" tools/rend/texdecode_cpu.cpp
${CXX:-g++} -fopenmp -o "$WORK/texdecode_test" "$WORK/test.o" "$WORK/cpu.o" core/rend/TexCache.o core/deps/xxhash/xxhash.o \
   -lvulkan -Wl,--unresolved-symbols=ignore-all

LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP
if [ -z "$TEXD_VALIDATE" ] && [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
   export TEXD_VALIDATE=1
fi
"$WORK/texdecode_test" "$WORK"
