#!/bin/sh
# The background plane's arithmetic. See bg_plane_test.c.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/bg_plane_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -std=c89 -pedantic -Wall -Werror -Wdeclaration-after-statement -O2 -Icore/hw/pvr \
   -o "$WORK/bg_plane_test" tools/rend/bg_plane_test.c
"$WORK/bg_plane_test"
