#!/bin/sh
# The Vulkan renderer's compute kernels as SPIR-V, made here and committed,
# so that the core compiles none of them when it runs, and a build of it
# needs no glslangValidator:
#
#   core/rend/ta_gpu_spv.inc     GPU-side TA processing's decode
#                                (rend/ta_gpu.h; vulkan/ta_decoder.cpp)
#   core/rend/texdecode_spv.inc  textures made on the GPU
#                                (rend/texdecode.h; vulkan/tex_decoder.cpp)
#
#   sh tools/rend/kernels_spv.sh update   both made again from the headers
#   sh tools/rend/kernels_spv.sh check    whether both are the headers' still
#
# Each kernel is the header with the lines the renderer used to put in front
# of it, compiled with glslangValidator -V -g0 --target-env vulkan1.0 (what
# the core's own glslang made of it: SPIR-V 1.0, without the names). The
# files say which glslangValidator made them, and the SHA-1 of each
# kernel's text. check compares those sums, which needs no glslangValidator;
# with the same glslangValidator it compares every word too.
# tools/pvr/ta_gpu_test.sh and tools/rend/texdecode_test.sh run check.
set -e
cd "$(dirname "$0")/../.."
TA_INC=core/rend/ta_gpu_spv.inc
TEXD_INC=core/rend/texdecode_spv.inc
TAG_KERNELS=$(awk '$1 == "#define" && $2 == "TAG_KERNELS" { print $3 }' core/rend/ta_gpu.h)
TEXD_FORMATS="r8ui r16ui r32ui"

WORK=${TMPDIR:-/tmp}/kernels_spv.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

glslang_version() {
   glslangValidator --version 2>/dev/null | sed -n 's/^Glslang Version: *//p' | head -n 1
}

ta_source() {
   echo "#version 450"; echo "#define TAG_SHADER_GLSL"; echo "#define TAG_KERNEL $1"; cat core/rend/ta_gpu.h
}

texd_source() {
   echo "#version 450"; echo "#define TEXD_SHADER_GLSL"; echo "#define TEXD_KERNEL_FORMAT $1"; cat core/rend/texdecode.h
}

# one array: @1 its name, @2 the kernel's text
spv_array() {
   glslangValidator -V -g0 --target-env vulkan1.0 -S comp -o "$WORK/k.spv" "$2" > "$WORK/log" || { cat "$WORK/log" >&2; exit 1; }
   echo "static const u32 $1[] = {"
   od -An -v -w32 -tx4 --endian=little "$WORK/k.spv" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
   echo "};"
}

sums() {
   # @1: ta or texd
   if [ "$1" = ta ]; then
      k=1
      while [ $k -le "$TAG_KERNELS" ]; do
         ta_source $k > "$WORK/s.comp"
         echo " * kernel $k $(sha1sum < "$WORK/s.comp" | cut -c1-40)"
         k=$((k + 1))
      done
   else
      for f in $TEXD_FORMATS; do
         texd_source $f > "$WORK/s.comp"
         echo " * kernel $f $(sha1sum < "$WORK/s.comp" | cut -c1-40)"
      done
   fi
}

make_ta() {
   echo "/* SPIR-V of GPU-side TA processing's kernels (rend/ta_gpu.h, each with"
   echo " * TAG_SHADER_GLSL and its TAG_KERNEL): made by tools/rend/kernels_spv.sh"
   echo " * update from it. Not to be edited."
   echo " * glslang $(glslang_version)"
   sums ta
   echo " */"
   k=1
   while [ $k -le "$TAG_KERNELS" ]; do
      ta_source $k > "$WORK/s.comp"
      spv_array ta_gpu_spv_$k "$WORK/s.comp"
      k=$((k + 1))
   done
   echo "// by TAG_K_*: the words, and their size in bytes"
   printf 'static const u32 * const ta_gpu_spv[] = { nullptr'
   k=1; while [ $k -le "$TAG_KERNELS" ]; do printf ', ta_gpu_spv_%d' $k; k=$((k + 1)); done
   echo ' };'
   printf 'static const size_t ta_gpu_spv_size[] = { 0'
   k=1; while [ $k -le "$TAG_KERNELS" ]; do printf ', sizeof(ta_gpu_spv_%d)' $k; k=$((k + 1)); done
   echo ' };'
}

make_texd() {
   echo "/* SPIR-V of the kernels that make textures on the GPU (rend/texdecode.h,"
   echo " * each with TEXD_SHADER_GLSL and its TEXD_KERNEL_FORMAT): made by"
   echo " * tools/rend/kernels_spv.sh update from it. Not to be edited."
   echo " * glslang $(glslang_version)"
   sums texd
   echo " */"
   for f in $TEXD_FORMATS; do
      texd_source $f > "$WORK/s.comp"
      spv_array texdecode_spv_$f "$WORK/s.comp"
   done
   echo "// by TEXD_OUT8, 16, 32: the words, and their size in bytes"
   echo "static const u32 * const texdecode_spv[] = { texdecode_spv_r8ui, texdecode_spv_r16ui, texdecode_spv_r32ui };"
   echo "static const size_t texdecode_spv_size[] = { sizeof(texdecode_spv_r8ui), sizeof(texdecode_spv_r16ui), sizeof(texdecode_spv_r32ui) };"
}

case "$1" in
update)
   command -v glslangValidator > /dev/null || { echo "$0: glslangValidator is needed to make them" >&2; exit 1; }
   make_ta > "$WORK/ta.inc"
   make_texd > "$WORK/texd.inc"
   cp "$WORK/ta.inc" $TA_INC
   cp "$WORK/texd.inc" $TEXD_INC
   echo "made $TA_INC and $TEXD_INC"
   ;;
check)
   rc=0
   for t in ta texd; do
      if [ $t = ta ]; then inc=$TA_INC; else inc=$TEXD_INC; fi
      sums $t > "$WORK/sums"
      grep '^ \* kernel ' $inc > "$WORK/had" || true
      if ! cmp -s "$WORK/sums" "$WORK/had"; then
         echo "FAIL: $inc is not the SPIR-V of its header as it is: run '$0 update'" >&2
         rc=1
         continue
      fi
      v=$(glslang_version)
      if [ -n "$v" ] && grep -q "^ \* glslang $v\$" $inc; then
         if [ $t = ta ]; then make_ta > "$WORK/new.inc"; else make_texd > "$WORK/new.inc"; fi
         if ! cmp -s "$WORK/new.inc" $inc; then
            echo "FAIL: $inc is not what glslangValidator $v makes of its header: run '$0 update'" >&2
            rc=1
            continue
         fi
         echo "kernels_spv: $inc is its header's SPIR-V (sums and words)"
      else
         echo "kernels_spv: $inc is its header's SPIR-V (sums; the words are from another glslangValidator)"
      fi
   done
   exit $rc
   ;;
*)
   echo "usage: $0 update | check" >&2
   exit 2
   ;;
esac
