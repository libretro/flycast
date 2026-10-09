#!/bin/sh
# Runs the live test's disc on a core with no display, no GPU and no
# RetroArch: with headless.c for a frontend and an OpenGL that does nothing.
# Nothing is drawn, so what is checked is the verdict the disc's program
# reaches about itself - its instruction tests, the length and evenness of
# its frames by the processor's timer, what it read back from the disc -
# and the sound, sample for sample. Both SH4 timings are run, and the
# first of them again with the MMU on. A NAOMI cartridge comes last, for
# the built-in BIOS's way into a game's test program, and romsets for the
# loader's way with a merged one.
#
# It is for cores that cannot be run any other way on the machine at hand,
# which is to say a core built for another processor, under qemu:
#
#   make platform=arm64 CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ \
#        AS=aarch64-linux-gnu-gcc
#   CC=aarch64-linux-gnu-gcc NM=aarch64-linux-gnu-nm GLES=1 \
#   RUN="qemu-aarch64 -cpu cortex-a72 -L /usr/aarch64-linux-gnu" \
#      tools/threads/headless.sh flycast_libretro.so
#
# (-cpu cortex-a72: the processor qemu emulates by default has the newest
# instructions for copying memory, the C library uses them, and qemu 8.2
# gives up - "QEMU internal SIGSEGV" - when a copy made with them runs into
# a page that is protected. The core protects pages on purpose and answers
# the fault; a state being saved or loaded copies all of video memory
# across such pages. A processor without those instructions gets the fault
# delivered as a real one does.)
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
#   WINDOWS=1 ALIGN=1 CC=x86_64-w64-mingw32-gcc \
#   OBJDUMP=x86_64-w64-mingw32-objdump \
#   RUN=wine tools/threads/headless.sh flycast_libretro.dll
#
# And a core for 32-bit ARM, whose recompiler is another one again:
#
#   make platform=armv7-neon-hardfloat CC=arm-linux-gnueabihf-gcc \
#        CXX=arm-linux-gnueabihf-g++ AS=arm-linux-gnueabihf-gcc \
#        LDFLAGS_END=-Wl,--unresolved-symbols=ignore-all
#   ALIGN=1 CC=arm-linux-gnueabihf-gcc NM=arm-linux-gnueabihf-nm \
#   RUN="qemu-arm -cpu cortex-a15 -L /usr/arm-linux-gnueabihf" \
#      tools/threads/headless.sh flycast_libretro.so
#
# (that one links against the desktop's libGL: an empty libGL.so in the
# cross compiler's library directory, which is also where qemu's -L finds
# it when the core is loaded.)
#
# ALIGN: the two cores above translate addresses through a table when the
# MMU is on, where the others map the pages into the host's memory. A
# table has to be asked whether the address is a multiple of the access's
# size - a misaligned access is an address error, and a table that is not
# asked lets it read on past the page - and ALIGN=1 puts the build of the
# test program on the disc that tries it (live_disc.py --mmu-align). A
# core that maps the pages lets the host do the access, which does not
# mind, and fails that build: not for those.
#
#   headless.sh CORE [key=value ...]     key=value are more core options
#
# CC    compiler for the frontend and the OpenGL, for the core's processor
# NM    nm for the core
# RUN   what to run the frontend with, if not directly
# GLES  set if the core is built for OpenGL ES
# WINDOWS  set if the core is a Windows DLL; OBJDUMP is then the objdump for it
# ALIGN  set for a core that translates through a table (see above)
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

python3 "$T/live_disc.py" "$WORK/test.gdi" ${ALIGN:+--mmu-align}
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
# The sixth saves a state 100 frames in and loads it 24 frames later, while
# the disc's sound plays: from the load on, the sound has to be the first
# run's from where the state was saved. (A channel the program has marked
# to start and not yet started is among what the state has to carry.)
# The seventh changes the Internal Resolution option to 2x and turns the
# widescreen hack on 120 frames in, as from the frontend's menu: the core has
# to tell the frontend the new size - 1280x960, a third wider - and the
# disc's program has to go on as if nothing had happened.
# The eighth turns threaded rendering on 120 frames in and off again at 200:
# the emulation thread is started and stopped between two frames, and the
# disc's program has to go on as if nothing had happened.
# The ninth asks for video on one frame in four, as fast-forward's frameskip
# does: the renders in between are not drawn, and the program and its sound
# have to be what they are with every frame drawn. (A frame the program
# writes to the framebuffer itself is still handed over: it is not written
# again. Nearly every frame is handed over when all are asked for.)
for RUN_AS in legacy accurate "legacy wince" "legacy reset" "legacy swap" "legacy state" "legacy resize" "legacy threads" "legacy skip"; do
   set -- $RUN_AS
   TIMING=$1
   NAME=$TIMING
   FRAMES=300
   unset HEADLESS_RESET HEADLESS_SWAP HEADLESS_SAVE HEADLESS_LOAD HEADLESS_OPTION HEADLESS_OPTION2 HEADLESS_SKIP
   SIZE_TOLD=
   if [ "$2" = skip ]; then
      NAME=$TIMING-skip
      WINCE=disabled
      GOOD=600d600d
      HEADLESS_SKIP=4
      export HEADLESS_SKIP
      echo "== headless: $TIMING SH4 timing, with video asked for on one frame in four"
   elif [ "$2" = threads ]; then
      NAME=$TIMING-threads
      WINCE=disabled
      GOOD=600d600d
      HEADLESS_OPTION=120:reicast_threaded_rendering=enabled
      HEADLESS_OPTION2=200:reicast_threaded_rendering=disabled
      export HEADLESS_OPTION HEADLESS_OPTION2
      echo "== headless: $TIMING SH4 timing, with threaded rendering turned on and off again"
   elif [ "$2" = resize ]; then
      SIZE_TOLD=1707x960
      NAME=$TIMING-resize
      WINCE=disabled
      GOOD=600d600d
      HEADLESS_OPTION=120:reicast_internal_resolution=2x,reicast_widescreen_hack=enabled
      export HEADLESS_OPTION
      echo "== headless: $TIMING SH4 timing, with the internal resolution and widescreen changed"
   elif [ "$2" = state ]; then
      NAME=$TIMING-state
      WINCE=disabled
      GOOD=600d600d
      HEADLESS_SAVE=100
      HEADLESS_LOAD=124
      export HEADLESS_SAVE HEADLESS_LOAD
      echo "== headless: $TIMING SH4 timing, with a state saved and loaded"
   elif [ "$2" = swap ]; then
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
   # (the first run's sound is kept: the run with a state in it is held against it)
   [ "$NAME" = legacy ] || rm -f "$WORK/sound.pcm"
   [ "$NAME" != legacy ] || rm -f "$WORK/sound-plain.pcm"
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
   SOUND=$WORK/sound.pcm
   [ "$NAME" != legacy ] || SOUND=$WORK/sound-plain.pcm
   HEADLESS_DIR=$WORK/dir HEADLESS_SOUND=$SOUND HEADLESS_PEEK=$VERDICT \
      $RUN "$WORK/$FRONTEND" "$CORE" "$WORK/test.gdi" $FRAMES \
      reicast_use_real_bios=disabled reicast_threaded_rendering=disabled \
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
   if [ -n "$SIZE_TOLD" ]; then
      tr -d '\r' < "$WORK/$NAME.out" | grep -q "^size $SIZE_TOLD\$" || {
         echo "FAIL: the frontend was not told the new size: $(cat "$WORK/$NAME.out")" >&2
         exit 1
      }
   fi
   if [ -n "$HEADLESS_SKIP" ]; then
      SHOWN=$(tr -d '\r' < "$WORK/$NAME.out" | sed -n 's/^shown //p')
      [ -n "$SHOWN" ] && [ "$SHOWN" -lt $((FRAMES / 2)) ] || {
         echo "FAIL: frames the frontend did not ask for were drawn: $(cat "$WORK/$NAME.out")" >&2
         exit 1
      }
   fi
   tr -d '\r' < "$WORK/$NAME.out" | grep -q "= $GOOD\$" || {
      echo "FAIL: the disc's program found something wrong: $(cat "$WORK/$NAME.out")" >&2
      exit 1
   }
   # (the sound of a run with a reset or a disc change in it is not the plain one: not looked at)
   if [ -n "$HEADLESS_SAVE" ]; then
      python3 "$T/live_audio.py" --reloaded "$WORK/sound-plain.pcm" "$SOUND" || {
         echo "FAIL: the sound did not go on from a loaded state as it had from there" >&2
         exit 1
      }
   elif [ -z "$HEADLESS_RESET$HEADLESS_SWAP" ]; then
      python3 "$T/live_audio.py" "$SOUND" || {
         echo "FAIL: wrong sound" >&2
         exit 1
      }
   fi
done

# And a NAOMI cartridge (naomi_cart.py), started without a BIOS: its program
# asks the built-in one for the system menu, as a game does when the
# cabinet's TEST switch is pressed. It has to be started again as the
# cartridge's test program, and from there - a test program's way out is
# the same routine - as the game once more, each time as the real BIOS
# starts them. naomi_prog.c has what it looks at.
echo "== headless: a NAOMI cartridge, into its test program and back"
unset HEADLESS_RESET HEADLESS_SWAP HEADLESS_SAVE HEADLESS_LOAD HEADLESS_OPTION HEADLESS_OPTION2 HEADLESS_SKIP
python3 "$T/naomi_cart.py" "$WORK/naomi-test.bin"
if [ -n "$WINDOWS" ]; then
   :
elif [ -z "$GLES" ]; then
   LD_PRELOAD=$WORK/libGLESv2.so.2
   export LD_PRELOAD
fi
HEADLESS_DIR=$WORK/dir HEADLESS_PEEK=$(python3 "$T/naomi_cart.py" --verdict) \
   $RUN "$WORK/$FRONTEND" "$CORE" "$WORK/naomi-test.bin" 120 \
   reicast_use_real_bios=disabled reicast_threaded_rendering=disabled $EXTRA \
   > "$WORK/naomi.out" 2> "$WORK/naomi.log" || {
   echo "FAIL: the run ended badly" >&2
   tail -n 20 "$WORK/naomi.log" >&2
   exit 1
}
unset LD_PRELOAD
tr -d '\r' < "$WORK/naomi.out" | grep -q "= 600d7e57\$" || {
   echo "FAIL: the cartridge's program found something wrong: $(cat "$WORK/naomi.out")" >&2
   exit 1
}

# And romsets (naomi_cart.py --merged): a merged set - a game and its
# clones in one archive, the clones' own ROMs in folders - has to start the
# game under its own name and under no set's name, and a clone under the
# clone's: as the set itself renamed, and as an empty file of that name
# beside the set. And a set whose ROMs are all inside a folder, their names
# in capitals, has to be found. Which program ROM was started is read from
# the serial in the header, of which the BIOS keeps a copy in main memory.
#
# Under the game's name the clone is chosen with a core option, which the
# core has to declare for such a set - with the sets in it for values - and
# for no other; a choice that is not one of the set's (another romset's,
# left in the frontend's file) starts the game, and a file named for a
# clone is that clone whatever the option says.
echo "== headless: NAOMI romsets, a merged one and its clones"
python3 "$T/naomi_cart.py" --merged "$WORK/sets"
OPTION=reicast_naomi_merged_set
DECLARED="option Game to Start From This Merged Romset; doa2m|doa2|doa2a"
# (the file, the option's value, the serial to find, whether the option is to be declared)
for SET in "doa2m.zip - 30535442 yes" "doa2m.zip doa2a 32535442 yes" "doa2m.zip doa2 31535442 yes" \
      "doa2m.zip vf4 30535442 yes" "doa2.zip doa2a 31535442 no" "doa2a.zip - 32535442 no" \
      "Dead_or_Alive_2.zip doa2a 32535442 yes" "folder/doa2m.zip doa2a 30535442 no"; do
   set -- $SET
   SET=$1
   # (the one with spaces in its name)
   [ "$SET" != Dead_or_Alive_2.zip ] || SET="Dead or Alive 2.zip"
   CHOICE=
   [ "$2" = - ] || CHOICE=$OPTION=$2
   GOOD=$3
   if [ -n "$WINDOWS" ]; then
      :
   elif [ -z "$GLES" ]; then
      LD_PRELOAD=$WORK/libGLESv2.so.2
      export LD_PRELOAD
   fi
   HEADLESS_DIR=$WORK/dir HEADLESS_PEEK=$(python3 "$T/naomi_cart.py" --serial) HEADLESS_SHOW=$OPTION \
      $RUN "$WORK/$FRONTEND" "$CORE" "$WORK/sets/$SET" 3 \
      reicast_use_real_bios=disabled reicast_threaded_rendering=disabled $CHOICE $EXTRA \
      > "$WORK/sets.out" 2> "$WORK/sets.log" || {
      echo "FAIL: $SET was not started" >&2
      tail -n 20 "$WORK/sets.log" >&2
      exit 1
   }
   unset LD_PRELOAD
   tr -d '\r' < "$WORK/sets.out" | grep -q "= $GOOD\$" || {
      echo "FAIL: $SET${CHOICE:+ with $CHOICE} started another set's program ROM: $(cat "$WORK/sets.out")" >&2
      exit 1
   }
   if [ "$4" = yes ]; then
      tr -d '\r' < "$WORK/sets.out" | grep -qxF "$DECLARED" || {
         echo "FAIL: $SET: the option was not declared with the set's games: $(cat "$WORK/sets.out")" >&2
         exit 1
      }
   elif grep -q "^option " "$WORK/sets.out"; then
      echo "FAIL: $SET: the option was declared for a set it is not for: $(cat "$WORK/sets.out")" >&2
      exit 1
   fi
done
echo "headless test passed"
