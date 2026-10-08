#!/bin/sh
# The NAOMI 2's geometry processor (core/hw/pvr/elan.c) without the rest of
# the machine: display lists in, tile accelerator parameters out.
#
#   sh tools/elan/run.sh
#
# Built as C89, once plain and once under ASan + UBSan: the lists the test
# sends include ones that lie about their length and point outside the
# chip's memory.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/elan_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
   ${CC:-cc} -std=c89 -pedantic -Wall -Wextra -Werror=declaration-after-statement -O2 -g $san \
      -I "$ROOT/core/hw/pvr" -I "$ROOT/core/libretro-common/include" \
      -o "$WORK/elan_test" \
      "$ROOT/tools/elan/elan_test.c" "$ROOT/core/hw/pvr/elan.c" -lm
   "$WORK/elan_test"
done
