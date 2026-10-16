#!/bin/sh
# Textures made on the GPU against the CPU's converters. See texdecode_test.c.
#
#   sh tools/rend/texdecode_test.sh          the test
#   sh tools/rend/texdecode_test.sh update   the kernels' SPIR-V
#                                            (core/rend/texdecode_spv.inc) made
#                                            again from core/rend/texdecode.h:
#                                            tools/rend/kernels_spv.sh update
#
# Needs the core built for this machine first (it links core/rend/TexCache.o),
# glslangValidator, and a Vulkan device: Mesa's software one (lavapipe,
# mesa-vulkan-drivers) is used when it is installed. The validation layer
# is on when it is installed; TEXD_VALIDATE=0 turns it off.
#
# Then the same textures on a Direct3D 11 device, with the Direct3D 11
# renderers' kernels (core/rend/texdecode_dxbc.inc, checked to be the
# header's by tools/rend/kernels_dxbc.sh): texdecode_d3d11.c, built with
# mingw-w64 and run under wine (wined3d) and xvfb-run, fed what the test
# writes with "dump". When x86_64-w64-mingw32-gcc or wine is not there it
# is left out; TEXD_D3D11=0 leaves it out too.
#
# Then on a Direct3D 9 device, the same way: the Direct3D 9 renderer's
# shader model 3 shaders (core/rend/texdecode_dx9.inc, checked by
# kernels_dxbc.sh), texdecode_d3d9.c (TEXD_D3D9=0 leaves it out).
#
# And on OpenGL, with no display (EGL, Mesa's llvmpipe): every way the
# OpenGL renderers make them - the SPIR-V (OpenGL 4.6, or 4.5 with
# ARB_gl_spirv), the text compiled for OpenGL 4.3, 3.3 and 3.0, and for
# OpenGL ES 3.0 - texdecode_gl.c (TEXD_GL=0 leaves it out, TEXD_GL_MODES
# says which: "gl46 gl43 gl33 gl30 es30").
set -e
cd "$(dirname "$0")/../.."
if [ "$1" = update ]; then
   exec sh tools/rend/kernels_spv.sh update
fi
WORK=${TMPDIR:-/tmp}/texdecode_test.$$
mkdir -p "$WORK"
[ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
# the SPIR-V the renderer has of the kernels is theirs (and the TA decode's), and so is the DXBC
# (the Direct3D 11 kernels', and the Direct3D 12 renderer's shaders')
sh tools/rend/kernels_spv.sh check
sh tools/rend/kernels_dxbc.sh check
sh tools/rend/kernels_dx12.sh check
[ -f core/rend/TexCache.o ] || { echo "build the core first: core/rend/TexCache.o is not there" >&2; exit 1; }

# the header as C89 and as C++
printf '#include "texdecode.h"\nint main(void) { struct texd_plan p; (void)p; return (int)texd_texel(0, 0, 0, 0, 0, 0); }\n' > "$WORK/c89.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/c89.o" "$WORK/c89.c"
${CXX:-c++} -x c++ -std=c++11 -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/cxx.o" "$WORK/c89.c"

# the kernels, as the renderer has them (tools/rend/kernels_spv.sh's way: rend/texdecode_spv.inc), and the Direct3D one
for f in r8ui r16ui r32ui; do
   { echo "#version 450"; echo "#define TEXD_SHADER_GLSL"; echo "#define TEXD_KERNEL_FORMAT $f"; cat core/rend/texdecode.h; } > "$WORK/k_$f.comp"
   glslangValidator -V -g0 --target-env vulkan1.0 -o "$WORK/k_$f.spv" "$WORK/k_$f.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
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

if [ "${TEXD_D3D11:-1}" != 0 ] && command -v x86_64-w64-mingw32-gcc > /dev/null && command -v wine > /dev/null; then
   x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Icore/rend -Icore/libretro-common/include -Itools/rend \
      -o "$WORK/texdecode_d3d11.exe" tools/rend/texdecode_d3d11.c -ld3d11
   # (wine wants the TMPDIR it was set up with)
   "$WORK/texdecode_test" "$WORK" dump 2> "$WORK/dump.log" \
      | env -u TMPDIR WINEDEBUG=-all xvfb-run -a wine "$WORK/texdecode_d3d11.exe" | tr -d '\r' > "$WORK/d3d11.log" || true
   cat "$WORK/d3d11.log"
   grep -q "^texdecode: all the same" "$WORK/dump.log" || { cat "$WORK/dump.log"; exit 1; }
   grep -q "^texdecode_d3d11: all the same" "$WORK/d3d11.log" || exit 1
fi

if [ "${TEXD_D3D9:-1}" != 0 ] && command -v x86_64-w64-mingw32-gcc > /dev/null && command -v wine > /dev/null; then
   x86_64-w64-mingw32-gcc -std=c99 -O2 -Wall -Icore/rend -Icore/libretro-common/include -Itools/rend \
      -o "$WORK/texdecode_d3d9.exe" tools/rend/texdecode_d3d9.c -ld3d9
   "$WORK/texdecode_test" "$WORK" dump 2> "$WORK/dump9.log" \
      | env -u TMPDIR WINEDEBUG=-all xvfb-run -a wine "$WORK/texdecode_d3d9.exe" | tr -d '\r' > "$WORK/d3d9.log" || true
   cat "$WORK/d3d9.log"
   grep -q "^texdecode: all the same" "$WORK/dump9.log" || { cat "$WORK/dump9.log"; exit 1; }
   grep -q "^texdecode_d3d9: all the same" "$WORK/d3d9.log" || exit 1
fi

if [ "${TEXD_GL:-1}" != 0 ]; then
   # (the kernels' text as C: rend/texdecode_gl.inc has it as raw strings)
   ${CC:-cc} -std=gnu11 -O2 -Wall -Icore/rend -Icore/libretro-common/include -Itools/rend \
      -o "$WORK/texdecode_gl" tools/rend/texdecode_gl.c -lEGL -lGL
   # (the dump made again for each: it is a gigabyte and more)
   for m in ${TEXD_GL_MODES:-gl46 gl43 gl33 gl30 es30}; do
      "$WORK/texdecode_test" "$WORK" dump 2> "$WORK/dumpgl.log" | "$WORK/texdecode_gl" $m > "$WORK/gl_$m.log" 2>&1 || true
      grep -q "^texdecode: all the same" "$WORK/dumpgl.log" || { cat "$WORK/dumpgl.log"; exit 1; }
      cat "$WORK/gl_$m.log"
      grep -q "^texdecode_gl: $m: all the same" "$WORK/gl_$m.log" || exit 1
   done
fi
