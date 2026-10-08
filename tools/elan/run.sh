#!/bin/sh
# The NAOMI 2's geometry processor (core/hw/pvr/elan.c) without the rest of
# the machine: display lists in, tile accelerator parameters out.
#
#   sh tools/elan/run.sh
#
# Built as C89, plain and under ASan + UBSan: the lists the test sends
# include ones that lie about their length and point outside the chip's
# memory.
#
# And built two ways: as it is in the core, working most vertices out four
# at a time (SSE2, or NEON on a 64-bit ARM), and with ELAN_NO_SIMD, one by
# one. The test lights 400 scenes made up at random and both builds have
# to send the same bits for them - which it checks against the number they
# have always come to. Where a cross compiler for 64-bit ARM and
# qemu-aarch64 are installed, the NEON build is run as well.
#
# -ffp-contract=off: the two ways agree to the bit when a multiplication
# and an addition are two operations in both.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/elan_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=c89 -pedantic -Wall -Wextra -Werror=declaration-after-statement -O2 -g -ffp-contract=off"
INC="-I $ROOT/core/hw/pvr -I $ROOT/core/libretro-common/include"
SRC="$ROOT/tools/elan/elan_test.c $ROOT/core/hw/pvr/elan.c"

for simd in "" "-DELAN_NO_SIMD"; do
   for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
      ${CC:-cc} $FLAGS $simd $san $INC -o "$WORK/elan_test" $SRC -lm
      "$WORK/elan_test"
   done
done

if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && command -v qemu-aarch64 >/dev/null 2>&1; then
   for simd in "" "-DELAN_NO_SIMD"; do
      aarch64-linux-gnu-gcc $FLAGS $simd -static $INC -o "$WORK/elan_test_arm64" $SRC -lm
      qemu-aarch64 "$WORK/elan_test_arm64"
   done
   echo "elan test: the 64-bit ARM builds as well"
fi
