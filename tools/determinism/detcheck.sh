#!/bin/sh
# Is the core deterministic - the same state and the same inputs making the
# same frames, every time, on any machine, with states saved and loaded
# along the way? What netplay needs of a core, and what its .info says
# with savestate_features = "deterministic". (detcheck.c says how.)
#
#   detcheck.sh CORE CONTENT STATE FRAMES [key=value ...]
#
# STATE is a state of that content to start from ("-" for none: the content
# from its start). key=value are core options. The game is played with
# inputs detcheck.c makes up, the same each time, and after every frame the
# state is written down part by part, with the frame's sound and picture:
#
#   plain       the frames one after another, which the others are held against
#   again       the same once more
#   reload      a state saved and loaded straight back every K frames
#   ahead       run-ahead's way: each frame run, saved, the next run, loaded
#               again, and the next run once more from there
#   rollback    netplay's: every D frames, back D frames and those run again
#
# Each has to be plain's, frame for frame, and every 50 frames a state
# saved into memory full of 0x00 has to be the one saved into memory full
# of 0xff. At the first frame that is not the same, the parts of the state
# that differ are named, and the bytes.
#
# For another machine or another build: REF=a trace from plain on it (its
# WORK/plain.trace) is held against plain here. Two traces can be held
# against each other at any time:  detcheck --compare A.trace B.trace
#
# CC     compiler for detcheck, for the core's processor (default cc)
# NM     nm for the core (default nm)
# RUN    what to run detcheck with, if not directly (qemu-aarch64 -L ...)
# GL     egl: a real OpenGL (the system's, through EGL, with no display),
#        and the frames' pictures hashed. Otherwise an OpenGL that does
#        nothing, put in front of the system's: nothing is drawn.
# GLLIB  the OpenGL library the core is linked against, for the one that
#        does nothing to stand in for (default libGL.so.1; libGLESv2.so.2
#        for a core for OpenGL ES)
# K, D   how often a state is reloaded, how far rolled back (default 7, 4)
# SEED   the inputs (default 1)
# RUNS   which of the runs above (default all of them)
# WORK   where to work (default /tmp/detcheck)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$1
CONTENT=$2
STATE=$3
FRAMES=$4
[ -n "$FRAMES" ] || { echo "usage: $0 CORE CONTENT STATE FRAMES [key=value ...]" >&2; exit 2; }
shift 4
CORE=$(cd "$(dirname "$CORE")" && pwd)/$(basename "$CORE")
WORK=${WORK:-/tmp/detcheck}
K=${K:-7}
D=${D:-4}
RUNS=${RUNS:-plain again reload ahead rollback}
mkdir -p "$WORK/dir"

if [ "$GL" = egl ]; then
   "${CC:-cc}" -O2 -DDET_EGL -o "$WORK/detcheck" "$HERE/detcheck.c" -ldl -lEGL -lGL
   DET_GL=egl
   export DET_GL
else
   "${CC:-cc}" -O2 -o "$WORK/detcheck" "$HERE/detcheck.c" -ldl
   # every gl function the core calls by name, doing nothing but for the
   # ones nogl.c has
   {
      cat "$HERE/nogl.c"
      "${NM:-nm}" -D --undefined-only "$CORE" | awk '{print $NF}' | sed 's/@.*//' | grep '^gl' | sort -u \
         | while read -r NAME; do
            grep -q "[ *]$NAME(" "$HERE/nogl.c" || echo "long $NAME(void) { return 0; }"
         done
   } > "$WORK/nogl.c"
   "${CC:-cc}" -O1 -shared -fPIC -Wl,-soname,"${GLLIB:-libGL.so.1}" -o "$WORK/${GLLIB:-libGL.so.1}" "$WORK/nogl.c"
   # (found by its name, and put in front as well, for a core that was
   # linked with none: that one finds the functions wherever they are)
   LD_LIBRARY_PATH=$WORK${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
   LD_PRELOAD=$WORK/${GLLIB:-libGL.so.1}
   export LD_LIBRARY_PATH LD_PRELOAD
   case "$GLLIB" in *GLES*) DET_GLES=1; export DET_GLES;; esac
fi

# where the core keeps the table of its state's parts (core/serialize.cpp)
sym() { "${NM:-nm}" "$CORE" | awk -v n="$1" '$3 == n { print $1; exit }'; }
DET_SYMS="base=$(sym retro_init) parts=$(sym dc_state_parts) at=$(sym dc_state_part_at) name=$(sym dc_state_part_name)"
export DET_SYMS
[ -n "$(sym dc_state_parts)" ] || echo "(the core has no table of its state's parts: the state is one part to this test)"

DET_DIR=$WORK/dir
export DET_DIR
[ "$STATE" = - ] || { DET_STATE=$(cd "$(dirname "$STATE")" && pwd)/$(basename "$STATE"); export DET_STATE; }
DET_SEED=${SEED:-1}
export DET_SEED

# run NAME MODE: a run, held against plain's trace as it goes (but plain)
STATUS=0
run() {
   NAME=$1
   MODE=$2
   shift 2
   rm -f "$WORK/$NAME.trace" "$WORK/$NAME.state" "$WORK/plain-$NAME.state"
   if [ "$NAME" = plain ]; then
      REFTRACE=
   else
      REFTRACE=$WORK/plain.trace
   fi
   T0=$(date +%s)
   set +e
   DET_MODE=$MODE DET_TRACE=$WORK/$NAME.trace DET_REF=$REFTRACE DET_DUMP=$WORK/$NAME.state \
      $RUN "$WORK/detcheck" "$CORE" "$CONTENT" "$FRAMES" "$@" > "$WORK/$NAME.out" 2> "$WORK/$NAME.log"
   RC=$?
   set -e
   T1=$(date +%s)
   if [ $RC = 0 ]; then
      echo "$NAME ($MODE): the same, $(tail -n 1 "$WORK/$NAME.out") ($((T1 - T0)) s)"
      return
   fi
   STATUS=3
   if [ $RC != 3 ]; then
      echo "$NAME ($MODE): the run did not go (status $RC):"
      tail -n 15 "$WORK/$NAME.log"
      return
   fi
   echo "$NAME ($MODE): NOT THE SAME ($((T1 - T0)) s)"
   cat "$WORK/$NAME.out"
   # where it parted from plain: plain's state at that frame, and the two side by side
   AT=$(sed -n 's/^not the reference.s after frame \([0-9]*\):$/\1/p' "$WORK/$NAME.out")
   if [ -n "$AT" ] && [ -s "$WORK/$NAME.state" ]; then
      DET_MODE=plain DET_DUMP=$AT:$WORK/plain-$NAME.state \
         $RUN "$WORK/detcheck" "$CORE" "$CONTENT" $((AT + 1)) "$@" > /dev/null 2>&1 || true
      $RUN "$WORK/detcheck" --compare "$WORK/plain.trace" "$WORK/$NAME.trace" \
         "$WORK/plain-$NAME.state" "$WORK/$NAME.state" | sed -n '/byte by byte/,$p' || true
   fi
}

for R in $RUNS; do
   case $R in
      plain) run plain plain "$@" ;;
      again) run again plain "$@" ;;
      reload) run reload "reload:$K" "$@" ;;
      ahead) run ahead ahead "$@" ;;
      rollback) run rollback "rollback:$D" "$@" ;;
   esac
done

if [ -n "$REF" ]; then
   echo "against $REF:"
   $RUN "$WORK/detcheck" --compare "$REF" "$WORK/plain.trace" || STATUS=3
fi
[ $STATUS = 0 ] && echo "deterministic" || echo "NOT DETERMINISTIC"
exit $STATUS
