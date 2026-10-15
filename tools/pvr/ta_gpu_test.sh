#!/bin/sh
# GPU-side TA processing's decode against the core's own parse, byte for
# byte, on the frames a game draws. See ta_gpu_test_cpu.cpp.
#
#   sh tools/pvr/ta_gpu_test.sh update
#        the kernels' SPIR-V (core/rend/ta_gpu_spv.inc) made again from
#        core/rend/ta_gpu.h: tools/rend/kernels_spv.sh update
#   sh tools/pvr/ta_gpu_test.sh CONTENT FRAMES [key=value ...]
#        CONTENT run for FRAMES frames with the core options given, every
#        frame it draws decoded on the device and by the model and compared
#        with the parse; the totals, and PASS or FAIL, at the end
#
# The frontend is tools/threads/headless.c, so its variables work: a save
# state to start from is HEADLESS_UNDUMP=frame:file, HEADLESS_DIR the
# system directory (BIOS and NAOMI ROMs), HEADLESS_AHEAD run-ahead. It is
# linked into one program with the core's own objects, so the core has to
# be built for this machine first (make platform=unix). Needs
# glslangValidator, and a Vulkan device: Mesa's software one (lavapipe,
# mesa-vulkan-drivers) is used where it is installed. The validation layer
# is on where it is installed; TAG_VALIDATE=0 turns it off. TAG_GL=1 runs
# the kernels in OpenGL 4.3 instead, as the OpenGL renderers do (EGL with no
# surface: Mesa's llvmpipe without a GPU), from their SPIR-V for OpenGL or,
# with TAG_GL_SPIRV=0, from their text; the frames are then made as those
# renderers draw them, and TAG_VALIDATE=1 is a debug context whose errors
# fail the test. TAG_FLAT=1 looks
# for flat strips (as SSAA does) and compares the draw groups' single-rate
# bits as well. The program and
# the kernels are made in $WORK (default: a new directory in $TMPDIR),
# kept with KEEP=1 or when $WORK is given.
set -e
cd "$(dirname "$0")/../.."
if [ "$1" = update ]; then
   exec sh tools/rend/kernels_spv.sh update
fi
if [ $# -lt 2 ]; then
   echo "usage: $0 update | CONTENT FRAMES [key=value ...]" >&2
   exit 2
fi
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/ta_gpu_test.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
# the SPIR-V the renderer has of the kernels is theirs (and texdecode's)
sh tools/rend/kernels_spv.sh check
[ -f core/hw/pvr/ta_vtx.o ] || { echo "build the core first: core/hw/pvr/ta_vtx.o is not there" >&2; exit 1; }

# the header as C89 and as C++
printf '#define TAG_IDX_PUT(at, v) ((void)(at), (void)(v))\n#include "ta_gpu.h"\nint main(void) { tag_elem e = tag_elem_none(); (void)e; return (int)tag_fadd(1u, 2u); }\n' > "$WORK/c89.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/c89.o" "$WORK/c89.c"
${CXX:-c++} -x c++ -std=c++11 -Wall -Wextra -Werror -Wno-unused-function -Icore/rend -Icore/libretro-common/include -c -o "$WORK/cxx.o" "$WORK/c89.c"

# the kernels, as the renderer has them (tools/rend/kernels_spv.sh's way: rend/ta_gpu_spv.inc)
k=1
while [ $k -le 14 ]; do
   { echo "#version 450"; echo "#define TAG_SHADER_GLSL"; echo "#define TAG_KERNEL $k"; cat core/rend/ta_gpu.h; } > "$WORK/k$k.comp"
   glslangValidator -V -g0 --target-env vulkan1.0 -o "$WORK/k$k.spv" "$WORK/k$k.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
   k=$((k + 1))
done
echo "ta_gpu: the C89 and C++ header and the 14 kernels compile"
if [ -n "$TAG_GL" ] && [ "$TAG_GL" != 0 ]; then
   # ...for OpenGL as well, as SPIR-V and as the text the renderers compile (tools/rend/kernels_spv.sh's)
   k=1
   while [ $k -le 14 ]; do
      { echo "#version 430"; echo "#define TAG_SHADER_GLSL"; echo "#define TAG_SHADER_GL"; echo "#define TAG_KERNEL $k"; cat core/rend/ta_gpu.h; } > "$WORK/k$k.gl.comp"
      glslangValidator -G -g0 --target-env opengl -S comp -o "$WORK/k$k.gl.spv" "$WORK/k$k.gl.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
      k=$((k + 1))
   done
   DEVICE_FLAGS=-DTGT_GL
   DEVICE_LIBS=-lEGL
   echo "ta_gpu: ...and for OpenGL"
else
   DEVICE_FLAGS=
   DEVICE_LIBS=-lvulkan
fi

# the program: the frontend, the comparison, and the core's objects, with
# ta_parse_vdrc() and the frontend's dlopen() wrapped
CORE_FLAGS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp core/hw/pvr/ta_vtx.o 2>/dev/null | grep -m1 'core/hw/pvr/ta_vtx.cpp' \
   | sed -e 's/^g++ //' -e 's/ -c / /' -e 's/ -MMD -MP .*$//' -e 's/-DGIT_VERSION=[^ ]* [^ ]*//')
OBJS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp 2>/dev/null | tail -1 | tr ' ' '\n' | grep '\.o$' | grep -v 'glslang.js.o')
${CC:-cc} -O1 -c -o "$WORK/headless.o" tools/threads/headless.c
${CC:-cc} -std=c99 -O2 -Wall $DEVICE_FLAGS -iquote core/rend -Icore/libretro-common/include -iquote tools/pvr -c -o "$WORK/test.o" tools/pvr/ta_gpu_test.c
${CXX:-g++} $CORE_FLAGS -Itools/pvr -w -c -o "$WORK/cpu.o" tools/pvr/ta_gpu_test_cpu.cpp
${CXX:-g++} -fopenmp -rdynamic -o "$WORK/ta_gpu_test" "$WORK/headless.o" "$WORK/test.o" "$WORK/cpu.o" $OBJS \
   -Wl,--wrap=dlopen -Wl,--wrap=_Z13ta_parse_vdrcP10TA_context $DEVICE_LIBS -lGL -lrt -lpthread -lm -ldl
# an OpenGL that does nothing, for the core's renderer (headless.sh's)
nm -D --undefined-only "$WORK/ta_gpu_test" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' \
   | python3 tools/threads/headless_gl.py > "$WORK/nogl.c" || true
${CC:-cc} -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"

LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP
if [ -z "$TAG_VALIDATE" ] && [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
   export TAG_VALIDATE=1
fi
export TAG_SPV="$WORK"
CONTENT=$1
FRAMES=$2
shift 2
LD_PRELOAD="$WORK/libGLESv2.so.2" "$WORK/ta_gpu_test" self "$CONTENT" "$FRAMES" reicast_threaded_rendering=disabled "$@" > "$WORK/out" 2> "$WORK/err" || true
grep '^softfloat\|^device\|^ta_gpu_test\|^validation' "$WORK/out" | tail -40
[ "$(grep '^ta_gpu_test: PASS$\|^ta_gpu_test: FAIL$' "$WORK/out" | tail -1)" = "ta_gpu_test: PASS" ]
