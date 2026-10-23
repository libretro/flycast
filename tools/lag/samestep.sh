#!/bin/sh
# Do the renderers show each frame's picture on the same retro_run?
#
#   samestep.sh CORE CONTENT STATE FRAMES [key=value ...]
#
# The game is run from STATE ("-" for none) for FRAMES frames with each of
# the renderers in RENDERERS, and the picture each hands the frontend after
# every frame from the 5th is held against the first renderer's: each has
# to be nearest the first's picture of the same frame, not of the frame
# before or after (lagmeter --align; the renderers do not draw alike to the
# pixel). The machine's memory has to be the same after every frame in all
# of them too: a renderer must not change what the game does.
#
# RENDERERS  the renderers, each "API-METHOD[-threaded]" (default
#            "vk-pp vk-pt vk-compute vk-pp-threaded gl-pt gl-pp gl-pp-threaded"):
#            vk Vulkan, gl OpenGL; pp per-pixel, pt per-triangle, compute
#            Compute
# BIOS, WORK, CC, NM as for lagmeter.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CONTENT=$2
STATE=$3
FRAMES=$4
[ -n "$FRAMES" ] || { echo "usage: $0 CORE CONTENT STATE FRAMES [key=value ...]" >&2; exit 2; }
shift 4
WORK=${WORK:-/tmp/samestep}
RENDERERS=${RENDERERS:-"vk-pp vk-pt vk-compute vk-pp-threaded gl-pt gl-pp gl-pp-threaded"}
mkdir -p "$WORK"
[ "$STATE" = - ] && STATE=
"${CC:-cc}" -O2 -I"$HERE/../../core/libretro-common/include" -o "$WORK/lagmeter" "$HERE/lagmeter.c" -ldl -lEGL -lGL
LAG_SYMS=$("${NM:-nm}" "$CORE" | awk '$3=="retro_init"{b=$1} $3=="pvr_regs"{r=$1} $3=="FrameCount"{c=$1} END{if(b&&r&&c)print "base="b" regs="r" count="c}')
LAG_STATE=$STATE
LAG_SHOT=$(seq -s, 5 $((FRAMES - 1)))
export LAG_SYMS LAG_STATE
first=
bad=0
keep=
for r in $RENDERERS; do
   case $r in vk-*) video=vulkan;; *) video=gl;; esac
   case $r in *-pt*) sort="per-triangle (normal)";; *) sort="per-pixel (accurate)";; esac
   case $r in *-compute*) method=compute;; *) method=rasterizer;; esac
   case $r in *-threaded) threaded=enabled;; *) threaded=disabled;; esac
   rm -rf "$WORK/sys"
   mkdir -p "$WORK/sys/dc"
   if [ -n "$BIOS" ]; then
      for f in "$BIOS"/*; do
         [ -f "$f" ] && ln -s "$(cd "$(dirname "$f")" && pwd)/$(basename "$f")" "$WORK/sys/dc/"
      done
   fi
   LAG_DIR=$WORK/sys LAG_VIDEO=$video LAG_TRACE=$WORK/$r.trace LAG_SHOT=$LAG_SHOT:$WORK/${r}_ \
      "$WORK/lagmeter" "$CORE" "$CONTENT" "$FRAMES" "reicast_alpha_sorting=$sort" "reicast_rendering_method=$method" \
      "reicast_threaded_rendering=$threaded" "$@" > "$WORK/$r.log" 2>&1 || { echo "$r: did not run (see $WORK/$r.log)"; bad=1; continue; }
   if [ -z "$first" ]; then
      first=$r
      continue
   fi
   printf '%s: ' "$r"
   if "$WORK/lagmeter" --align "$WORK/${first}_" "$WORK/${r}_" 5 $((FRAMES - 1)) > "$WORK/$r.align"; then
      echo "in step"
      rm -f "$WORK/${r}_"*.ppm
   else
      # (a picture drawn unlike the first renderer's can be nearest another
      # frame of it too: the pictures are kept, to be looked at)
      cat "$WORK/$r.align"
      echo "   pictures kept: $WORK/${r}_N.ppm against $WORK/${first}_N.ppm"
      bad=1
      keep=1
   fi
   awk '{print $4}' "$WORK/$first.trace" > "$WORK/mem_a"
   awk '{print $4}' "$WORK/$r.trace" > "$WORK/mem_b"
   cmp -s "$WORK/mem_a" "$WORK/mem_b" || { echo "   and the machine's memory is not $first's"; bad=1; }
done
[ -n "$keep" ] || rm -f "$WORK/${first}_"*.ppm
rm -f "$WORK/mem_a" "$WORK/mem_b"
rm -rf "$WORK/sys"
exit $bad
