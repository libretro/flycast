#!/bin/sh
# retro_atomic.h's C++11 backend on a libstdc++ before P0558R1 (GCC 8
# and older, issue #1215).
#
#   sh tools/threads/old_stl.sh
#
# Builds tools/threads/old_stl_test.cpp against the host <atomic> and
# runs it, then parses it again with old_stl/atomic ahead of the host's:
# that header has the shape libstdc++ 8 shipped, whose free functions
# deduce the value parameter from the argument too, so the enumerators
# the core passes fail to deduce unless the macros cast them.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

CXX=${CXX:-c++}
T=tools/threads
INC="-Icore/libretro-common/include"
WORK=${TMPDIR:-/tmp}/old_stl_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

$CXX -std=c++11 -Wall -Wextra -pedantic -O1 -g $INC \
   -o "$WORK/old_stl_test" $T/old_stl_test.cpp
"$WORK/old_stl_test"
$CXX -std=c++11 -Wall -Wextra -pedantic -fsyntax-only -I$T/old_stl $INC \
   $T/old_stl_test.cpp
echo "old <atomic>: ok"
