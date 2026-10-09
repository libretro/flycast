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
   # The modem's network thread: the core's own picoppp.cpp with picoTCP,
   # and the test playing the Dreamcast's end of the line. picoTCP itself
   # is built without UBSan, which it was not written to pass.
   mkdir -p "$WORK/pico"
   for src in core/deps/picotcp/stack/*.c \
         core/deps/picotcp/modules/pico_udp.c core/deps/picotcp/modules/pico_tcp.c \
         core/deps/picotcp/modules/pico_socket_udp.c core/deps/picotcp/modules/pico_socket_tcp.c \
         core/deps/picotcp/modules/pico_fragments.c core/deps/picotcp/modules/pico_arp.c \
         core/deps/picotcp/modules/pico_ipv4.c core/deps/picotcp/modules/pico_ethernet.c \
         core/deps/picotcp/modules/pico_dns_client.c core/deps/picotcp/modules/pico_dns_common.c \
         core/deps/picotcp/modules/pico_dhcp_server.c core/deps/picotcp/modules/pico_dhcp_common.c \
         core/deps/picotcp/modules/pico_dev_ppp.c \
         $L/rthreads/retro_eventcount.c $L/queues/retro_spsc.c; do
      $CC -O1 -g -w -fsanitize=$(echo $san | sed 's/,undefined//') $DEFS $INC \
         -Icore/deps/picotcp/include -Icore/deps/picotcp/modules \
         -c $src -o "$WORK/pico/$(basename $src .c).o"
   done
   $CXX -std=c++11 -fpermissive -O1 -g -w $DEFS \
      -fsanitize=$san -fno-sanitize-recover=undefined $INC -Icore/deps/miniupnpc \
      -Icore/deps/picotcp/include -Icore/deps/picotcp/modules \
      -o "$WORK/pico_test" tools/threads/pico_test.cpp core/network/picoppp.cpp \
      core/hw/modem/dns.cpp "$WORK"/pico/*.o "$WORK/rthreads.o" "$WORK/features_cpu.o" \
      "$WORK/rtime.o" -lpthread
   # The save writer: the core's own savewriter.c over libretro-common's
   # file streams, with a file system of the test's that can be slow and
   # can fail.
   $CC -O1 -g -fsanitize=$san -fno-sanitize-recover=undefined $INC \
      -o "$WORK/savewriter_test" tools/threads/savewriter_test.c core/savewriter.c \
      $L/streams/file_stream.c $L/vfs/vfs_implementation.c $L/file/file_path.c \
      $L/file/file_path_io.c $L/compat/compat_strl.c $L/compat/fopen_utf8.c \
      $L/compat/compat_strcasestr.c $L/encodings/encoding_utf.c $L/string/stdstring.c \
      $L/string/rstrtod.c "$WORK/rthreads.o" "$WORK/retro_eventcount.o" \
      "$WORK/features_cpu.o" "$WORK/rtime.o" -lpthread -lm
   echo "== -fsanitize=$san"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/threads_test"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/savewriter_test" "$WORK"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/pico_test"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/pico_test" bba
   TSAN_OPTIONS=halt_on_error=1 "$WORK/m3comm_test"
   TSAN_OPTIONS=halt_on_error=1 "$WORK/naomi_net_test"
done
echo "threads test passed"
