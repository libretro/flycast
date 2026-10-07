#!/bin/sh
# The renderers' transforms against the 4x4 matrices they stand for.
# See xform_test.c.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/xform_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
# -ffp-contract=off: a multiply and an add stay two operations, on both sides
${CC:-cc} -std=c89 -pedantic -Wall -Werror -Wdeclaration-after-statement -O2 -ffp-contract=off \
   -Icore/rend -Icore/libretro-common/include -o "$WORK/xform_test" tools/rend/xform_test.c
"$WORK/xform_test"
