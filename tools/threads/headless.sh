#!/bin/sh
# Runs the live test's disc on a core with no display, no GPU and no
# RetroArch: with headless.c for a frontend and an OpenGL that does nothing.
# Nothing is drawn, so what is checked is the verdict the disc's program
# reaches about itself - its instruction tests, the length and evenness of
# its frames by the processor's timer, what it read back from the disc -
# and the sound, sample for sample. Both SH4 timings are run, and the
# first of them again with the MMU on.
#
# It is for cores that cannot be run any other way on the machine at hand,
# which is to say a core built for another processor, under qemu:
#
#   make platform=arm64 CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
#        AS=aarch64-linux-gnu-gcc
#   CC=aarch64-linux-gnu-gcc NM=aarch64-linux-gnu-nm GLES=1 \
#   RUN="qemu-aarch64 -L /usr/aarch64-linux-gnu" \
#      tools/threads/headless.sh flycast_libretro.so
#
# That is the only way the ARM recompilers get run on an x86-64 machine.
# (A core for OpenGL ES wants a libGLESv2 to link against; an empty one in
# the cross compiler's library directory will do with -Wl,--unresolved-
# symbols=ignore-all, or the one this script builds, from a first run.)
#
#   headless.sh CORE [key=value ...]     key=value are more core options
#
# CC    compiler for the frontend and the OpenGL, for the core's processor
# NM    nm for the core
# RUN   what to run the frontend with, if not directly
# GLES  set if the core is built for OpenGL ES
# WORK  where to work (default /tmp/flycast-headless)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=$1
[ -n "$CORE" ] || { echo "usage: $0 CORE [key=value ...]" >&2; exit 2; }
shift
CORE=$(cd "$(dirname "$CORE")" && pwd)/$(basename "$CORE")
WORK=${WORK:-/tmp/flycast-headless}
T=$ROOT/tools/threads
mkdir -p "$WORK/dir"

python3 "$T/live_disc.py" "$WORK/test.gdi"
"${CC:-cc}" -O1 -o "$WORK/headless" "$T/headless.c" -ldl
"${NM:-nm}" -D --undefined-only "$CORE" | awk '{print $NF}' | sed 's/@.*//' \
   | grep '^gl' | python3 "$T/headless_gl.py" > "$WORK/nogl.c" || true
"${CC:-cc}" -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 \
   -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"

EXTRA=$*
VERDICT=$(python3 "$T/live_disc.py" --verdict)
# The third run makes the disc a Windows CE one to the core, which is what
# turns its MMU on; the disc's program then turns the SH4's on and leaves
# it on, and everything is done the way a Windows CE game has it done.
for RUN_AS in legacy accurate "legacy wince"; do
   set -- $RUN_AS
   TIMING=$1
   NAME=$TIMING
   if [ "$2" = wince ]; then
      NAME=$TIMING-mmu
      WINCE=enabled
      GOOD=600d4d4d
      echo "== headless: $TIMING SH4 timing, with the MMU on"
   else
      WINCE=disabled
      GOOD=600d600d
      echo "== headless: $TIMING SH4 timing"
   fi
   rm -f "$WORK/sound.pcm"
   if [ -n "$GLES" ]; then
      # the core asks for libGLESv2.so.2 by name: let it find this one
      HEADLESS_GLES=1 LD_LIBRARY_PATH=$WORK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
      export HEADLESS_GLES LD_LIBRARY_PATH
   else
      # it has the system's OpenGL by name: put this one in front of it
      LD_PRELOAD=$WORK/libGLESv2.so.2
      export LD_PRELOAD
   fi
   HEADLESS_DIR=$WORK/dir HEADLESS_SOUND=$WORK/sound.pcm HEADLESS_PEEK=$VERDICT \
      $RUN "$WORK/headless" "$CORE" "$WORK/test.gdi" 300 \
      reicast_hle_bios=enabled reicast_threaded_rendering=disabled \
      reicast_sh4_timing=$TIMING reicast_force_wince=$WINCE $EXTRA \
      > "$WORK/$NAME.out" 2> "$WORK/$NAME.log" || {
      echo "FAIL: the run ended badly" >&2
      tail -n 20 "$WORK/$NAME.log" >&2
      exit 1
   }
   unset LD_PRELOAD
   grep -q "SH4 timing: $TIMING" "$WORK/$NAME.log" || {
      echo "FAIL: the core did not take the timing" >&2
      exit 1
   }
   if [ $WINCE = enabled ]; then
      grep -q "Enabling Full MMU support" "$WORK/$NAME.log" || {
         echo "FAIL: the MMU was not turned on" >&2
         exit 1
      }
   fi
   grep -q "= $GOOD" "$WORK/$NAME.out" || {
      echo "FAIL: the disc's program found something wrong: $(cat "$WORK/$NAME.out")" >&2
      exit 1
   }
   python3 "$T/live_audio.py" "$WORK/sound.pcm" || {
      echo "FAIL: wrong sound" >&2
      exit 1
   }
done
echo "headless test passed"
