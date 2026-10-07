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
# A core built for Windows runs the same way under wine, which is how the
# Windows build - its calling convention, and the paths only it takes, such
# as the MMU without the host's own mapping - gets run on Linux:
#
#   make platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
#   WINDOWS=1 CC=x86_64-w64-mingw32-gcc OBJDUMP=x86_64-w64-mingw32-objdump \
#   RUN=wine tools/threads/headless.sh flycast_libretro.dll
#
#   headless.sh CORE [key=value ...]     key=value are more core options
#
# CC    compiler for the frontend and the OpenGL, for the core's processor
# NM    nm for the core
# RUN   what to run the frontend with, if not directly
# GLES  set if the core is built for OpenGL ES
# WINDOWS  set if the core is a Windows DLL; OBJDUMP is then the objdump for it
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
if [ -n "$WINDOWS" ]; then
   FRONTEND=headless.exe
   "$CC" -O1 -o "$WORK/headless.exe" "$T/headless.c"
   # what the core imports from opengl32.dll, into one of our own beside the program
   "${OBJDUMP:-objdump}" -p "$CORE" \
      | awk '/DLL Name: /{dll=tolower($3)} dll=="opengl32.dll" && NF>=3 && $1 ~ /^[0-9a-f]+$/ {print $3}' \
      | python3 "$T/headless_gl.py" > "$WORK/nogl.c"
   "$CC" -O1 -shared -o "$WORK/opengl32.dll" "$WORK/nogl.c"
   # and the compiler's runtime libraries, if the core wants them
   for DLL in libgomp-1.dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll; do
      F=$("$CC" -print-file-name=$DLL)
      [ -f "$F" ] && cp "$F" "$WORK/"
   done
   WINEDLLOVERRIDES=opengl32=n
   WINEDEBUG=${WINEDEBUG:--all}
   export WINEDLLOVERRIDES WINEDEBUG
else
   FRONTEND=headless
   "${CC:-cc}" -O1 -o "$WORK/headless" "$T/headless.c" -ldl
   "${NM:-nm}" -D --undefined-only "$CORE" | awk '{print $NF}' | sed 's/@.*//' \
      | grep '^gl' | python3 "$T/headless_gl.py" > "$WORK/nogl.c" || true
   "${CC:-cc}" -O1 -shared -fPIC -Wl,-soname,libGLESv2.so.2 \
      -o "$WORK/libGLESv2.so.2" "$WORK/nogl.c"
fi

EXTRA=$*
VERDICT=$(python3 "$T/live_disc.py" --verdict)
# The third run makes the disc a Windows CE one to the core, which is what
# turns its MMU on; the disc's program then turns the SH4's on and leaves
# it on, and everything is done the way a Windows CE game has it done.
# The fourth presses reset 120 frames in, with the audio track playing and
# the disc's tests behind it: the program has to come up again and find
# everything as it does from a cold start.
# The fifth opens the drive's lid 120 frames in and shuts it on the same
# disc 20 frames later. The program watches the drive: it has to be busy
# for a second and then say that the medium may have changed.
for RUN_AS in legacy accurate "legacy wince" "legacy reset" "legacy swap"; do
   set -- $RUN_AS
   TIMING=$1
   NAME=$TIMING
   FRAMES=300
   unset HEADLESS_RESET HEADLESS_SWAP
   if [ "$2" = swap ]; then
      NAME=$TIMING-swap
      WINCE=disabled
      GOOD=600d5a9d
      HEADLESS_SWAP=120
      export HEADLESS_SWAP
      echo "== headless: $TIMING SH4 timing, with the disc taken out and put back"
   elif [ "$2" = reset ]; then
      NAME=$TIMING-reset
      WINCE=disabled
      GOOD=600d600d
      FRAMES=420
      HEADLESS_RESET=120
      export HEADLESS_RESET
      echo "== headless: $TIMING SH4 timing, with a reset"
   elif [ "$2" = wince ]; then
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
   if [ -n "$WINDOWS" ]; then
      :
   elif [ -n "$GLES" ]; then
      # the core asks for libGLESv2.so.2 by name: let it find this one
      HEADLESS_GLES=1 LD_LIBRARY_PATH=$WORK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
      export HEADLESS_GLES LD_LIBRARY_PATH
   else
      # it has the system's OpenGL by name: put this one in front of it
      LD_PRELOAD=$WORK/libGLESv2.so.2
      export LD_PRELOAD
   fi
   HEADLESS_DIR=$WORK/dir HEADLESS_SOUND=$WORK/sound.pcm HEADLESS_PEEK=$VERDICT \
      $RUN "$WORK/$FRONTEND" "$CORE" "$WORK/test.gdi" $FRAMES \
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
   # (a Windows program ends its lines its own way)
   tr -d '\r' < "$WORK/$NAME.out" | grep -q "= $GOOD\$" || {
      echo "FAIL: the disc's program found something wrong: $(cat "$WORK/$NAME.out")" >&2
      exit 1
   }
   # (the sound of a run with a reset or a disc change in it is not the plain one: not looked at)
   [ -n "$HEADLESS_RESET$HEADLESS_SWAP" ] || python3 "$T/live_audio.py" "$WORK/sound.pcm" || {
      echo "FAIL: wrong sound" >&2
      exit 1
   }
done
echo "headless test passed"
