#!/bin/sh
# Checks on the PowerVR2 arithmetic the renderers copy, done on the CPU.
#
#   sh tools/pvr/run.sh
#
# fog_test.c: the fog coefficient of the shaders against the hardware's,
# for every depth there is.
#
# gles2_test.c: the core's own OpenGL ES 2 shaders, compiled and drawn
# with on an OpenGL ES 2 context: the fog across a polygon. Needs EGL and
# OpenGL ES 2 (headers and a driver; Mesa's software one will do, and is
# asked for when there is no display).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/pvr_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

${CC:-cc} -std=c99 -O2 -Wall -Wextra -ffp-contract=off \
   -o "$WORK/fog_test" "$ROOT/tools/pvr/fog_test.c" -lm
"$WORK/fog_test"

python3 "$ROOT/tools/pvr/gles2_shaders.py" "$ROOT/core/rend/gles/gles.cpp" > "$WORK/gles2_shaders.h"
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I "$WORK" \
   -o "$WORK/gles2_test" "$ROOT/tools/pvr/gles2_test.c" -lEGL -lGLESv2
if [ -z "$DISPLAY$WAYLAND_DISPLAY" ]; then
   EGL_PLATFORM=surfaceless
   export EGL_PLATFORM
fi
"$WORK/gles2_test"
