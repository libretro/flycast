#!/bin/sh
# Thread handoff stress test.
#
#   sh tools/threads/run.sh
#
# Builds tools/threads/threads_test.cpp against the core's own headers, so
# it runs the very events, lock-free containers and thread handshakes the
# core does, and runs it twice: under ThreadSanitizer, which reports any
# handoff that lets two threads at the same data, and under
# AddressSanitizer + UBSan with leak detection.
#
# The preprocessor flags are read back out of the core's own build, so the
# test compiles the headers the way the core does.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CC=${CC:-cc}
CXX=${CXX:-c++}
DEFS=$(make -n -B core/hw/pvr/ta_ctx.o \
       | grep -o -- '-D[A-Za-z0-9_]*\(=[A-Za-z0-9_]*\)\?' \
       | grep -v GIT_VERSION | sort -u | tr '\n' ' ')
case "$DEFS" in
   *-DTARGET_NO_THREADS*) echo "core build has threads disabled" >&2; exit 1 ;;
esac

L=core/libretro-common
INC="-Icore -Icore/libretro -Icore/deps -I$L/include"
WORK=${TMPDIR:-/tmp}/threads_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

for san in thread address,undefined; do
   $CC -O1 -g -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -c $L/rthreads/rthreads.c -o "$WORK/rthreads.o"
   $CC -O1 -g -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -c $L/rthreads/retro_eventcount.c -o "$WORK/retro_eventcount.o"
   $CXX -std=c++11 -fpermissive -O1 -g $DEFS \
      -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -o "$WORK/threads_test" tools/threads/threads_test.cpp \
      "$WORK/rthreads.o" "$WORK/retro_eventcount.o" -lpthread
   echo "== -fsanitize=$san"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/threads_test"
done
echo "threads test passed"
