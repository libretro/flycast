#!/bin/sh
# The Direct3D 12 renderer's shaders as bytecode, made here and committed,
# so that the core compiles none of them when it runs and a build of it
# needs no shader compiler:
#
#   core/rend/dx12/dx12_dxbc.inc  rend/dx12/shaders/dx12_raster.hlsl, each
#                                 shader with its defines and
#                                 rend/dx12/dx12_shader_flags.h in front
#
#   sh tools/rend/kernels_dx12.sh update   made again from the sources
#   sh tools/rend/kernels_dx12.sh check    whether it is the sources' still
#
# DXBC, shader model 5.1 (descriptor tables indexed in the shader, for the
# bindless textures), which Direct3D 12 takes as it is: no DXIL, so no
# dxc either. Made with the same d3dcompiler_47.dll (D3DCompile,
# optimisation level 3) and tools/rend/dxbc_compile.c as the texture
# kernels (tools/rend/kernels_dxbc.sh), which the renderer takes from
# rend/texdecode_dxbc.inc as they are (shader model 5.0):
#   D3DCOMPILER=/path/to/d3dcompiler_47.dll  the compiler (needed to update)
#   CC_WIN=x86_64-w64-mingw32-gcc            what builds dxbc_compile.exe
#   WINE=wine                                what runs it ("" on Windows)
# The file says which d3dcompiler_47.dll made it (its SHA-1) and the SHA-1
# of each shader's text. check compares those sums, which needs no
# compiler; with the same compiler given, it compares every byte too.
# tools/rend/texdecode_test.sh runs check.
set -e
cd "$(dirname "$0")/../.."
INC=core/rend/dx12/dx12_dxbc.inc
SRC=core/rend/dx12/shaders/dx12_raster.hlsl
FLAGS=core/rend/dx12/dx12_shader_flags.h
# number:profile:defines (the numbers are dx12_shaders.h's DX12Shader)
SHADERS="1:vs_5_1:DX12_POLY_VS,DX12_GOURAUD=1 2:vs_5_1:DX12_POLY_VS,DX12_GOURAUD=0 3:ps_5_1:DX12_POLY_PS,DX12_GOURAUD=1 4:ps_5_1:DX12_POLY_PS,DX12_GOURAUD=0 5:vs_5_1:DX12_MODVOL_VS 6:ps_5_1:DX12_MODVOL_PS 7:vs_5_1:DX12_QUAD_VS 8:ps_5_1:DX12_QUAD_PS"
# the per-pixel renderer's: the vertex shaders, then a pixel shader for each pass and interpolation
SHADERS="$SHADERS 9:vs_5_1:DX12_OIT_VS,DX12_GOURAUD=1 10:vs_5_1:DX12_OIT_VS,DX12_GOURAUD=0"
SHADERS="$SHADERS 11:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=0 12:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=0"
SHADERS="$SHADERS 13:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=1 14:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=1"
SHADERS="$SHADERS 15:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=1,DX12_OIT_PASS=2 16:ps_5_1:DX12_OIT_PS,DX12_GOURAUD=0,DX12_OIT_PASS=2"
SHADERS="$SHADERS 17:vs_5_1:DX12_OIT_FINAL_VS 18:ps_5_1:DX12_OIT_FINAL_PS 19:ps_5_1:DX12_OIT_MODVOL_PS"
COUNT=19
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}
WINE=${WINE-wine}

WORK=${TMPDIR:-/tmp}/kernels_dx12.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# @1 the defines, a,b=1,...
source_of() {
   echo "$1" | tr ',' '\n' | while read d; do
      case "$d" in *=*) echo "#define ${d%%=*} ${d#*=}" ;; *) echo "#define $d" ;; esac
   done
   cat $FLAGS $SRC
}

compiler_sum() {
   if [ -n "$D3DCOMPILER" ] && [ -f "$D3DCOMPILER" ]; then sha1sum < "$D3DCOMPILER" | cut -c1-40; fi
}

winpath() {
   case "$1" in /*) echo "Z:$1" ;; *) echo "$1" ;; esac
}

sums() {
   for k in $SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/s.hlsl"
      echo " * shader $n $p $(sha1sum < "$WORK/s.hlsl" | cut -c1-40)"
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
   echo "/* DXBC of the Direct3D 12 renderer's shaders (rend/dx12/shaders/dx12_raster.hlsl,"
   echo " * each with its defines and rend/dx12/dx12_shader_flags.h): made by"
   echo " * tools/rend/kernels_dx12.sh update from them. Not to be edited."
   echo " * d3dcompiler_47 $(compiler_sum)"
   sums
   echo " */"
   for k in $SHADERS; do
      n=${k%%:*}; r=${k#*:}; p=${r%%:*}; d=${r#*:}
      source_of $d > "$WORK/k$n.hlsl"
      compile "$WORK/k$n.hlsl" $p "$WORK/k$n.dxbc"
      echo "static const unsigned char dx12_dxbc_$n[] = {"
      od -An -v -tx1 -w16 "$WORK/k$n.dxbc" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by DX12Shader: the bytes, and how many"
   l="static const unsigned char * const dx12_dxbc[] = { 0"
   z="static const size_t dx12_dxbc_size[] = { 0"
   i=1
   while [ $i -le $COUNT ]; do l="$l, dx12_dxbc_$i"; z="$z, sizeof(dx12_dxbc_$i)"; i=$((i + 1)); done
   echo "$l };"
   echo "$z };"
}

case "$1" in
update)
   [ -n "$(compiler_sum)" ] || { echo "$0: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed to make them" >&2; exit 1; }
   make_inc > "$WORK/new.inc"
   cp "$WORK/new.inc" $INC
   echo "made $INC"
   ;;
check)
   sums > "$WORK/sums"
   grep '^ \* shader ' $INC > "$WORK/had" || true
   if ! cmp -s "$WORK/sums" "$WORK/had"; then
      echo "FAIL: $INC is not the bytecode of its sources as they are: run '$0 update'" >&2
      exit 1
   fi
   c=$(compiler_sum)
   if [ -n "$c" ] && grep -q "^ \* d3dcompiler_47 $c\$" $INC; then
      make_inc > "$WORK/new.inc"
      if ! cmp -s "$WORK/new.inc" $INC; then
         echo "FAIL: $INC is not what that d3dcompiler_47.dll makes of its sources: run '$0 update'" >&2
         exit 1
      fi
      echo "kernels_dx12: $INC is its sources' bytecode (sums and bytes)"
   else
      echo "kernels_dx12: $INC is its sources' bytecode (sums; the bytes not compared: no D3DCOMPILER, or another one)"
   fi
   ;;
*)
   echo "usage: $0 update | check" >&2
   exit 2
   ;;
esac
