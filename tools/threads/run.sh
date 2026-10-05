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
   # The NAOMI link board: the core's own naomi_m3comm.cpp against a
   # stand-in for the network.
   $CC -O1 -g -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -c $L/time/rtime.c -o "$WORK/rtime.o"
   $CC -O1 -g -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -c $L/features/features_cpu.c -o "$WORK/features_cpu.o"
   $CXX -std=c++11 -fpermissive -O1 -g $DEFS \
      -fsanitize=$san -fno-sanitize-recover=undefined $INC -Icore/deps/miniupnpc \
      -o "$WORK/m3comm_test" tools/threads/m3comm_test.cpp \
      core/hw/naomi/naomi_m3comm.cpp "$WORK/rthreads.o" "$WORK/features_cpu.o" \
      "$WORK/rtime.o" -lpthread
   # The NAOMI network itself: the core's own naomi_network.cpp, a server
   # and a client of it over the loopback interface. Built without the
   # modem, so that it does not go looking for a UPnP router.
   $CXX -std=c++11 -fpermissive -O1 -g $(echo "$DEFS" | sed 's/-DENABLE_MODEM//') \
      -fsanitize=$san -fno-sanitize-recover=undefined $INC -Icore/deps/miniupnpc \
      -o "$WORK/naomi_net_test" tools/threads/naomi_net_test.cpp \
      core/network/naomi_network.cpp "$WORK/rthreads.o" "$WORK/features_cpu.o" \
      "$WORK/rtime.o" -lpthread
   echo "== -fsanitize=$san"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/threads_test"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/m3comm_test"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/naomi_net_test"
done
echo "threads test passed"
