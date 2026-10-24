#!/bin/sh
# Specks a GPU draws and a software device never does: the raster renderers
# against the two ways they have come about.
#
# 1. Depth computed differently by two passes that test it against each
#    other (EQUAL, or a decal's greater or equal over the polygon it lies
#    on). A software device rounds every pipeline alike, a GPU does not: the
#    depth has to be the vertex position's, from one invariant (GLSL) or
#    precise (HLSL) expression, and no fragment shader may write a depth
#    (rend/depth_scale.h). Checked in the sources, with no device:
#      - no gl_FragDepth, SV_Depth or DEPTH output in any raster renderer
#        (Vulkan, OpenGL/GLES, Direct3D 9, 11, 12)
#      - every Vulkan vertex shader whose position is depth tested places it
#        with pvr_position() (vulkan/shaders/position.glsl, which declares
#        it invariant); the OpenGL ones declare gl_Position invariant; the
#        Direct3D 12 vertex shaders place it with the precise
#        transformVertex(), the Direct3D 11 ones' position is precise
#
# 2. The per-pixel renderers' translucent modifier volumes change the
#    fragments in the lists from their shaders: one draw toggles a stencil
#    bit, the draw that closes a volume reads and clears it. Nothing orders
#    what the shaders of two draws do to the same fragment but a barrier: a
#    GPU runs the draws of a subpass at once, in any order, a software
#    device one after the other. Without the barrier after a volume's
#    closing draw, its clearing lands in the middle of the next volume's
#    toggles, and translucent polygons under the volumes - the graffiti on
#    Jet Grind Radio's select screen - are shadowed where they are not, in
#    specks. Checked
#      - in the sources: each of the per-pixel renderers' (Vulkan, OpenGL 4,
#        Direct3D 12; Direct3D 11 orders a draw's UAV writes before the
#        next's itself) closing draw has a barrier before it and after it
#      - on a device, when CONTENT and STATE are given: the core is built
#        again with RACE_TEST, which records the draws between two
#        barriers a triangle at a time, the draws' triangles taken in turn
#        (vulkan/oit/oit_drawer.cpp, gl4/abuffer.cpp, dx12/oit/
#        dx12_oitrenderer.cpp: ListDraws), as a GPU may run them, and the
#        picture it draws has to be the same, byte for byte, as the core's
#        own - with Vulkan (lavapipe) and OpenGL 4 (llvmpipe), per-pixel
#        sorting, TA processing on the CPU and on the GPU, SSAA off and at
#        what the device has of 2x, 4x and 8x (lavapipe draws 1 or 4
#        samples), threaded rendering on and off, 1x and 2x.
#
#   sh tools/rend/speckle_test.sh                  the source checks
#   sh tools/rend/speckle_test.sh CONTENT STATE    ...and the device runs, from STATE (a
#                                                  RetroArch state, as lagmeter reads it)
#   ROOT=DIR sh tools/rend/speckle_test.sh         the source checks on another tree's
#                                                  files (an old commit's, checked out)
#
# PASS (exit 0) when everything holds; FAIL (exit 1) says what did not.
# The device runs need the core built for this machine first (make
# platform=unix: its objects are linked again), lavapipe and llvmpipe (or
# VK_ICD_FILENAMES / the EGL driver set to a GPU's), and take a few
# minutes; SYSTEM is the system directory (default: $WORK). Programs and
# pictures are made in $WORK (default: a new directory in $TMPDIR), kept
# with KEEP=1.
set -e
cd "$(dirname "$0")/../.."
ROOT=${ROOT:-.}
fail=0
bad() { echo "speckle_test: FAIL: $*"; fail=1; }

# ---- 1. the depth -------------------------------------------------------
R=$ROOT/core/rend
for f in $R/vulkan/shaders/*.frag $R/vulkan/shaders/*.glsl; do
   grep -q 'gl_FragDepth' "$f" && bad "$f writes gl_FragDepth"
done
for f in $R/gles/*.cpp $R/gl4/*.cpp; do
   grep -Eq 'gl_FragDepth *=' "$f" && bad "$f writes gl_FragDepth"
done
for f in $R/dx11/*.cpp $R/dx11/oit/*.cpp $R/dx12/shaders/*.hlsl; do
   [ -f "$f" ] || continue
   grep -Eq ':[[:space:]]*SV_Depth' "$f" && bad "$f writes SV_Depth"
done
for f in $R/dx9/*.cpp; do
   grep -Eq ':[[:space:]]*DEPTH[0-9]?\b' "$f" && bad "$f writes DEPTH"
done
if [ -f $R/vulkan/shaders/position.glsl ]; then
   grep -q '^invariant gl_Position;' $R/vulkan/shaders/position.glsl || bad "vulkan/shaders/position.glsl: gl_Position not invariant"
   for f in $R/vulkan/shaders/*.vert; do
      case $(basename "$f") in
      quad.vert|oit_final.vert|ssaa_resolve.vert) continue ;;	# (a picture over the screen: no depth test)
      esac
      grep -q 'gl_Position = pvr_position(' "$f" || bad "$f places its vertices without pvr_position()"
   done
else
   bad "no vulkan/shaders/position.glsl: the Vulkan vertex shaders do not share one position"
fi
for f in $R/gles/gles.cpp $R/gl4/gles.cpp; do
   grep -q '^invariant gl_Position;' "$f" || bad "$f: gl_Position not invariant"
done
H=$R/dx12/shaders/dx12_raster.hlsl
if [ -f $H ]; then
   grep -q '^precise float4 transformVertex' $H || bad "$H: transformVertex() not precise"
   for vs in DX12_POLY_VS DX12_OIT_VS DX12_MODVOL_VS; do
      awk -v vs="$vs" '$0 ~ "^#ifdef " vs "$" { on = 1 } on && /transformVertex\(/ { found = 1 } on && /^#endif/ { exit } END { exit !found }' $H \
         || bad "$H: $vs places its vertices without transformVertex()"
   done
fi
for f in $R/dx11/dx11_shaders.cpp $R/dx11/oit/dx11_oitshaders.cpp; do
   grep -q 'precise float4 pos *: *SV_POSITION' "$f" || bad "$f: the position is not precise"
done

# ---- 2. the lists' order, in the sources --------------------------------
# (the volume's closing draw - the line that picks the inclusion/exclusion
# shader - with a barrier between it and the volume's draws before, and one
# after its draw, before the next volume)
closing() {
   awk -v pick="$2" '
      $0 ~ pick { state = 1; if (barrier) before = 1; next }
      state == 1 && /listDraws\.Draw\(/ { state = 2; next }
      state == 2 && /listDraws\.Barrier\(\)/ { after = 1 }
      state == 2 && /mod_base = -1/ { state = 3 }
      /listDraws\.Barrier\(\)/ { barrier = 1 }
      /listDraws\.Draw\(/ && state == 0 { barrier = 0 }
      END { exit !(before && after) }' "$1" || bad "$1: the translucent modifier volumes' closing draw has no barrier before and after it ($3)"
}
closing $R/vulkan/oit/oit_drawer.cpp 'GetTrModifierVolumePipeline[(]mv_mode == 1' Vulkan
closing $R/gl4/abuffer.cpp 'g_abuffer_tr_modvol_shaders[[]mv_mode == 1' "OpenGL 4"
closing $R/dx12/oit/dx12_oitrenderer.cpp 'DX12_OF_MV_INCLUSION : DX12_OF_MV_EXCLUSION' "Direct3D 12"

if [ $# -lt 2 ]; then
   [ $fail = 0 ] && echo "speckle_test: PASS (the sources; give CONTENT STATE for the device runs)"
   exit $fail
fi

# ---- 2. the lists' order, on a device -----------------------------------
CONTENT=$1
STATE=$2
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/speckle_test.$$
   [ -n "$KEEP" ] || trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK/shots"
[ -f parallel_dc_libretro.so ] && [ -f core/rend/vulkan/oit/oit_drawer.o ] || { echo "speckle_test: build the core first (make platform=unix)" >&2; exit 2; }
LVP=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f $LVP ] && [ -z "$VK_ICD_FILENAMES" ] && export VK_ICD_FILENAMES=$LVP

${CC:-cc} -O2 -Icore/libretro-common/include -o "$WORK/lagmeter" tools/lag/lagmeter.c -ldl -lEGL -lGL
# the core again, its per-pixel renderers' list draws recorded as a GPU may run them
LINK=$(make -n platform=unix -W core/rend/vulkan/oit/oit_drawer.cpp 2>/dev/null | tail -1)
for src in core/rend/vulkan/oit/oit_drawer core/rend/gl4/abuffer; do
   o=$WORK/$(basename $src)_race.o
   line=$(make -n platform=unix -W $src.cpp $src.o 2>/dev/null | grep -m1 "$src.cpp")
   eval "$(echo "$line" | sed -e "s| -o $src.o| -o $o|" -e 's| -c | -DRACE_TEST -c |' -e 's| -MMD -MP||')"
   LINK=$(echo "$LINK" | sed "s| ./$src.o | $o |")
done
eval "$(echo "$LINK" | sed "s| -o parallel_dc_libretro.so| -o $WORK/race.so|")"

SYS=${SYSTEM:-$WORK}
FRAME=${FRAME:-6}
run() {	# run CORE NAME VIDEO key=value...: the picture after FRAME, $WORK/shots/NAME_FRAME.ppm
   r_core=$1; r_name=$2; r_video=$3; shift 3
   LAG_VIDEO=$r_video LAG_STATE=$STATE LAG_SHOT=$FRAME:$WORK/shots/${r_name}_ LAG_DIR=$SYS LAG_TRACE=/dev/null LAG_VERBOSE=1 \
      "$WORK/lagmeter" "$r_core" "$CONTENT" $((FRAME + 1)) "reicast_alpha_sorting=per-pixel (accurate)" \
      reicast_rendering_method=rasterizer reicast_use_real_bios=disabled "$@" > "$WORK/shots/$r_name.log" 2>&1
}
cases=0
check() {	# check NAME VIDEO key=value...
   name=$1; video=$2; shift 2
   run "$PWD/parallel_dc_libretro.so" $name $video "$@"
   run "$WORK/race.so" ${name}_race $video "$@"
   a=$WORK/shots/${name}_$FRAME.ppm; b=$WORK/shots/${name}_race_$FRAME.ppm
   if [ ! -s "$a" ] || [ ! -s "$b" ]; then
      bad "$name: no picture (see $WORK/shots/$name.log)"
      return
   fi
   grep -q 'per-pixel renderer' "$WORK/shots/$name.log" || { bad "$name: the per-pixel renderer was not the one made"; return; }
   samples=$(grep -o 'SSAA [0-9]* samples a pixel' "$WORK/shots/$name.log" | head -1)
   cases=$((cases + 1))
   if cmp -s "$a" "$b"; then
      echo "speckle_test: $name ($*${samples:+; $samples}): the same picture"
   else
      bad "$name ($*${samples:+; $samples}): the picture changes with the draws between two barriers run in another order"
   fi
}
for ssaa in disabled 2x 4x 8x; do
   for ta in cpu gpu; do
      check vk_${ssaa}_$ta vulkan reicast_ssaa=$ssaa reicast_ta_processing=$ta reicast_internal_resolution=1x reicast_threaded_rendering=disabled
   done
done
check vk_4x_gpu_2x_threaded vulkan reicast_ssaa=4x reicast_ta_processing=gpu reicast_internal_resolution=2x reicast_threaded_rendering=enabled
check vk_off_cpu_2x_threaded vulkan reicast_ssaa=disabled reicast_ta_processing=cpu reicast_internal_resolution=2x reicast_threaded_rendering=enabled
for ta in cpu gpu; do
   check gl_$ta gl reicast_ta_processing=$ta reicast_internal_resolution=1x reicast_threaded_rendering=disabled
done
[ $fail = 0 ] && echo "speckle_test: PASS (the sources, and $cases pictures each the same with the list draws reordered)"
exit $fail
