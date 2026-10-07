#!/bin/sh
# The SH4's vector instructions, as the x86-64 recompiler emits them,
# against their reference implementations.
#
#   sh tools/sh4/run.sh [operands]
#
# FIPR and FTRV are what a game transforms its vertices with, and the
# x86-64 recompiler has its own code for them (core/rec-x64/x64_vector.h)
# where it used to call a C function. That code has to give exactly what the
# function gives, and what the interpreter gives: vector_test.cpp assembles
# it, as the recompiler does, and runs both on 5 million random operands
# each unless a number is given.
#
# Needs a C++ compiler; only does anything on x86-64.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
case $(uname -m) in
   x86_64|amd64) ;;
   *) echo "sh4 vectors: skipped (the code under test is for x86-64)"; exit 0 ;;
esac
WORK=${TMPDIR:-/tmp}/sh4_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
# -ffp-contract=off: the reference must not be given a fused multiply-add
# by a compiler that has one to give
${CXX:-c++} -O2 -std=c++11 -ffp-contract=off -DXBYAK_NO_OP_NAMES -I"$ROOT/core" -I"$ROOT/core/deps" \
   -o "$WORK/vector_test" "$ROOT/tools/sh4/vector_test.cpp"
"$WORK/vector_test" "$@"
