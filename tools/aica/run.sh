#!/bin/sh
# The sound chip's DSP, compiled against interpreted.
#
#   sh tools/aica/run.sh [cases]
#
# The DSP runs a 128-step program for every sample. The core has it twice:
# a compiler that turns the program into machine code (dsp_x64.cpp here,
# dsp_arm64.cpp on ARM64) and an interpreter (dsp_interp.cpp) for
# everything else. Nothing ran the two against each other.
#
# dsp_test.cpp is built around each of them in turn, with nothing of the
# rest of the core, and both are given the same made-up programs, 1000
# unless a number is given: 1 to 64 random steps, random coefficients and
# addresses, 64 samples of random input. What each leaves behind - its 16
# outputs after every sample, its registers and its delay memory at the
# end - has to be the same from both, for every program.
#
# Every program is also run a second time with a step added at the very
# end of the 128 that is not empty and does nothing. The DSP leaves out
# the empty steps after a program's last; the added step makes it run them
# all. Both runs have to leave the same behind, from the compiler and from
# the interpreter.
#
# One difference between the two is known and kept out of the programs. A
# step can take a value that a step two before it read from memory. The
# compiler keeps the four places those values wait in from one sample to
# the next; the interpreter empties them at the start of every sample. So
# a program that takes a value without having read one two steps before
# gets an old value from one and zero from the other. Which the hardware
# does is not known here, and no program that means anything does it, so
# the made-up programs only take a value two steps after a read.
#
# Needs a C++ compiler and the core's makefile for its flags. Where the
# core is not built for x86-64 there is no compiler to compare with and
# only the interpreter's check of itself is run.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CASES=${1:-1000}
WORK=${TMPDIR:-/tmp}/aica_dsp.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# the compiler and flags the core compiles this file with
CMD=$(make -n -B core/hw/aica/dsp_x64.o 2>/dev/null | grep 'dsp_x64\.cpp' | tail -n 1)
[ -n "$CMD" ] || { echo "cannot get the core's compile flags from make" >&2; exit 1; }
CMD=${CMD% -o *}
FLAGS=$(echo "$CMD" | sed 's| [^ ]*dsp_x64\.cpp||; s/ -MMD -MP//')
CXX=${FLAGS%% *}

build() {
   # $1: name, $2: extra define
   for SRC in core/hw/aica/dsp.cpp core/hw/aica/dsp_x64.cpp core/hw/aica/dsp_interp.cpp tools/aica/dsp_test.cpp; do
      # the flags come out of make quoted for a shell
      eval "$FLAGS $2 \"$SRC\" -o \"$WORK/$1-$(basename "$SRC" .cpp).o\""
   done
   "$CXX" -o "$WORK/dsp_test_$1" "$WORK/$1"-*.o
}

same() {
   # $1, $2: outputs to compare, $3: what they are; the case number and the
   # program's length lead each line and are left out of the comparison
   cut -d' ' -f3- "$1" > "$WORK/a.txt"
   cut -d' ' -f3- "$2" > "$WORK/b.txt"
   if ! cmp -s "$WORK/a.txt" "$WORK/b.txt"; then
      echo "FAIL: $3 differ, first in these cases:" >&2
      diff "$1" "$2" | grep '^<' | head -n 5 | cut -d' ' -f2-3 >&2
      exit 1
   fi
}

build interp -DFEAT_DSPREC=DYNAREC_NONE
"$WORK/dsp_test_interp" "$CASES" > "$WORK/interp.txt"
"$WORK/dsp_test_interp" "$CASES" all > "$WORK/interp-all.txt"
same "$WORK/interp.txt" "$WORK/interp-all.txt" "the interpreter's steps in use and all 128"

case $(uname -m) in
   x86_64|amd64)
      build jit ""
      "$WORK/dsp_test_jit" "$CASES" > "$WORK/jit.txt"
      "$WORK/dsp_test_jit" "$CASES" all > "$WORK/jit-all.txt"
      same "$WORK/jit.txt" "$WORK/jit-all.txt" "the compiler's steps in use and all 128"
      same "$WORK/jit.txt" "$WORK/interp.txt" "the compiler and the interpreter"
      echo "dsp: ok ($CASES programs: compiled and interpreted agree, with and without the empty steps)"
      ;;
   *)
      echo "dsp: ok ($CASES programs through the interpreter, with and without the empty steps; no compiler to compare on this machine)"
      ;;
esac
