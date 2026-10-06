#!/bin/sh
# The sound chip's ARM7, recompiled against interpreted.
#
#   sh tools/arm7/run.sh [programs]
#
# On x86-64 the ARM7 is run by a recompiler (core/hw/arm7/arm7_rec.cpp and
# arm7_rec_x64.cpp); everywhere the recompilers do not reach, and for the
# instructions this one hands back, by an interpreter (arm7.cpp with
# arm-new.h). arm7_test.cpp is built around each, with nothing else of the
# core, and both are given the same made-up programs, 10000 unless a number
# is given. The registers, the flags and all of memory have to be the same
# from both at the end of every program. arm7_test.cpp says what the
# programs are made of and what is left out of them.
#
# A program that stores over its own instructions is not compared, and
# neither is one that stops on a check after doing so.
#
# The build with the recompiler has the interpreter in it as well, for the
# "Sound CPU Recompiler" core option to switch to. The programs are run
# twice more through that build: with the interpreter chosen, and changing
# between the two from one sample to the next. Both have to come out as
# the others.
#
# Needs a C++ compiler and the core's makefile for its flags, and only
# does anything on x86-64: there is no recompiler to compare elsewhere.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
PROGRAMS=${1:-10000}
case $(uname -m) in
   x86_64|amd64) ;;
   *) echo "arm7: skipped (the recompiler under test is for x86-64)"; exit 0 ;;
esac
WORK=${TMPDIR:-/tmp}/arm7_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# the compiler and flags the core compiles this code with
CMD=$(make -n -B core/hw/arm7/arm7.o 2>/dev/null | grep 'arm7\.cpp' | tail -n 1)
[ -n "$CMD" ] || { echo "cannot get the core's compile flags from make" >&2; exit 1; }
CMD=${CMD% -o *}
FLAGS=$(echo "$CMD" | sed 's| [^ ]*arm7\.cpp||; s/ -MMD -MP//')
CXX=${FLAGS%% *}

build() {
   # $1: name, $2: extra define
   for SRC in core/hw/arm7/arm7.cpp core/hw/arm7/arm_mem.cpp core/hw/arm7/arm7_rec.cpp \
              core/hw/arm7/arm7_rec_x64.cpp tools/arm7/arm7_test.cpp; do
      # the flags come out of make quoted for a shell
      eval "$FLAGS $2 \"$SRC\" -o \"$WORK/$1-$(basename "$SRC" .cpp).o\""
   done
   "$CXX" -o "$WORK/arm7_test_$1" "$WORK/$1"-*.o
}

build rec ""
build interp -DFEAT_AREC=DYNAREC_NONE
"$WORK/arm7_test_rec" "$PROGRAMS" > "$WORK/rec.txt"
"$WORK/arm7_test_interp" "$PROGRAMS" > "$WORK/interp.txt"
ARM7_TEST_INTERPRET=1 "$WORK/arm7_test_rec" "$PROGRAMS" > "$WORK/chosen.txt"
ARM7_TEST_SWITCH=1 "$WORK/arm7_test_rec" "$PROGRAMS" > "$WORK/switched.txt"

same() {
   # $1, $2: outputs to compare, $3 and $4: what each is
   [ "$(wc -l < "$1")" -eq "$PROGRAMS" ] && [ "$(wc -l < "$2")" -eq "$PROGRAMS" ] || {
      echo "FAIL: not every program was run to its end ($3, $4)" >&2
      exit 1
   }
   paste -d'|' "$1" "$2" > "$WORK/both.txt"
   if ! awk -F'|' -v a="$3" -v b="$4" '
      $1 ~ /wrote over/ || $2 ~ /wrote over/ { skipped++; next }
      $1 != $2 { if (bad++ < 5) { print a ": " $1; print b ": " $2 } }
      END { printf "%d %d\n", skipped, bad > "'"$WORK"'/count.txt"; exit bad != 0 }' "$WORK/both.txt" >&2; then
      echo "FAIL: $3 and $4 differ on $(cut -d' ' -f2 "$WORK/count.txt") programs; the first are above" >&2
      exit 1
   fi
}

same "$WORK/chosen.txt" "$WORK/interp.txt" "the interpreter chosen by the option" "the interpreter"
same "$WORK/switched.txt" "$WORK/interp.txt" "changing between the two" "the interpreter"
same "$WORK/rec.txt" "$WORK/interp.txt" "recompiled" "interpreted"
SKIPPED=$(cut -d' ' -f1 "$WORK/count.txt")
echo "arm7: ok ($((PROGRAMS - SKIPPED)) programs: recompiled, interpreted and changing between the two agree; $SKIPPED more wrote over themselves and were not compared)"
