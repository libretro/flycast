#!/bin/sh
# The core's input lag, in frames (lagmeter.c says how).
#
#   lagmeter.sh CORE CONTENT STATE PRESS ID FRAMES [key=value ...]
#
# STATE is a state of that content ("-" for none: from the start, and PRESS
# counts from there); a state RetroArch wrote (compressed, or with its own
# header) will do. The game is run twice for FRAMES frames: without a
# press, and with the first pad's button ID (a RETRO_DEVICE_ID_JOYPAD_) held
# from frame PRESS. One line comes out:
#
#   press N  read R  drawn D  shown S  present P  |  lag P-N  game S-N  core P-S
#
# key=value are core options. The environment:
#   VIDEO  gl or vulkan (default gl); the renderer option has to match
#   POLL   early, normal or late (default): when the frontend reads the pad
#   BIOS   a directory with the BIOS files (naomi.zip, dc_boot.bin, ...):
#          each run gets a new system directory with them in its dc/, so
#          that what one run saves (a NAOMI's settings, a VMU) is not what
#          the next one starts from
#   WORK   where to work (default /tmp/lagmeter)
#   CC, NM the compiler, nm
#   SHOT   frames to write pictures of, for both runs: "F,F,..."
#   KEEP   set: keep the traces (WORK/a.trace, WORK/b.trace)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
CONTENT=$2
STATE=$3
PRESS=$4
ID=$5
FRAMES=$6
[ -n "$FRAMES" ] || { echo "usage: $0 CORE CONTENT STATE PRESS ID FRAMES [key=value ...]" >&2; exit 2; }
shift 6
WORK=${WORK:-/tmp/lagmeter}
mkdir -p "$WORK"
[ "$STATE" = - ] && STATE=
"${CC:-cc}" -O2 -I"$HERE/../../core/libretro-common/include" -o "$WORK/lagmeter" "$HERE/lagmeter.c" -ldl -lEGL -lGL
syms=$("${NM:-nm}" "$CORE" | awk '$3=="retro_init"{b=$1} $3=="pvr_regs"{r=$1} $3=="FrameCount"{c=$1} END{if(b&&r&&c)print "base="b" regs="r" count="c}')
LAG_SYMS=$syms LAG_VIDEO=${VIDEO:-gl} LAG_STATE=$STATE LAG_POLL=${POLL:-late}
export LAG_SYMS LAG_VIDEO LAG_STATE LAG_POLL

# a system directory of its own for a run, with the BIOS files in it
sysdir() {
   rm -rf "$WORK/sys_$1"
   mkdir -p "$WORK/sys_$1/dc"
   if [ -n "$BIOS" ]; then
      for f in "$BIOS"/*; do
         [ -f "$f" ] && ln -s "$(cd "$(dirname "$f")" && pwd)/$(basename "$f")" "$WORK/sys_$1/dc/"
      done
   fi
   echo "$WORK/sys_$1"
}

env LAG_DIR="$(sysdir a)" LAG_TRACE="$WORK/a.trace" ${SHOT:+LAG_SHOT=$SHOT:$WORK/a_} \
   "$WORK/lagmeter" "$CORE" "$CONTENT" "$FRAMES" "$@"
env LAG_DIR="$(sysdir b)" LAG_TRACE="$WORK/b.trace" LAG_PRESS="$PRESS:$ID" ${SHOT:+LAG_SHOT=$SHOT:$WORK/b_} \
   "$WORK/lagmeter" "$CORE" "$CONTENT" "$FRAMES" "$@"
"$WORK/lagmeter" --compare "$WORK/a.trace" "$WORK/b.trace" "$PRESS"
rm -rf "$WORK/sys_a" "$WORK/sys_b"
[ -n "$KEEP" ] || rm -f "$WORK/a.trace" "$WORK/b.trace"
