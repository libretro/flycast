#!/bin/sh
# GPU-side TA processing's decode in Direct3D 11 against the core's own
# parse, byte for byte, on the frames a game draws: ta_gpu_test.sh's test
# with the Direct3D 11 renderers' kernels (rend/dx11/dx11_ta_dxbc.inc) and
# their way of running them (dx11/dx11_ta_decoder.cpp) for the device, as
# ta_gpu_test_d3d12.sh has it for Direct3D 12.
#
#   sh tools/pvr/ta_gpu_test_d3d11.sh CONTENT FRAMES [key=value ...]
#
# The frames are run twice by ta_gpu_test built with TGT_REPLAY
# (ta_gpu_test_replay.c): the first time every decode asked for is written
# to $WORK/dec and answered with the model's; tools/pvr/ta_gpu_test_d3d11.c,
# built with mingw-w64, decodes them all with Direct3D 11 and writes what
# each made; the second time each decode is answered with that, and
# compared as ta_gpu_test.sh compares a Vulkan device's - the vertices,
# indices and volumes, and what the draws are given: the draw groups'
# commands and the translucent draw records, whose room past each group's
# count has to be zeros (what NVAPI's multi-draw, which the renderers draw
# the records with and which this does not run, draws nothing of). On a
# machine that is not Windows the Direct3D 11 program runs under wine
# (wined3d, feature level 11.0 or later). The variables are ta_gpu_test.sh's
# (HEADLESS_UNDUMP, HEADLESS_DIR, TAG_FLAT, ...), and:
#   CC_WIN=x86_64-w64-mingw32-gcc   what builds the Direct3D 11 program
#   WINE=wine                       what runs it ("" on Windows; a 64-bit
#                                   wine: on Debian and Ubuntu plain wine
#                                   can be the 32-bit loader, which cannot,
#                                   so /usr/lib/wine/wine64 is taken there)
# The core has to be built for this machine first (make platform=unix).
# Disk: each decode's data and outputs, a few MB a frame, in $WORK/dec,
# which goes at the end unless KEEP=1 or $WORK is given.
set -e
cd "$(dirname "$0")/../.."
if [ $# -lt 2 ]; then
   echo "usage: $0 CONTENT FRAMES [key=value ...]" >&2
   exit 2
fi
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/ta_gpu_test_d3d11.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK/dec"
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
if [ -z "${WINE+set}" ]; then
   if [ -x /usr/lib/wine/wine64 ]; then WINE=/usr/lib/wine/wine64; else WINE=wine; fi
fi
sh tools/rend/kernels_dx12.sh check
[ -f core/hw/pvr/ta_vtx.o ] || { echo "build the core first: core/hw/pvr/ta_vtx.o is not there" >&2; exit 1; }

# the program: ta_gpu_test.sh's, with the device another program's
CORE_FLAGS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp core/hw/pvr/ta_vtx.o 2>/dev/null | grep -m1 'core/hw/pvr/ta_vtx.cpp' \
   | sed -e 's/^g++ //' -e 's/ -c / /' -e 's/ -MMD -MP .*$//' -e 's/-DGIT_VERSION=[^ ]* [^ ]*//')
OBJS=$(make -n platform=unix -W core/hw/pvr/ta_vtx.cpp 2>/dev/null | tail -1 | tr ' ' '\n' | grep '\.o$' | grep -v 'glslang.js.o')
${CC:-cc} -O1 -c -o "$WORK/headless.o" tools/threads/headless.c
${CC:-cc} -std=c99 -O2 -Wall -DTGT_REPLAY -iquote core/rend -Icore/libretro-common/include -iquote tools/pvr -c -o "$WORK/test.o" tools/pvr/ta_gpu_test.c
${CXX:-g++} $CORE_FLAGS -Itools/pvr -w -c -o "$WORK/cpu.o" tools/pvr/ta_gpu_test_cpu.cpp
${CXX:-g++} -fopenmp -rdynamic -o "$WORK/ta_gpu_test" "$WORK/headless.o" "$WORK/test.o" "$WORK/cpu.o" $OBJS \
   -Wl,--wrap=dlopen -Wl,--wrap=_exit -Wl,--wrap=_Z13ta_parse_vdrcP10TA_context -lGL -lrt -lpthread -lm -ldl
nm -D --undefined-only "$WORK/ta_gpu_test" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' \
   | python3 tools/threads/headless_gl.py > "$WORK/nogl.c" || true
${CC:-cc} -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"
# ...and the Direct3D 11 one
$CC_WIN -std=c99 -O2 -Wall -Icore/rend -Icore/libretro-common/include -Itools/pvr -o "$WORK/ta_gpu_test_d3d11.exe" \
   tools/pvr/ta_gpu_test_d3d11.c -ld3d11

CONTENT=$1
FRAMES=$2
shift 2
winpath() {
   if [ -n "$WINE" ]; then echo "Z:$1"; else echo "$1"; fi
}
# the decodes, the model's answers
TAG_REPLAY=dump TAG_DIR="$WORK/dec" LD_PRELOAD="$WORK/libGLESv2.so.2" "$WORK/ta_gpu_test" self "$CONTENT" "$FRAMES" \
   reicast_threaded_rendering=disabled "$@" > "$WORK/out.dump" 2> "$WORK/err.dump" || true
echo "ta_gpu_test_d3d11: $(ls "$WORK/dec" | grep -c '\.in$') decodes written"
# ...made by Direct3D 11
# (wine's Direct3D wants a display: a virtual one where there is none)
XVFB=
[ -n "$WINE" ] && [ -z "$DISPLAY" ] && command -v xvfb-run > /dev/null && XVFB="xvfb-run -a"
( cd "$WORK" && $XVFB env -u TMPDIR WINEDEBUG=-all $WINE ./ta_gpu_test_d3d11.exe "$(winpath "$WORK/dec")" ) 2>&1 | tr -d '\r' | grep '^ta_gpu_test_d3d11' || true
# ...and its answers compared
TAG_REPLAY=read TAG_DIR="$WORK/dec" LD_PRELOAD="$WORK/libGLESv2.so.2" "$WORK/ta_gpu_test" self "$CONTENT" "$FRAMES" \
   reicast_threaded_rendering=disabled "$@" > "$WORK/out" 2> "$WORK/err" || true
grep '^softfloat\|^device\|^ta_gpu_test\|^validation' "$WORK/out" | tail -40
[ "$(grep '^ta_gpu_test: PASS$\|^ta_gpu_test: FAIL$' "$WORK/out" | tail -1)" = "ta_gpu_test: PASS" ]
