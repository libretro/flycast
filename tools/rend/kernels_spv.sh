#!/bin/sh
# The Vulkan renderers' shaders as SPIR-V, made here and committed, so that
# the core compiles none of them when it runs, and a build of it needs no
# glslangValidator:
#
#   core/rend/ta_gpu_spv.inc     GPU-side TA processing's decode
#                                (rend/ta_gpu.h; vulkan/ta_decoder.cpp)
#   core/rend/texdecode_spv.inc  textures made on the GPU
#                                (rend/texdecode.h; vulkan/tex_decoder.cpp)
#   core/rend/vulkan/raster_spv.inc
#                                the raster renderers' shaders
#                                (vulkan/shaders/; vulkan/shaders.cpp)
#   core/rend/vulkan/compute_spv.inc
#                                the compute renderer's kernels
#                                (rend/tile/pvr_compute.h, after the shared
#                                rules' headers; vulkan/compute_drawer.cpp)
#
# and the OpenGL renderers' (gles/ta_decoder_gl.cpp), whose OpenGL takes
# SPIR-V (4.6, ARB_gl_spirv) or else compiles the text once:
#
#   core/rend/ta_gpu_gl_spv.inc  the decode's kernels for OpenGL
#                                (TAG_SHADER_GL: --target-env opengl)
#   core/rend/ta_gpu_glsl.inc    rend/ta_gpu.h itself as text, a line a string
#   core/rend/texdecode_gl.inc   textures made on the GPU (gles/gl_texdecoder.cpp):
#                                rend/texdecode.h's text, and its kernels as
#                                SPIR-V for OpenGL 4.6 (the bytes from a shader
#                                storage buffer, uniforms at the locations
#                                they are given)
#
#   sh tools/rend/kernels_spv.sh update   all made again from their sources
#   sh tools/rend/kernels_spv.sh check    whether all are their sources' still
#
# The raster shaders are modules of a few #defines each: what changes what
# a shader declares (interpolation, the sample rate, the per-pixel
# renderer's pass, SSAA). Everything else a polygon needs is in the bits of
# a specialization constant (vulkan/pso_key.h), so the same words serve all
# of them and no module is made for a combination of them.
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
TA_GL_INC=core/rend/ta_gpu_gl_spv.inc
TA_GLSL_INC=core/rend/ta_gpu_glsl.inc
TAG_KERNELS=$(awk '$1 == "#define" && $2 == "TAG_KERNELS" { print $3 }' core/rend/ta_gpu.h)
TEXD_FORMATS="r8ui r16ui r32ui"
RASTER_INC=core/rend/vulkan/raster_spv.inc
COMPUTE_INC=core/rend/vulkan/compute_spv.inc
PVRC_KERNELS=$(awk '$1 == "#define" && $2 == "PVRC_KERNELS" { print $3 }' core/rend/tile/pvr_compute.h)
PVRC_K_WALK=$(awk '$1 == "#define" && $2 == "PVRC_K_WALK" { print $3 }' core/rend/tile/pvr_compute.h)
RS=core/rend/vulkan/shaders
GL_INC=core/rend/texdecode_gl.inc
# kernel: stage
GL_KERNELS="1:comp 2:comp 3:vert 4:frag"

WORK=${TMPDIR:-/tmp}/kernels_spv.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

glslang_version() {
   glslangValidator --version 2>/dev/null | sed -n 's/^Glslang Version: *//p' | head -n 1
}

ta_source() {
   echo "#version 450"; echo "#define TAG_SHADER_GLSL"; echo "#define TAG_KERNEL $1"; cat core/rend/ta_gpu.h
}

ta_gl_source() {
   echo "#version 430"; echo "#define TAG_SHADER_GLSL"; echo "#define TAG_SHADER_GL"; echo "#define TAG_KERNEL $1"; cat core/rend/ta_gpu.h
}

texd_source() {
   echo "#version 450"; echo "#define TEXD_SHADER_GLSL"; echo "#define TEXD_KERNEL_FORMAT $1"; cat core/rend/texdecode.h
}

gl_source() {
   echo "#version 450"; echo "#define TEXD_SHADER_GLSL"; echo "#define TEXD_KERNEL_GL $1"; echo "#define TEXD_GL_RING 1"
   echo "#define TEXD_GL_LOC(n) layout(location = n)"; cat core/rend/texdecode.h
}

# one array: @1 its name, @2 the kernel's text (@3: opengl, for OpenGL's SPIR-V)
spv_array() {
   if [ "$3" = opengl ]; then
      glslangValidator -G -g0 --target-env opengl -S comp -o "$WORK/k.spv" "$2" > "$WORK/log" || { cat "$WORK/log" >&2; exit 1; }
   else
      glslangValidator -V -g0 --target-env vulkan1.0 -S comp -o "$WORK/k.spv" "$2" > "$WORK/log" || { cat "$WORK/log" >&2; exit 1; }
   fi
   echo "static const u32 $1[] = {"
   od -An -v -w32 -tx4 --endian=little "$WORK/k.spv" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
   echo "};"
}

# a compute kernel's text: the shared rules' headers without their
# #includes, then pvr_compute.h; the walk exchanges between lanes through
# shared memory (pvr_lane.h), the others have no use for it
compute_source() {
   echo "#version 450"; echo "#define PVR_SHADER_GLSL"; echo "#define PVRC_SHADER_GLSL"; echo "#define PVRC_KERNEL $1"
   if [ "$1" = "$PVRC_K_WALK" ]; then echo "#define PVR_LANE_COUNT 8"; echo "#define PVR_LANE_GROUP_SIZE 256"; else echo "#define PVR_LANE_NONE"; fi
   for h in core/rend/tile/pvr_lane.h core/rend/tile/pvr_tile.h core/rend/tile/pvr_modvol.h core/rend/tile/pvr_resolve.h core/rend/ssaa_pattern.h core/rend/tile/pvr_compute.h; do
      sed '/^#include/d' $h
   done
}

sums() {
   # @1: ta, tagl, texd, gl or compute
   if [ "$1" = gl ]; then
      echo " * text $(sha1sum < core/rend/texdecode.h | cut -c1-40)"
      for k in $GL_KERNELS; do
         gl_source ${k%%:*} > "$WORK/s.glsl"
         echo " * kernel gl${k%%:*} $(sha1sum < "$WORK/s.glsl" | cut -c1-40)"
      done
   elif [ "$1" = compute ]; then
      k=1
      while [ $k -le "$PVRC_KERNELS" ]; do
         compute_source $k > "$WORK/s.comp"
         echo " * kernel $k $(sha1sum < "$WORK/s.comp" | cut -c1-40)"
         k=$((k + 1))
      done
   elif [ "$1" = ta ] || [ "$1" = tagl ]; then
      k=1
      while [ $k -le "$TAG_KERNELS" ]; do
         if [ "$1" = ta ]; then ta_source $k > "$WORK/s.comp"; else ta_gl_source $k > "$WORK/s.comp"; fi
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

make_ta_gl() {
   echo "/* SPIR-V for OpenGL (ARB_gl_spirv) of GPU-side TA processing's kernels"
   echo " * (rend/ta_gpu.h, each with TAG_SHADER_GLSL, TAG_SHADER_GL and its"
   echo " * TAG_KERNEL): made by tools/rend/kernels_spv.sh update from it. Not to be"
   echo " * edited."
   echo " * glslang $(glslang_version)"
   sums tagl
   echo " */"
   k=1
   while [ $k -le "$TAG_KERNELS" ]; do
      ta_gl_source $k > "$WORK/s.comp"
      spv_array ta_gpu_gl_spv_$k "$WORK/s.comp" opengl
      k=$((k + 1))
   done
   echo "// by TAG_K_*: the words, and their size in bytes"
   printf 'static const u32 * const ta_gpu_gl_spv[] = { nullptr'
   k=1; while [ $k -le "$TAG_KERNELS" ]; do printf ', ta_gpu_gl_spv_%d' $k; k=$((k + 1)); done
   echo ' };'
   printf 'static const size_t ta_gpu_gl_spv_size[] = { 0'
   k=1; while [ $k -le "$TAG_KERNELS" ]; do printf ', sizeof(ta_gpu_gl_spv_%d)' $k; k=$((k + 1)); done
   echo ' };'
}

# rend/ta_gpu.h as text, for an OpenGL without SPIR-V to compile: a line a
# string (no one string too long for any compiler), each ending in its new line
make_ta_glsl() {
   echo "/* rend/ta_gpu.h as text, a line a string, for the OpenGL renderers to"
   echo " * compile the kernels from where OpenGL takes no SPIR-V (the lines that"
   echo " * go in front: gles/ta_decoder_gl.cpp). Made by tools/rend/kernels_spv.sh"
   echo " * update from it. Not to be edited."
   echo " * header $(sha1sum < core/rend/ta_gpu.h | cut -c1-40)"
   echo " */"
   echo "static const char * const ta_gpu_glsl[] = {"
   sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/?/\\?/g' -e 's/^/"/' -e 's/$/\\n",/' core/rend/ta_gpu.h
   echo "};"
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

make_gl() {
   echo "/* The OpenGL renderers' texture kernels (rend/texdecode.h, with"
   echo " * TEXD_SHADER_GLSL and its TEXD_KERNEL_GL): the header's text, which they"
   echo " * compile when they start, and the kernels as SPIR-V for OpenGL 4.6 (the"
   echo " * bytes from a shader storage buffer). Made by tools/rend/kernels_spv.sh"
   echo " * update from it. Not to be edited."
   echo " * glslang $(glslang_version)"
   sums gl
   echo " */"
   echo "// (in pieces, of no more than some 16 KB each, for every compiler)"
   echo "static const char texdecode_glsl[] ="
   split -l 120 core/rend/texdecode.h "$WORK/text."
   for f in "$WORK"/text.*; do
      printf 'R"TEXD('; cat "$f"; echo ')TEXD"'
   done
   echo ";"
   for k in $GL_KERNELS; do
      n=${k%%:*}
      gl_source $n > "$WORK/k.${k#*:}"
      glslangValidator -G -g0 --target-env opengl -o "$WORK/k.spv" "$WORK/k.${k#*:}" > "$WORK/log" || { cat "$WORK/log" >&2; exit 1; }
      echo "static const u32 texdecode_glspv_$n[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/k.spv" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by TEXD_KERNEL_GL: the words, and their size in bytes"
   echo "static const u32 * const texdecode_glspv[] = { 0, texdecode_glspv_1, texdecode_glspv_2, texdecode_glspv_3, texdecode_glspv_4 };"
   echo "static const size_t texdecode_glspv_size[] = { 0, sizeof(texdecode_glspv_1), sizeof(texdecode_glspv_2), sizeof(texdecode_glspv_3), sizeof(texdecode_glspv_4) };"
}

make_compute() {
   echo "/* SPIR-V of the compute renderer's kernels (rend/tile/pvr_compute.h, each"
   echo " * after the shared rules' headers, with PVRC_SHADER_GLSL and its"
   echo " * PVRC_KERNEL): made by tools/rend/kernels_spv.sh update from them. Not"
   echo " * to be edited."
   echo " * glslang $(glslang_version)"
   sums compute
   echo " */"
   k=1
   while [ $k -le "$PVRC_KERNELS" ]; do
      compute_source $k > "$WORK/s.comp"
      spv_array compute_spv_$k "$WORK/s.comp"
      k=$((k + 1))
   done
   echo "// by PVRC_K_*: the words, and their size in bytes"
   printf 'static const u32 * const compute_spv[] = { nullptr'
   k=1; while [ $k -le "$PVRC_KERNELS" ]; do printf ', compute_spv_%d' $k; k=$((k + 1)); done
   echo ' };'
   printf 'static const size_t compute_spv_size[] = { 0'
   k=1; while [ $k -le "$PVRC_KERNELS" ]; do printf ', sizeof(compute_spv_%d)' $k; k=$((k + 1)); done
   echo ' };'
}

# The raster modules, one a line: the name, the stage, the #defines
# (NAME=VALUE, comma separated, - for none) and what the text is made of
raster_list() {
   for g in 0 1; do echo "TRI_VERT_G$g vert pp_Gouraud=$g tri.vert"; done
   for g in 0 1; do for r in 0 1 2; do
      echo "TRI_FRAG_G${g}_R$r frag pp_Gouraud=$g,pp_SampleRate=$r key tri.frag"
   done; done
   echo "MODVOL_VERT vert - modvol.vert"
   echo "MODVOL_FRAG frag - modvol.frag"
   echo "QUAD_VERT vert - quad.vert"
   echo "QUAD_FRAG frag - quad.frag"
   echo "SSAA_RESOLVE_VERT vert - ssaa_resolve.vert"
   echo "SSAA_RESOLVE_FRAG frag - key resolve ssaa_resolve.frag"
   for t in 0 1; do for g in 0 1; do for r in 0 1 2; do
      echo "PREPASS_FRAG_T${t}_G${g}_R$r frag pp_Texture=$t,pp_Gouraud=$g,pp_SampleRate=$r key prepass.frag"
   done; done; done
   for g in 0 1; do echo "OIT_VERT_G$g vert pp_Gouraud=$g oit.vert"; done
   for g in 0 1; do for p in 0 1 2; do for r in 0 1 2; do
      [ $p = 2 ] && [ $r = 1 ] && continue
      ss=; [ $r != 0 ] && ss=,SSAA=1
      echo "OIT_FRAG_G${g}_P${p}_R$r frag pp_Gouraud=$g,PASS=$p,pp_SampleRate=$r$ss key oit_header.glsl oit.frag"
   done; done; done
   echo "OIT_MODVOL_FRAG frag - key oit_header.glsl oit_modvol.frag"
   echo "OIT_CLEAR_FRAG frag - key oit_header.glsl oit_clear.frag"
   for m in 0 1 2 3; do
      echo "OIT_TRMODVOL_FRAG_M${m}_S0 frag MV_MODE=$m key oit_header.glsl oit_trmodvol.frag"
      echo "OIT_TRMODVOL_FRAG_M${m}_S1 frag MV_MODE=$m,SSAA=1 key oit_header.glsl oit_trmodvol.frag"
   done
   echo "OIT_FINAL_FRAG_S0 frag - key oit_header.glsl oit_final.frag"
   echo "OIT_FINAL_FRAG_S1 frag SSAA=1 key resolve oit_header.glsl oit_final.frag"
   echo "OIT_FINAL_FRAG_S2 frag SSAA=1,SSAA_PER_SAMPLE=1 key resolve oit_header.glsl oit_final.frag"
   echo "OIT_FINAL_VERT vert - oit_final.vert"
}

# a raster module's text: @1 its defines, then what it is made of
raster_source() {
   echo "#version 450"
   if [ "$1" != - ]; then echo "$1" | tr ',' '\n' | sed 's/^\([^=]*\)=\(.*\)$/#define \1 \2/'; fi
   shift
   for part in "$@"; do
      case $part in
      key) cat core/rend/vulkan/pso_key.h ;;
      resolve) echo "#define PVR_SHADER_GLSL"; echo "#define PVR_LANE_NONE"; sed -e '1,2d' -e '$d' core/rend/tile/pvr_resolve_text.inc ;;
      *) cat $RS/$part ;;
      esac
   done
}

raster_sums() {
   raster_list | while read name stage defs parts; do
      raster_source $defs $parts > "$WORK/s.glsl"
      echo " * module $name $(sha1sum < "$WORK/s.glsl" | cut -c1-40)"
   done
}

make_raster() {
   echo "/* SPIR-V of the raster renderers' shaders (vulkan/shaders/, with the"
   echo " * #defines of each module and vulkan/pso_key.h): made by"
   echo " * tools/rend/kernels_spv.sh update from them. Not to be edited."
   echo " * glslang $(glslang_version)"
   raster_sums
   echo " */"
   echo "#ifndef VK_RASTER_SPV_IDS"
   echo "#define VK_RASTER_SPV_IDS"
   echo "enum RasterSpv {"
   raster_list | while read name stage defs parts; do echo "	RSPV_$name,"; done
   echo "	RSPV_COUNT"
   echo "};"
   echo "#endif"
   echo "#if defined(RASTER_SPV_WORDS) && !defined(VK_RASTER_SPV_WORDS)"
   echo "#define VK_RASTER_SPV_WORDS"
   raster_list | while read name stage defs parts; do
      raster_source $defs $parts > "$WORK/s.$stage"
      glslangValidator -V -g0 --target-env vulkan1.0 -S $stage -o "$WORK/k.spv" "$WORK/s.$stage" > "$WORK/log" || { cat "$WORK/log" >&2; echo "$name" >&2; exit 1; }
      echo "static const u32 raster_spv_$name[] = {"
      od -An -v -w32 -tx4 --endian=little "$WORK/k.spv" | awk '{ s = ""; for (i = 1; i <= NF; i++) s = s "0x" $i ","; print s }'
      echo "};"
   done
   echo "// by RasterSpv: the words, and their size in bytes"
   printf 'static const u32 * const raster_spv[] = {'
   raster_list | while read name stage defs parts; do printf ' raster_spv_%s,' $name; done
   echo ' };'
   printf 'static const size_t raster_spv_size[] = {'
   raster_list | while read name stage defs parts; do printf ' sizeof(raster_spv_%s),' $name; done
   echo ' };'
   echo "#endif"
}

case "$1" in
update)
   command -v glslangValidator > /dev/null || { echo "$0: glslangValidator is needed to make them" >&2; exit 1; }
   make_ta > "$WORK/ta.inc"
   make_texd > "$WORK/texd.inc"
   make_raster > "$WORK/raster.inc"
   make_ta_gl > "$WORK/tagl.inc"
   make_ta_glsl > "$WORK/taglsl.inc"
   make_gl > "$WORK/gl.inc"
   make_compute > "$WORK/compute.inc"
   cp "$WORK/ta.inc" $TA_INC
   cp "$WORK/texd.inc" $TEXD_INC
   cp "$WORK/raster.inc" $RASTER_INC
   cp "$WORK/tagl.inc" $TA_GL_INC
   cp "$WORK/taglsl.inc" $TA_GLSL_INC
   cp "$WORK/gl.inc" $GL_INC
   cp "$WORK/compute.inc" $COMPUTE_INC
   echo "made $TA_INC, $TEXD_INC, $RASTER_INC, $TA_GL_INC, $TA_GLSL_INC, $GL_INC and $COMPUTE_INC"
   ;;
check)
   rc=0
   make_ta_glsl > "$WORK/taglsl.inc"
   if cmp -s "$WORK/taglsl.inc" $TA_GLSL_INC; then
      echo "kernels_spv: $TA_GLSL_INC is its header's text"
   else
      echo "FAIL: $TA_GLSL_INC is not its header's text as it is: run '$0 update'" >&2
      rc=1
   fi
   for t in ta texd raster tagl gl compute; do
      case $t in ta) inc=$TA_INC ;; texd) inc=$TEXD_INC ;; raster) inc=$RASTER_INC ;; tagl) inc=$TA_GL_INC ;; gl) inc=$GL_INC ;; compute) inc=$COMPUTE_INC ;; esac
      if [ $t = raster ]; then
         raster_sums > "$WORK/sums"
         grep '^ \* module ' $inc > "$WORK/had" || true
      else
         sums $t > "$WORK/sums"
         # (the file's first comment: the OpenGL one has the header's text after it)
         sed -n '1,/\*\//p' $inc | grep '^ \* kernel \|^ \* text ' > "$WORK/had" || true
      fi
      if ! cmp -s "$WORK/sums" "$WORK/had"; then
         echo "FAIL: $inc is not the SPIR-V of its header as it is: run '$0 update'" >&2
         rc=1
         continue
      fi
      v=$(glslang_version)
      if [ -n "$v" ] && grep -q "^ \* glslang $v\$" $inc; then
         case $t in ta) make_ta > "$WORK/new.inc" ;; texd) make_texd > "$WORK/new.inc" ;; raster) make_raster > "$WORK/new.inc" ;; tagl) make_ta_gl > "$WORK/new.inc" ;; gl) make_gl > "$WORK/new.inc" ;; compute) make_compute > "$WORK/new.inc" ;; esac
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
