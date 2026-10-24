#!/bin/sh
# The compute renderer's kernels timed on frames tile_dump wrote, one set of
# kernels against another (tile_bench.c):
#
#   sh tools/rend/tile_bench.sh SOURCE[,SOURCE...] FRAME...
#
# A SOURCE is a git revision of this repository (its kernels' headers as
# that revision has them) or "." (the working tree's); each is made into
# SPIR-V as kernels_spv.sh makes the renderer's, and the sets draw every
# frame in turn. BENCH_SAMPLES, BENCH_ROUNDS, BENCH_KEEP and BENCH_PPM as
# tile_bench.c says; WORK as tile_test.sh's; BENCH_BIN an already built
# tile_bench (its sets still made). Mesa's software device (lavapipe) is
# used where it is installed: its times are a CPU's, not a GPU's, and say
# only how one set compares with another on the same frames.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/tile_bench.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
[ $# -ge 2 ] || { echo "usage: $0 SOURCE[,SOURCE...] FRAME..." >&2; exit 2; }
HEADERS="core/rend/tile/pvr_lane.h core/rend/tile/pvr_tile.h core/rend/tile/pvr_modvol.h core/rend/tile/pvr_resolve.h core/rend/ssaa_pattern.h core/rend/tile/pvr_compute.h"
SETS=
i=0
for src in $(echo "$1" | tr ',' ' '); do
   d="$WORK/set$i"
   mkdir -p "$d/core/rend/tile"
   for h in $HEADERS; do
      if [ "$src" = . ]; then cp "$h" "$d/$h"; else git show "$src:$h" > "$d/$h"; fi
   done
   kernels=$(awk '$1 == "#define" && $2 == "PVRC_KERNELS" { print $3 }' "$d/core/rend/tile/pvr_compute.h")
   walk=$(awk '$1 == "#define" && $2 == "PVRC_K_WALK" { print $3 }' "$d/core/rend/tile/pvr_compute.h")
   k=1
   while [ $k -le "$kernels" ]; do
      { echo "#version 450"; echo "#define PVR_SHADER_GLSL"; echo "#define PVRC_SHADER_GLSL"; echo "#define PVRC_KERNEL $k"
        if [ $k = "$walk" ]; then echo "#define PVR_LANE_COUNT 8"; echo "#define PVR_LANE_GROUP_SIZE 256"; else echo "#define PVR_LANE_NONE"; fi
        for h in $HEADERS; do sed '/^#include/d' "$d/$h"; done
      } > "$d/k$k.comp"
      glslangValidator -V -g0 --target-env vulkan1.0 -S comp -o "$d/k$k.spv" "$d/k$k.comp" > "$d/log" || { cat "$d/log"; exit 1; }
      k=$((k + 1))
   done
   echo "tile_bench: set $i is $src"
   SETS="$SETS${SETS:+,}$d"
   i=$((i + 1))
done
shift
[ -n "$BENCH_BIN" ] || ${CC:-cc} -std=c99 -D_GNU_SOURCE -O2 -Wall -Wno-unused-function -Icore/rend/tile -Icore/libretro-common/include -o "$WORK/tile_bench" tools/rend/tile_bench.c tools/rend/tile_ref.c -lvulkan -lm
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP
# (lavapipe: the kernels run in this thread, so that the CPU time is the
# work's, whatever else the machine runs; and malloc's thresholds fixed,
# for its compute shaders' large frames would otherwise come from mmap()
# or not as glibc's moving threshold has it, which can change a walk's
# time several times over from one run to the next)
export LP_NUM_THREADS=${LP_NUM_THREADS-0}
export GLIBC_TUNABLES=${GLIBC_TUNABLES-glibc.malloc.mmap_threshold=33554432:glibc.malloc.trim_threshold=1073741824}
"${BENCH_BIN:-$WORK/tile_bench}" "$SETS" "$@"
