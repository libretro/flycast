#!/bin/sh
# Frames a game draws, drawn by the reference software renderer
# (tools/rend/refsw: libswirl's refsw, which models the chip's CORE at the
# pixel level), as the oracle the renderers are compared against.
#
#   sh tools/rend/refsw_test.sh CONTENT FRAMES [key=value ...]
#        CONTENT run for FRAMES frames with the core options given, as
#        tools/threads/headless.c runs it (HEADLESS_UNDUMP=frame:file loads
#        a state before that frame, HEADLESS_DIR is the system directory);
#        the renders chosen by REFSW_FROM (the first render dumped, default
#        3), REFSW_EVERY (default 1) and REFSW_MAX (default 1) are written
#        as frames (tools/rend/refsw/refsw_dump.h) and each is drawn into
#        $WORK/refsw_NNNNN.ppm (and .png where ImageMagick is installed)
#   sh tools/rend/refsw_test.sh -f FRAME.bin [X,Y ...]
#        a frame written before, drawn; each X,Y pixel probed (every
#        triangle covering it, with its interpolated 1/w, and the tile
#        buffers after each list: refsw_frame.cpp)
#   sh tools/rend/refsw_test.sh -b
#        only build the programs
#
# The core has to be built for this machine first (make platform=unix): the
# dumper links its objects, the renderer links its texture cache. The
# programs and frames are made in $WORK (default: a new directory in
# $TMPDIR), kept with KEEP=1 or when $WORK is given.
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/refsw_test.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
[ -f core/rend/TexCache.o ] || { echo "build the core first: core/rend/TexCache.o is not there" >&2; exit 1; }

CORE_DEFS="-DTARGET_LINUX_x64 -DHOST_CPU=0x20000004 -D__LIBRETRO__ -DHAVE_GLSYM_PRIVATE -DNO_VERIFY -DNDEBUG -DHAVE_GL3 -DHAVE_GL4 -DCORE -DHAVE_TEXUPSCALE -DHAVE_OPENGL -DHAVE_VULKAN"
CORE_INC="-I./core/libretro -I./core -I./core/deps -I./core/libretro-common/include -I./core/deps/khronos"

# the renderer: refsw as it is, this core's texture cache, the frame walk
if [ ! -x "$WORK/refsw_frame" ] || [ -n "$REFSW_REBUILD" ]; then
   for f in refsw refsw_pixel refsw_frame; do
      ${CXX:-g++} $CORE_INC -Itools/rend/refsw $CORE_DEFS -std=c++11 -O2 -ffp-contract=off -fno-strict-aliasing -fno-rtti -fopenmp -w \
         -c -o "$WORK/$f.o" tools/rend/refsw/$f.cpp
   done
   ${CXX:-g++} -fopenmp -o "$WORK/refsw_frame" "$WORK/refsw.o" "$WORK/refsw_pixel.o" "$WORK/refsw_frame.o" \
      core/rend/TexCache.o core/deps/xxhash/xxhash.o -lm -Wl,--unresolved-symbols=ignore-all
fi

draw() {
   f=$1; shift
   out=$(dirname "$f")/$(basename "$f" .bin)
   "$WORK/refsw_frame" "$f" "$out.ppm" "$@" | tee "$out.txt"
   if command -v convert > /dev/null; then
      convert "$out.ppm" "$out.png" && echo "refsw_test: $out.png"
   fi
}

if [ "$1" = -b ]; then
   exit 0
fi
if [ "$1" = -f ]; then
   shift
   draw "$@"
   exit 0
fi
if [ $# -lt 2 ]; then
   echo "usage: $0 CONTENT FRAMES [key=value ...] | -f FRAME.bin [X,Y ...] | -b" >&2
   exit 2
fi
[ -f core/hw/pvr/ta_vtx.o ] || { echo "build the core first: core/hw/pvr/ta_vtx.o is not there" >&2; exit 1; }

# the dumper: the frontend, the dump and the core's objects, with
# ta_parse_vdrc() and the frontend's dlopen() wrapped (tile_test.sh's way)
if [ ! -x "$WORK/tile_dump" ]; then
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
fi

CONTENT=$1
FRAMES=$2
shift 2
mkdir -p "$WORK/frames"
REFSW_DUMP_DIR="$WORK/frames" TILE_DUMP_FROM=${REFSW_FROM:-3} TILE_DUMP_EVERY=${REFSW_EVERY:-1} TILE_DUMP_MAX=${REFSW_MAX:-1} \
   LD_PRELOAD="$WORK/libGLESv2.so.2" "$WORK/tile_dump" self "$CONTENT" "$FRAMES" \
   reicast_threaded_rendering=disabled reicast_ta_processing=cpu "$@" > "$WORK/dump.out" 2> "$WORK/dump.err" || true
grep '^tile_dump\|^state' "$WORK/dump.out" || true
ls "$WORK/frames"/refsw_*.bin > /dev/null 2>&1 || { echo "refsw_test: no frames written" >&2; tail -5 "$WORK/dump.err"; exit 1; }
for f in "$WORK/frames"/refsw_*.bin; do
   draw "$f"
done
