#!/bin/sh
# Checks on the PowerVR2 arithmetic the renderers copy, done on the CPU.
#
#   sh tools/pvr/run.sh
#
# fog_test.c: the fog coefficient of the shaders against the hardware's,
# for every depth there is.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/pvr_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

${CC:-cc} -std=c99 -O2 -Wall -Wextra -ffp-contract=off \
   -o "$WORK/fog_test" "$ROOT/tools/pvr/fog_test.c" -lm
"$WORK/fog_test"
