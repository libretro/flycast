#!/bin/sh
# The compute renderer's kernels against their reference on the CPU,
# picture for picture, on frames a game draws. See tile_test.c.
#
#   sh tools/rend/tile_test.sh CONTENT FRAMES [key=value ...]
#        CONTENT run for FRAMES frames with the core options given; some of
#        the frames it draws to the screen written as the compute
#        renderer's words (tile_dump.cpp: TILE_DUMP_EVERY, TILE_DUMP_MAX,
#        TILE_DUMP_FROM), and some of its renders to a texture
#        (TILE_DUMP_RTT, TILE_DUMP_RTT_EVERY), then each drawn by the kernels
#        on the device and by the reference (TILE_SAMPLES, TILE_KEEP), and
#        compared
#   sh tools/rend/tile_test.sh -f FRAME...
#        frames written before, drawn and compared
#
# The frontend is tools/threads/headless.c, so its variables work (a state
# to start from is HEADLESS_UNDUMP=frame:file, HEADLESS_DIR the system
# directory). The core has to be built for this machine first (make
# platform=unix). Needs glslangValidator and a Vulkan device with 64-bit
# integers in shaders: Mesa's software one (lavapipe) is used where it is
# installed. TILE_VALIDATE=1 the validation layer. The programs, kernels
# and frames are made in $WORK (default: a new directory in $TMPDIR), kept
# with KEEP=1 or when $WORK is given. TILE_WATERFALL=1: the walk that takes
# its textures in turns, for a device without non-uniform indexing.
#
# TILE_DEVICE=d3d12: the frames drawn by the Direct3D 12 renderers' kernels
# instead (tile_test_d3d12.c), as HLSL made DXIL by kernels_dx12.sh
# compute-test, which needs DXC=/path/to/dxc; the program built with
# mingw-w64 (CC_WIN, default x86_64-w64-mingw32-gcc) and run under wine
# (WINE, "" on Windows) with vkd3d-proton's d3d12.dll and d3d12core.dll
# from $D3D12_DLLS where that is given.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/tile_test.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
sh tools/rend/kernels_spv.sh check
DEVICE=${TILE_DEVICE:-vulkan}
[ "$DEVICE" = vulkan ] || [ "$DEVICE" = d3d12 ] || { echo "TILE_DEVICE is vulkan or d3d12" >&2; exit 2; }
[ "$DEVICE" = vulkan ] || sh tools/rend/kernels_dx12.sh check

# the header as C89 and as C++
printf '#include "pvr_compute.h"\nint main(void) { return (int)pvrc_fixed(1.0f) + (int)pvrc_fragments_kept(16384u); }\n' > "$WORK/c89.c"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror -Wno-unused-function -Icore/rend/tile -Icore/libretro-common/include -c -o "$WORK/c89.o" "$WORK/c89.c"
${CXX:-c++} -x c++ -std=c++11 -Wall -Wextra -Werror -Wno-unused-function -Icore/rend/tile -Icore/libretro-common/include -c -o "$WORK/cxx.o" "$WORK/c89.c"

# the kernels, as kernels_spv.sh makes them, with nothing fused (PVRC_EXACT)
K_WALK=$(awk '$1 == "#define" && $2 == "PVRC_K_WALK" { print $3 }' core/rend/tile/pvr_compute.h)
k=1
while [ $k -le "$(awk '$1 == "#define" && $2 == "PVRC_KERNELS" { print $3 }' core/rend/tile/pvr_compute.h)" ]; do
   { echo "#version 450"; echo "#define PVR_SHADER_GLSL"; echo "#define PVRC_SHADER_GLSL"; echo "#define PVRC_KERNEL $k"; echo "#define PVRC_EXACT"
     if [ $k = "$K_WALK" ]; then echo "#define PVR_LANE_COUNT 8"; echo "#define PVR_LANE_GROUP_SIZE 256"; else echo "#define PVR_LANE_NONE"; fi
     if [ $k = "$K_WALK" ] && [ -n "$TILE_WATERFALL" ]; then echo "#define PVRC_TEX_WATERFALL"; fi
     for h in core/rend/tile/pvr_lane.h core/rend/tile/pvr_tile.h core/rend/tile/pvr_modvol.h core/rend/tile/pvr_resolve.h core/rend/ssaa_pattern.h core/rend/tile/pvr_compute.h; do
        sed '/^#include/d' $h
     done
   } > "$WORK/k$k.comp"
   env=vulkan1.0; if [ $k = "$K_WALK" ] && [ -n "$TILE_WATERFALL" ]; then env=vulkan1.1; fi
   glslangValidator -V -g0 --target-env $env -S comp -o "$WORK/k$k.spv" "$WORK/k$k.comp" > "$WORK/log" || { cat "$WORK/log"; exit 1; }
   k=$((k + 1))
done

# the comparison: the device and the reference, both without fused operations
${CC:-cc} -std=c89 -pedantic -O2 -ffp-contract=off -Wall -Icore/rend/tile -Icore/libretro-common/include -c -o "$WORK/tile_ref.o" tools/rend/tile_ref.c
${CC:-cc} -std=c99 -O2 -ffp-contract=off -Wall -Icore/rend/tile -Icore/libretro-common/include -c -o "$WORK/tile_test.o" tools/rend/tile_test.c
${CC:-cc} -o "$WORK/tile_test" "$WORK/tile_test.o" "$WORK/tile_ref.o" -lvulkan -lm

LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP

# ...or Direct3D 12's: the kernels for each count of samples, and K (its own,
# pvrc_fragments_kept() of 32 KiB, for 0) the test draws with
if [ "$DEVICE" = d3d12 ]; then
   K_D3D12=$(awk -F= '$1 == "PVRC_KEEP_D3D12" { print $2 }' tools/rend/kernels_dx12.sh)
   mkdir -p "$WORK/dxil"
   for n in ${TILE_SAMPLES:-1 2 4 8}; do
      for k in ${TILE_KEEP:-0 1}; do
         [ "$k" = 0 ] && k=$K_D3D12
         sh tools/rend/kernels_dx12.sh compute-test "$WORK/dxil" $n $k
      done
   done
   ${CC_WIN:-x86_64-w64-mingw32-gcc} -std=c99 -O2 -ffp-contract=off -Wall -Icore/rend/tile -Icore/libretro-common/include \
      -o "$WORK/dxil/tile_test_d3d12.exe" tools/rend/tile_test_d3d12.c tools/rend/tile_ref.c -ld3d12 -ldxguid
   if [ -n "$D3D12_DLLS" ]; then
      cp "$D3D12_DLLS/d3d12.dll" "$D3D12_DLLS/d3d12core.dll" "$WORK/dxil/"
      export WINEDLLOVERRIDES="d3d12,d3d12core=n"
   fi
fi
WINE=${WINE-wine}
winpath() {
   if [ -n "$WINE" ]; then echo "Z:$1"; else echo "$1"; fi
}
# @1... the frames
compare() {
   if [ "$DEVICE" = vulkan ]; then
      "$WORK/tile_test" "$WORK" "$@"
   else
      # (wine's Vulkan, which vkd3d-proton draws with, wants a display: a virtual one where there is none)
      XVFB=
      [ -n "$WINE" ] && [ -z "$DISPLAY" ] && command -v xvfb-run > /dev/null && XVFB="xvfb-run -a"
      w=
      for f in "$@"; do w="$w $(winpath "$f")"; done
      ( cd "$WORK/dxil" && $XVFB env -u TMPDIR WINEDEBUG=-all $WINE ./tile_test_d3d12.exe "$(winpath "$WORK/dxil")" $w ) 2>&1 | grep -v '^X connection'
   fi
}

if [ "$1" = -f ]; then
   shift
   compare "$@" | tee "$WORK/out"
   [ "$(tail -1 "$WORK/out")" = "tile_test: PASS" ]
   exit $?
fi
if [ $# -lt 2 ]; then
   echo "usage: $0 CONTENT FRAMES [key=value ...] | -f FRAME..." >&2
   exit 2
fi
[ -f core/hw/pvr/ta_vtx.o ] || { echo "build the core first: core/hw/pvr/ta_vtx.o is not there" >&2; exit 1; }

# the frames: the frontend, the dump, and the core's objects, with
# ta_parse_vdrc() and the frontend's dlopen() wrapped
CORE_FLAGS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp core/hw/pvr/ta_vtx.o 2>/dev/null | grep -m1 'core/hw/pvr/ta_vtx.cpp' \
   | sed -e 's/^g++ //' -e 's/ -c / /' -e 's/ -MMD -MP .*$//' -e 's/-DGIT_VERSION=[^ ]* [^ ]*//')
OBJS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp 2>/dev/null | tail -1 | tr ' ' '\n' | grep '\.o$' | grep -v 'glslang.js.o')
${CC:-cc} -O1 -c -o "$WORK/headless.o" tools/threads/headless.c
${CXX:-g++} $CORE_FLAGS -w -c -o "$WORK/dump.o" tools/rend/tile_dump.cpp
${CXX:-g++} -fopenmp -rdynamic -o "$WORK/tile_dump" "$WORK/headless.o" "$WORK/dump.o" $OBJS \
   -Wl,--wrap=dlopen -Wl,--wrap=_Z13ta_parse_vdrcP10TA_context -lvulkan -lGL -lrt -lpthread -lm -ldl
nm -D --undefined-only "$WORK/tile_dump" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' \
   | python3 tools/threads/headless_gl.py > "$WORK/nogl.c" || true
${CC:-cc} -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"

CONTENT=$1
FRAMES=$2
shift 2
mkdir -p "$WORK/frames"
TILE_DUMP_DIR="$WORK/frames" LD_PRELOAD="$WORK/libGLESv2.so.2" "$WORK/tile_dump" self "$CONTENT" "$FRAMES" \
   reicast_threaded_rendering=disabled "$@" > "$WORK/dump.out" 2> "$WORK/dump.err" || true
grep '^tile_dump' "$WORK/dump.out" || true
DUMPED=$(ls "$WORK/frames"/frame_*.bin "$WORK/frames"/rtt_*.bin 2>/dev/null || true)
[ -n "$DUMPED" ] || { echo "tile_test: no frames written" >&2; exit 1; }
compare $DUMPED | tee "$WORK/out"
[ "$(tail -1 "$WORK/out")" = "tile_test: PASS" ]
