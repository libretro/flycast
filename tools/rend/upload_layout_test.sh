#!/bin/sh
# Where a texture's mipmap levels are in the memory they are uploaded from.
# See upload_layout_test.c.
set -e
cd "$(dirname "$0")/../.."
WORK=${TMPDIR:-/tmp}/upload_layout_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -std=c89 -pedantic -Wall -Werror -Wdeclaration-after-statement -O2 \
   -Icore/rend -Icore/libretro-common/include -o "$WORK/upload_layout_test" tools/rend/upload_layout_test.c
"$WORK/upload_layout_test"
