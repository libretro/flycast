#!/bin/sh
# The Direct3D renderers' texture kernels as bytecode, made here and
# committed, so that the core compiles none of them when it runs and a
# build of it needs no shader compiler:
#
#   core/rend/texdecode_dxbc.inc  textures made on the GPU, Direct3D 11
#                                 (rend/texdecode.h with TEXD_KERNEL_D3D;
#                                 dx11/dx11_texdecoder.cpp)
#   core/rend/texdecode_dx9.inc   the same for Direct3D 9, shader model 3
#                                 (rend/texdecode_sm3.h with TEXD_SM3_KERNEL;
#                                 dx9/dx9_texdecoder.cpp)
#
#   sh tools/rend/kernels_dxbc.sh update   both made again from the headers
#   sh tools/rend/kernels_dxbc.sh check    whether both are the headers' still
#
# Each kernel is the header with the two lines in front of it below,
# compiled with Microsoft's d3dcompiler_47.dll (D3DCompile, entry point
# main, optimisation level 3) by tools/rend/dxbc_compile.c - built with
# mingw-w64 and run under wine where this is not Windows:
#   D3DCOMPILER=/path/to/d3dcompiler_47.dll  the compiler (needed to update)
#   CC_WIN=x86_64-w64-mingw32-gcc            what builds dxbc_compile.exe
#   WINE=wine                                what runs it ("" on Windows)
# The file says which d3dcompiler_47.dll made it (its SHA-1) and the SHA-1
# of each kernel's text. check compares those sums, which needs no
# compiler; with the same compiler given, it compares every byte too.
# tools/rend/texdecode_test.sh runs check.
set -e
cd "$(dirname "$0")/../.."
INC=core/rend/texdecode_dxbc.inc
INC9=core/rend/texdecode_dx9.inc
# kernel: profile
KERNELS="1:cs_5_0 2:cs_5_0 3:vs_5_0 4:ps_5_0"
KERNELS9="1:vs_3_0 2:ps_3_0"
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
WINE=${WINE-wine}

WORK=${TMPDIR:-/tmp}/kernels_dxbc.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

source_of() {
   echo "#define TEXD_SHADER_HLSL"; echo "#define TEXD_KERNEL_D3D $1"; cat core/rend/texdecode.h
}

source9_of() {
   echo "#define TEXD_SM3_KERNEL $1"; cat core/rend/texdecode_sm3.h
}

compiler_sum() {
   if [ -n "$D3DCOMPILER" ] && [ -f "$D3DCOMPILER" ]; then sha1sum < "$D3DCOMPILER" | cut -c1-40; fi
}

winpath() {
   case "$1" in /*) echo "Z:$1" ;; *) echo "$1" ;; esac
}

sums() {
   for k in $KERNELS; do
      n=${k%%:*}; p=${k#*:}
      source_of $n > "$WORK/s.hlsl"
      echo " * kernel $n $p $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

sums9() {
   for k in $KERNELS9; do
      n=${k%%:*}; p=${k#*:}
      source9_of $n > "$WORK/s.hlsl"
      echo " * kernel $n $p $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
   done
}

# @1 the source, @2 the profile, @3 the bytecode
compile() {
   [ -f "$WORK/dxbc_compile.exe" ] || $CC_WIN -O1 -o "$WORK/dxbc_compile.exe" tools/rend/dxbc_compile.c
   # (wine wants the TMPDIR it was set up with)
   env -u TMPDIR WINEDEBUG=-all $WINE "$WORK/dxbc_compile.exe" "$(winpath "$D3DCOMPILER")" "$(winpath "$1")" $2 \
      "$(winpath "$3")" > "$WORK/log" 2>&1 || { cat "$WORK/log" >&2; exit 1; }
}

make_inc() {
   echo "/* DXBC of the Direct3D 11 renderers' texture kernels (rend/texdecode.h,"
   echo " * each with TEXD_SHADER_HLSL and its TEXD_KERNEL_D3D): made by"
   echo " * tools/rend/kernels_dxbc.sh update from it. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   sums
   echo " */"
   for k in $KERNELS; do
      n=${k%%:*}; p=${k#*:}
      source_of $n > "$WORK/k$n.hlsl"
      compile "$WORK/k$n.hlsl" $p "$WORK/k$n.dxbc"
      echo "static const unsigned char texdecode_dxbc_$n[] = {"
      od -An -v -tx1 -w16 "$WORK/k$n.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by TEXD_KERNEL_D3D: the bytes, and how many"
   echo "static const unsigned char * const texdecode_dxbc[] = { 0, texdecode_dxbc_1, texdecode_dxbc_2, texdecode_dxbc_3, texdecode_dxbc_4 };"
   echo "static const size_t texdecode_dxbc_size[] = { 0, sizeof(texdecode_dxbc_1), sizeof(texdecode_dxbc_2), sizeof(texdecode_dxbc_3), sizeof(texdecode_dxbc_4) };"
}

# (Direct3D 9 takes its bytecode as words)
make_inc9() {
   echo "/* Bytecode of the Direct3D 9 renderer's texture shaders (rend/texdecode_sm3.h,"
   echo " * each with its TEXD_SM3_KERNEL): made by tools/rend/kernels_dxbc.sh update"
   echo " * from it. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   sums9
   echo " */"
   for k in $KERNELS9; do
      n=${k%%:*}; p=${k#*:}
      source9_of $n > "$WORK/n$n.hlsl"
      compile "$WORK/n$n.hlsl" $p "$WORK/n$n.dxbc"
      echo "static const u32 texdecode_dx9_$n[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/n$n.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by TEXD_SM3_KERNEL: the words"
   echo "static const u32 * const texdecode_dx9[] = { 0, texdecode_dx9_1, texdecode_dx9_2 };"
}

# @1 the file, @2 its sums, @3 what makes it
check_one() {
   $2 > "$WORK/sums"
   grep '^ \* kernel ' $1 > "$WORK/had" || true
   if ! cmp -s "$WORK/sums" "$WORK/had"; then
      echo "FAIL: $1 is not the bytecode of its header as it is: run '$0 update'" >&2
      return 1
   fi
   c=$(compiler_sum)
   if [ -n "$c" ] && grep -q "^ \* d3dcompiler_47 $c\$" $1; then
      $3 > "$WORK/new.inc"
      if ! cmp -s "$WORK/new.inc" $1; then
         echo "FAIL: $1 is not what that d3dcompiler_47.dll makes of its header: run '$0 update'" >&2
         return 1
      fi
      echo "kernels_dxbc: $1 is its header's bytecode (sums and bytes)"
   else
      echo "kernels_dxbc: $1 is its header's bytecode (sums; the bytes not compared: no D3DCOMPILER, or another one)"
   fi
}

case "$1" in
update)
   [ -n "$(compiler_sum)" ] || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed to make them" >&2; exit 1; }
   make_inc > "$WORK/new.inc"
   make_inc9 > "$WORK/new9.inc"
   cp "$WORK/new.inc" $INC
   cp "$WORK/new9.inc" $INC9
   echo "made $INC and $INC9"
   ;;
check)
   rc=0
   check_one $INC sums make_inc || rc=1
   check_one $INC9 sums9 make_inc9 || rc=1
   exit $rc
   ;;
*)
   echo "usage: $0 update | check" >&2
   exit 2
   ;;
esac
